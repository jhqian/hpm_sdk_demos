# Copyright 2021 hpmicro
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.13)

set(CONFIG_SDMMC 1)
set(CONFIG_AUDIO_CODEC 1)
set(CONFIG_AUDIO_DECODER_WAV 1)

set(CONFIG_FREERTOS 1)
set(CONFIG_TINYUSB 1)
set(CONFIG_USB_HOST 1)
set(CONFIG_FATFS 1)
set(CONFIG_USB_FATFS 1)

find_package(hpm-sdk REQUIRED HINTS $ENV{HPM_SDK_BASE})

sdk_compile_definitions(-DINIT_EXT_RAM_FOR_DATA=1)

if(CONFIG_FREERTOS)
  sdk_compile_definitions(-D__freertos_irq_stack_top=_stack)
  sdk_compile_definitions(-DCONFIG_FREERTOS=1)
  sdk_compile_definitions(-DUSE_NONVECTOR_MODE=1)
  sdk_compile_definitions(-DDISABLE_IRQ_PREEMPTIVE=1)
endif()

sdk_compile_definitions(-DCFG_TUSB_MCU=OPT_MCU_HPM)
sdk_compile_definitions(-DUSB_HOST_MCU_CORE=HPM_CORE0)
sdk_compile_definitions(-DUSB_FATFS_ENABLE=1)

project(tusb_host_msc_wav_playback)
sdk_inc(inc)
sdk_app_src(src/main.c)
sdk_app_src(src/msc_app.c)
sdk_app_src(src/file_op.c)
sdk_app_src(src/audio_codec.c)
generate_ses_project()
