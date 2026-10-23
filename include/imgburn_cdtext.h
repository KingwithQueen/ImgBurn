#ifndef IMGBURN_CDTEXT_H
#define IMGBURN_CDTEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "imgburn_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMGBURN_CDTEXT_PACK_BYTES 18
#define IMGBURN_CDTEXT_MAX_BYTES (8 * 256 * IMGBURN_CDTEXT_PACK_BYTES)

typedef struct {
    uint8_t characterCode, language, firstTrack, lastTrack;
    uint16_t packCount, omittedCrcCount;
    bool doubleByte, supported, hasUnmapped;
} CdTextBlock;

typedef struct {
    uint8_t* packs;
    size_t bytes;
    unsigned blockCount;
    CdTextBlock blocks[8];
} CdText;

/* 只接收完整18字节pack流，保留全部原字节；输出对象须为空，由free释放。CRC全零可表示规范允许的缺省字段。 */
bool imgburn_cdtext_parse(const uint8_t* packs, size_t bytes, CdText* text);

/* 从UTF-8 CUE元数据生成英语Latin1单block；不可无损表达的字符、字段或容量均明确拒绝。 */
bool imgburn_cdtext_generate(const CueSheet* sheet, CdText* text);

/* 只填充已存在且轨号范围匹配的CUE元数据，不改变布局；失败时整份CUE保持原样，原pack始终保留。 */
bool imgburn_cdtext_extract(const CdText* text, unsigned block, CueSheet* sheet);
void imgburn_cdtext_free(CdText* text);

#ifdef __cplusplus
}
#endif

#endif
