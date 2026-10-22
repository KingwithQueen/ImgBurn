/**
 * @file imgburn_udf.h
 * @brief ImgBurn Core C - ECMA-167 / OSTA UDF (1.02 & 2.50) Specification Engine
 *
 * Implements complete UDF structures, Tag Checksums, CRC-CCITT, OSTA compressed
 * unicode strings, AVDP, VDS, FSD, ICB File Entries, and File Identifier Descriptors.
 */

#ifndef IMGBURN_UDF_H
#define IMGBURN_UDF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#define UDF_SECTOR_SIZE             2048
#define UDF_AVDP_LBA                256

/* UDF Descriptor Tag Identifiers (ECMA-167 3/7.2) */
#define UDF_TAG_PVD                 0x0001 /* Primary Volume Descriptor */
#define UDF_TAG_AVDP                0x0002 /* Anchor Volume Descriptor Pointer */
#define UDF_TAG_VDP                 0x0003 /* Volume Descriptor Pointer */
#define UDF_TAG_IUVD                0x0004 /* Implementation Use Volume Descriptor */
#define UDF_TAG_PD                  0x0005 /* Partition Descriptor */
#define UDF_TAG_LVD                 0x0006 /* Logical Volume Descriptor */
#define UDF_TAG_USD                 0x0007 /* Unallocated Space Descriptor */
#define UDF_TAG_TD                  0x0008 /* Terminating Descriptor */
#define UDF_TAG_LVID                0x0009 /* Logical Volume Integrity Descriptor */
#define UDF_TAG_FSD                 0x0100 /* File Set Descriptor */
#define UDF_TAG_FID                 0x0101 /* File Identifier Descriptor */
#define UDF_TAG_ALLOC_EXTENT        0x0102 /* Allocation Extent Descriptor */
#define UDF_TAG_ICB_INDIRECT        0x0103 /* Indirect Entry */
#define UDF_TAG_ICB_TERMINAL        0x0104 /* Terminal Entry */
#define UDF_TAG_FE                  0x0105 /* File Entry */
#define UDF_TAG_EXTENDED_FE         0x010A /* Extended File Entry (UDF 2.00+) */

/* ICB File Types (ECMA-167 4/14.6.6) */
#define UDF_FILE_TYPE_UNKNOWN       0
#define UDF_FILE_TYPE_DIRECTORY     4
#define UDF_FILE_TYPE_REGULAR       5
#define UDF_FILE_TYPE_METADATA_MAIN 250 /* UDF 2.50 Metadata File */
#define UDF_FILE_TYPE_METADATA_MIRR 251 /* UDF 2.50 Metadata Mirror File */
#define UDF_FILE_TYPE_METADATA_BMAP 252 /* UDF 2.50 Metadata Bitmap File */

/* FID Characteristics Flags (ECMA-167 4/14.4.3) */
#define UDF_FID_FLAG_EXISTENCE      0x01 /* Hidden */
#define UDF_FID_FLAG_DIRECTORY      0x02 /* Directory */
#define UDF_FID_FLAG_DELETED        0x04 /* Deleted */
#define UDF_FID_FLAG_PARENT         0x08 /* Parent Directory (..) */
#define UDF_FID_FLAG_METADATA       0x10

#pragma pack(push, 1)

/**
 * @brief Standard 16-byte Descriptor Tag (ECMA-167 3/7.2)
 */
typedef struct {
    uint16_t tagId;
    uint16_t descVersion;
    uint8_t  tagChecksum;
    uint8_t  reserved;
    uint16_t tagSerial;
    uint16_t descCrc;
    uint16_t descCrcLen;
    uint32_t tagLocation;           /* LBA of this descriptor */
} UdfDescriptorTag;

/**
 * @brief Terminating Descriptor (ECMA-167 3/10.9)
 */
typedef struct {
    UdfDescriptorTag descTag;
    uint8_t          reserved[496];
} UdfTerminatingDesc;

/**
 * @brief UDF Timestamp (ECMA-167 1/7.3)
 */
typedef struct {
    uint16_t typeAndTimezone;       /* Type: 4 bits, Timezone: 12 bits */
    int16_t  year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  centiseconds;
    uint8_t  hundredsOfMicroseconds;
    uint8_t  microseconds;
} UdfTimestamp;

/**
 * @brief Extent Allocation Descriptor (extent_ad)
 */
typedef struct {
    uint32_t length;
    uint32_t location;              /* Logical block number */
} UdfExtentAd;

/**
 * @brief Short Allocation Descriptor (short_ad)
 */
typedef struct {
    uint32_t length;                /* 30 bits len + 2 bits type (00=recorded, 01=allocated not rec) */
    uint32_t position;              /* Logical block number */
} UdfShortAd;

/**
 * @brief Long Allocation Descriptor (long_ad)
 */
typedef struct {
    uint32_t length;
    uint32_t logicalBlockNum;
    uint16_t partitionRefNum;
    uint8_t  implementationUse[6];
} UdfLongAd;

/**
 * @brief Entity Identifier (EntityID / regid)
 */
typedef struct {
    uint8_t flags;
    char    ident[23];
    char    identSuffix[8];
} UdfEntityId;

/**
 * @brief Anchor Volume Descriptor Pointer (AVDP) (ECMA-167 3/10.2)
 */
typedef struct {
    UdfDescriptorTag descTag;       /* TagId = 2 */
    UdfExtentAd      mainVdsExtent;
    UdfExtentAd      reserveVdsExtent;
    uint8_t          reserved[480];
} UdfAnchorVolDescPtr;

/**
 * @brief Primary Volume Descriptor (PVD) (ECMA-167 3/10.1)
 */
typedef struct {
    UdfDescriptorTag descTag;       /* TagId = 1 */
    uint32_t         vdsNum;
    uint32_t         pvdNum;
    char             volumeIdent[32];
    uint16_t         volumeSeqNum;
    uint16_t         maxVolumeSeqNum;
    uint16_t         interchangeLevel;
    uint16_t         maxInterchangeLevel;
    uint32_t         charSetList;
    uint32_t         maxCharSetList;
    char             volumeSetIdent[128];
    uint8_t          descriptorCharSet[64];
    uint8_t          explanatoryCharSet[64];
    UdfExtentAd      volumeAbstract;
    UdfExtentAd      volumeNotice;
    UdfEntityId      applicationIdent;
    UdfTimestamp     recordingDateAndTime;
    UdfEntityId      implementationIdent;
    uint8_t          implementationUse[64];
    uint32_t         predecessorVdsLocation;
    uint16_t         flags;
    uint8_t          reserved[22];
} UdfPrimaryVolDesc;

/**
 * @brief Partition Descriptor (PD) (ECMA-167 3/10.5)
 */
typedef struct {
    UdfDescriptorTag descTag;       /* TagId = 5 */
    uint32_t         vdsNum;
    uint16_t         partitionFlags;
    uint16_t         partitionNum;
    UdfEntityId      partitionContents;
    uint8_t          partitionContentsUse[128];
    uint32_t         accessType;    /* 1 = Read-only */
    uint32_t         partitionStart;/* LBA */
    uint32_t         partitionLen;  /* Sector count */
    UdfEntityId      implementationIdent;
    uint8_t          implementationUse[128];
    uint8_t          reserved[156];
} UdfPartitionDesc;

/**
 * @brief Logical Volume Descriptor (LVD) (ECMA-167 3/10.6)
 */
typedef struct {
    UdfDescriptorTag descTag;       /* TagId = 6 */
    uint32_t         vdsNum;
    uint8_t          descriptorCharSet[64];
    char             logicalVolumeIdent[128];
    uint32_t         logicalBlockSize; /* 2048 */
    UdfEntityId      domainIdent;
    uint8_t          logicalVolumeContentsUse[16]; /* Holds LongAd to FSD */
    uint32_t         mapTableLength;
    uint32_t         numPartitionMaps;
    UdfEntityId      implementationIdent;
    uint8_t          implementationUse[128];
    UdfExtentAd      integritySequenceExtent;
    uint8_t          partitionMaps[160];
} UdfLogicalVolDesc;

/**
 * @brief File Set Descriptor (FSD) (ECMA-167 4/14.1)
 */
typedef struct {
    UdfDescriptorTag descTag;       /* TagId = 256 (0x0100) */
    UdfTimestamp     recordingDateAndTime;
    uint16_t         interchangeLevel;
    uint16_t         maxInterchangeLevel;
    uint32_t         charSetList;
    uint32_t         maxCharSetList;
    uint32_t         fileSetNum;
    uint32_t         fileSetDescNum;
    uint8_t          logicalVolumeIdentCharSet[64];
    char             logicalVolumeIdent[128];
    uint8_t          fileSetCharSet[64];
    char             fileSetIdent[32];
    char             copyrightFileIdent[32];
    char             abstractFileIdent[32];
    UdfLongAd        rootDirectoryIcb; /* Root directory File Entry */
    UdfEntityId      domainIdent;
    UdfLongAd        nextExtent;
    UdfLongAd        systemStreamDirIcb;
    uint8_t          reserved[32];
} UdfFileSetDesc;

/**
 * @brief File Entry (FE) (ECMA-167 4/14.9)
 */
typedef struct {
    UdfDescriptorTag descTag;       /* TagId = 261 (0x0105) */
    /* ICB Tag */
    uint32_t         priorRecNum;
    uint16_t         strategyType;  /* 4 */
    uint16_t         strategyParams;
    uint16_t         maxNumEntries; /* 1 */
    uint8_t          reserved1;
    uint8_t          fileType;      /* 4 = Directory, 5 = File */
    uint8_t          parentIcbLocation[6];
    uint16_t         icbFlags;      /* 0x0000 = ShortAD */
    /* File Entry Body */
    uint32_t         uid;
    uint32_t         gid;
    uint32_t         permissions;   /* 0755 for dirs, 0644 for files */
    uint16_t         fileLinkCount; /* 1 */
    uint8_t          recordFormat;
    uint8_t          recordDisplayAttr;
    uint32_t         recordLength;
    uint64_t         infoLength;    /* Actual file size */
    uint64_t         logicalBlocksRecorded;
    UdfTimestamp     accessDateAndTime;
    UdfTimestamp     modDateAndTime;
    UdfTimestamp     attrDateAndTime;
    uint32_t         checkpoint;
    UdfLongAd        extendedAttrIcb;
    UdfEntityId      implementationIdent;
    uint64_t         uniqueId;
    uint32_t         lengthOfExtendedAttrs;
    uint32_t         lengthOfAllocationDescriptors;
    /* Followed by: Extended Attributes, then Allocation Descriptors (ShortADs) */
} UdfFileEntry;

/**
 * @brief File Identifier Descriptor (FID) (ECMA-167 4/14.4)
 */
typedef struct {
    UdfDescriptorTag descTag;       /* TagId = 257 (0x0101) */
    uint16_t         fileVersionNum;/* 1 */
    uint8_t          fileCharacteristics; /* 0x02=Directory, 0x08=Parent */
    uint8_t          lengthOfFileIdent;   /* Length of filename in bytes */
    UdfLongAd        icb;           /* Points to child's File Entry */
    uint16_t         lengthOfImplementationUse;
    /* Followed by: Implementation Use, then File Identifier string, then padding */
} UdfFileIdDesc;

#pragma pack(pop)

/**
 * @brief Compute 16-bit CRC-CCITT (polynomial 0x1021) for UDF Descriptors
 */
uint16_t udf_crc16(const uint8_t* data, size_t length);

/**
 * @brief Calculate and finalize the 16-byte UdfDescriptorTag
 *
 * Computes tagChecksum (sum mod 256) and descCrc (CRC16 over data past tag).
 *
 * @param tag Pointer to tag structure
 * @param tagId UDF_TAG_* identifier
 * @param lba Sector LBA location of descriptor
 * @param fullDescSize Total size of descriptor in bytes
 */
void udf_finalize_tag(UdfDescriptorTag* tag, uint16_t tagId, uint32_t lba, uint16_t fullDescSize);

/**
 * @brief Convert Win32 FILETIME to UDF Timestamp
 */
void udf_filetime_to_timestamp(const FILETIME* ft, UdfTimestamp* outTs);

/**
 * @brief Encode Unicode UTF-16 string into OSTA compressed dstring format
 *
 * Automatically uses 8-bit compression (compression ID = 0x08) if all characters
 * fit in Latin-1 (<= 0xFF), or 16-bit (compression ID = 0x10) if Asian/Unicode characters present.
 *
 * @param src Wide string
 * @param dst Output buffer
 * @param maxDstLen Max bytes of output buffer
 * @return Number of bytes written
 */
uint8_t udf_encode_dstring(const wchar_t* src, uint8_t* dst, size_t maxDstLen);

/**
 * @brief Initialize Volume Recognition Sequence (VRS) sectors (LBA 16..18)
 *
 * @param outBuffer Buffer of at least 3 * 2048 bytes
 * @param isUdf250 True for NSR03 (UDF 2.50+), false for NSR02 (UDF 1.02)
 */
void udf_build_vrs(uint8_t* outBuffer, bool isUdf250);

/**
 * @brief Build Anchor Volume Descriptor Pointer (AVDP) sector
 */
void udf_build_avdp(uint8_t* outSector, uint32_t avdpLba, uint32_t mvdsLba, uint32_t mvdsSectors);

/**
 * @brief Build Main Volume Descriptor Sequence (PVD, PD, LVD, TD)
 *
 * @param outBuffer Buffer of at least 4 * 2048 bytes
 * @param startLba Starting LBA of MVDS
 * @param volumeLabel Label string
 * @param partitionStartLba Start LBA of partition
 * @param partitionSectorCount Total sectors of partition
 * @param fsdLba LBA of File Set Descriptor
 * @param isUdf250 True for UDF 2.50
 */
void udf_build_mvds(uint8_t* outBuffer,
                    uint32_t startLba,
                    const wchar_t* volumeLabel,
                    uint32_t partitionStartLba,
                    uint32_t partitionSectorCount,
                    uint32_t fsdLba,
                    bool isUdf250);

/**
 * @brief Build File Set Descriptor (FSD) sector
 */
void udf_build_fsd(uint8_t* outSector, uint32_t fsdLba, uint32_t rootDirFeLba);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_UDF_H */
