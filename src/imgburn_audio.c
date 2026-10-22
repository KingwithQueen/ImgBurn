/**
 * @file imgburn_audio.c
 * @brief ImgBurn Core C - Audio CD Engine & Decoder Pipeline Implementation
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_audio.h"
#include "imgburn_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

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
    if (!str || !outMsf || strlen(str) != 8 || str[2] != ':' || str[5] != ':') return false;
    for (int i = 0; i < 8; i++) if (i != 2 && i != 5 && (str[i] < '0' || str[i] > '9')) return false;
    unsigned m = (str[0] - '0') * 10 + str[1] - '0', s = (str[3] - '0') * 10 + str[4] - '0', f = (str[6] - '0') * 10 + str[7] - '0';
    if (s >= 60 || f >= 75) return false;
    *outMsf = (MsfTime){(uint8_t)m, (uint8_t)s, (uint8_t)f}; return true;
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
            DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) decoder->lastError = error;
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

bool audio_measure_decoded(const wchar_t* path, uint64_t* pcmBytes) {
    if (!path || !pcmBytes || wcslen(path) >= MAX_PATH || wcschr(path, L'"')) return false;
    *pcmBytes = 0;
    HANDLE source = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (source == INVALID_HANDLE_VALUE) return false;
    AudioDecoder decoder; uint32_t sectors = 0;
    if (!audio_decoder_open(&decoder, path, NULL, &sectors)) { CloseHandle(source); return false; }
    uint8_t buffer[65536]; uint64_t bytes = 0;
    for (;;) { uint32_t actual = audio_decoder_read(&decoder, buffer, sizeof(buffer)); if (!actual) break; if (bytes > UINT64_MAX - actual) { decoder.lastError = ERROR_FILE_TOO_LARGE; break; } bytes += actual; }
    bool ok = decoder.lastError == 0 && bytes && bytes % CDDA_BYTES_PER_SAMPLE == 0;
    if (decoder.isPipe) {
        DWORD code = 1;
        if (WaitForSingleObject(decoder.hProcess, 30000) != WAIT_OBJECT_0) { TerminateProcess(decoder.hProcess, ERROR_TIMEOUT); ok = false; }
        else if (!GetExitCodeProcess(decoder.hProcess, &code) || code != 0) ok = false;
    }
    audio_decoder_close(&decoder); CloseHandle(source);
    if (ok) *pcmBytes = bytes;
    return ok;
}

bool cue_sheet_parse(const wchar_t* cuePath, AudioCompilation* outComp) {
    if (!outComp) return false;
    memset(outComp, 0, sizeof(*outComp));
    CueSheet* sheet = (CueSheet*)calloc(1, sizeof(*sheet));
    if (!sheet) return false;
    if (!imgburn_cue_parse(cuePath, sheet)) { free(sheet); return false; }
    for (int i = 0; i < sheet->trackCount; i++) if (sheet->tracks[i].mode != TRACK_MODE_AUDIO || sheet->tracks[i].sectorCount >= 100 * 60 * 75) { free(sheet); return false; }
    strcpy(outComp->cdTitle, sheet->discTitle); strcpy(outComp->cdPerformer, sheet->discPerformer);
    strcpy(outComp->catalogNumber, sheet->catalogNumber);
    strcpy(outComp->songwriter, sheet->songwriter); wcscpy(outComp->cdTextFile, sheet->cdTextFile); outComp->sessionCount = sheet->sessionCount;
    memcpy(outComp->sessionLeadIn, sheet->sessionLeadIn, sizeof(outComp->sessionLeadIn)); memcpy(outComp->sessionLeadOut, sheet->sessionLeadOut, sizeof(outComp->sessionLeadOut)); memcpy(outComp->sessionEndLba, sheet->sessionEndLba, sizeof(outComp->sessionEndLba));
    outComp->trackCount = sheet->trackCount; outComp->totalSectors = sheet->totalSectors;
    for (int i = 0; i < sheet->trackCount; i++) {
        const CueTrack* source = &sheet->tracks[i]; AudioTrack* target = &outComp->tracks[i];
        target->trackNumber = source->trackNumber; wcscpy(target->filePath, source->sourceFilePath);
        strcpy(target->title, source->title); strcpy(target->performer, source->performer);
        strcpy(target->isrc, source->isrc);
        target->startLba = source->startLba; target->sectorCount = source->sectorCount; target->pregapSectors = source->pregapSectors;
        target->postgapSectors = source->postgapSectors; target->sourceIsWave = source->sourceIsWave;
        target->sourceType = source->sourceType; target->flags = source->flags; target->sessionNumber = source->sessionNumber; target->sourceFileGroup = source->sourceFileGroup; strcpy(target->songwriter, source->songwriter);
        target->indexCount = source->indexCount; memcpy(target->indices, source->indices, (size_t)source->indexCount * sizeof(CueIndex));
        target->duration = lba_to_msf(source->sectorCount);
        for (int j = 0; j < source->indexCount; j++) {
            if (source->indices[j].indexNumber == 1) target->startTime = lba_to_msf(source->indices[j].lba);
            else if (source->indices[j].indexNumber == 0) { target->hasIndex00 = true; target->index00 = lba_to_msf(source->indices[j].lba); }
        }
    }
    free(sheet); return true;
}

static bool cue_append(char* buffer, size_t capacity, size_t* used, const char* format, ...) {
    va_list arguments; va_start(arguments, format);
    int count = vsnprintf(buffer + *used, capacity - *used, format, arguments); va_end(arguments);
    if (count < 0 || (size_t)count >= capacity - *used) return false;
    *used += (size_t)count; return true;
}

static bool cue_metadata(const char* value, size_t capacity) {
    size_t length = strnlen(value, capacity);
    if (length == capacity) return false;
    for (size_t i = 0; i < length; i++) if ((uint8_t)value[i] < 32 || value[i] == '"') return false;
    return true;
}

bool cue_sheet_generate(const AudioCompilation* comp, const wchar_t* outCuePath) {
    if (!comp || !outCuePath || comp->trackCount <= 0 || comp->trackCount > MAX_AUDIO_TRACKS || wcslen(outCuePath) >= MAX_PATH || !cue_metadata(comp->cdTitle, 128) || !cue_metadata(comp->cdPerformer, 128)) return false;
    wchar_t destination[MAX_PATH], temporary[MAX_PATH], previous[MAX_PATH] = L"";
    DWORD chars = GetFullPathNameW(outCuePath, MAX_PATH, destination, NULL);
    if (!chars || chars >= MAX_PATH) return false;
    const size_t capacity = 256 * 1024;
    char* text = (char*)calloc(1, capacity);
    if (!text) return false;
    bool ok = true; CueFileType previousType = CUE_FILE_BINARY; unsigned session = 0, previousGroup = 0;
    size_t used = 0;
    if (comp->cdTitle[0]) ok = cue_append(text, capacity, &used, "TITLE \"%s\"\n", comp->cdTitle);
    if (ok && comp->cdPerformer[0]) ok = cue_append(text, capacity, &used, "PERFORMER \"%s\"\n", comp->cdPerformer);
    if (!cue_metadata(comp->songwriter, 128)) ok = false;
    if (ok && comp->songwriter[0]) ok = cue_append(text, capacity, &used, "SONGWRITER \"%s\"\n", comp->songwriter);
    if (ok && comp->cdTextFile[0]) {
        char utf8[MAX_PATH * 3]; wchar_t cdText[MAX_PATH];
        chars = wcsnlen(comp->cdTextFile, MAX_PATH) == MAX_PATH ? 0 : GetFullPathNameW(comp->cdTextFile, MAX_PATH, cdText, NULL);
        if (!chars || chars >= MAX_PATH || !_wcsicmp(cdText, destination) || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, cdText, -1, utf8, sizeof(utf8), NULL, NULL) || !cue_metadata(utf8, sizeof(utf8))) ok = false;
        else ok = cue_append(text, capacity, &used, "CDTEXTFILE \"%s\"\n", utf8);
    }
    if (comp->catalogNumber[0]) {
        if (strnlen(comp->catalogNumber, 16) != 13 || strspn(comp->catalogNumber, "0123456789") != 13) ok = false;
        else if (ok) ok = cue_append(text, capacity, &used, "CATALOG %s\n", comp->catalogNumber);
    }
    for (int i = 0; i < comp->trackCount && ok; i++) {
        const AudioTrack* track = &comp->tracks[i];
        wchar_t source[MAX_PATH]; char utf8[MAX_PATH * 3];
        if (wcsnlen(track->filePath, MAX_PATH) == MAX_PATH || !track->filePath[0] || track->trackNumber != i + 1 || !cue_metadata(track->title, 128) || !cue_metadata(track->performer, 128)) { ok = false; break; }
        chars = GetFullPathNameW(track->filePath, MAX_PATH, source, NULL);
        if (!chars || chars >= MAX_PATH || !_wcsicmp(source, destination) || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source, -1, utf8, sizeof(utf8), NULL, NULL)) { ok = false; break; }
        unsigned currentSession = track->sessionNumber ? track->sessionNumber : 1;
        if (currentSession != session) {
            if (currentSession != session + 1 || currentSession > 99) { ok = false; break; }
            if (session) {
                uint32_t end = comp->sessionEndLba[session - 1];
                if (!end || end >= 100 * 60 * 75) { ok = false; break; }
                MsfTime leadOut = lba_to_msf(end);
                /* 原版在下一会话前要求显式LEAD-OUT，不能只输出SESSION。 */
                ok = cue_append(text, capacity, &used, "REM LEAD-OUT %02u:%02u:%02u\n", leadOut.minute, leadOut.second, leadOut.frame);
            }
            if (ok) ok = cue_append(text, capacity, &used, "REM SESSION %u\n", currentSession); session = currentSession;
            if (currentSession > 1 && comp->sessionLeadIn[currentSession - 1] >= 100 * 60 * 75) ok = false;
            if (ok && currentSession > 1 && comp->sessionLeadIn[currentSession - 1]) { MsfTime lead = lba_to_msf(comp->sessionLeadIn[currentSession - 1]); ok = cue_append(text, capacity, &used, "REM LEAD-IN %02u:%02u:%02u\n", lead.minute, lead.second, lead.frame); }
        }
        CueFileType type = track->sourceIsWave && track->sourceType == CUE_FILE_BINARY ? CUE_FILE_WAVE : track->sourceType;
        static const char* typeNames[] = {"BINARY", "MOTOROLA", "WAVE", "AIFF", "MP3", "WAVE"};
        if ((unsigned)type >= sizeof(typeNames) / sizeof(*typeNames)) { ok = false; break; }
        if (wcscmp(previous, source) || previousType != type || (track->sourceFileGroup && track->sourceFileGroup != previousGroup)) {
            if (ok) ok = cue_append(text, capacity, &used, "FILE \"%s\" %s\n", utf8, typeNames[type]);
            wcscpy(previous, source); previousType = type; previousGroup = track->sourceFileGroup;
        }
        if (ok) ok = cue_append(text, capacity, &used, "  TRACK %02d AUDIO\n", track->trackNumber);
        if (ok && track->title[0]) ok = cue_append(text, capacity, &used, "    TITLE \"%s\"\n", track->title);
        if (ok && track->performer[0]) ok = cue_append(text, capacity, &used, "    PERFORMER \"%s\"\n", track->performer);
        if (!cue_metadata(track->songwriter, 128)) ok = false;
        if (ok && track->songwriter[0]) ok = cue_append(text, capacity, &used, "    SONGWRITER \"%s\"\n", track->songwriter);
        if (track->flags & ~(CUE_FLAG_DCP | CUE_FLAG_4CH | CUE_FLAG_PRE | CUE_FLAG_SCMS)) ok = false;
        if (ok && track->flags) ok = cue_append(text, capacity, &used, "    FLAGS%s%s%s%s\n", track->flags & CUE_FLAG_DCP ? " DCP" : "", track->flags & CUE_FLAG_4CH ? " 4CH" : "", track->flags & CUE_FLAG_PRE ? " PRE" : "", track->flags & CUE_FLAG_SCMS ? " SCMS" : "");
        if (track->isrc[0]) {
            if (strnlen(track->isrc, 16) != 12 || !cue_metadata(track->isrc, 16)) ok = false;
            for (size_t j = 0; ok && j < 12; j++) if (!((track->isrc[j] >= '0' && track->isrc[j] <= '9') || (track->isrc[j] >= 'A' && track->isrc[j] <= 'Z') || (track->isrc[j] >= 'a' && track->isrc[j] <= 'z'))) ok = false;
            if (ok) ok = cue_append(text, capacity, &used, "    ISRC %s\n", track->isrc);
        }
        if (ok && track->pregapSectors) {
            MsfTime gap = lba_to_msf(track->pregapSectors);
            if (track->pregapSectors >= 100 * 60 * 75) ok = false;
            else ok = cue_append(text, capacity, &used, "    PREGAP %02u:%02u:%02u\n", gap.minute, gap.second, gap.frame);
        }
        if (track->indexCount < 0 || track->indexCount > MAX_CUE_INDICES) ok = false;
        if (track->indexCount) {
            for (int index = 0; ok && index < track->indexCount; index++) {
                const CueIndex* entry = &track->indices[index];
                if (entry->indexNumber < 0 || entry->indexNumber > 99 || entry->lba >= 100 * 60 * 75 || (index && (entry->indexNumber <= track->indices[index - 1].indexNumber || entry->lba < track->indices[index - 1].lba))) { ok = false; break; }
                MsfTime time = lba_to_msf(entry->lba); ok = cue_append(text, capacity, &used, "    INDEX %02d %02u:%02u:%02u\n", entry->indexNumber, time.minute, time.second, time.frame);
            }
        } else if (ok && track->hasIndex00) {
            MsfTime time = track->index00;
            if (time.minute >= 100 || time.second >= 60 || time.frame >= 75) ok = false;
            else ok = cue_append(text, capacity, &used, "    INDEX 00 %02u:%02u:%02u\n", time.minute, time.second, time.frame);
        }
        MsfTime time = track->startTime;
        if (time.minute >= 100 || time.second >= 60 || time.frame >= 75) ok = false;
        if (ok && !track->indexCount) ok = cue_append(text, capacity, &used, "    INDEX 01 %02u:%02u:%02u\n", time.minute, time.second, time.frame);
        if (ok && track->postgapSectors) {
            MsfTime gap = lba_to_msf(track->postgapSectors);
            if (track->postgapSectors >= 100 * 60 * 75) ok = false;
            else ok = cue_append(text, capacity, &used, "    POSTGAP %02u:%02u:%02u\n", gap.minute, gap.second, gap.frame);
        }
    }
    if (!ok) { free(text); return false; }
    /* 先完成临时文件，再替换目标，保证失败不会留下半份CUE。 */
    static volatile LONG sequence = 0;
    HANDLE output = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 16; i++) {
        if (swprintf(temporary, MAX_PATH, L"%ls.partial.%lu.%ld", destination, GetCurrentProcessId(), InterlockedIncrement(&sequence)) < 0) { free(text); return false; }
        output = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (output != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    ok = output != INVALID_HANDLE_VALUE;
    size_t offset = 0;
    while (ok && offset < used) { DWORD actual = 0; ok = WriteFile(output, text + offset, (DWORD)(used - offset), &actual, NULL) && actual && actual <= used - offset; offset += actual; }
    if (ok) ok = FlushFileBuffers(output) != FALSE;
    if (output != INVALID_HANDLE_VALUE && !CloseHandle(output)) ok = false;
    if (ok) ok = MoveFileExW(temporary, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok && output != INVALID_HANDLE_VALUE) DeleteFileW(temporary);
    free(text); return ok;
}
