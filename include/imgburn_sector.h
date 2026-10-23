#ifndef IMGBURN_SECTOR_H
#define IMGBURN_SECTOR_H
#include "imgburn_mmc.h"
typedef enum { SECTOR_USER_DATA, SECTOR_RAW, SECTOR_RAW_SUBCHANNEL } SectorMode;
typedef struct { uint32_t lba; SectorMode mode; bool useLast; HANDLE cancelEvent; } SectorRequest;
typedef struct { uint32_t lba, lastLba, bytes; MmcProfile profile; uint8_t data[2448]; } SectorData;
/* 单扇区只读；失败时输出清零，不让旧内容被误认为新的读取结果。 */
bool imgburn_sector_read(OpticalDrive* drive, const SectorRequest* request, SectorData* output);
bool imgburn_sector_format(const SectorData* sector, wchar_t* text, size_t capacity);
#endif
