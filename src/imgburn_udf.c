/**
 * @file imgburn_udf.c
 * @brief ImgBurn Core C - ECMA-167 / OSTA UDF Engine Implementation
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_udf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Standard CRC-CCITT (polynomial 0x1021) lookup table */
static uint16_t s_crc16_table[256];
static bool s_crc16_init = false;

static void init_udf_crc16_table(void) {
    if (s_crc16_init) return;
    for (int i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (int j = 0; j < 8; j++) {
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
        }
        s_crc16_table[i] = c;
    }
    s_crc16_init = true;
}

uint16_t udf_crc16(const uint8_t* data, size_t length) {
    init_udf_crc16_table();
    uint16_t crc = 0;
    for (size_t i = 0; i < length; i++) {
        crc = (uint16_t)((crc << 8) ^ s_crc16_table[(uint8_t)((crc >> 8) ^ data[i])]);
    }
    return crc;
}

void udf_finalize_tag(UdfDescriptorTag* tag, uint16_t tagId, uint32_t lba, uint16_t fullDescSize) {
    if (!tag) return;

    tag->tagId = tagId;
    tag->descVersion = 2;
    tag->reserved = 0;
    tag->tagSerial = 1;
    tag->tagLocation = lba;

    if (fullDescSize > 16) {
        tag->descCrcLen = fullDescSize - 16;
        tag->descCrc = udf_crc16((const uint8_t*)tag + 16, tag->descCrcLen);
    } else {
        tag->descCrcLen = 0;
        tag->descCrc = 0;
    }

    /* Tag Checksum: sum of bytes 0..3 and 5..15 modulo 256 */
    const uint8_t* b = (const uint8_t*)tag;
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        if (i != 4) sum += b[i];
    }
    tag->tagChecksum = (uint8_t)(sum & 0xFF);
}

void udf_filetime_to_timestamp(const FILETIME* ft, UdfTimestamp* outTs) {
    if (!outTs) return;
    memset(outTs, 0, sizeof(*outTs));

    SYSTEMTIME st = {0};
    if (ft && (ft->dwLowDateTime || ft->dwHighDateTime)) {
        if (!FileTimeToSystemTime(ft, &st)) GetSystemTime(&st);
    } else {
        GetSystemTime(&st);
    }

    outTs->typeAndTimezone = 0x1000; /* Type 1 (local/UTC), offset 0 */
    outTs->year = (int16_t)st.wYear;
    outTs->month = (uint8_t)st.wMonth;
    outTs->day = (uint8_t)st.wDay;
    outTs->hour = (uint8_t)st.wHour;
    outTs->minute = (uint8_t)st.wMinute;
    outTs->second = (uint8_t)st.wSecond;
    outTs->centiseconds = (uint8_t)(st.wMilliseconds / 10);
}

void udf_finalize_tag_revision(UdfDescriptorTag* tag, uint16_t tagId, uint32_t block, uint16_t bytes, uint16_t revision) {
    udf_finalize_tag(tag, tagId, block, bytes);
    tag->descVersion = revision >= 0x0200 ? 3 : 2;
    tag->tagChecksum = 0;
    const uint8_t* data = (const uint8_t*)tag;
    for (int i = 0; i < 16; i++) if (i != 4) tag->tagChecksum = (uint8_t)(tag->tagChecksum + data[i]);
}

void udf_set_charset(uint8_t* charset) {
    memset(charset, 0, 64);
    memcpy(charset + 1, "OSTA Compressed Unicode", 23);
}

uint8_t udf_encode_name(const wchar_t* src, uint8_t* dst, size_t capacity) {
    if (!src || !dst || capacity < 1) return 0;
    size_t count = wcslen(src);
    bool narrow = true;
    for (size_t i = 0; i < count; i++) {
        uint16_t value = (uint16_t)src[i];
        if (value == 0xFFFE || value == 0xFEFF || (value >= 0xD800 && value <= 0xDFFF)) return 0;
        if (value > 255) narrow = false;
    }
    size_t bytes = 1 + count * (narrow ? 1 : 2);
    if (bytes > capacity || bytes > 255) return 0;
    dst[0] = narrow ? 8 : 16;
    for (size_t i = 0; i < count; i++) {
        if (narrow) dst[1 + i] = (uint8_t)src[i];
        else { dst[1 + 2 * i] = (uint8_t)(src[i] >> 8); dst[2 + 2 * i] = (uint8_t)src[i]; }
    }
    return (uint8_t)bytes;
}

uint8_t udf_encode_dstring(const wchar_t* src, uint8_t* dst, size_t capacity) {
    if (!src || !dst || capacity < 2 || capacity > 256) return 0;
    memset(dst, 0, capacity);
    if (!src[0]) return 0;
    wchar_t clipped[128] = {0};
    size_t count = wcslen(src), limit = capacity - 2;
    bool narrow = true;
    for (size_t i = 0; i < count; i++) if ((uint16_t)src[i] > 255) narrow = false;
    if (!narrow) limit /= 2;
    if (count > limit) count = limit;
    if (count > 127) count = 127;
    wmemcpy(clipped, src, count);
    uint8_t bytes = udf_encode_name(clipped, dst, capacity - 1);
    dst[capacity - 1] = bytes;
    return bytes;
}

void udf_build_vrs(uint8_t* outBuffer, bool isUdf250) {
    if (!outBuffer) return;
    memset(outBuffer, 0, 3 * UDF_SECTOR_SIZE);

    /* Sector 0: BEA01 (Beginning Extended Area Descriptor) */
    uint8_t* s0 = outBuffer;
    s0[0] = 0x00; /* Structure Type 0 */
    memcpy(&s0[1], "BEA01\x01", 6);

    /* Sector 1: NSR02 (for UDF 1.02) or NSR03 (for UDF 2.50) */
    uint8_t* s1 = outBuffer + UDF_SECTOR_SIZE;
    s1[0] = 0x00;
    const char* nsr = isUdf250 ? "NSR03\x01" : "NSR02\x01";
    memcpy(&s1[1], nsr, 6);

    /* Sector 2: TEA01 (Terminating Extended Area Descriptor) */
    uint8_t* s2 = outBuffer + 2 * UDF_SECTOR_SIZE;
    s2[0] = 0x00;
    memcpy(&s2[1], "TEA01\x01", 6);
}
