/**
 * @file imgburn_audio.h
 * @brief ImgBurn Core C - Audio CD Engine & Decoder Pipeline
 *
 * Provides Audio CD (Red Book 44.1kHz, 16-bit stereo, 2352 bytes/sector)
 * decoding using pure C native WAV parsing and FFmpeg pipe decoding for
 * all compressed formats (FLAC, APE, MP3, OGG, AAC, Opus, WMA).
 * Also includes CUE Sheet parsing and MSF time calculation.
 */

#ifndef IMGBURN_AUDIO_H
#define IMGBURN_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include "imgburn_image.h"

#define CDDA_SAMPLE_RATE     44100
#define CDDA_CHANNELS        2
#define CDDA_BITS_PER_SAMPLE 16
#define CDDA_BYTES_PER_SAMPLE (CDDA_CHANNELS * (CDDA_BITS_PER_SAMPLE / 8)) /* 4 bytes */
#define CDDA_FRAMES_PER_SEC  75
#define CDDA_SAMPLES_PER_FRAME (CDDA_SAMPLE_RATE / CDDA_FRAMES_PER_SEC)     /* 588 samples */
#define CDDA_BYTES_PER_FRAME  (CDDA_SAMPLES_PER_FRAME * CDDA_BYTES_PER_SAMPLE) /* 2352 bytes/sector */

#define MAX_AUDIO_TRACKS     99

/**
 * @brief Minute : Second : Frame time representation (1 second = 75 frames)
 */
typedef struct {
    uint8_t minute;
    uint8_t second;
    uint8_t frame;
} MsfTime;

/**
 * @brief Audio Track metadata
 */
typedef struct {
    int trackNumber;                /* 1 to 99 */
    wchar_t filePath[MAX_PATH];     /* Path to source audio file */
    char title[128];                /* CD-Text Title */
    char performer[128];            /* CD-Text Performer / Artist */
    char isrc[16];                  /* 曲目标识码。 */
    uint32_t startLba;              /* Starting LBA on disc */
    uint32_t sectorCount;           /* Total 2352-byte sectors */
    uint32_t pregapSectors;         /* Pregap (default 150 = 2 seconds) */
    uint32_t postgapSectors;        /* POSTGAP产生的静音扇区。 */
    bool sourceIsWave;
    CueFileType sourceType;
    uint32_t flags;
    char songwriter[128];
    CueIndex indices[MAX_CUE_INDICES];
    int indexCount;
    unsigned sessionNumber;
    unsigned sourceFileGroup;
    uint32_t decodedSourceSectors;  /* 保留源文件的解码长度缓存，导出时仍属于FILE。 */
    bool hasIndex00;
    MsfTime index00;
    MsfTime startTime;
    MsfTime duration;
} AudioTrack;

/**
 * @brief Audio CD Compilation
 */
typedef struct {
    char cdTitle[128];
    char cdPerformer[128];
    char catalogNumber[16];         /* 光盘目录号。 */
    int trackCount;
    AudioTrack tracks[MAX_AUDIO_TRACKS];
    uint32_t totalSectors;          /* Total sectors including lead-in pregap */
    char songwriter[128];
    wchar_t cdTextFile[MAX_PATH];
    unsigned sessionCount;
    uint32_t sessionLeadIn[99], sessionLeadOut[99], sessionEndLba[99];
} AudioCompilation;

/* 标准CD音频PCM的数据区域；末扇区允许包含不足2352字节的完整样本。 */
typedef struct {
    uint64_t dataOffset;
    uint64_t dataBytes;
    uint32_t sectorCount;
} AudioWaveLayout;

/* 借用同步只读文件句柄解析WAVE，保留文件位置，不取得句柄所有权；调用期间不能并发定位该句柄。 */
bool audio_wave_get_layout(HANDLE file, AudioWaveLayout* layout);

/**
 * @brief Abstract Audio Stream Decoder (WAV or FFmpeg pipe)
 */
typedef struct {
    HANDLE hFile;                   /* For native WAV file */
    HANDLE hProcess;                /* For FFmpeg pipe process */
    HANDLE hPipeRead;               /* Standard output pipe from FFmpeg */
    bool isPipe;
    uint32_t totalSamples;
    uint32_t samplesRead;
    uint64_t dataBytesRemaining;    /* 原生WAV只读取声明的PCM数据，不越过data块。 */
    DWORD lastError;               /* 读取错误与正常流结束分别记录。 */
} AudioDecoder;

/**
 * @brief Convert LBA sector address to MSF time
 */
MsfTime lba_to_msf(uint32_t lba);

/**
 * @brief Convert MSF time to LBA sector address
 */
uint32_t msf_to_lba(MsfTime msf);

/**
 * @brief Format MSF time into standard string "MM:SS:FF"
 */
void format_msf(MsfTime msf, char* outStr, size_t maxLen);

/**
 * @brief Parse MSF time from string "MM:SS:FF"
 */
bool parse_msf(const char* str, MsfTime* outMsf);

/**
 * @brief Open an audio file for streaming decoding into 44.1kHz 16-bit stereo PCM
 *
 * Automatically detects whether file is uncompressed WAV or compressed audio.
 * Uses native pure C parser for standard WAV, and spawns FFmpeg pipe for others.
 *
 * @param decoder Pointer to AudioDecoder
 * @param filePath Path to audio file
 * @param ffmpegPath Optional path to ffmpeg.exe (if NULL, checks standard locations and PATH)
 * @param outTotalSectors Output total 2352-byte sectors
 * @return True on success, false on failure
 */
bool audio_decoder_open(AudioDecoder* decoder,
                        const wchar_t* filePath,
                        const wchar_t* ffmpegPath,
                        uint32_t* outTotalSectors);

/**
 * @brief Read decoded PCM frames into buffer (guaranteed 44.1kHz, 16-bit stereo)
 * @param decoder Open AudioDecoder
 * @param outBuffer Buffer to receive PCM bytes
 * @param bytesToRead Number of bytes to read (should be multiple of 2352)
 * @return Actual bytes read (0 = EOF)
 */
uint32_t audio_decoder_read(AudioDecoder* decoder, void* outBuffer, uint32_t bytesToRead);

/**
 * @brief Close audio decoder and cleanup pipe/process
 */
void audio_decoder_close(AudioDecoder* decoder);
/* 完整解码并检查子进程退出结果，返回真实PCM字节数而不是估算时长。 */
bool audio_measure_decoded(const wchar_t* path, uint64_t* pcmBytes);

/**
 * @brief Parse a standard CUE Sheet file into AudioCompilation
 */
bool cue_sheet_parse(const wchar_t* cuePath, AudioCompilation* outComp);

/**
 * @brief Generate a standard CUE Sheet text file from AudioCompilation
 */
bool cue_sheet_generate(const AudioCompilation* comp, const wchar_t* outCuePath);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_AUDIO_H */
