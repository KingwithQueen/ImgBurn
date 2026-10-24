/* 2048 字节逻辑数据源：普通镜像与 MDS 引用的分卷共用地址规则。 */
#ifndef IMGBURN_DATA_SOURCE_H
#define IMGBURN_DATA_SOURCE_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    HANDLE descriptor;
    struct DataImagePart* parts;
    unsigned partCount;
    uint32_t sectors, layerBreakLba;
    unsigned layerCount;
    bool oppositeTrackPath, mds;
} DataImageSource;

/* 支持 ISO/BIN、单会话单轨的 DVD 或 MODE1/2048 CD MDS；打开期间锁定全部引用文件。 */
bool imgburn_data_source_open(DataImageSource* source, const wchar_t* path);
bool imgburn_data_source_read(DataImageSource* source, uint32_t lba, uint16_t sectors, void* buffer);
void imgburn_data_source_close(DataImageSource* source);

#endif
