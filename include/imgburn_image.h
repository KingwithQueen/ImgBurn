/**
 * @file imgburn_image.h
 * @brief Optical Disc Image Parsers & Generators (CUE, BIN, MDS, ISO, DVD-Video IFO)
 *
 * Implements:
 * - Image format auto-detection (.iso, .bin, .cue, .mds, .img, .nrg)
 * - CUE Sheet Parser & Validator (Multi-track, CD-DA, Mixed Mode, Pregap/Postgap, CD-Text)
 * - Media Descriptor Sheet (.mds) Parser & Generator (Dual-layer Layer Break L0 size)
 * - DVD-Video .IFO Parser & Layer Break Candidate Analyzer (Cell boundaries, seamless flags)
 *
 * Pure C99/C11, Win32 API. No external dependencies.
 */

#ifndef IMGBURN_IMAGE_H
#define IMGBURN_IMAGE_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CUE_TRACKS       99
#define MAX_CUE_INDICES      10
#define MAX_MDS_TRACKS       100
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
    TRACK_MODE_MODE2_2352    /* Mode 2 CD-ROM XA (2352 bytes) */
} TrackDataMode;

/* -------------------------------------------------------------------------
 * CUE Sheet Structures
 * ------------------------------------------------------------------------- */
typedef struct {
    int indexNumber;         /* 0 = Pre-gap, 1 = Track start, 2..9 = Sub-indices */
    uint32_t lba;            /* Absolute LBA on disc */
    char msfStr[10];         /* "MM:SS:FF" string */
} CueIndex;

typedef struct {
    int trackNumber;         /* 1..99 */
    TrackDataMode mode;
    wchar_t sourceFilePath[MAX_PATH]; /* Associated audio/data file */
    char title[128];         /* CD-Text Title */
    char performer[128];     /* CD-Text Performer */
    char isrc[16];           /* ISRC code */

    uint32_t pregapSectors;  /* PREGAP command duration */
    uint32_t postgapSectors; /* POSTGAP command duration */

    CueIndex indices[MAX_CUE_INDICES];
    int indexCount;

    uint32_t startLba;       /* Absolute start LBA */
    uint32_t sectorCount;    /* Total sectors in track */
} CueTrack;

typedef struct {
    wchar_t cueFilePath[MAX_PATH];
    char discTitle[128];     /* CD-Text Album Title */
    char discPerformer[128]; /* CD-Text Album Artist */
    char catalogNumber[16];  /* UPC/EAN Barcode */

    CueTrack tracks[MAX_CUE_TRACKS];
    int trackCount;
    uint32_t totalSectors;
} CueSheet;

/* -------------------------------------------------------------------------
 * MDS (Media Descriptor Sheet) Structures
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct {
    char signature[16];      /* "MEDIA DESCRIPTOR" */
    uint8_t version[2];      /* e.g. 0x01, 0x03 */
    uint16_t mediumType;     /* 0=CD, 1=DVD, 2=DVD-DL */
    uint16_t sessionCount;
    uint16_t dummy1[2];
    uint16_t bcaLength;
    uint32_t dummy2[2];
    uint32_t bcaOffset;
    uint32_t dummy3[6];
    uint32_t discStructuresOffset;
    uint32_t dummy4[3];
    uint32_t sessionsOffset;
    uint32_t tracksOffset;
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
} MdsSessionBlock;

typedef struct {
    uint8_t trackMode;       /* 0xA9 = Mode1, 0xAA = Mode2, 0xAB = Audio */
    uint8_t subChannel;
    uint8_t trackNumber;
    uint8_t dummy1;
    uint32_t sectorSize;     /* 2048, 2352, etc. */
    uint32_t dummy2;
    uint32_t startLba;
    uint64_t startByteOffset;
    uint32_t dummy3[6];
} MdsTrackBlock;
#pragma pack(pop)

typedef struct {
    wchar_t mdsFilePath[MAX_PATH];
    wchar_t dataFilePath[MAX_PATH]; /* Path to .mdf or .iso */
    uint16_t mediumType;
    uint32_t totalSectors;
    uint32_t layerBreakLba;  /* L0 sector count for dual-layer DVD, 0 if single layer */
    int trackCount;
} MdsSheet;

/* -------------------------------------------------------------------------
 * DVD-Video Layer Break Candidate Structure
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t lba;            /* Sector LBA on disc */
    uint16_t vobId;          /* DVD VOB ID */
    uint16_t cellId;         /* DVD Cell ID */
    bool isSeamless;         /* Seamless playback bit (0 = non-seamless, ideal for LB) */
    bool isChapterStart;     /* Cell coincides with Chapter / PTT start */
    int suitabilityRating;   /* 0..100 (ImgBurn stars rating: 100=optimal chapter non-seamless) */
    char description[64];    /* e.g. "VTS_01, Cell 14 (Chapter 8)" */
} DvdLayerBreakCandidate;

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
 * @brief Scan a DVD-Video folder (VIDEO_TS) and find all valid Layer Break candidate LBAs
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
