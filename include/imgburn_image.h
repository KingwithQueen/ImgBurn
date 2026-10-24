/**
 * @file imgburn_image.h
 * @brief C17镜像识别、CUE与MDS元数据接口。
 *
 * CUE支持多编码、混合轨道、多会话及音频解码后的精确计数。
 * MDS支持1.x多会话CD、DVD与分卷数据引用，分层使用真实PFI字段。
 * DVD-Video导航通过PGC、PTT、单元地址及NAV包交叉检查后关联实际布局。
 */

#ifndef IMGBURN_IMAGE_H
#define IMGBURN_IMAGE_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include "imgburn_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CUE_TRACKS       99
#define MAX_CUE_INDICES      100
#define MAX_MDS_TRACKS       100
#define MAX_MDS_FILES        512
#define MAX_MDS_SESSIONS     99
#define MAX_DVD_LAYER_BREAKS 64

/* -------------------------------------------------------------------------
 * Image Type Enumeration
 * ------------------------------------------------------------------------- */
typedef enum {
    IMAGE_TYPE_UNKNOWN = 0,
    IMAGE_TYPE_ISO,          /* Standard ISO9660/UDF (2048 bytes/sector) */
    IMAGE_TYPE_BIN,          /* Raw binary image (2048 or 2352 bytes/sector) */
    IMAGE_TYPE_CUE,          /* CUE Sheet descriptor */
    IMAGE_TYPE_MDS,          /* Media Descriptor Sheet (Alcohol / ImgBurn) */
    IMAGE_TYPE_DVD_FOLDER    /* Folder containing VIDEO_TS / IFO files */
} DiscImageType;

/* -------------------------------------------------------------------------
 * Track Data Mode
 * ------------------------------------------------------------------------- */
typedef enum {
    TRACK_MODE_AUDIO = 0,    /* CD-DA 2352 bytes/sector audio */
    TRACK_MODE_MODE1_2048,   /* Standard CD-ROM / DVD data (2048 bytes) */
    TRACK_MODE_MODE1_2352,   /* Raw Mode 1 with Sync/Header/ECC (2352 bytes) */
    TRACK_MODE_MODE2_2336,   /* Mode 2 Formless (2336 bytes) */
    TRACK_MODE_MODE2_2352,
    TRACK_MODE_MODE2_2048,
    TRACK_MODE_MODE2_2324,
    TRACK_MODE_CDI_2336,
    TRACK_MODE_CDI_2352,
    TRACK_MODE_CDG_2448
} TrackDataMode;

typedef enum { CUE_FILE_BINARY, CUE_FILE_MOTOROLA, CUE_FILE_WAVE, CUE_FILE_AIFF, CUE_FILE_MP3, CUE_FILE_AUDIO } CueFileType;
enum { CUE_FLAG_DCP = 1, CUE_FLAG_4CH = 2, CUE_FLAG_PRE = 4, CUE_FLAG_SCMS = 8 };

/* -------------------------------------------------------------------------
 * CUE Sheet Structures
 * ------------------------------------------------------------------------- */
typedef struct {
    int indexNumber;         /* 0为预间隙，1为轨道起点，2至99为子索引。 */
    uint32_t lba;            /* CUE的INDEX地址相对于当前FILE，不是光盘绝对地址。 */
    char msfStr[10];         /* "MM:SS:FF" string */
} CueIndex;

typedef struct {
    int trackNumber;         /* 1..99 */
    TrackDataMode mode;
    wchar_t sourceFilePath[MAX_PATH]; /* Associated audio/data file */
    bool sourceIsWave;       /* FILE声明的类型，不能由扩展名猜测。 */
    CueFileType sourceType;
    uint32_t sectorSize, flags;
    uint64_t sourceByteOffset, sourceByteLength; /* INDEX01相对源数据或解码PCM的字节位置及有效长度。 */
    uint32_t decodedSourceSectors; /* REM解码长度缓存，0表示需实际解码测量。 */
    unsigned sessionNumber;
    unsigned sourceFileGroup; /* 保留同路径重复FILE声明与跨会话共用FILE的区别。 */
    char songwriter[128];
    char composer[128], arranger[128], message[128]; /* CD-TEXT作曲、编曲及文字信息。 */
    char title[128];         /* CD-Text Title */
    char performer[128];     /* CD-Text Performer */
    char isrc[16];           /* ISRC code */

    uint32_t pregapSectors;  /* PREGAP command duration */
    uint32_t postgapSectors; /* POSTGAP command duration */

    CueIndex indices[MAX_CUE_INDICES];
    int indexCount;

    uint32_t startLba;       /* INDEX01在光盘中的绝对LBA，已扣除未引用的FILE前缀。 */
    uint32_t sectorCount;    /* 从INDEX01起的扇区数，不含INDEX00、PREGAP和POSTGAP。 */
} CueTrack;

typedef struct {
    wchar_t cueFilePath[MAX_PATH];
    char discTitle[128];     /* CD-Text Album Title */
    char discPerformer[128]; /* CD-Text Album Artist */
    char catalogNumber[16];  /* Q子通道MCN，与CD-TEXT中的UPC独立。 */

    CueTrack tracks[MAX_CUE_TRACKS];
    int trackCount;
    uint32_t totalSectors;
    char songwriter[128];
    char composer[128], arranger[128], message[128]; /* 光盘级CD-TEXT文字。 */
    char discId[128], cdTextUpc[16]; /* DISCID与UPC分别对应86h、8Eh包。 */
    wchar_t cdTextFile[MAX_PATH];
    unsigned sessionCount;
    uint32_t sessionLeadIn[99], sessionLeadOut[99], sessionEndLba[99];
} CueSheet;

/* -------------------------------------------------------------------------
 * MDS (Media Descriptor Sheet) Structures
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct {
    char signature[16];      /* "MEDIA DESCRIPTOR" */
    uint8_t version[2];      /* e.g. 0x01, 0x03 */
    uint16_t mediumType;     /* 0x00/01/02为CD；0x10/12为DVD。 */
    uint16_t sessionCount;
    uint16_t dummy1[2];
    uint16_t bcaLength;
    uint32_t dummy2[2];
    uint32_t bcaOffset;
    uint32_t dummy3[6];
    uint32_t discStructuresOffset;
    uint32_t dummy4[3];
    uint32_t sessionsOffset;
    uint32_t dpmOffset;
} MdsHeader;

typedef struct {
    int32_t sessionStartLba;
    int32_t sessionEndLba;
    uint16_t sessionNumber;
    uint8_t totalTracks;
    uint8_t leadInTracks;
    uint16_t firstTrack;
    uint16_t lastTrack;
    uint32_t dummy;
    uint32_t tracksOffset;
} MdsSessionBlock;

typedef struct {
    uint8_t trackMode;       /* 0xA9为音频；0xAA为Mode 1。 */
    uint8_t subChannel;
    uint8_t adrControl;
    uint8_t trackNumber;
    uint8_t point;
    uint8_t time[7];
    uint32_t extraOffset;
    uint16_t sectorSize;
    uint8_t reserved[18];
    uint32_t startLba;
    uint64_t startByteOffset;
    uint32_t fileCount;
    uint32_t footerOffset;
    uint8_t reserved2[24];
} MdsTrackBlock;
#pragma pack(pop)

typedef struct { wchar_t path[MAX_PATH]; uint64_t bytes; } MdsDataFile;
typedef struct {
    uint16_t sessionNumber, trackNumber, sectorSize;
    uint8_t mode, subChannel, control;
    int32_t startLba;
    uint32_t sectorCount, pregapSectors;
    uint64_t startByteOffset;
    uint16_t firstFile, fileCount;
} MdsTrack;

typedef struct {
    wchar_t mdsFilePath[MAX_PATH];
    wchar_t dataFilePath[MAX_PATH]; /* Path to .mdf or .iso */
    uint16_t mediumType;
    uint32_t totalSectors;
    uint32_t layerBreakLba;  /* L0 sector count for dual-layer DVD, 0 if single layer */
    unsigned layerCount;
    bool oppositeTrackPath; /* 两层PTP允许L1大于L0；不可据此直接按OTP刻录。 */
    int trackCount;
    unsigned sessionCount, dataFileCount;
    MdsSessionBlock sessions[MAX_MDS_SESSIONS];
    MdsTrack tracks[MAX_MDS_TRACKS];
    MdsDataFile files[MAX_MDS_FILES];
    uint32_t bcaOffset, bcaLength, dpmOffset;
} MdsSheet;

/* 分卷DVD的所有数据文件均按逻辑顺序列出；原单文件入口由此实现。 */
bool imgburn_mds_create_files(const wchar_t* mdsPath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba);
bool imgburn_mds_create_files_ex(const wchar_t* mdsPath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath);
/* Read 保存已由源盘 PFI 核对的布局，保留原层位置，不套用新刻录镜像的对齐与介质容量限制。 */
bool imgburn_mds_create_dvd_read(const wchar_t* mdsPath, const wchar_t* dataFile, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath);

/* -------------------------------------------------------------------------
 * DVD-Video Layer Break Candidate Structure
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t lba;            /* Sector LBA on disc */
    uint16_t vobId;          /* DVD VOB ID */
    uint16_t cellId;         /* DVD Cell ID */
    bool isSeamless;         /* Seamless playback bit (0 = non-seamless, ideal for LB) */
    bool isChapterStart;     /* Cell coincides with Chapter / PTT start */
    int suitabilityRating;   /* 暂未复刻原版星级，0表示未评级。 */
    char description[64];    /* e.g. "VTS_01, Cell 14 (Chapter 8)" */
    uint16_t titleSet, pgc, cell, program, title, chapter;
    uint32_t vobSector, paddingSectors, imageSectors;
    bool seamlessAllowed;
    DvdNavigationDomain domain;
    uint16_t languageUnit, languageCode;
    uint8_t menuType;
    bool isInterleaved, isAngleBlock;
} DvdLayerBreakCandidate;

typedef struct {
    uint16_t titleSet, pgc, cell, program, title, chapter, vobId, cellId;
    uint32_t firstSector, lastSector, lastVobuSector, lba, category, ifoOffset;
    bool chapterStart, previousCellCommand;
    DvdNavigationDomain domain;
    uint16_t languageUnit, languageCode;
    uint8_t menuType;
    uint32_t firstIlvuEndSector;
    uint16_t angleBlockFirstCell;
    uint32_t playbackMilliseconds;
    uint32_t pgcOffset;
} DvdNavigationCell;

typedef struct { DvdNavigationCell* cells; size_t cellCount; } DvdNavigation;
typedef struct { uint32_t minimumL0, maximumL0, mediaCapacity; } DvdMediaProfile;
enum { DVD_LAYER_SHOW_INTERLEAVED = 1 };

/* 输入必须已完成DVD-Video布局；返回的LBA为当前镜像物理地址。 */
bool imgburn_dvd_read_navigation(const ImageBuilder* builder, DvdNavigation* navigation);
void imgburn_dvd_free_navigation(DvdNavigation* navigation);
bool imgburn_dvd_layer_break_candidates(const ImageBuilder* builder, const DvdMediaProfile* profile, DvdLayerBreakCandidate* candidates, size_t capacity, size_t* count);
bool imgburn_dvd_layer_break_candidates_ex(const ImageBuilder* builder, const DvdMediaProfile* profile, unsigned options, DvdLayerBreakCandidate* candidates, size_t capacity, size_t* count);
/* 选择后需重新计算布局；无效、过期的选择会在重新计算时失败。 */
bool imgburn_dvd_select_layer_break(ImageBuilder* builder, const DvdMediaProfile* profile, const DvdLayerBreakCandidate* candidate, bool seamless);

/* 预览只播放所选单元及其前一播放单元，不执行DVD虚拟机指令。 */
typedef struct {
    unsigned streamId, format, channels, sampleRate;
    uint16_t languageCode;
} DvdPreviewAudio;

typedef struct {
    DvdNavigationCell cells[2];
    unsigned cellCount, selectedIndex, width, height, aspectNumerator, aspectDenominator;
    unsigned frameRateNumerator, frameRateDenominator;
    DvdPreviewAudio audio[8];
    unsigned audioCount, videoCoding;
} DvdPreviewPlan;
typedef bool (*DvdVideoWriteCallback)(const void* data, size_t bytes, void* context);
bool imgburn_dvd_preview_plan(const ImageBuilder* builder, DvdNavigationDomain domain, unsigned titleSet, unsigned languageUnit, unsigned pgc, unsigned cell, DvdPreviewPlan* plan);
bool imgburn_dvd_read_cell(const ImageBuilder* builder, const DvdNavigationCell* cell, DvdVideoWriteCallback write, void* context, uint64_t* bytesWritten);

/* -------------------------------------------------------------------------
 * Image Engine APIs
 * ------------------------------------------------------------------------- */

/**
 * @brief Auto-detect disc image format by extension and header sniffing
 */
DiscImageType imgburn_image_detect_type(const wchar_t *filePath);

/**
 * @brief Parse a CUE sheet file and validate track references
 * @param cueFilePath Path to .cue file
 * @param outCue Pointer to CueSheet structure to fill
 * @return true on success, false on syntax or file not found error
 */
bool imgburn_cue_parse(const wchar_t *cueFilePath, CueSheet *outCue);

/* 无解码缓存的音源由调用者提供真实PCM字节数，允许写入与校验复用可取消的已锁定来源。 */
typedef bool (*CueSourceResolver)(const wchar_t* path, CueFileType type, uint64_t* pcmBytes, void* context);
bool imgburn_cue_parse_ex(const wchar_t* cueFilePath, CueSheet* outCue, CueSourceResolver resolver, void* context);

/**
 * @brief Parse an Alcohol / ImgBurn .MDS Media Descriptor file
 * @param mdsFilePath Path to .mds file
 * @param outMds Pointer to MdsSheet structure to fill
 * @return true on success, false on format error
 */
bool imgburn_mds_parse(const wchar_t *mdsFilePath, MdsSheet *outMds);

/**
 * @brief Create a valid .MDS file for a single-layer or dual-layer ISO/BIN image
 * @param mdsFilePath Output path of .mds file to create
 * @param dataFileName Relative or absolute path to .iso / .mdf file
 * @param totalSectors Total sector count of image
 * @param layerBreakLba L0 sectors for dual-layer DVD (0 for single layer / CD / BD)
 * @return true on success, false on write error
 */
bool imgburn_mds_create(const wchar_t *mdsFilePath,
                        const wchar_t *dataFileName,
                        uint32_t totalSectors,
                        uint32_t layerBreakLba);

/**
 * @brief 旧目录接口不能确定调用者的镜像布局，继续返回ERROR_NOT_SUPPORTED。
 * 请使用接收ImageBuilder的imgburn_dvd_layer_break_candidates接口。
 * @param videoTsPath Path to folder containing VIDEO_TS.IFO and VTS_*.IFO
 * @param totalImageSectors Total image sectors of DVD
 * @param outCandidates Array to receive candidate entries
 * @param maxCandidates Maximum capacity of outCandidates array
 * @param outCount Receives actual number of candidates found
 * @return true on success, false if no IFO found or invalid DVD layout
 */
bool imgburn_dvd_find_layer_breaks(const wchar_t *videoTsPath,
                                   uint32_t totalImageSectors,
                                   DvdLayerBreakCandidate *outCandidates,
                                   int maxCandidates,
                                   int *outCount);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_IMAGE_H */
