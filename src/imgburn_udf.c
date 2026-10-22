/**
 * @file imgburn_udf.c
 * @brief ImgBurn Core C - ECMA-167 / OSTA UDF Engine Implementation
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_udf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    tag->descVersion = (tagId >= 0x0100) ? 3 : 2;
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

    SYSTEMTIME st;
    if (ft && (ft->dwLowDateTime || ft->dwHighDateTime)) {
        FileTimeToSystemTime(ft, &st);
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

uint8_t udf_encode_dstring(const wchar_t* src, uint8_t* dst, size_t maxDstLen) {
    if (!src || !dst || maxDstLen < 2) return 0;

    size_t len = wcslen(src);
    bool use8Bit = true;
    for (size_t i = 0; i < len; i++) {
        if ((uint16_t)src[i] > 0xFF) {
            use8Bit = false;
            break;
        }
    }

    if (use8Bit) {
        if (len + 1 > maxDstLen) len = maxDstLen - 1;
        dst[0] = 0x08; /* 8-bit compression ID */
        for (size_t i = 0; i < len; i++) {
            dst[1 + i] = (uint8_t)src[i];
        }
        return (uint8_t)(len + 1);
    } else {
        if (len * 2 + 1 > maxDstLen) len = (maxDstLen - 1) / 2;
        dst[0] = 0x10; /* 16-bit compression ID */
        for (size_t i = 0; i < len; i++) {
            dst[1 + i * 2] = (uint8_t)(src[i] >> 8);
            dst[1 + i * 2 + 1] = (uint8_t)(src[i] & 0xFF);
        }
        return (uint8_t)(len * 2 + 1);
    }
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

void udf_build_avdp(uint8_t* outSector, uint32_t avdpLba, uint32_t mvdsLba, uint32_t mvdsSectors) {
    if (!outSector) return;
    memset(outSector, 0, UDF_SECTOR_SIZE);

    UdfAnchorVolDescPtr* avdp = (UdfAnchorVolDescPtr*)outSector;

    avdp->mainVdsExtent.location = mvdsLba;
    avdp->mainVdsExtent.length = mvdsSectors * UDF_SECTOR_SIZE;

    /* Reserve VDS (can point to identical extent or backup) */
    avdp->reserveVdsExtent = avdp->mainVdsExtent;

    udf_finalize_tag(&avdp->descTag, UDF_TAG_AVDP, avdpLba, 512);
}

void udf_build_mvds(uint8_t* outBuffer,
                    uint32_t startLba,
                    const wchar_t* volumeLabel,
                    uint32_t partitionStartLba,
                    uint32_t partitionSectorCount,
                    uint32_t fsdLba,
                    bool isUdf250) {
    if (!outBuffer) return;
    memset(outBuffer, 0, 4 * UDF_SECTOR_SIZE);

    /* --- Sector 0: Primary Volume Descriptor (PVD) --- */
    UdfPrimaryVolDesc* pvd = (UdfPrimaryVolDesc*)outBuffer;
    pvd->vdsNum = 1;
    pvd->pvdNum = 0;
    udf_encode_dstring(volumeLabel ? volumeLabel : L"UDF_VOLUME", (uint8_t*)pvd->volumeIdent, 32);
    pvd->volumeSeqNum = 1;
    pvd->maxVolumeSeqNum = 1;
    pvd->interchangeLevel = 2;
    pvd->maxInterchangeLevel = 2;
    pvd->charSetList = 1;
    pvd->maxCharSetList = 1;
    udf_encode_dstring(volumeLabel ? volumeLabel : L"UDF_VOLSET", (uint8_t*)pvd->volumeSetIdent, 128);

    /* CS0 charset */
    pvd->descriptorCharSet[0] = 0; /* CS0 */
    memcpy(&pvd->descriptorCharSet[1], "+OSTA/CS0", 9);

    /* Application ID & Impl ID */
    pvd->applicationIdent.flags = 0;
    memcpy(pvd->applicationIdent.ident, "*LIGHTNING UK! ImgBurn", 22);

    pvd->implementationIdent.flags = 0;
    memcpy(pvd->implementationIdent.ident, "*LIGHTNING UK! ImgBurn", 22);

    udf_filetime_to_timestamp(NULL, &pvd->recordingDateAndTime);
    udf_finalize_tag(&pvd->descTag, UDF_TAG_PVD, startLba, 512);

    /* --- Sector 1: Partition Descriptor (PD) --- */
    UdfPartitionDesc* pd = (UdfPartitionDesc*)(outBuffer + UDF_SECTOR_SIZE);
    pd->vdsNum = 1;
    pd->partitionFlags = 1; /* Allocated */
    pd->partitionNum = 0;
    pd->partitionContents.flags = 0;
    memcpy(pd->partitionContents.ident, isUdf250 ? "+NSR03" : "+NSR02", 6);
    pd->accessType = 1; /* Read-only */
    pd->partitionStart = partitionStartLba;
    pd->partitionLen = partitionSectorCount;
    pd->implementationIdent.flags = 0;
    memcpy(pd->implementationIdent.ident, "*LIGHTNING UK! ImgBurn", 22);
    udf_finalize_tag(&pd->descTag, UDF_TAG_PD, startLba + 1, 512);

    /* --- Sector 2: Logical Volume Descriptor (LVD) --- */
    UdfLogicalVolDesc* lvd = (UdfLogicalVolDesc*)(outBuffer + 2 * UDF_SECTOR_SIZE);
    lvd->vdsNum = 1;
    lvd->descriptorCharSet[0] = 0;
    memcpy(&lvd->descriptorCharSet[1], "+OSTA/CS0", 9);
    udf_encode_dstring(volumeLabel ? volumeLabel : L"UDF_VOLUME", (uint8_t*)lvd->logicalVolumeIdent, 128);
    lvd->logicalBlockSize = UDF_SECTOR_SIZE;

    lvd->domainIdent.flags = 0;
    memcpy(lvd->domainIdent.ident, "*OSTA UDF Compliant", 19);
    lvd->domainIdent.identSuffix[0] = isUdf250 ? 0x50 : 0x02; /* UDF 2.50 or UDF 1.02 */
    lvd->domainIdent.identSuffix[1] = isUdf250 ? 0x02 : 0x01;

    /* LongAd to File Set Descriptor (FSD) in LogicalVolumeContentsUse */
    UdfLongAd* fsdAd = (UdfLongAd*)lvd->logicalVolumeContentsUse;
    fsdAd->length = UDF_SECTOR_SIZE;
    fsdAd->logicalBlockNum = fsdLba - partitionStartLba; /* Relative to partition */
    fsdAd->partitionRefNum = 0;

    /* Type 1 Partition Map (ECMA-167 3/10.7.2): 6 bytes */
    lvd->mapTableLength = 6;
    lvd->numPartitionMaps = 1;
    uint8_t* pmap = lvd->partitionMaps;
    pmap[0] = 1; /* Map type = 1 */
    pmap[1] = 6; /* Map length = 6 */
    *(uint16_t*)&pmap[2] = 1; /* Volume Seq Num = 1 */
    *(uint16_t*)&pmap[4] = 0; /* Partition Num = 0 */

    lvd->implementationIdent.flags = 0;
    memcpy(lvd->implementationIdent.ident, "*LIGHTNING UK! ImgBurn", 22);
    udf_finalize_tag(&lvd->descTag, UDF_TAG_LVD, startLba + 2, 512);

    /* --- Sector 3: Terminating Descriptor (TD) --- */
    UdfTerminatingDesc* td = (UdfTerminatingDesc*)(outBuffer + 3 * UDF_SECTOR_SIZE);
    udf_finalize_tag((UdfDescriptorTag*)td, UDF_TAG_TD, startLba + 3, 512);
}

void udf_build_fsd(uint8_t* outSector, uint32_t fsdLba, uint32_t rootDirFeLba) {
    if (!outSector) return;
    memset(outSector, 0, UDF_SECTOR_SIZE);

    UdfFileSetDesc* fsd = (UdfFileSetDesc*)outSector;
    udf_filetime_to_timestamp(NULL, &fsd->recordingDateAndTime);
    fsd->interchangeLevel = 3;
    fsd->maxInterchangeLevel = 3;
    fsd->charSetList = 1;
    fsd->maxCharSetList = 1;
    fsd->fileSetNum = 0;
    fsd->fileSetDescNum = 0;

    fsd->logicalVolumeIdentCharSet[0] = 0;
    memcpy(&fsd->logicalVolumeIdentCharSet[1], "+OSTA/CS0", 9);
    fsd->fileSetCharSet[0] = 0;
    memcpy(&fsd->fileSetCharSet[1], "+OSTA/CS0", 9);

    /* Root directory ICB (LongAd) */
    fsd->rootDirectoryIcb.length = UDF_SECTOR_SIZE;
    fsd->rootDirectoryIcb.logicalBlockNum = rootDirFeLba;
    fsd->rootDirectoryIcb.partitionRefNum = 0;

    fsd->domainIdent.flags = 0;
    memcpy(fsd->domainIdent.ident, "*OSTA UDF Compliant", 19);

    udf_finalize_tag(&fsd->descTag, UDF_TAG_FSD, fsdLba, 512);
}
