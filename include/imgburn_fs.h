/**
 * @file imgburn_fs.h
 * @brief ImgBurn Core C - Optical Disc Filesystem Generator
 *
 * Implements ISO9660 (Level 1/2/X), Joliet (UCS-2), UDF (1.02/2.50),
 * El Torito bootable disc support, isolinux.bin boot table patching,
 * DVD-Video 32KB IFO/BUP ECC padding, and file deduplication.
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

/**
 * @brief Represents a file inside the disc compilation
 */
typedef struct FsFileNode {
    char nameA[128];                /* Standard 8.3 / ISO9660 name */
    wchar_t nameW[128];             /* Full Joliet / UDF Unicode name */
    wchar_t sourceDiskPath[MAX_PATH];/* Actual source file on host disk */
    uint64_t fileSize;
    FILETIME modTime;

    uint32_t allocatedLba;          /* Starting LBA on disc */
    uint32_t sectorCount;           /* Ceil(fileSize / 2048) */

    uint32_t crc32;                 /* For deduplication check */
    bool isDuplicate;
    struct FsFileNode* duplicateOf; /* Points to first unique file */

    struct FsFileNode* next;
} FsFileNode;

/**
 * @brief Represents a directory inside the disc compilation
 */
typedef struct FsDirectoryNode {
    char nameA[128];
    wchar_t nameW[128];
    struct FsDirectoryNode* parent;

    struct FsFileNode* firstFile;
    struct FsDirectoryNode* firstChildDir;
    struct FsDirectoryNode* nextSibling;

    uint32_t allocatedLbaIso;
    uint32_t allocatedLbaJoliet;
    uint32_t sectorCountIso;
    uint32_t sectorCountJoliet;
    uint16_t dirIndex;              /* 1-based index in Path Table */
} FsDirectoryNode;

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
