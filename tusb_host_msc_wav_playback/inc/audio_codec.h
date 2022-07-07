#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H
#include <string.h>
#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

hpm_stat_t hpm_playbackwav(char* fname);
void init_dao(void);
FRESULT sd_mount_fs(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CODEC_H */
