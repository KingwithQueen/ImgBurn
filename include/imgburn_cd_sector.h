#ifndef IMGBURN_CD_SECTOR_H
#define IMGBURN_CD_SECTOR_H

#include <stdbool.h>
#include <stdint.h>
#include "imgburn_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 保留正文并重建2352字节扇区的同步、地址及校验；支持音频、MODE1原始扇区和重复子头的MODE2 XA。 */
bool imgburn_cd_normalize_sector(TrackDataMode mode, uint32_t lba, uint8_t frame[2352]);

/* 生成完整零正文扇区；MODE2采用XA Form1，重复子头为00 00 08 00。 */
bool imgburn_cd_zero_sector(TrackDataMode mode, uint32_t lba, uint8_t frame[2352]);

#ifdef __cplusplus
}
#endif

#endif
