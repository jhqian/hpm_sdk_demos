/*
 * Copyright (c) 2022 hpmicro
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
#define USING_DAO 1

#include <stdio.h>
#include "board.h"
#include "hpm_clock_drv.h"
#include "hpm_i2s_drv.h"

#if USING_CODEC
#include "hpm_sgtl5000.h"
#elif USING_DAO
#include "hpm_dao_drv.h"
#endif

#include "ff.h"
#include "hpm_dma_drv.h"
#include "hpm_dmamux_drv.h"
#include <string.h>
#include "hpm_wav_codec.h"
#include "hpm_l1c_drv.h"
#include "diskio.h"

#ifdef USING_CODEC
#define CODEC_I2C            BOARD_APP_I2C_BASE
#define TARGET_I2S            BOARD_APP_I2S_BASE
#define TARGET_I2S_CLK_NAME   BOARD_APP_I2S_CLK_NAME
#define TARGET_I2S_DATA_LINE  BOARD_APP_I2S_DATA_LINE
#define TARGET_I2S_TX_DMAMUX_SRC HPM_DMA_SRC_I2S0_TX
#define CODEC_I2C_ADDRESS    SGTL5000_I2C_ADDR

sgtl_config_t sgtl5000_config = {
    .route = sgtl_route_playback_record,  /*!< Audio data route.*/
    .bus = sgtl_bus_left_justified,       /*!< Audio transfer protocol */
    .master = false,                      /*!< Master or slave. True means master, false means slave. */
    .format = {.mclk_hz = TARGET_I2S_MCLK_HZ,
               .sample_rate = CODEC_SAMPLE_RATE_HZ,
               .bit_width = CODEC_BIT_WIDTH,
               .sclk_edge = sgtl_sclk_valid_edge_rising}, /*!< audio format */
};

sgtl_context_t sgtl5000_context = {
    .ptr = CODEC_I2C,
    .slave_address = CODEC_I2C_ADDRESS, /* I2C address */
};

#elif USING_DAO
#define TARGET_I2S            DAO_I2S
#define TARGET_I2S_CLK_NAME   clock_dao
#define TARGET_I2S_DATA_LINE  0
#define TARGET_I2S_TX_DMAMUX_SRC HPM_DMA_SRC_I2S1_TX
#else
#error define USING_CODEC or USING_DAO
#endif

#define TARGET_I2S_MCLK_HZ    24576000UL
#define CODEC_SAMPLE_RATE_HZ 48000U
#define CODEC_BIT_WIDTH      32U

#define CODEC_BUFF_SIZE      50480

volatile bool dma_transfer_done = false;
volatile bool dma_transfer_error = false;

ATTR_PLACE_AT_NONCACHEABLE_BSS FIL wav_file;
ATTR_PLACE_AT_NONCACHEABLE_BSS hpm_wav_ctrl wav_ctrl;
ATTR_PLACE_AT_NONCACHEABLE_WITH_ALIGNMENT(32) uint8_t i2s_buff1[CODEC_BUFF_SIZE];
ATTR_PLACE_AT_NONCACHEABLE_WITH_ALIGNMENT(32) uint8_t i2s_buff2[CODEC_BUFF_SIZE];
ATTR_PLACE_AT_NONCACHEABLE_WITH_ALIGNMENT(32) uint8_t i2s_buff3[CODEC_BUFF_SIZE];

hpm_stat_t hpm_audiocodec_search_file(char * file_name, HPM_AUDIOCODEC_FILE * fil)
{
    FRESULT rsl = f_open(&wav_file, file_name, FA_READ);
    if(rsl == FR_OK) {
        *fil = (uint32_t) &wav_file;
        return status_audio_codec_true;
    } else {
        return status_audio_codec_none_file;
    }
}

hpm_stat_t hpm_audiocodec_read_file(HPM_AUDIOCODEC_FILE fil, uint32_t num_bytes, uint8_t *data, uint32_t *br)
{
    FRESULT rsl = f_read((FIL *)fil, data, num_bytes, (UINT*)br);
    if(rsl == FR_OK) {
        return status_audio_codec_true;
    } else {
        return status_audio_codec_false;
    }
}

hpm_stat_t hpm_audiocodec_write_file(HPM_AUDIOCODEC_FILE fil, uint32_t num_bytes, uint8_t *data, uint32_t *br)
{
    FRESULT rsl = f_write((FIL *)fil, data, num_bytes, (UINT*)br);
    if(rsl == FR_OK) {
        return status_audio_codec_true;
    } else {
        return status_audio_codec_false;
    }
}

hpm_stat_t hpm_audiocodec_close_file(HPM_AUDIOCODEC_FILE fil)
{
    FRESULT rsl = f_close((FIL *)fil);
    if(rsl == FR_OK) {
        return status_audio_codec_true;
    } else {
        return status_audio_codec_false;
    }
}

void isr_dma(void)
{
    volatile hpm_stat_t stat;
    dma_transfer_done = true;
    stat = dma_check_transfer_status(BOARD_APP_HDMA, 2);
    if (0 == (stat & DMA_CHANNEL_STATUS_TC)) {
        dma_transfer_error = true;
    }
}
SDK_DECLARE_EXT_ISR_M(BOARD_APP_HDMA_IRQ, isr_dma)

uint32_t * strdata = 0;
void i2s_dma_cfg(void)
{
    dma_channel_config_t ch_config = {0};

    intc_m_enable_irq_with_priority(BOARD_APP_HDMA_IRQ, 5);

    dma_default_channel_config(BOARD_APP_HDMA, &ch_config);
    ch_config.src_addr = core_local_mem_to_sys_address(HPM_CORE0, (uint32_t)strdata);
    ch_config.dst_addr = (uint32_t)&TARGET_I2S->TXD[TARGET_I2S_DATA_LINE];
    ch_config.src_width = DMA_TRANSFER_WIDTH_WORD;
    ch_config.dst_width = DMA_TRANSFER_WIDTH_WORD;
    ch_config.src_addr_ctrl = DMA_ADDRESS_CONTROL_INCREMENT;
    ch_config.dst_addr_ctrl = DMA_ADDRESS_CONTROL_FIXED;
    ch_config.size_in_byte = CODEC_BUFF_SIZE;
    ch_config.dst_mode = DMA_HANDSHAKE_MODE_HANDSHAKE;
    ch_config.src_burst_size = 0;

    if (status_success != dma_setup_channel(BOARD_APP_HDMA, 2, &ch_config)) {
        printf(" dma setup channel failed\n");
        return;
    }
    dmamux_config(BOARD_APP_DMAMUX, 2, TARGET_I2S_TX_DMAMUX_SRC, 1);
}

#ifdef USING_CODEC
void test_sgtl5000_playback_record(void)
{
    i2s_config_t i2s_config;
    i2s_transfer_config_t transfer;

    /* Config I2S interface to CODEC */
    i2s_get_default_config(TARGET_I2S, &i2s_config);
    i2s_config.enable_mclk_out = true;
    i2s_init(TARGET_I2S, &i2s_config);

    i2s_get_default_transfer_config(&transfer);
    transfer.data_line = I2S_DATA_LINE_2;
    transfer.sample_rate = CODEC_SAMPLE_RATE_HZ;
    transfer.master_mode = true;
    /* configure I2S RX and TX */
    if (status_success != i2s_config_transfer(TARGET_I2S, TARGET_I2S_MCLK_HZ, &transfer))
    {
        printf("I2S config failed for CODEC\n");
        while(1);
    }
    i2s_enable_tx_dma_request(TARGET_I2S);

    sgtl5000_config.route = sgtl_route_playback;
    sgtl_init(&sgtl5000_context, &sgtl5000_config);

    printf("Test WAV decode\n");
}
#endif

hpm_stat_t hpm_playbackwav(char* fname)
{
    hpm_stat_t res;
    uint32_t fillnum;
    FIL file = {0};
    wav_ctrl.func.close_file = hpm_audiocodec_close_file;
    wav_ctrl.func.read_file = hpm_audiocodec_read_file;
    wav_ctrl.func.search_file = hpm_audiocodec_search_file;
    wav_ctrl.func.write_file = hpm_audiocodec_write_file;

    res = hpm_wav_decode_init(fname, &wav_ctrl);
    if(res == status_audio_codec_true) {
        printf("music playing time:%d seconds ...\r\n", wav_ctrl.sec_total);
        f_lseek(&file, wav_ctrl.data_pos);
        fillnum = hpm_wav_decode(&wav_ctrl, i2s_buff1, i2s_buff3, 512);
        dma_transfer_done = true;
        while (1) {
            fillnum = hpm_wav_decode(&wav_ctrl, i2s_buff1, i2s_buff3, CODEC_BUFF_SIZE);
            while(dma_transfer_done == false) {
                vTaskDelay(0);
            }
            strdata = (uint32_t *)&i2s_buff1[0];
            dma_transfer_done = false;
            l1c_dc_writeback_all();
            i2s_dma_cfg();
            i2s_enable_tx_dma_request(TARGET_I2S);

            fillnum = hpm_wav_decode(&wav_ctrl, i2s_buff2, i2s_buff3, CODEC_BUFF_SIZE);
            while(dma_transfer_done == false) {
                vTaskDelay(0);
            }
            strdata = (uint32_t *)&i2s_buff2[0];
            dma_transfer_done = false;
            l1c_dc_writeback_all();
            i2s_dma_cfg();
            i2s_enable_tx_dma_request(TARGET_I2S);
            if (fillnum < CODEC_BUFF_SIZE) {
                printf("music end.\r\n");
                return status_audio_codec_end;
            }
        }
    } else {
        printf("music file error.\r\n");
        res = status_audio_codec_false;
    }
	return res;
}
const char *show_error_string(FRESULT fresult)
{
    const char *result_str;

    switch (fresult) {
    case FR_OK:
        result_str = "succeeded";
        break;
    case FR_DISK_ERR:
        result_str = "A hard error occurred in the low level disk I/O level";
        break;
    case FR_INT_ERR:
        result_str = "Assertion failed";
        break;
    case FR_NOT_READY:
        result_str = "The physical drive cannot work";
        break;
    case FR_NO_FILE:
        result_str = "Could not find the file";
        break;
    case FR_NO_PATH:
        result_str = "Could not find the path";
        break;
    case FR_INVALID_NAME:
        result_str = "Tha path name format is invalid";
        break;
    case FR_DENIED:
        result_str = "Access denied due to prohibited access or directory full";
        break;
    case FR_EXIST:
        result_str = "Access denied due to prohibited access";
        break;
    case FR_INVALID_OBJECT:
        result_str = "The file/directory object is invalid";
        break;
    case FR_WRITE_PROTECTED:
        result_str = "The physical drive is write protected";
        break;
    case FR_INVALID_DRIVE:
        result_str = "The logical driver number is invalid";
        break;
    case FR_NOT_ENABLED:
        result_str = "The volume has no work area";
        break;
    case FR_NO_FILESYSTEM:
        result_str = "There is no valid FAT volume";
        break;
    case FR_MKFS_ABORTED:
        result_str = "THe f_mkfs() aborted due to any problem";
        break;
    case FR_TIMEOUT:
        result_str = "Could not get a grant to access the volume within defined period";
        break;
    case FR_LOCKED:
        result_str = "The operation is rejected according to the file sharing policy";
        break;
    case FR_NOT_ENOUGH_CORE:
        result_str = "LFN working buffer could not be allocated";
        break;
    case FR_TOO_MANY_OPEN_FILES:
        result_str = "Number of open files > FF_FS_LOCK";
        break;
    case FR_INVALID_PARAMETER:
        result_str = "Given parameter is invalid";
        break;
    default:
        result_str = "Unknown error";
        break;
    }
    return result_str;
}


#ifdef USING_CODEC
void init_codec(void)
{
    board_init_i2c(CODEC_I2C);

    init_i2s_pins(TARGET_I2S);
    board_init_i2s_clock(TARGET_I2S);
}
#endif

#ifdef USING_DAO
void init_dao(void)
{
    i2s_config_t i2s_config;
    i2s_transfer_config_t transfer;
    dao_config_t dao_config;

    board_init_dao_clock();
    init_dao_pins();

    i2s_get_default_config(TARGET_I2S, &i2s_config);
    i2s_init(TARGET_I2S, &i2s_config);
    /*
     * config transfer for DAO
     */
    i2s_get_default_transfer_config_for_dao(&transfer);
    if (status_success != i2s_config_tx(TARGET_I2S, TARGET_I2S_MCLK_HZ, &transfer)) {
        printf("I2S config failed for DAO\n");
        while (1) {
        }
    }

    dao_get_default_config(HPM_DAO, &dao_config);
    dao_config.enable_mono_output = true;
    dao_init(HPM_DAO, &dao_config);

    dao_start(HPM_DAO);
}
#endif
