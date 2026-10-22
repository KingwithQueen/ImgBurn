/**
 * @file imgburn_audio.c
 * @brief ImgBurn Core C - Audio CD Engine & Decoder Pipeline Implementation
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MsfTime lba_to_msf(uint32_t lba) {
    MsfTime msf;
    uint32_t totalSec = lba / CDDA_FRAMES_PER_SEC;
    msf.frame = (uint8_t)(lba % CDDA_FRAMES_PER_SEC);
    msf.second = (uint8_t)(totalSec % 60);
    msf.minute = (uint8_t)(totalSec / 60);
    return msf;
}

uint32_t msf_to_lba(MsfTime msf) {
    return ((uint32_t)msf.minute * 60 + (uint32_t)msf.second) * CDDA_FRAMES_PER_SEC + msf.frame;
}

void format_msf(MsfTime msf, char* outStr, size_t maxLen) {
    if (!outStr || maxLen == 0) return;
    snprintf(outStr, maxLen, "%02u:%02u:%02u", msf.minute, msf.second, msf.frame);
}

bool parse_msf(const char* str, MsfTime* outMsf) {
    if (!str || !outMsf) return false;
    unsigned int m = 0, s = 0, f = 0;
    if (sscanf(str, "%u:%u:%u", &m, &s, &f) == 3) {
        outMsf->minute = (uint8_t)m;
        outMsf->second = (uint8_t)s;
        outMsf->frame = (uint8_t)f;
        return true;
    }
    return false;
}

/* RIFF WAVE Header structures */
#pragma pack(push, 1)
typedef struct {
    char riffId[4];         /* "RIFF" */
    uint32_t riffSize;
    char waveId[4];         /* "WAVE" */
} RiffHeader;

typedef struct {
    char chunkId[4];        /* "fmt " */
    uint32_t chunkSize;
    uint16_t audioFormat;   /* 1 = PCM */
    uint16_t numChannels;   /* 2 */
    uint32_t sampleRate;    /* 44100 */
    uint32_t byteRate;      /* 176400 */
    uint16_t blockAlign;    /* 4 */
    uint16_t bitsPerSample; /* 16 */
} WaveFmtChunk;

typedef struct {
    char chunkId[4];
    uint32_t chunkSize;
} ChunkHeader;
#pragma pack(pop)

static bool try_open_native_wav(AudioDecoder* decoder, const wchar_t* filePath, uint32_t* outTotalSectors) {
    HANDLE file = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    RiffHeader riff;
    DWORD actual = 0;
    bool success = false, foundFmt = false, foundData = false;
    WaveFmtChunk fmt = {0};
    uint64_t dataOffset = 0;
    uint32_t dataSize = 0;
    if (!GetFileSizeEx(file, &size) || !ReadFile(file, &riff, sizeof(riff), &actual, NULL) || actual != sizeof(riff)) goto cleanup;
    if (memcmp(riff.riffId, "RIFF", 4) != 0 || memcmp(riff.waveId, "WAVE", 4) != 0) goto cleanup;
    uint64_t end = (uint64_t)riff.riffSize + 8, position = sizeof(riff);
    if (end > (uint64_t)size.QuadPart || end < sizeof(riff)) goto cleanup;
    while (position + sizeof(ChunkHeader) <= end) {
        LARGE_INTEGER offset; offset.QuadPart = (LONGLONG)position;
        ChunkHeader chunk;
        if (!SetFilePointerEx(file, offset, NULL, FILE_BEGIN) || !ReadFile(file, &chunk, sizeof(chunk), &actual, NULL) || actual != sizeof(chunk)) goto cleanup;
        position += sizeof(chunk);
        uint64_t next = position + chunk.chunkSize + (chunk.chunkSize & 1);
        if (next > end) goto cleanup;
        if (memcmp(chunk.chunkId, "fmt ", 4) == 0) {
            /* PCM 格式块至少有16字节；短读和缺少字段不能作为有效格式。 */
            if (chunk.chunkSize < 16 || !ReadFile(file, &fmt.audioFormat, 16, &actual, NULL) || actual != 16) goto cleanup;
            foundFmt = true;
        } else if (memcmp(chunk.chunkId, "data", 4) == 0 && !foundData) {
            dataOffset = position;
            dataSize = chunk.chunkSize;
            foundData = true;
        }
        if (foundFmt && foundData) break;
        position = next;
    }
    if (!foundFmt || !foundData || fmt.audioFormat != 1 || fmt.numChannels != CDDA_CHANNELS || fmt.sampleRate != CDDA_SAMPLE_RATE ||
        fmt.bitsPerSample != CDDA_BITS_PER_SAMPLE || fmt.blockAlign != CDDA_BYTES_PER_SAMPLE || fmt.byteRate != CDDA_SAMPLE_RATE * CDDA_BYTES_PER_SAMPLE ||
        dataSize % CDDA_BYTES_PER_SAMPLE != 0) goto cleanup;
    LARGE_INTEGER pcm; pcm.QuadPart = (LONGLONG)dataOffset;
    if (!SetFilePointerEx(file, pcm, NULL, FILE_BEGIN)) goto cleanup;
    decoder->hFile = file;
    decoder->isPipe = false;
    decoder->totalSamples = dataSize / CDDA_BYTES_PER_SAMPLE;
    decoder->dataBytesRemaining = dataSize;
    if (outTotalSectors) *outTotalSectors = (uint32_t)(((uint64_t)dataSize + CDDA_BYTES_PER_FRAME - 1) / CDDA_BYTES_PER_FRAME);
    success = true;
cleanup:
    if (!success) CloseHandle(file);
    return success;
}

static void find_ffmpeg_executable(const wchar_t* userPath, wchar_t* outPath, size_t maxLen) {
    if (userPath && GetFileAttributesW(userPath) != INVALID_FILE_ATTRIBUTES) {
        wcsncpy(outPath, userPath, maxLen - 1);
        return;
    }

    /* Common locations */
    const wchar_t* candidates[] = {
        L"E:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
        L"C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
        L"ffmpeg.exe"
    };

    for (int i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        if (GetFileAttributesW(candidates[i]) != INVALID_FILE_ATTRIBUTES) {
            wcsncpy(outPath, candidates[i], maxLen - 1);
            return;
        }
    }

    /* Default fallback */
    wcsncpy(outPath, L"ffmpeg.exe", maxLen - 1);
}

bool audio_decoder_open(AudioDecoder* decoder,
                        const wchar_t* filePath,
                        const wchar_t* ffmpegPath,
                        uint32_t* outTotalSectors) {
    if (!decoder || !filePath) return false;
    memset(decoder, 0, sizeof(*decoder));

    /* 1. Try native WAV first (zero overhead) */
    if (try_open_native_wav(decoder, filePath, outTotalSectors)) {
        return true;
    }

    /* 2. Launch FFmpeg pipe to decode any format directly into 44.1kHz 16-bit PCM */
    wchar_t exePath[MAX_PATH];
    find_ffmpeg_executable(ffmpegPath, exePath, MAX_PATH);

    HANDLE hPipeRead = NULL;
    HANDLE hPipeWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0)) {
        return false;
    }

    /* Ensure read handle is NOT inherited */
    SetHandleInformation(hPipeRead, HANDLE_FLAG_INHERIT, 0);

    /* Construct FFmpeg command line:
     * ffmpeg -v error -i "input" -f s16le -acodec pcm_s16le -ac 2 -ar 44100 -vn -
     */
    wchar_t cmdLine[1024];
    swprintf(cmdLine, 1024,
             L"\"%s\" -v error -i \"%s\" -f s16le -acodec pcm_s16le -ac 2 -ar 44100 -vn -",
             exePath, filePath);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdOutput = hPipeWrite;
    si.hStdError = NULL;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    BOOL created = CreateProcessW(NULL, cmdLine, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    /* Close write handle in parent so EOF is triggered when FFmpeg exits */
    CloseHandle(hPipeWrite);

    if (!created) {
        CloseHandle(hPipeRead);
        return false;
    }

    CloseHandle(pi.hThread);
    decoder->hProcess = pi.hProcess;
    decoder->hPipeRead = hPipeRead;
    decoder->isPipe = true;

    /* For streaming pipes, total sectors can be updated during read */
    if (outTotalSectors) {
        *outTotalSectors = 0;
    }

    return true;
}

uint32_t audio_decoder_read(AudioDecoder* decoder, void* outBuffer, uint32_t bytesToRead) {
    if (!decoder || !outBuffer || bytesToRead == 0) return 0;
    DWORD bytesRead = 0;

    if (!decoder->isPipe) {
        if (decoder->dataBytesRemaining == 0) return 0;
        if (bytesToRead > decoder->dataBytesRemaining) bytesToRead = (uint32_t)decoder->dataBytesRemaining;
        if (!ReadFile(decoder->hFile, outBuffer, bytesToRead, &bytesRead, NULL)) {
            decoder->lastError = GetLastError();
            return 0;
        }
        if (bytesRead == 0) decoder->lastError = ERROR_HANDLE_EOF;
        decoder->dataBytesRemaining -= bytesRead;
    } else {
        if (!ReadFile(decoder->hPipeRead, outBuffer, bytesToRead, &bytesRead, NULL)) {
            return 0;
        }
    }

    decoder->samplesRead += bytesRead / CDDA_BYTES_PER_SAMPLE;
    return bytesRead;
}

void audio_decoder_close(AudioDecoder* decoder) {
    if (!decoder) return;

    if (decoder->isPipe) {
        if (decoder->hPipeRead) {
            CloseHandle(decoder->hPipeRead);
            decoder->hPipeRead = NULL;
        }
        if (decoder->hProcess) {
            WaitForSingleObject(decoder->hProcess, 1000);
            CloseHandle(decoder->hProcess);
            decoder->hProcess = NULL;
        }
    } else {
        if (decoder->hFile && decoder->hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(decoder->hFile);
            decoder->hFile = NULL;
        }
    }

    memset(decoder, 0, sizeof(*decoder));
}

bool cue_sheet_parse(const wchar_t* cuePath, AudioCompilation* outComp) {
    if (!cuePath || !outComp) return false;
    memset(outComp, 0, sizeof(*outComp));

    FILE* f = _wfopen(cuePath, L"r");
    if (!f) return false;

    char line[512];
    AudioTrack* currTrack = NULL;

    while (fgets(line, sizeof(line), f)) {
        /* Trim whitespace */
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t len = strlen(p);
        while (len > 0 && (p[len-1] == '\r' || p[len-1] == '\n' || p[len-1] == ' ')) {
            p[--len] = '\0';
        }
        if (len == 0) continue;

        if (strncmp(p, "TITLE ", 6) == 0) {
            char title[128];
            if (sscanf(p + 6, "\"%127[^\"]\"", title) == 1 || sscanf(p + 6, "%127s", title) == 1) {
                if (currTrack) {
                    strncpy(currTrack->title, title, sizeof(currTrack->title) - 1);
                } else {
                    strncpy(outComp->cdTitle, title, sizeof(outComp->cdTitle) - 1);
                }
            }
        } else if (strncmp(p, "PERFORMER ", 10) == 0) {
            char perf[128];
            if (sscanf(p + 10, "\"%127[^\"]\"", perf) == 1 || sscanf(p + 10, "%127s", perf) == 1) {
                if (currTrack) {
                    strncpy(currTrack->performer, perf, sizeof(currTrack->performer) - 1);
                } else {
                    strncpy(outComp->cdPerformer, perf, sizeof(outComp->cdPerformer) - 1);
                }
            }
        } else if (strncmp(p, "FILE ", 5) == 0) {
            char filename[MAX_PATH];
            if (sscanf(p + 5, "\"%259[^\"]\"", filename) == 1) {
                /* Store file name for next track */
            }
        } else if (strncmp(p, "TRACK ", 6) == 0) {
            int tNum = 0;
            char tType[32];
            if (sscanf(p + 6, "%d %31s", &tNum, tType) == 2) {
                if (outComp->trackCount < MAX_AUDIO_TRACKS) {
                    currTrack = &outComp->tracks[outComp->trackCount++];
                    memset(currTrack, 0, sizeof(*currTrack));
                    currTrack->trackNumber = tNum;
                    currTrack->pregapSectors = (tNum == 1) ? 150 : 0;
                }
            }
        } else if (strncmp(p, "INDEX 01 ", 9) == 0) {
            if (currTrack) {
                char msfStr[32];
                if (sscanf(p + 9, "%31s", msfStr) == 1) {
                    parse_msf(msfStr, &currTrack->startTime);
                    currTrack->startLba = msf_to_lba(currTrack->startTime);
                }
            }
        }
    }

    fclose(f);
    return outComp->trackCount > 0;
}

bool cue_sheet_generate(const AudioCompilation* comp, const wchar_t* outCuePath) {
    if (!comp || !outCuePath || comp->trackCount == 0) return false;

    FILE* f = _wfopen(outCuePath, L"w");
    if (!f) return false;

    if (comp->cdTitle[0]) {
        fprintf(f, "TITLE \"%s\"\n", comp->cdTitle);
    }
    if (comp->cdPerformer[0]) {
        fprintf(f, "PERFORMER \"%s\"\n", comp->cdPerformer);
    }

    for (int i = 0; i < comp->trackCount; i++) {
        const AudioTrack* t = &comp->tracks[i];
        /* Convert wide path to basename for CUE */
        wchar_t fname[MAX_PATH];
        _wsplitpath(t->filePath, NULL, NULL, fname, NULL);
        wchar_t ext[32];
        _wsplitpath(t->filePath, NULL, NULL, NULL, ext);
        wcscat(fname, ext);

        fprintf(f, "FILE \"%ls\" WAVE\n", fname);
        fprintf(f, "  TRACK %02d AUDIO\n", t->trackNumber);
        if (t->title[0]) {
            fprintf(f, "    TITLE \"%s\"\n", t->title);
        }
        if (t->performer[0]) {
            fprintf(f, "    PERFORMER \"%s\"\n", t->performer);
        }
        if (t->pregapSectors > 0) {
            MsfTime pregapMsf = lba_to_msf(t->pregapSectors);
            char pregapStr[32];
            format_msf(pregapMsf, pregapStr, sizeof(pregapStr));
            fprintf(f, "    PREGAP %s\n", pregapStr);
        }

        char msfStr[32];
        format_msf(t->startTime, msfStr, sizeof(msfStr));
        fprintf(f, "    INDEX 01 %s\n", msfStr);
    }

    fclose(f);
    return true;
}
