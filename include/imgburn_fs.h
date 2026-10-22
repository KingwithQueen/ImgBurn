/**
 * @file imgburn_fs.h
 * @brief C17光盘文件树与镜像生成接口。
 *
 * 提供ISO9660桥、Joliet、UDF 1.02/2.50及逐字节确认的文件去重。
 * 支持DVD-Video与Blu-ray文件布局；引导镜像尚未实现。
 */

#ifndef IMGBURN_FS_H
#define IMGBURN_FS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#define FS_SECTOR_SIZE       2048
#define FS_MAX_PATH_LEN      MAX_PATH

/* Filesystem Types */
typedef enum {
    FS_TYPE_ISO9660          = (1 << 0),
    FS_TYPE_JOLIET           = (1 << 1),
    FS_TYPE_UDF              = (1 << 2),
    FS_TYPE_ISO_JOLIET       = (FS_TYPE_ISO9660 | FS_TYPE_JOLIET),
    FS_TYPE_ISO_UDF          = (FS_TYPE_ISO9660 | FS_TYPE_UDF),
    FS_TYPE_ALL              = (FS_TYPE_ISO9660 | FS_TYPE_JOLIET | FS_TYPE_UDF)
} FsTypeFlags;

/* UDF Revisions */
typedef enum {
    UDF_REV_102              = 0x0102, /* DVD-Video standard */
    UDF_REV_150              = 0x0150,
    UDF_REV_201              = 0x0201,
    UDF_REV_250              = 0x0250, /* Blu-ray standard */
    UDF_REV_260              = 0x0260
} UdfRevision;

/* El Torito Boot Emulation Type */
typedef enum {
    BOOT_EMU_NONE            = 0, /* No Emulation (Hard Disk/Custom, e.g. isolinux, WinPE) */
    BOOT_EMU_FLOPPY_120      = 1, /* 1.2 MB Floppy */
    BOOT_EMU_FLOPPY_144      = 2, /* 1.44 MB Floppy */
    BOOT_EMU_FLOPPY_288      = 3, /* 2.88 MB Floppy */
    BOOT_EMU_HARDDISK        = 4  /* Hard Disk */
} BootEmulationType;

/* El Torito Boot Options */
typedef struct {
    bool isBootable;
    BootEmulationType emuType;
    wchar_t bootImagePath[MAX_PATH];
    char developerId[32];           /* Developer ID (e.g. "LIGHTNING UK!") */
    uint16_t loadSegment;           /* e.g. 0x07C0 */
    uint16_t sectorCount;           /* Number of sectors to load */
    bool patchBootInfoTable;        /* Patch isolinux.bin boot info table */
} BootOptions;

/* Forward declarations */
struct FsDirectoryNode;
typedef struct { uint64_t offset; uint32_t value; } FsBytePatch;

/**
 * @brief Represents a file inside the disc compilation
 */
typedef struct FsFileNode {
    char nameA[128];                /* Standard 8.3 / ISO9660 name */
    wchar_t nameW[128];             /* Full Joliet / UDF Unicode name */
    wchar_t jolietNameW[65];        /* Joliet标准名称；原始Unicode名称供UDF保留。 */
    uint32_t udfIcb;
    uint32_t udfAllocationLba;
    uint32_t udfAllocationBlocks;
    uint64_t udfUniqueId;
    wchar_t sourceDiskPath[MAX_PATH];/* Actual source file on host disk */
    uint64_t fileSize;
    FILETIME modTime;

    uint32_t allocatedLba;          /* Starting LBA on disc */
    uint32_t sectorCount;           /* Ceil(fileSize / 2048) */

    uint32_t crc32;                 /* For deduplication check */
    bool isDuplicate;
    struct FsFileNode* duplicateOf; /* Points to first unique file */

    struct FsFileNode* next;
    struct FsDirectoryNode* parent;
    bool excluded;
    FsBytePatch* patches;
    size_t patchCount;
} FsFileNode;

/**
 * @brief Represents a directory inside the disc compilation
 */
typedef struct FsDirectoryNode {
    char nameA[128];
    wchar_t nameW[128];
    wchar_t jolietNameW[65];
    struct FsDirectoryNode* parent;

    struct FsFileNode* firstFile;
    struct FsDirectoryNode* firstChildDir;
    struct FsDirectoryNode* nextSibling;

    uint32_t allocatedLbaIso;
    uint32_t allocatedLbaJoliet;
    uint32_t sectorCountIso;
    uint32_t sectorCountJoliet;
    uint16_t dirIndex;              /* 1-based index in Path Table */
    uint16_t dirIndexJoliet;
    uint32_t udfIcb;
    uint32_t udfData;
    uint32_t udfBytes;
    uint64_t udfUniqueId;
    bool excluded;
} FsDirectoryNode;

typedef enum { DVD_DOMAIN_TITLE, DVD_DOMAIN_VTS_MENU, DVD_DOMAIN_VMG_MENU } DvdNavigationDomain;

/**
 * @brief Image Build Configuration & Stats
 */
typedef struct {
    char volumeLabelIso[33];        /* Max 32 chars */
    wchar_t volumeLabelJoliet[17];  /* Max 16 chars */
    wchar_t volumeLabelUdf[127];    /* Max 126 chars */

    FsTypeFlags fsTypes;
    UdfRevision udfRev;

    bool optimizeDuplicateFiles;    /* File deduplication */
    bool dvdVideoCompliance;        /* Auto 32KB padding between IFO and BUP */
    bool blurayCompliance;          /* Strict Blu-ray UDF 2.50 check */

    /* 菜单还需保存语言单元，VMGM的标题集编号为0；PGC非0表示已选择。 */
    uint16_t dvdBreakTitleSet, dvdBreakPgc, dvdBreakCell;
    uint16_t dvdBreakLanguageUnit;
    DvdNavigationDomain dvdBreakDomain;
    bool dvdBreakSeamless, dvdBreakInterleaved;
    uint32_t dvdBreakPadding, dvdLayerBreakLba;
    uint32_t dvdMinimumL0, dvdMaximumL0, dvdMediaCapacity;

    BootOptions boot;

    /* Compilation statistics */
    uint32_t totalFiles;
    uint32_t totalDirectories;
    uint64_t totalSourceBytes;
    uint64_t deduplicatedBytesSaved;
    uint32_t totalImageSectors;     /* Final LBA size of the generated image */
} ImageBuildConfig;

/**
 * @brief Image Builder session context
 */
typedef struct {
    ImageBuildConfig config;
    FsDirectoryNode* rootDir;

    uint32_t pvdLba;
    uint32_t svdLba;
    uint32_t pathTableLbaL;
    uint32_t pathTableLbaM;
    uint32_t pathTableSizeIso;

    uint32_t pathTableLbaLJoliet;
    uint32_t pathTableLbaMJoliet;
    uint32_t pathTableSizeJoliet;

    uint32_t bootCatalogLba;
    uint32_t bootImageLba;

    uint32_t dataStartLba;
    void* layout;                   /* 内部布局计划，由销毁函数回收。 */
} ImageBuilder;

/**
 * @brief Initialize image builder with default settings
 */
bool image_builder_init(ImageBuilder* b, const char* volumeLabel);

/**
 * @brief Free all allocated memory in image builder
 */
void image_builder_destroy(ImageBuilder* b);

/**
 * @brief Add a local file into the disc hierarchy
 */
bool image_builder_add_file(ImageBuilder* b,
                            const wchar_t* sourceFilePath,
                            const wchar_t* discRelativePath);

/**
 * @brief Add an entire host folder recursively
 */
bool image_builder_add_folder(ImageBuilder* b,
                              const wchar_t* hostFolderPath,
                              const wchar_t* discDestFolder);

/**
 * @brief Scan, calculate layout, deduplicate, and prepare sector offsets
 * @return Total sectors of the resulting image
 */
uint32_t image_builder_calculate_layout(ImageBuilder* b);

/* 检查源文件快照及目标别名，供ISO和伴随描述文件共用。 */
bool image_builder_check_destination(const ImageBuilder* b, const wchar_t* path);

/**
 * @brief Write the complete generated image to an output ISO file
 *
 * Supports progress callback for streaming status reporting.
 *
 * @param b Prepared ImageBuilder
 * @param outputIsoPath Path to write the output .iso file
 * @param progressCallback Optional progress function (percent 0..100)
 * @return True on success, false on failure
 */
typedef void (*ImageProgressFn)(int percent, uint32_t currentLba, uint32_t totalLba, void* userData);

bool image_builder_create_iso(ImageBuilder* b,
                              const wchar_t* outputIsoPath,
                              ImageProgressFn progressCallback,
                              void* userData);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_FS_H */
