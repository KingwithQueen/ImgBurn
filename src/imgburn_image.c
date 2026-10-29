/**
 * @file imgburn_image.c
 * @brief Optical Disc Image Parsers & Generators (CUE, BIN, MDS, ISO, DVD-Video IFO)
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_image.h"
#include "imgburn_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>

/* -------------------------------------------------------------------------
 * Image Format Detection
 * ------------------------------------------------------------------------- */
DiscImageType imgburn_image_detect_type(const wchar_t *filePath) {
    if (!filePath || wcslen(filePath) >= MAX_PATH) return IMAGE_TYPE_UNKNOWN;

    /* Check if directory */
    DWORD attr = GetFileAttributesW(filePath);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        wchar_t checkPath[MAX_PATH];
        swprintf(checkPath, MAX_PATH, L"%ls\\VIDEO_TS", filePath);
        if (GetFileAttributesW(checkPath) != INVALID_FILE_ATTRIBUTES) {
            return IMAGE_TYPE_DVD_FOLDER;
        }
        swprintf(checkPath, MAX_PATH, L"%ls\\BDMV", filePath);
        if (GetFileAttributesW(checkPath) != INVALID_FILE_ATTRIBUTES) {
            return IMAGE_TYPE_DVD_FOLDER;
        }
        return IMAGE_TYPE_UNKNOWN;
    }

    if (attr == INVALID_FILE_ATTRIBUTES) return IMAGE_TYPE_UNKNOWN;

    /* Check extension */
    const wchar_t *dot = wcsrchr(filePath, L'.');
    if (dot) {
        if (_wcsicmp(dot, L".cue") == 0) return IMAGE_TYPE_CUE;
        if (_wcsicmp(dot, L".mds") == 0) return IMAGE_TYPE_MDS;
        if (_wcsicmp(dot, L".iso") == 0) return IMAGE_TYPE_ISO;
        if (_wcsicmp(dot, L".img") == 0) return IMAGE_TYPE_ISO;
        if (_wcsicmp(dot, L".bin") == 0) return IMAGE_TYPE_BIN;
        if (_wcsicmp(dot, L".mdf") == 0) return IMAGE_TYPE_BIN;
    }

    /* Content magic inspection */
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return IMAGE_TYPE_UNKNOWN;

    uint8_t header[64] = { 0 };
    DWORD readBytes = 0;
    ReadFile(hFile, header, sizeof(header), &readBytes, NULL);

    /* Check MDS magic */
    if (readBytes >= 16 && memcmp(header, "MEDIA DESCRIPTOR", 16) == 0) {
        CloseHandle(hFile);
        return IMAGE_TYPE_MDS;
    }

    /* Check CUE text */
    if (readBytes >= 4) {
        if (memcmp(header, "FILE", 4) == 0 ||
            memcmp(header, "TRACK", 5) == 0 ||
            memcmp(header, "CATALOG", 7) == 0 ||
            memcmp(header, "TITLE", 5) == 0 ||
            memcmp(header, "REM", 3) == 0) {
            CloseHandle(hFile);
            return IMAGE_TYPE_CUE;
        }
    }

    /* Check ISO9660 / UDF header at sector 16 (offset 32768) */
    LARGE_INTEGER offset;
    offset.QuadPart = 16 * 2048;
    if (SetFilePointerEx(hFile, offset, NULL, FILE_BEGIN)) {
        uint8_t s16[16] = { 0 };
        ReadFile(hFile, s16, sizeof(s16), &readBytes, NULL);
        if (readBytes >= 6 && memcmp(&s16[1], "CD001", 5) == 0) {
            CloseHandle(hFile);
            return IMAGE_TYPE_ISO;
        }
        if (readBytes >= 6 && memcmp(&s16[1], "BEA01", 5) == 0) {
            CloseHandle(hFile);
            return IMAGE_TYPE_ISO;
        }
    }

    CloseHandle(hFile);
    return IMAGE_TYPE_BIN;
}

/* -------------------------------------------------------------------------
 * CUE Sheet Parser
 * ------------------------------------------------------------------------- */
/* 严格分词，拒绝过长、未闭合引号和多余字段，不把未知语法当作成功。 */
static bool cue_token(char** cursor, char* output, size_t capacity) {
    char* p = *cursor;
    while (*p && isspace((unsigned char)*p)) p++;
    bool quoted = *p == '"';
    if (quoted) p++;
    size_t count = 0;
    while (*p && (quoted ? *p != '"' : !isspace((unsigned char)*p))) {
        if (count + 1 >= capacity) return false;
        output[count++] = *p++;
    }
    if (quoted && *p++ != '"') return false;
    if (*p && !isspace((unsigned char)*p)) return false;
    output[count] = 0; *cursor = p; return count != 0 || quoted;
}

static bool cue_end(const char* text) { while (*text && isspace((unsigned char)*text)) text++; return *text == 0; }
/* IsoBuster会在REM会话字段后附加分号说明，只忽略这类注释的尾部。 */
static bool cue_rem_end(const char* text) { while (*text && isspace((unsigned char)*text)) text++; return !*text || *text == ';'; }

static bool cue_number(const char* text, unsigned limit, unsigned* result) {
    unsigned value = 0;
    if (!*text) return false;
    while (*text) {
        if (*text < '0' || *text > '9' || value > limit / 10) return false;
        value = value * 10 + (unsigned)(*text++ - '0');
        if (value > limit) return false;
    }
    *result = value; return true;
}

static bool cue_msf(const char* text, uint32_t* result) {
    if (strlen(text) != 8 || text[2] != ':' || text[5] != ':') return false;
    for (int i = 0; i < 8; i++) if (i != 2 && i != 5 && (text[i] < '0' || text[i] > '9')) return false;
    unsigned m = (text[0] - '0') * 10 + text[1] - '0', s = (text[3] - '0') * 10 + text[4] - '0', f = (text[6] - '0') * 10 + text[7] - '0';
    if (s >= 60 || f >= 75) return false;
    *result = (m * 60 + s) * 75 + f; return true;
}

static uint32_t cue_le32(const uint8_t* value) { return (uint32_t)value[0] | (uint32_t)value[1] << 8 | (uint32_t)value[2] << 16 | (uint32_t)value[3] << 24; }

static bool cue_read(FILE* file, uint64_t offset, void* data, size_t bytes) { return offset <= INT64_MAX && _fseeki64(file, (int64_t)offset, SEEK_SET) == 0 && fread(data, 1, bytes, file) == bytes; }

static char* cue_load_text(FILE* file) {
    if (_fseeki64(file, 0, SEEK_END)) return NULL;
    int64_t length = _ftelli64(file);
    if (length <= 0 || length > 4 * 1024 * 1024) return NULL;
    uint8_t* raw = (uint8_t*)calloc((size_t)length + 2, 1);
    if (!raw || !cue_read(file, 0, raw, (size_t)length)) { free(raw); return NULL; }
    wchar_t* wide = NULL; int count = 0;
    bool utf16 = length >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF));
    if (utf16) {
        if ((length - 2) % 2) { free(raw); return NULL; }
        count = (int)((length - 2) / 2);
        wide = (wchar_t*)calloc((size_t)count + 1, sizeof(wchar_t));
        if (!wide) { free(raw); return NULL; }
        for (int i = 0; i < count; i++) wide[i] = raw[0] == 0xFF ? (wchar_t)(raw[2 + i * 2] | (uint16_t)raw[3 + i * 2] << 8) : (wchar_t)((uint16_t)raw[2 + i * 2] << 8 | raw[3 + i * 2]);
    } else {
        int skip = length >= 3 && !memcmp(raw, "\xEF\xBB\xBF", 3) ? 3 : 0;
        UINT page = CP_UTF8; count = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, (const char*)raw + skip, (int)length - skip, NULL, 0);
        if (!count && !skip) { page = CP_ACP; count = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, (const char*)raw, (int)length, NULL, 0); }
        if (!count) { free(raw); return NULL; }
        wide = (wchar_t*)calloc((size_t)count + 1, sizeof(wchar_t));
        if (!wide || !MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, (const char*)raw + skip, (int)length - skip, wide, count)) { free(raw); free(wide); return NULL; }
    }
    free(raw);
    for (int i = 0; i < count; i++) if (!wide[i]) { free(wide); return NULL; }
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, count, NULL, 0, NULL, NULL);
    char* text = bytes > 0 ? (char*)calloc((size_t)bytes + 1, 1) : NULL;
    if (text && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, count, text, bytes, NULL, NULL)) { free(text); text = NULL; }
    free(wide); return text;
}

static uint32_t cue_sector_size(TrackDataMode mode) {
    switch (mode) {
        case TRACK_MODE_MODE1_2048: case TRACK_MODE_MODE2_2048: return 2048;
        case TRACK_MODE_MODE2_2324: return 2324;
        case TRACK_MODE_MODE2_2336: case TRACK_MODE_CDI_2336: return 2336;
        case TRACK_MODE_CDG_2448: return 2448;
        default: return 2352;
    }
}

static bool cue_source_bytes(const wchar_t* path, CueFileType type, uint64_t* bytes) {
    if (type != CUE_FILE_BINARY && type != CUE_FILE_MOTOROLA) return audio_measure_decoded(path, bytes);
    FILE* file = _wfopen(path, L"rb"); if (!file) return false;
    bool ok = !_fseeki64(file, 0, SEEK_END); int64_t size = ok ? _ftelli64(file) : -1;
    fclose(file); if (size <= 0) return false; *bytes = (uint64_t)size; return true;
}

static bool cue_path(const wchar_t* descriptor, const char* utf8, wchar_t* result) {
    wchar_t name[MAX_PATH], absolute[MAX_PATH], combined[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, name, MAX_PATH)) return false;
    DWORD count = GetFullPathNameW(descriptor, MAX_PATH, absolute, NULL);
    if (!count || count >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(absolute, L'\\'); if (!slash) return false; slash[1] = 0;
    if (name[0] == '\\' || name[0] == '/' || (wcslen(name) > 1 && name[1] == ':')) {
        if (swprintf(combined, MAX_PATH, L"%ls", name) < 0) return false;
    } else if (swprintf(combined, MAX_PATH, L"%ls%ls", absolute, name) < 0) return false;
    count = GetFullPathNameW(combined, MAX_PATH, result, NULL); return count && count < MAX_PATH;
}

bool imgburn_cue_parse_ex(const wchar_t* cueFilePath, CueSheet* outCue, CueSourceResolver resolver, void* context) {
    if (!outCue) return false;
    memset(outCue, 0, sizeof(*outCue));
    if (!cueFilePath || wcslen(cueFilePath) >= MAX_PATH) return false;
    FILE* file = _wfopen(cueFilePath, L"rb"); if (!file) return false;
    CueSheet* sheet = (CueSheet*)calloc(1, sizeof(*sheet)); char* text = cue_load_text(file); bool ok = false;
    if (!sheet || !text) goto done;
    DWORD chars = GetFullPathNameW(cueFilePath, MAX_PATH, sheet->cueFilePath, NULL);
    if (!chars || chars >= MAX_PATH) goto done;
    wchar_t currentFile[MAX_PATH] = L""; CueFileType type = CUE_FILE_BINARY;
    unsigned fileId = 0, session = 1, groups[99] = {0}; uint32_t decodedHints[100] = {0}, declaredSessionEnd[99] = {0};
    bool pregaps[99] = {0}, postgaps[99] = {0}, closedSessions[99] = {0}; CueTrack* track = NULL;
    sheet->sessionCount = 1;
    for (unsigned i = 1; i < 99; i++) sheet->sessionLeadIn[i] = 4500;
    for (unsigned i = 0; i < 99; i++) sheet->sessionLeadOut[i] = i ? 2250 : 6750;
    char* next = text;
    while (*next) {
        char* line = next; char* end = strpbrk(next, "\r\n");
        /* 原版也读取CR单独换行；CRLF作为一个行尾消费。 */
        if (end) { char separator = *end; *end = 0; next = end + 1; if (separator == '\r' && *next == '\n') next++; } else next += strlen(next);
        if (strlen(line) >= 2048) goto done;
        char* cursor = line; if (cue_end(cursor)) continue;
        char command[32], value[1024], second[32];
        if (!cue_token(&cursor, command, sizeof(command))) goto done;
        if (!_stricmp(command, "REM")) {
            if (cue_end(cursor)) continue;
            if (!cue_token(&cursor, command, sizeof(command))) goto done;
            if (!_stricmp(command, "SESSION")) {
                unsigned number;
                if (!cue_token(&cursor, second, sizeof(second)) || !cue_rem_end(cursor) || !cue_number(second, 99, &number) || !number || (number == 1 ? (session != 1 || sheet->trackCount != 0) : number != session + 1)) goto done;
                if (number > 1 && (!sheet->trackCount || sheet->tracks[sheet->trackCount - 1].sessionNumber != session || !closedSessions[session - 1])) goto done;
                session = number; sheet->sessionCount = session; track = NULL;
            } else if (!_stricmp(command, "LEAD-IN")) {
                if (!cue_token(&cursor, second, sizeof(second)) || !cue_rem_end(cursor) || !cue_msf(second, &sheet->sessionLeadIn[session - 1])) goto done;
            } else if (!_stricmp(command, "FILE-DECODED-SIZE")) {
                if (!fileId || !cue_token(&cursor, second, sizeof(second)) || !cue_rem_end(cursor) || !cue_msf(second, &decodedHints[fileId])) goto done;
            } else if (!_stricmp(command, "LEAD-OUT")) {
                /* 暂存声明坐标；独立新FILE的后续会话再换算成绝对A2。 */
                if (!sheet->trackCount || sheet->tracks[sheet->trackCount - 1].sessionNumber != session || closedSessions[session - 1] || !cue_token(&cursor, second, sizeof(second)) || !cue_rem_end(cursor) || !cue_msf(second, &declaredSessionEnd[session - 1]) || !declaredSessionEnd[session - 1]) goto done;
                closedSessions[session - 1] = true; track = NULL;
            }
            continue;
        }
        if (!cue_token(&cursor, value, sizeof(value))) goto done;
        if (!_stricmp(command, "FILE")) {
            if (!value[0] || !cue_token(&cursor, second, sizeof(second)) || !cue_end(cursor) || !cue_path(cueFilePath, value, currentFile) || ++fileId > 99) goto done;
            if (!_stricmp(second, "BINARY")) type = CUE_FILE_BINARY;
            else if (!_stricmp(second, "MOTOROLA")) type = CUE_FILE_MOTOROLA;
            else if (!_stricmp(second, "WAVE")) type = CUE_FILE_WAVE;
            else if (!_stricmp(second, "AIFF")) type = CUE_FILE_AIFF;
            else if (!_stricmp(second, "MP3")) type = CUE_FILE_MP3;
            else if (!_stricmp(second, "FLAC") || !_stricmp(second, "OGG") || !_stricmp(second, "APE") || !_stricmp(second, "WAVPACK")) type = CUE_FILE_AUDIO;
            else goto done;
            track = NULL; continue;
        }
        if (!_stricmp(command, "TRACK")) {
            unsigned number;
            if (!currentFile[0] || closedSessions[session - 1] || sheet->trackCount == 99 || !cue_number(value, 99, &number) || number != (unsigned)sheet->trackCount + 1 || !cue_token(&cursor, second, sizeof(second)) || !cue_end(cursor)) goto done;
            track = &sheet->tracks[sheet->trackCount]; groups[sheet->trackCount++] = fileId;
            track->trackNumber = (int)number; track->sessionNumber = session; track->sourceFileGroup = fileId; track->sourceType = type; track->sourceIsWave = type == CUE_FILE_WAVE; wcscpy(track->sourceFilePath, currentFile);
            if (!_stricmp(second, "AUDIO")) track->mode = TRACK_MODE_AUDIO;
            else if (!_stricmp(second, "MODE1/2048")) track->mode = TRACK_MODE_MODE1_2048;
            else if (!_stricmp(second, "MODE1/2352")) track->mode = TRACK_MODE_MODE1_2352;
            else if (!_stricmp(second, "MODE2/2336")) track->mode = TRACK_MODE_MODE2_2336;
            else if (!_stricmp(second, "MODE2/2352")) track->mode = TRACK_MODE_MODE2_2352;
            else if (!_stricmp(second, "MODE2/2048")) track->mode = TRACK_MODE_MODE2_2048;
            else if (!_stricmp(second, "MODE2/2324")) track->mode = TRACK_MODE_MODE2_2324;
            else if (!_stricmp(second, "CDI/2336")) track->mode = TRACK_MODE_CDI_2336;
            else if (!_stricmp(second, "CDI/2352")) track->mode = TRACK_MODE_CDI_2352;
            /* 原版2.5.8.0明确拒绝CUE的CDG轨道，不能仅凭2448字节长度宣称支持。 */
            else if (!_stricmp(second, "CDG")) { SetLastError(ERROR_NOT_SUPPORTED); goto done; }
            else goto done;
            track->sectorSize = cue_sector_size(track->mode);
            if (type >= CUE_FILE_WAVE && track->mode != TRACK_MODE_AUDIO) goto done;
            continue;
        }
        if (!_stricmp(command, "INDEX")) {
            unsigned index; uint32_t address;
            if (!track || track->indexCount == MAX_CUE_INDICES || !cue_number(value, 99, &index) || !cue_token(&cursor, second, sizeof(second)) || !cue_end(cursor) || !cue_msf(second, &address)) goto done;
            if (!track->indexCount && index > 1) goto done;
            if (track->indexCount && (index <= (unsigned)track->indices[track->indexCount - 1].indexNumber || address < track->indices[track->indexCount - 1].lba)) goto done;
            CueIndex* entry = &track->indices[track->indexCount++]; entry->indexNumber = (int)index; entry->lba = address; strcpy(entry->msfStr, second); continue;
        }
        if (!_stricmp(command, "FLAGS")) {
            if (!track || track->flags) goto done;
            do {
                unsigned flag = !_stricmp(value, "DCP") ? CUE_FLAG_DCP : !_stricmp(value, "4CH") ? CUE_FLAG_4CH : !_stricmp(value, "PRE") ? CUE_FLAG_PRE : !_stricmp(value, "SCMS") ? CUE_FLAG_SCMS : 0;
                if (!flag || (track->flags & flag)) goto done; track->flags |= flag;
                if (cue_end(cursor)) break;
                if (!cue_token(&cursor, value, sizeof(value))) goto done;
            } while (true);
            if (track->mode != TRACK_MODE_AUDIO && (track->flags & (CUE_FLAG_PRE | CUE_FLAG_4CH))) goto done;
            continue;
        }
        if (!cue_end(cursor)) goto done;
        if (!_stricmp(command, "TITLE") || !_stricmp(command, "PERFORMER") || !_stricmp(command, "SONGWRITER") || !_stricmp(command, "COMPOSER") || !_stricmp(command, "ARRANGER") || !_stricmp(command, "MESSAGE")) {
            if (strlen(value) >= 128) goto done;
            char* destination = !_stricmp(command, "TITLE") ? (track ? track->title : sheet->discTitle) : !_stricmp(command, "PERFORMER") ? (track ? track->performer : sheet->discPerformer) : !_stricmp(command, "SONGWRITER") ? (track ? track->songwriter : sheet->songwriter) : !_stricmp(command, "COMPOSER") ? (track ? track->composer : sheet->composer) : !_stricmp(command, "ARRANGER") ? (track ? track->arranger : sheet->arranger) : (track ? track->message : sheet->message);
            strcpy(destination, value);
        } else if (!_stricmp(command, "DISCID")) {
            if (track || strlen(value) >= sizeof(sheet->discId)) goto done; strcpy(sheet->discId, value);
        } else if (!_stricmp(command, "UPC")) {
            if (track || strlen(value) != 13 || strspn(value, "0123456789") != 13) goto done; strcpy(sheet->cdTextUpc, value);
        } else if (!_stricmp(command, "CDTEXTFILE")) {
            /* 原版允许外部CDT缺失时使用CUE文字；解析保留路径，使用方决定读取或回退。 */
            if (track || !cue_path(cueFilePath, value, sheet->cdTextFile)) goto done;
        } else if (!_stricmp(command, "PREGAP") || !_stricmp(command, "POSTGAP")) {
            uint32_t duration; if (!track || !cue_msf(value, &duration)) goto done;
            unsigned i = (unsigned)sheet->trackCount - 1;
            if (!_stricmp(command, "PREGAP")) { if (pregaps[i]) goto done; pregaps[i] = true; track->pregapSectors = duration; }
            else { if (postgaps[i]) goto done; postgaps[i] = true; track->postgapSectors = duration; }
        } else if (!_stricmp(command, "CATALOG")) {
            if (strlen(value) != 13 || strspn(value, "0123456789") != 13 || track) goto done; strcpy(sheet->catalogNumber, value);
        } else if (!_stricmp(command, "ISRC")) {
            if (!track || strlen(value) != 12) goto done;
            for (int i = 0; i < 12; i++) if (!isalnum((unsigned char)value[i])) goto done; strcpy(track->isrc, value);
        } else goto done;
    }
    if (!sheet->trackCount || groups[sheet->trackCount - 1] != fileId || sheet->tracks[sheet->trackCount - 1].sessionNumber != session) goto done;
    uint64_t total = 0; unsigned previousSession = 1;
    for (int first = 0; first < sheet->trackCount;) {
        int last = first + 1; while (last < sheet->trackCount && groups[last] == groups[first]) last++;
        if (groups[first] != (first ? groups[first - 1] + 1 : 1)) goto done;
        CueTrack* initial = &sheet->tracks[first];
        if (!initial->indexCount) goto done;
        /* 仅首轨省略INDEX00时保留源零帧起的隐式前缀，其余FILE从首个声明INDEX裁去未引用内容。 */
        uint32_t origin = first || initial->indices[0].indexNumber == 0 ? initial->indices[0].lba : 0;
        bool localSessionEnd = initial->sessionNumber > 1;
        if (initial->sessionNumber != previousSession) {
            total += (uint64_t)sheet->sessionLeadOut[previousSession - 1] + sheet->sessionLeadIn[initial->sessionNumber - 1];
            /* 新FILE的会话首轨自动暂停与FILE裁剪、显式PREGAP及存储INDEX00独立。 */
            total += 150;
            previousSession = initial->sessionNumber;
        }
        uint64_t sourceBytes; uint32_t cached = initial->sourceType >= CUE_FILE_WAVE ? decodedHints[groups[first]] : 0;
        if (cached) {
            /* 原版把此REM作为解码结果缓存，载入时不重新解码或要求与实际长度相等。 */
            DWORD attributes = GetFileAttributesW(initial->sourceFilePath); if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) goto done;
            sourceBytes = (uint64_t)cached * 2352;
        } else if (resolver && initial->sourceType >= CUE_FILE_WAVE) {
            if (!resolver(initial->sourceFilePath,initial->sourceType,&sourceBytes,context)) goto done;
        } else if (!cue_source_bytes(initial->sourceFilePath, initial->sourceType, &sourceBytes)) goto done;
        uint64_t usedBytes = 0, gaps = 0, sourceFrames = 0; uint32_t fileEnd = 0;
        for (int i = first; i < last; i++) {
            CueTrack* current = &sheet->tracks[i];
            current->decodedSourceSectors = cached;
            if (!current->indexCount) goto done;
            bool indexOne = false; uint32_t start = 0, segment = i == first ? 0 : current->indices[0].lba;
            for (int j = 0; j < current->indexCount; j++) if (current->indices[j].indexNumber == 1) { indexOne = true; start = current->indices[j].lba; }
            if (current->sessionNumber != previousSession) {
                /* 同FILE也重置会话逻辑原点；INDEX仍引用原FILE位置，不能把源间隔从字节偏移中扣掉。 */
                if (segment < fileEnd) goto done;
                usedBytes += (uint64_t)(segment - fileEnd) * sheet->tracks[i - 1].sectorSize; sourceFrames += segment - fileEnd;
                total = (uint64_t)sheet->sessionEndLba[previousSession - 1] + sheet->sessionLeadOut[previousSession - 1] + sheet->sessionLeadIn[current->sessionNumber - 1] + 150;
                origin = current->indices[0].lba; gaps = 0; localSessionEnd = true; previousSession = current->sessionNumber;
            }
            if (!indexOne || start < segment || start < origin || usedBytes > sourceBytes) goto done;
            uint64_t available = sourceBytes - usedBytes;
            uint64_t end = i + 1 < last && sheet->tracks[i + 1].indexCount ? sheet->tracks[i + 1].indices[0].lba : segment + (available + current->sectorSize - 1) / current->sectorSize;
            if (i + 1 == last && current->sourceType < CUE_FILE_WAVE && available % current->sectorSize) goto done;
            if (i + 1 == sheet->trackCount || sheet->tracks[i + 1].sessionNumber != current->sessionNumber) {
                uint32_t declared = declaredSessionEnd[current->sessionNumber - 1];
                uint64_t overhead = total + gaps + current->pregapSectors + current->postgapSectors;
                /* 后续会话的LEAD-OUT使用FILE坐标；首会话保留既有结束地址换算。 */
                uint64_t declaredEnd = localSessionEnd ? declared : (uint64_t)declared + origin;
                if (localSessionEnd) overhead = 0;
                if (declared) { if (declaredEnd <= overhead || declaredEnd - overhead > end) goto done; end = declaredEnd - overhead; }
            }
            if (end <= start || end > UINT32_MAX || current->indices[current->indexCount - 1].lba >= end) goto done;
            uint64_t segmentBytes = (end - segment) * current->sectorSize;
            if (i + 1 < last && segmentBytes > available) goto done;
            gaps += current->pregapSectors;
            if (total + start - origin + gaps > UINT32_MAX) goto done;
            current->startLba = (uint32_t)(total + start - origin + gaps); current->sectorCount = (uint32_t)(end - start);
            current->sourceByteOffset = usedBytes + (uint64_t)(start - segment) * current->sectorSize;
            current->sourceByteLength = (uint64_t)current->sectorCount * current->sectorSize;
            if (current->sourceByteOffset >= sourceBytes) goto done;
            if (current->sourceByteLength > sourceBytes - current->sourceByteOffset) current->sourceByteLength = sourceBytes - current->sourceByteOffset;
            usedBytes += segmentBytes; sourceFrames += end - segment; gaps += current->postgapSectors; fileEnd = (uint32_t)end;
            if (i + 1 == sheet->trackCount || sheet->tracks[i + 1].sessionNumber != current->sessionNumber) {
                uint64_t sessionEnd = total + end - origin + gaps; uint32_t declared = declaredSessionEnd[current->sessionNumber - 1];
                if (sessionEnd > UINT32_MAX || (declared && !localSessionEnd && declared != sessionEnd)) goto done;
                sheet->sessionEndLba[current->sessionNumber - 1] = (uint32_t)sessionEnd;
            }
            previousSession = current->sessionNumber;
        }
        /* 解码长度提示描述整个源文件，LEAD-OUT截短引用范围后也不能把尾部漏算。 */
        uint32_t lastSectorSize = sheet->tracks[last - 1].sectorSize;
        uint64_t decodedFrames = sourceFrames + (usedBytes < sourceBytes ? (sourceBytes - usedBytes + lastSectorSize - 1) / lastSectorSize : 0);
        if (decodedHints[groups[first]] && decodedHints[groups[first]] != decodedFrames) goto done;
        total += fileEnd - origin + gaps; if (total > UINT32_MAX) goto done;
        first = last;
    }
    sheet->totalSectors = (uint32_t)total; memcpy(outCue, sheet, sizeof(*sheet)); ok = true;
done:
    { DWORD error = GetLastError(); free(text); free(sheet); fclose(file); SetLastError(error); return ok; }
}

bool imgburn_cue_parse(const wchar_t* cueFilePath, CueSheet* outCue) { return imgburn_cue_parse_ex(cueFilePath,outCue,NULL,NULL); }

/* -------------------------------------------------------------------------
 * Media Descriptor Sheet (.MDS) Parser & Creator
 * ------------------------------------------------------------------------- */
static bool mds_range(uint64_t size, uint32_t offset, uint64_t count) { return offset >= 88 && offset <= size && count <= size - offset; }
static uint32_t mds_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void mds_set_be32(uint8_t* p, uint32_t value) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (24 - 8 * i)); }

static bool mds_resolve(const wchar_t* descriptor, const wchar_t* name, wchar_t* result) {
    wchar_t absolute[MAX_PATH], combined[MAX_PATH];
    DWORD length = GetFullPathNameW(descriptor, MAX_PATH, absolute, NULL);
    if (!length || length >= MAX_PATH || !name[0]) return false;
    wchar_t* slash = wcsrchr(absolute, L'\\'); if (!slash) return false; slash[1] = 0;
    if (name[0] == L'\\' || name[0] == L'/' || (wcslen(name) > 1 && name[1] == L':')) {
        if (swprintf(combined, MAX_PATH, L"%ls", name) < 0) return false;
    } else if (swprintf(combined, MAX_PATH, L"%ls%ls", absolute, name) < 0) return false;
    length = GetFullPathNameW(combined, MAX_PATH, result, NULL);
    return length && length < MAX_PATH;
}

/* carry表示本卷末扇区借用下一卷的字节数；末卷只计完整扇区。 */
static bool mds_part_geometry(uint64_t bytes, uint64_t skipped, uint32_t incomingCarry, bool last, bool splitSectors, uint32_t* sectors, uint32_t* carry) {
    if (bytes <= skipped) { SetLastError(ERROR_INVALID_DATA); return false; }
    uint64_t available = bytes - skipped;
    if (available < incomingCarry) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    available -= incomingCarry;
    uint32_t remainder = (uint32_t)(available % 2048);
    if (!splitSectors && remainder) { SetLastError(ERROR_INVALID_DATA); return false; }
    *carry = !last && remainder ? 2048 - remainder : 0;
    uint64_t count = available / 2048 + (*carry != 0);
    if (count > INT32_MAX) { SetLastError(splitSectors ? ERROR_NOT_SUPPORTED : ERROR_INVALID_DATA); return false; }
    *sectors = (uint32_t)count; return true;
}

static bool mds_filename(const uint8_t* data, uint64_t size, uint32_t footer, const wchar_t* descriptor, MdsDataFile* result, bool sourceLayout, bool splitSectors) {
    if (!mds_range(size, footer, 16)) return false;
    uint32_t offset = cue_le32(data + footer), wide = cue_le32(data + footer + 4);
    /* 来源重算实际分卷几何，不把footer的carry或扇区数当作寻址依据。 */
    if (splitSectors) { if (wide & 0xFFFF0000u) return false; wide &= 15; }
    if (wide > 1 || !mds_range(size, offset, wide ? 2 : 1)) return false;
    wchar_t name[MAX_PATH] = {0}; size_t count = 0;
    if (wide) {
        while (count < MAX_PATH && offset + (uint64_t)count * 2 + 2 <= size) {
            uint16_t value = (uint16_t)(data[offset + count * 2] | (uint16_t)data[offset + count * 2 + 1] << 8);
            if (!value) break; name[count++] = (wchar_t)value;
        }
        if (count == MAX_PATH || offset + (uint64_t)count * 2 + 2 > size) return false;
    } else {
        while (offset + count < size && data[offset + count] && count < MAX_PATH * 3) count++;
        if (offset + count >= size || count >= MAX_PATH * 3 || !MultiByteToWideChar(CP_ACP, 0, (const char*)data + offset, (int)count + 1, name, MAX_PATH)) return false;
    }
    if (name[0] == '*' && (name[1] == '.' || (sourceLayout && !name[1]))) {
        if (wcslen(name) >= 16 || wcschr(name + 1, L'*') || wcschr(name, L'\\') || wcschr(name, L'/')) return false;
        wchar_t local[MAX_PATH]; if (wcslen(descriptor) >= MAX_PATH) return false; wcscpy(local, descriptor);
        wchar_t* dot = wcsrchr(local, L'.');
        /* 来源裸*替换描述文件的基本名主干；父目录中的点不是扩展名。 */
        if (sourceLayout) {
            wchar_t* basename = local;
            for (wchar_t* p = local; *p; p++) if (*p == L'\\' || *p == L'/') basename = p + 1;
            dot = wcsrchr(basename,L'.'); if (!dot) dot = local + wcslen(local);
        }
        if (!dot || (size_t)(dot - local) + wcslen(name + 1) >= MAX_PATH) return false; wcscpy(dot, name + 1);
        DWORD length = GetFullPathNameW(local, MAX_PATH, result->path, NULL); if (!length || length >= MAX_PATH) return false;
    } else if (!mds_resolve(descriptor, name, result->path)) return false;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(result->path, GetFileExInfoStandard, &attributes) || (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    result->bytes = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
    return true;
}

static bool mds_track_mode(uint8_t mode, uint8_t sub, uint16_t bytes, bool dvd) {
    if (sub != 0 && sub != 8) return false;
    if (sub == 8) { if (bytes < 96) return false; bytes -= 96; }
    if (dvd) return (mode == 0x02 || mode == 0xAA) && !sub && bytes == 2048;
    if (mode == 0xA9) return bytes == 2352;
    if (mode == 0xAA) return bytes == 2048 || bytes == 2352;
    if (mode == 0xAB) return bytes == 2336 || bytes == 2352;
    if (mode == 0xAC) return bytes == 2048 || bytes == 2352;
    if (mode == 0xAD) return bytes == 2324 || bytes == 2328 || bytes == 2352;
    return false;
}

static bool mds_parse(const wchar_t* mdsFilePath, MdsSheet* outMds, bool sourceLayout) {
    if (!outMds) return false;
    memset(outMds, 0, sizeof(*outMds));
    if (!mdsFilePath || wcslen(mdsFilePath) >= MAX_PATH) return false;
    FILE* file = _wfopen(mdsFilePath, L"rb"); if (!file) return false;
    bool ok = false; uint8_t* data = NULL; MdsSheet* result = (MdsSheet*)calloc(1, sizeof(*result));
    if (!result || _fseeki64(file, 0, SEEK_END)) goto done;
    int64_t fileSize = _ftelli64(file); if (fileSize < 88 || fileSize > 16 * 1024 * 1024) goto done;
    uint64_t size = (uint64_t)fileSize; data = (uint8_t*)malloc((size_t)size);
    if (!data || !cue_read(file, 0, data, (size_t)size)) goto done;
    const MdsHeader* header = (const MdsHeader*)data;
    bool dvd = header->mediumType == 0x10 || header->mediumType == 0x12 || (sourceLayout && (header->mediumType == 0x13 || header->mediumType == 0x14 || header->mediumType == 0x15));
    if (memcmp(header->signature, "MEDIA DESCRIPTOR", 16) || header->version[0] != 1 || (!dvd && header->mediumType > 2) || !header->sessionCount || header->sessionCount > MAX_MDS_SESSIONS || !mds_range(size, header->sessionsOffset, (uint64_t)header->sessionCount * 24)) goto done;
    result->mediumType = header->mediumType; result->sessionCount = header->sessionCount;
    result->bcaOffset = header->bcaOffset; result->bcaLength = header->bcaLength; result->dpmOffset = header->dpmOffset;
    if (header->bcaLength && !mds_range(size, header->bcaOffset, header->bcaLength)) goto done;
    if (header->dpmOffset) {
        if (!mds_range(size, header->dpmOffset, 4)) goto done;
        uint32_t count = cue_le32(data + header->dpmOffset);
        if (!count || count > 1024 || !mds_range(size, header->dpmOffset, 4 + (uint64_t)count * 4)) goto done;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t offset = cue_le32(data + header->dpmOffset + 4 + i * 4);
            if (!mds_range(size, offset, 16) || !mds_range(size, offset, 16 + (uint64_t)cue_le32(data + offset + 12) * 4)) goto done;
        }
    }
    unsigned previousTrack = 0; int32_t previousEnd = INT32_MIN;
    for (unsigned s = 0; s < header->sessionCount; s++) {
        MdsSessionBlock session; memcpy(&session, data + header->sessionsOffset + s * 24, sizeof(session));
        bool compactCd = !dvd && header->sessionCount == 1 && session.totalTracks == 1 && session.leadInTracks == 3 && session.firstTrack == 1 && session.lastTrack == 1;
        if (session.sessionNumber != s + 1 || session.sessionStartLba < previousEnd || session.sessionEndLba <= session.sessionStartLba || session.sessionEndLba <= 0 || !session.totalTracks || (dvd ? (session.leadInTracks != 0 && session.leadInTracks != 3) : !compactCd && session.leadInTracks >= session.totalTracks) || session.firstTrack != previousTrack + 1 || session.lastTrack < session.firstTrack || session.lastTrack > 99 || !mds_range(size, session.tracksOffset, (uint64_t)session.totalTracks * 80)) goto done;
        unsigned actual = 0, leadIn = 0; int64_t previousTrackEnd = session.sessionStartLba;
        for (unsigned i = 0; i < session.totalTracks; i++) {
            MdsTrackBlock track; memcpy(&track, data + session.tracksOffset + i * 80, sizeof(track));
            if (track.point < 1 || track.point > 99) { leadIn++; continue; }
            if (track.point != previousTrack + 1 || track.point < session.firstTrack || track.point > session.lastTrack || result->trackCount >= MAX_MDS_TRACKS || !mds_track_mode(track.trackMode, track.subChannel, track.sectorSize, dvd) || !track.fileCount || track.fileCount > MAX_MDS_FILES - result->dataFileCount || !mds_range(size, track.footerOffset, (uint64_t)track.fileCount * 16)) goto done;
            MdsTrack* target = &result->tracks[result->trackCount++];
            target->sessionNumber = session.sessionNumber; target->trackNumber = track.point; target->mode = track.trackMode; target->subChannel = track.subChannel; target->control = track.adrControl; target->sectorSize = track.sectorSize; target->startLba = (int32_t)track.startLba; target->startByteOffset = track.startByteOffset;
            if (target->startLba < previousTrackEnd || target->startLba >= session.sessionEndLba) goto done;
            if (dvd) target->sectorCount = track.extraOffset ? track.extraOffset : (uint32_t)(session.sessionEndLba - target->startLba);
            else {
                if (!mds_range(size, track.extraOffset, 8)) goto done;
                target->pregapSectors = cue_le32(data + track.extraOffset); target->sectorCount = cue_le32(data + track.extraOffset + 4);
            }
            if (!target->sectorCount || (int64_t)target->startLba + target->sectorCount > session.sessionEndLba) goto done;
            previousTrackEnd = (int64_t)target->startLba + target->sectorCount;
            target->firstFile = (uint16_t)result->dataFileCount; target->fileCount = (uint16_t)track.fileCount;
            uint64_t available = 0; uint32_t incomingCarry = 0;
            bool splitSectors = sourceLayout && dvd && track.sectorSize == 2048 && !track.subChannel;
            for (uint32_t f = 0; f < track.fileCount; f++) {
                MdsDataFile* part = &result->files[result->dataFileCount++];
                if (!mds_filename(data, size, track.footerOffset + f * 16, mdsFilePath, part, sourceLayout, splitSectors)) goto done;
                uint64_t skipped = f ? 0 : track.startByteOffset;
                if (skipped > part->bytes || available > UINT64_MAX - (part->bytes - skipped)) goto done;
                available += part->bytes - skipped;
                if (splitSectors) {
                    uint32_t sectors, carry;
                    if (!mds_part_geometry(part->bytes,skipped,incomingCarry,f + 1 == track.fileCount,true,&sectors,&carry)) goto done;
                    incomingCarry = carry;
                } else if (f + 1 < track.fileCount && (part->bytes - skipped) % track.sectorSize) goto done;
            }
            if (available < (uint64_t)target->sectorCount * target->sectorSize) goto done;
            previousTrack = track.point; actual++;
        }
        /* 原版也接受单轨CD省略TOC块、保留leadInTracks=3的精简描述。 */
        if (actual != (unsigned)(session.lastTrack - session.firstTrack + 1) || (!dvd && !compactCd && leadIn != session.leadInTracks) || previousTrack != session.lastTrack) goto done;
        result->sessions[s] = session; previousEnd = session.sessionEndLba;
    }
    result->totalSectors = (uint32_t)previousEnd;
    if (header->discStructuresOffset) {
        if (!dvd || !mds_range(size, header->discStructuresOffset, 4100)) goto done;
        const uint8_t* pfi = data + header->discStructuresOffset + 2052; unsigned layers = (pfi[2] >> 5) & 3;
        if (layers > 1) goto done;
        result->layerCount = layers + 1; result->oppositeTrackPath = (pfi[2] & 0x10) != 0;
        if (layers) {
            if (!mds_range(size, header->discStructuresOffset, 8200)) goto done;
            uint32_t start = mds_be32(pfi + 4) & 0xFFFFFF, end = mds_be32(pfi + (result->oppositeTrackPath ? 12 : 8)) & 0xFFFFFF;
            if (start != 0x30000) goto done;
            /* 来源OTP/PTP复刻原版DWORD提示：零末地址不赋值，下溢不恢复物理层位置。 */
            if (sourceLayout) result->layerBreakLba = end ? end - start + 1 : 0;
            else {
                if (end < start) goto done;
                result->layerBreakLba = end - start + 1;
                if (result->layerBreakLba >= result->totalSectors) goto done;
            }
            if (result->oppositeTrackPath) { if (!sourceLayout && result->layerBreakLba < ((uint64_t)result->totalSectors + 1) / 2) goto done; }
            else {
                /* PTP每层均从内圈开始；来源只核低24位投影，不反推被省略的高位。 */
                const uint8_t* second = pfi + 4100; uint32_t secondStart = mds_be32(second + 4) & 0xFFFFFF, secondEnd = mds_be32(second + 8) & 0xFFFFFF;
                if ((second[2] & 0x70) != 0x20 || secondStart != 0x30000) goto done;
                if (sourceLayout) {
                    /* 端点0的公开提示仍为0，校验跨度则独立保留其模24结果。 */
                    uint32_t firstSpan = (end - start + 1) & 0xFFFFFF, secondSpan = (secondEnd - secondStart + 1) & 0xFFFFFF;
                    if (((firstSpan + secondSpan) & 0xFFFFFF) != (result->totalSectors & 0xFFFFFF)) goto done;
                } else if (secondEnd < secondStart || (uint64_t)result->layerBreakLba + secondEnd - secondStart + 1 != result->totalSectors) goto done;
            }
        }
    }
    wcscpy(result->mdsFilePath, mdsFilePath); if (result->dataFileCount) wcscpy(result->dataFilePath, result->files[0].path);
    memcpy(outMds, result, sizeof(*result)); ok = true;
done:
    free(result); free(data); fclose(file); return ok;
}

bool imgburn_mds_parse(const wchar_t* mdsFilePath, MdsSheet* outMds) { return mds_parse(mdsFilePath,outMds,false); }
bool imgburn_mds_parse_source(const wchar_t* mdsFilePath, MdsSheet* outMds) { return mds_parse(mdsFilePath,outMds,true); }

bool imgburn_mds_create(const wchar_t* mdsFilePath, const wchar_t* dataFileName, uint32_t totalSectors, uint32_t layerBreakLba) {
    const wchar_t* files[] = {dataFileName};
    return imgburn_mds_create_files(mdsFilePath, files, 1, totalSectors, layerBreakLba);
}

bool imgburn_mds_create_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba) {
    return imgburn_mds_create_files_ex(mdsFilePath, dataFiles, fileCount, totalSectors, layerBreakLba, true);
}

static bool mds_create_data(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath, bool sourceLayout, bool cd, uint16_t sectorSize, uint8_t cdMode) {
    if (!mdsFilePath || !dataFiles || !fileCount || fileCount > MAX_MDS_FILES || wcslen(mdsFilePath) >= MAX_PATH || !totalSectors || totalSectors > INT32_MAX) return false;
    if (layerBreakLba && (layerBreakLba >= totalSectors || (oppositeTrackPath && layerBreakLba < (totalSectors + 1) / 2) || layerBreakLba > 0xFD0000 || totalSectors - layerBreakLba > 0xFD0000)) return false;
    if (!sourceLayout && layerBreakLba && (layerBreakLba > 2086912 || totalSectors - layerBreakLba > 2086912 || layerBreakLba % 16)) return false;
    if (!layerBreakLba && totalSectors > 2295104) return false;
    wchar_t source[MAX_PATH], destination[MAX_PATH], temporary[MAX_PATH];
    DWORD length = GetFullPathNameW(mdsFilePath, MAX_PATH, destination, NULL);
    if (!length || length >= MAX_PATH) return false;
    uint64_t sourceBytes = 0; uint32_t structures = 192 + (uint32_t)fileCount * 16;
    for (size_t i = 0; i < fileCount; i++) {
        if (!dataFiles[i] || !dataFiles[i][0] || wcslen(dataFiles[i]) >= MAX_PATH || !mds_resolve(mdsFilePath, dataFiles[i], source) || !_wcsicmp(source, destination)) return false;
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(source, GetFileExInfoStandard, &attributes) || (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        uint64_t part = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
        if (!part || part % sectorSize || sourceBytes > UINT64_MAX - part) return false;
        sourceBytes += part; structures += (uint32_t)(wcslen(dataFiles[i]) + 1) * 2;
    }
    if (sourceBytes != (uint64_t)totalSectors * sectorSize) return false;
    structures = (structures + 3) & ~3u;
    uint32_t bytes = structures + (cd ? 8 : layerBreakLba ? 8200 : 4100);
    uint8_t* data = (uint8_t*)calloc(1, bytes);
    if (!data) return false;
    MdsHeader* header = (MdsHeader*)data;
    memcpy(header->signature, "MEDIA DESCRIPTOR", 16); header->version[0] = 1; header->version[1] = 3; header->mediumType = cd ? 0 : 0x10; header->sessionCount = 1; header->sessionsOffset = 88; header->discStructuresOffset = cd ? 0 : structures;
    MdsSessionBlock* session = (MdsSessionBlock*)(data + 88);
    session->sessionStartLba = cd ? -150 : 0; session->sessionEndLba = (int32_t)totalSectors; session->sessionNumber = 1; session->totalTracks = 1; session->leadInTracks = 3; session->firstTrack = 1; session->lastTrack = 1; session->tracksOffset = 112;
    MdsTrackBlock* track = (MdsTrackBlock*)(data + 112);
    track->trackMode = cd ? cdMode : 0x02; track->adrControl = 0x14; track->point = 1; track->time[5] = 2; track->extraOffset = cd ? structures : totalSectors; track->sectorSize = sectorSize; track->fileCount = (uint32_t)fileCount; track->footerOffset = 192;
    uint32_t nameOffset = 192 + (uint32_t)fileCount * 16;
    for (size_t i = 0; i < fileCount; i++) {
        uint32_t footer[4] = {nameOffset, 1, 0, 0}, nameBytes = (uint32_t)(wcslen(dataFiles[i]) + 1) * 2;
        memcpy(data + 192 + i * 16, footer, sizeof(footer)); memcpy(data + nameOffset, dataFiles[i], nameBytes); nameOffset += nameBytes;
    }
    if (cd) {
        uint32_t extra[2] = {150,totalSectors}; memcpy(data + structures,extra,sizeof(extra));
    } else {
        uint8_t* pfi = data + structures + 2052;
        /* 原版DVD-ROM描述使用120mm、10.08Mbps、压制层和对应记录密度。 */
        pfi[0] = 1; pfi[1] = 2; pfi[2] = layerBreakLba ? (oppositeTrackPath ? 0x31 : 0x21) : 1; pfi[3] = 0x10;
        mds_set_be32(pfi + 4, 0x30000);
        mds_set_be32(pfi + 8, layerBreakLba ? (oppositeTrackPath ? ((~(0x30000 + layerBreakLba - 1) + totalSectors - layerBreakLba - 1) & 0xFFFFFF) : 0x30000 + layerBreakLba - 1) : 0x30000 + totalSectors - 1);
        if (layerBreakLba) {
            if (oppositeTrackPath) mds_set_be32(pfi + 12, 0x30000 + layerBreakLba - 1);
            memcpy(data + structures + 4100, data + structures, 4100);
            if (!oppositeTrackPath) mds_set_be32(pfi + 4100 + 8, 0x30000 + totalSectors - layerBreakLba - 1);
        }
    }
    static volatile LONG sequence = 0;
    HANDLE output = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 16; i++) {
        if (swprintf(temporary, MAX_PATH, L"%ls.partial.%lu.%ld", destination, GetCurrentProcessId(), InterlockedIncrement(&sequence)) < 0) { free(data); return false; }
        output = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (output != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    bool ok = output != INVALID_HANDLE_VALUE;
    uint32_t offset = 0;
    while (ok && offset < bytes) { DWORD actual = 0; ok = WriteFile(output, data + offset, bytes - offset, &actual, NULL) && actual && actual <= bytes - offset; offset += actual; }
    if (ok) ok = FlushFileBuffers(output) != FALSE;
    if (output != INVALID_HANDLE_VALUE && !CloseHandle(output)) ok = false;
    if (ok) ok = MoveFileExW(temporary, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok && output != INVALID_HANDLE_VALUE) DeleteFileW(temporary);
    free(data); return ok;
}

/* BD的逻辑数据长度独立于虚拟DVD的24位PFI；不改变既有CD/DVD生成规则。 */
bool imgburn_mds_create_bd_read_layout_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t virtualL0, bool oppositeTrackPath) {
    if (!mdsFilePath || !mdsFilePath[0] || !dataFiles || !fileCount || fileCount > MAX_MDS_FILES || wcslen(mdsFilePath) >= MAX_PATH || !totalSectors || totalSectors > INT32_MAX) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    /* 原版虚拟缓存是原始DWORD提示；非零提示只接OTP，不据N或物理BD容量重算。 */
    if (virtualL0 && !oppositeTrackPath) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    wchar_t source[MAX_PATH], destination[MAX_PATH], temporary[MAX_PATH];
    DWORD length = GetFullPathNameW(mdsFilePath, MAX_PATH, destination, NULL);
    if (!length || length >= MAX_PATH) { SetLastError(ERROR_INVALID_NAME); return false; }
    uint64_t sourceBytes = 0;
    uint32_t footerOffset = virtualL0 ? 8392 : 4292;
    uint32_t bytes = footerOffset + (uint32_t)fileCount * 16;
    for (size_t i = 0; i < fileCount; i++) {
        if (!dataFiles[i] || !dataFiles[i][0] || wcslen(dataFiles[i]) >= MAX_PATH || !mds_resolve(mdsFilePath, dataFiles[i], source) || !_wcsicmp(source, destination)) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(source, GetFileExInfoStandard, &attributes)) return false;
        uint64_t part = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
        if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !part || part % 2048 || sourceBytes > UINT64_MAX - part) { SetLastError(ERROR_INVALID_DATA); return false; }
        sourceBytes += part; bytes += (uint32_t)(wcslen(dataFiles[i]) + 1) * 2;
    }
    if (sourceBytes != (uint64_t)totalSectors * 2048) { SetLastError(ERROR_INVALID_DATA); return false; }
    uint8_t* data = (uint8_t*)calloc(1, bytes);
    if (!data) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    MdsHeader* header = (MdsHeader*)data;
    memcpy(header->signature, "MEDIA DESCRIPTOR", 16); header->version[0] = 1; header->version[1] = 3; header->mediumType = 0x12; header->sessionCount = 1; header->dummy1[0] = 2; header->discStructuresOffset = 192; header->sessionsOffset = 88;
    MdsSessionBlock* session = (MdsSessionBlock*)(data + 88);
    session->sessionEndLba = (int32_t)totalSectors; session->sessionNumber = 1; session->totalTracks = 1; session->leadInTracks = 3; session->firstTrack = 1; session->lastTrack = 1; session->tracksOffset = 112;
    MdsTrackBlock* track = (MdsTrackBlock*)(data + 112);
    track->trackMode = 0x02; track->adrControl = 0x14; track->point = 1; track->extraOffset = totalSectors; track->sectorSize = 2048; track->fileCount = (uint32_t)fileCount; track->footerOffset = footerOffset;
    uint8_t* pfi = data + 192 + 2052;
    pfi[0] = 0x21; pfi[1] = 2; pfi[2] = (uint8_t)(1 | (virtualL0 ? 0x20 : 0) | (oppositeTrackPath ? 0x10 : 0)); pfi[3] = 0x10;
    mds_set_be32(pfi + 4, 0x30000);
    mds_set_be32(pfi + 8, (virtualL0 ? ((0x30000 + virtualL0) ^ 0xFFFFFF) + (totalSectors - virtualL0) : 0x30000 + totalSectors - 1) & 0xFFFFFF);
    /* 零提示仍可带OTP位；原版此时只有一份结构，但end8与end12都保存逻辑末端低24位。 */
    if (oppositeTrackPath) mds_set_be32(pfi + 12, (0x30000 + (virtualL0 ? virtualL0 : totalSectors) - 1) & 0xFFFFFF);
    if (virtualL0) {
        /* 原版OTP重复同一4100字节结构；它不是BD第二物理层的PIC。 */
        memcpy(data + 4292, data + 192, 4100);
    }
    uint32_t nameOffset = footerOffset + (uint32_t)fileCount * 16;
    for (size_t i = 0; i < fileCount; i++) {
        uint32_t footer[4] = {nameOffset, 1, 0, 0}, nameBytes = (uint32_t)(wcslen(dataFiles[i]) + 1) * 2;
        memcpy(data + footerOffset + i * 16, footer, sizeof(footer)); memcpy(data + nameOffset, dataFiles[i], nameBytes); nameOffset += nameBytes;
    }
    static volatile LONG sequence = 0;
    HANDLE output = INVALID_HANDLE_VALUE;
    DWORD error = ERROR_SUCCESS;
    for (int i = 0; i < 16; i++) {
        if (swprintf(temporary, MAX_PATH, L"%ls.partial.%lu.%ld", destination, GetCurrentProcessId(), InterlockedIncrement(&sequence)) < 0) { error = ERROR_FILENAME_EXCED_RANGE; break; }
        output = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (output != INVALID_HANDLE_VALUE) break;
        error = GetLastError(); if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
    }
    bool ok = output != INVALID_HANDLE_VALUE;
    uint32_t offset = 0;
    while (ok && offset < bytes) {
        DWORD actual = 0;
        if (!WriteFile(output, data + offset, bytes - offset, &actual, NULL)) { error = GetLastError(); ok = false; }
        else if (!actual || actual > bytes - offset) { error = ERROR_WRITE_FAULT; ok = false; }
        else offset += actual;
    }
    if (ok && !FlushFileBuffers(output)) { error = GetLastError(); ok = false; }
    if (output != INVALID_HANDLE_VALUE && !CloseHandle(output) && ok) { error = GetLastError(); ok = false; }
    if (ok && !MoveFileExW(temporary, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { error = GetLastError(); ok = false; }
    if (!ok && output != INVALID_HANDLE_VALUE) DeleteFileW(temporary);
    free(data); SetLastError(ok ? ERROR_SUCCESS : error); return ok;
}

bool imgburn_mds_create_bd_read_files_ex(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t virtualL0) {
    return imgburn_mds_create_bd_read_layout_files(mdsFilePath,dataFiles,fileCount,totalSectors,virtualL0,virtualL0 != 0);
}

bool imgburn_mds_create_bd_read_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors) {
    return imgburn_mds_create_bd_read_files_ex(mdsFilePath,dataFiles,fileCount,totalSectors,0);
}

static bool mds_tools_equal(const wchar_t* first, size_t firstLength, const wchar_t* second, size_t secondLength) {
    if (!firstLength || !secondLength) return firstLength == secondLength;
    return CompareStringW(LOCALE_USER_DEFAULT,NORM_IGNORECASE,first,(int)firstLength,second,(int)secondLength) == CSTR_EQUAL;
}

/* LST先独立识别原始首行，未匹配的二进制文件仍能沿原版回到raw。 */
static bool mds_tools_lst_header(const uint8_t* raw, DWORD bytes) {
    bool wide = bytes >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF));
    DWORD skip = wide ? 2 : bytes >= 3 && !memcmp(raw,"\xEF\xBB\xBF",3) ? 3 : 0, end = skip;
    if (wide) {
        while (end + 1 < bytes) {
            uint16_t value = raw[0] == 0xFF ? (uint16_t)(raw[end] | (uint16_t)raw[end + 1] << 8) : (uint16_t)((uint16_t)raw[end] << 8 | raw[end + 1]);
            if (!value || value == 13 || value == 10) break;
            end += 2;
        }
    } else while (end < bytes && raw[end] && raw[end] != 13 && raw[end] != 10) end++;
    UINT page = skip == 3 ? CP_UTF8 : CP_ACP; DWORD flags = skip == 3 ? MB_ERR_INVALID_CHARS : 0;
    int count = wide ? (int)((end - skip) / 2) : end > skip ? MultiByteToWideChar(page,flags,(const char*)raw + skip,(int)(end - skip),NULL,0) : 0;
    if (!count) { SetLastError(ERROR_BAD_FORMAT); return false; }
    wchar_t* header = calloc((size_t)count + 1,sizeof(wchar_t));
    if (!header) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    bool decoded = true;
    if (wide) for (int i = 0; i < count; i++) header[i] = raw[0] == 0xFF ? (wchar_t)(raw[2 + i * 2] | (uint16_t)raw[3 + i * 2] << 8) : (wchar_t)((uint16_t)raw[2 + i * 2] << 8 | raw[3 + i * 2]);
    else decoded = MultiByteToWideChar(page,flags,(const char*)raw + skip,(int)(end - skip),header,count) != 0;
    bool matched = decoded && CompareStringW(LOCALE_USER_DEFAULT,NORM_IGNORECASE,header,count,L"IMAGELIST",9) == CSTR_EQUAL;
    free(header); SetLastError(matched ? ERROR_SUCCESS : ERROR_BAD_FORMAT); return matched;
}

/* Tools文本沿原版BOM/系统ANSI选择，不复用CUE的无BOM UTF-8优先规则。 */
static wchar_t* mds_tools_text(const wchar_t* path, bool list) {
    HANDLE file = CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size; DWORD error = ERROR_SUCCESS, got = 0; uint8_t* raw = NULL; wchar_t* text = NULL;
    if (!GetFileSizeEx(file,&size)) { error = GetLastError(); goto done; }
    if (list && (size.QuadPart < 10 || size.QuadPart > 1024 * 1024)) { error = ERROR_BAD_FORMAT; goto done; }
    if (size.QuadPart <= 0) { error = ERROR_INVALID_DATA; goto done; }
    if (size.QuadPart > 4 * 1024 * 1024) { error = ERROR_NOT_SUPPORTED; goto done; }
    raw = malloc((size_t)size.QuadPart);
    if (!raw) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if (!ReadFile(file,raw,(DWORD)size.QuadPart,&got,NULL)) { error = GetLastError(); goto done; }
    if (got != (DWORD)size.QuadPart) { error = ERROR_READ_FAULT; goto done; }
    if (list && !mds_tools_lst_header(raw,got)) { error = GetLastError(); goto done; }
    int count = 0;
    if (got >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF))) {
        if ((got - 2) % 2) { error = ERROR_INVALID_DATA; goto done; }
        count = (int)((got - 2) / 2); text = calloc((size_t)count + 1,sizeof(wchar_t));
        if (!text) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
        for (int i = 0; i < count; i++) text[i] = raw[0] == 0xFF ? (wchar_t)(raw[2 + i * 2] | (uint16_t)raw[3 + i * 2] << 8) : (wchar_t)((uint16_t)raw[2 + i * 2] << 8 | raw[3 + i * 2]);
    } else {
        int skip = got >= 3 && !memcmp(raw,"\xEF\xBB\xBF",3) ? 3 : 0;
        UINT page = skip ? CP_UTF8 : CP_ACP; DWORD flags = skip ? MB_ERR_INVALID_CHARS : 0;
        count = MultiByteToWideChar(page,flags,(const char*)raw + skip,(int)got - skip,NULL,0);
        if (!count) { error = ERROR_INVALID_DATA; goto done; }
        text = calloc((size_t)count + 1,sizeof(wchar_t));
        if (!text) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
        if (!MultiByteToWideChar(page,flags,(const char*)raw + skip,(int)got - skip,text,count)) { error = ERROR_INVALID_DATA; goto done; }
    }
    for (int i = 0; i < count; i++) if (!text[i]) { error = ERROR_INVALID_DATA; goto done; }
    /* 本批拒绝损坏Unicode，不宣称复制原版替换无效字符的全部容错域。 */
    if (count && !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,count,NULL,0,NULL,NULL)) error = ERROR_INVALID_DATA;
done:
    CloseHandle(file); free(raw);
    if (error) { free(text); text = NULL; }
    SetLastError(error); return text;
}

static wchar_t* mds_tools_trim(wchar_t* text) {
    while (*text && *text <= 0x20) text++;
    wchar_t* end = text + wcslen(text); while (end > text && end[-1] <= 0x20) *--end = 0;
    return text;
}

/* Delphi正int64才更新低DWORD；十六进制负号按64位补码取负。 */
static bool mds_tools_layer_hint(const wchar_t* text, uint32_t* hint) {
    bool negative = *text == L'-'; if (*text == L'-' || *text == L'+') text++;
    unsigned base = 10;
    if (*text == L'$' || *text == L'x' || *text == L'X') { base = 16; text++; }
    else if (text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) { base = 16; text += 2; }
    if (!*text) return false;
    uint64_t value = 0, limit = base == 16 ? UINT64_MAX : (uint64_t)INT64_MAX + (negative ? 1 : 0);
    for (; *text; text++) {
        unsigned digit = *text >= L'0' && *text <= L'9' ? (unsigned)(*text - L'0') : *text >= L'a' && *text <= L'f' ? (unsigned)(*text - L'a' + 10) : *text >= L'A' && *text <= L'F' ? (unsigned)(*text - L'A' + 10) : 16;
        if (digit >= base || value > (limit - digit) / base) return false;
        value = value * base + digit;
    }
    if (negative) value = (uint64_t)0 - value;
    if (!value || value > INT64_MAX) return false;
    *hint = (uint32_t)value; return true;
}

static unsigned mds_tools_dvd_line_count(const wchar_t* text) {
    unsigned count = 0;
    while (*text) {
        const wchar_t* start = text; while (*text && *text != L'\r' && *text != L'\n') text++;
        while (start < text && *start <= 0x20) start++;
        if (start < text) count++;
        if (*text) { wchar_t ending = *text++; if (ending == L'\r' && *text == L'\n') text++; }
    }
    return count;
}

/* 原版ChangeExt的保留大小写模式，针对最后基本名的扩展名。 */
static bool mds_tools_dvd_change_ext(const wchar_t* name, const wchar_t* extension, wchar_t* result) {
    const wchar_t* old = wcsrchr(name,L'.'); size_t stem = old ? (size_t)(old - name) : wcslen(name);
    if (!old) old = L"";
    size_t oldLength = wcslen(old), newLength = wcslen(extension);
    if (oldLength >= MAX_PATH || stem + newLength >= MAX_PATH) return false;
    wchar_t lower[MAX_PATH], upper[MAX_PATH], changed[MAX_PATH];
    wcscpy(lower,old); wcscpy(upper,old); wcscpy(changed,extension);
    if (oldLength) { CharLowerBuffW(lower,(DWORD)oldLength); CharUpperBuffW(upper,(DWORD)oldLength); }
    if (newLength && (!oldLength || CompareStringW(LOCALE_USER_DEFAULT,0,old,(int)oldLength,lower,(int)oldLength) == CSTR_EQUAL)) CharLowerBuffW(changed,(DWORD)newLength);
    else if (newLength && CompareStringW(LOCALE_USER_DEFAULT,0,old,(int)oldLength,upper,(int)oldLength) == CSTR_EQUAL) CharUpperBuffW(changed,(DWORD)newLength);
    else if (newLength) {
        CharLowerBuffW(changed,(DWORD)newLength); bool makeUpper = false;
        for (size_t i = 0; i < newLength; i++) {
            if (i < oldLength) makeUpper = old[i] == upper[i];
            if (makeUpper) CharUpperBuffW(changed + i,1);
        }
    }
    wmemcpy(result,name,stem); wcscpy(result + stem,changed); return true;
}

static bool mds_tools_dvd_candidate(const wchar_t* directory, const wchar_t* name, wchar_t* path, WIN32_FILE_ATTRIBUTE_DATA* attributes) {
    wchar_t combined[MAX_PATH], full[MAX_PATH]; WIN32_FILE_ATTRIBUTE_DATA found;
    if (swprintf(combined,MAX_PATH,L"%ls%ls",directory,name) < 0) return false;
    DWORD length = GetFullPathNameW(combined,MAX_PATH,full,NULL);
    if (!length || length >= MAX_PATH || !GetFileAttributesExW(full,GetFileExInfoStandard,&found) || (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    wcscpy(path,full); *attributes = found; return true;
}

/* 仅自动试原记录目录的三个序列，不猜目录搜索或交互替代文件。 */
static bool mds_tools_dvd_recover(const wchar_t* descriptor, const wchar_t* line, wchar_t* path, WIN32_FILE_ATTRIBUTE_DATA* attributes) {
    wchar_t directory[MAX_PATH], description[MAX_PATH], candidate[MAX_PATH], appended[MAX_PATH];
    wcscpy(directory,path); wchar_t* slash = wcsrchr(directory,L'\\'); if (!slash) return false; slash[1] = 0;
    DWORD length = GetFullPathNameW(descriptor,MAX_PATH,description,NULL); if (!length || length >= MAX_PATH) return false;
    const wchar_t* descriptionName = wcsrchr(description,L'\\'); if (!descriptionName) return false; descriptionName++;
    const wchar_t* lineName = line; for (const wchar_t* p = line; *p; p++) if (*p == L'\\' || *p == L':') lineName = p + 1;
    const wchar_t* extension = wcsrchr(lineName,L'.'); if (!extension) extension = L"";
    if (mds_tools_dvd_change_ext(descriptionName,extension,candidate) && mds_tools_dvd_candidate(directory,candidate,path,attributes)) return true;
    static const wchar_t* extensions[] = {L".ISO",L".IMG",L".000",L".001"};
    for (unsigned i = 0; i < ARRAYSIZE(extensions); i++) {
        if (!mds_tools_dvd_change_ext(lineName,extensions[i],candidate)) continue;
        if (CompareStringW(LOCALE_USER_DEFAULT,NORM_IGNORECASE,lineName,-1,candidate,-1) != CSTR_EQUAL && mds_tools_dvd_candidate(directory,candidate,path,attributes)) return true;
    }
    if (swprintf(appended,MAX_PATH,L"%ls.",lineName) < 0) return false;
    for (unsigned i = 0; i < ARRAYSIZE(extensions); i++) if (mds_tools_dvd_change_ext(appended,extensions[i],candidate) && mds_tools_dvd_candidate(directory,candidate,path,attributes)) return true;
    return false;
}

bool imgburn_mds_load_tools_dvd(const wchar_t* descriptionPath, MdsToolsInput* outInput) {
    if (outInput) memset(outInput,0,sizeof(*outInput));
    if (!descriptionPath || !descriptionPath[0] || wcslen(descriptionPath) >= MAX_PATH || !outInput) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    wchar_t* text = mds_tools_text(descriptionPath,false); if (!text) return false;
    MdsToolsInput* parsed = calloc(1,sizeof(*parsed)); DWORD error = ERROR_INVALID_DATA; uint64_t total = 0; bool ok = false;
    unsigned lineCount = mds_tools_dvd_line_count(text), metadataCount = 0;
    if (!parsed) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    for (wchar_t* cursor = text; *cursor;) {
        wchar_t* line = cursor; while (*cursor && *cursor != L'\r' && *cursor != L'\n') cursor++;
        if (*cursor) { wchar_t ending = *cursor; *cursor++ = 0; if (ending == L'\r' && *cursor == L'\n') cursor++; }
        line = mds_tools_trim(line); if (!*line) continue;
        wchar_t path[MAX_PATH] = {0}; DWORD fileError = ERROR_INVALID_NAME; WIN32_FILE_ATTRIBUTE_DATA attributes = {0}; bool resolved = false;
        /* 原版简单文件名按描述目录，带目录相对路径按当前目录；本批不猜后缀恢复。 */
        if (wcschr(line,L'/') || (line[0] && line[1] == L':' && line[2] != L'\\')) fileError = ERROR_NOT_SUPPORTED;
        else if (wcschr(line,L'\\') || wcschr(line,L':')) { DWORD length = GetFullPathNameW(line,MAX_PATH,path,NULL); resolved = length && length < MAX_PATH; }
        else resolved = mds_resolve(descriptionPath,line,path);
        bool exists = false;
        if (resolved) {
            if (GetFileAttributesExW(path,GetFileExInfoStandard,&attributes)) { exists = !(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY); if (!exists) fileError = ERROR_INVALID_DATA; }
            else fileError = GetLastError();
        }
        /* 元信息只计已经处理的行，后面的key=value不能提前放宽此门槛。 */
        if (!exists && resolved && (lineCount == 1 || lineCount == metadataCount + 1)) exists = mds_tools_dvd_recover(descriptionPath,line,path,&attributes);
        if (exists) {
            uint64_t bytes = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
            if (!bytes || bytes % 2048) { error = ERROR_INVALID_DATA; goto done; }
            if (parsed->fileCount == MAX_MDS_FILES || bytes / 2048 > (uint64_t)INT32_MAX - total) { error = ERROR_NOT_SUPPORTED; goto done; }
            MdsDataFile* part = &parsed->files[parsed->fileCount++]; wcscpy(part->path,path); part->bytes = bytes; total += bytes / 2048; continue;
        }
        wchar_t* equals = wcschr(line,L'=');
        if (!equals) { error = fileError; goto done; }
        *equals++ = 0; wchar_t* key = mds_tools_trim(line); wchar_t* value = mds_tools_trim(equals);
        if (CompareStringW(LOCALE_USER_DEFAULT,NORM_IGNORECASE,key,(int)wcslen(key),L"LayerBreak",10) == CSTR_EQUAL) mds_tools_layer_hint(value,&parsed->layerBreakLba);
        metadataCount++;
    }
    if (!parsed->fileCount) goto done;
    parsed->totalSectors = (uint32_t)total; *outInput = *parsed; ok = true; error = ERROR_SUCCESS;
done:
    free(parsed); free(text); SetLastError(error); return ok;
}

bool imgburn_mds_load_tools_lst(const wchar_t* descriptionPath, MdsToolsInput* outInput) {
    if (outInput) memset(outInput,0,sizeof(*outInput));
    if (!descriptionPath || !descriptionPath[0] || wcslen(descriptionPath) >= MAX_PATH || !outInput) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    wchar_t* text = mds_tools_text(descriptionPath,true); if (!text) return false;
    MdsToolsInput* parsed = calloc(1,sizeof(*parsed)); DWORD error = ERROR_INVALID_DATA; uint64_t total = 0; bool ok = false;
    if (!parsed) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    wchar_t* cursor = text; while (*cursor && *cursor != L'\r' && *cursor != L'\n') cursor++;
    if (*cursor) { wchar_t ending = *cursor++; if (ending == L'\r' && *cursor == L'\n') cursor++; }
    unsigned fileCount = mds_tools_dvd_line_count(cursor);
    if (!fileCount) goto done;
    if (fileCount > MAX_MDS_FILES) { error = ERROR_NOT_SUPPORTED; goto done; }
    wchar_t description[MAX_PATH]; DWORD length = GetFullPathNameW(descriptionPath,MAX_PATH,description,NULL);
    if (!length || length >= MAX_PATH) { error = ERROR_INVALID_NAME; goto done; }
    while (*cursor) {
        wchar_t* line = cursor; while (*cursor && *cursor != L'\r' && *cursor != L'\n') cursor++;
        if (*cursor) { wchar_t ending = *cursor; *cursor++ = 0; if (ending == L'\r' && *cursor == L'\n') cursor++; }
        line = mds_tools_trim(line); if (!*line) continue;
        if (wcschr(line,L'/') || (line[0] == L'\\' && line[1] != L'\\') || (line[0] && line[1] == L':' && line[2] != L'\\')) { error = ERROR_NOT_SUPPORTED; goto done; }
        wchar_t path[MAX_PATH]; WIN32_FILE_ATTRIBUTE_DATA attributes = {0};
        if (!mds_resolve(description,line,path)) { error = ERROR_INVALID_NAME; goto done; }
        bool exists = GetFileAttributesExW(path,GetFileExInfoStandard,&attributes) != 0;
        DWORD fileError = exists ? ERROR_INVALID_DATA : GetLastError(); exists = exists && !(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (!exists && fileCount == 1) {
            const wchar_t* name = line; for (const wchar_t* p = line; *p; p++) if (*p == L'\\' || *p == L':') name = p + 1;
            const wchar_t* extension = wcsrchr(name,L'.'); if (!extension) extension = L"";
            const wchar_t* descriptionName = wcsrchr(description,L'\\'); wchar_t changed[MAX_PATH], candidate[MAX_PATH];
            if (descriptionName && mds_tools_dvd_change_ext(descriptionName + 1,extension,changed) && mds_resolve(description,changed,candidate) && !mds_tools_equal(description,wcslen(description),candidate,wcslen(candidate))) {
                WIN32_FILE_ATTRIBUTE_DATA found = {0};
                if (GetFileAttributesExW(candidate,GetFileExInfoStandard,&found) && !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { wcscpy(path,candidate); attributes = found; exists = true; }
            }
        }
        if (!exists) { error = fileError; goto done; }
        uint64_t bytes = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
        if (!bytes || bytes % 2048) { error = ERROR_INVALID_DATA; goto done; }
        if (bytes / 2048 > (uint64_t)INT32_MAX - total) { error = ERROR_NOT_SUPPORTED; goto done; }
        MdsDataFile* part = &parsed->files[parsed->fileCount++]; wcscpy(part->path,path); part->bytes = bytes; total += bytes / 2048;
    }
    parsed->totalSectors = (uint32_t)total; *outInput = *parsed; ok = true; error = ERROR_SUCCESS;
done:
    free(parsed); free(text); SetLastError(error); return ok;
}

/* 原版18种卷标记仅用于排除先行候选；本批带头输入只接已确认的CD001/2048偏移。 */
static bool mds_tools_volume_marker(const uint8_t* sector) {
    static const struct { const char* text; uint8_t offset, bytes; } markers[] = {
        {"BEA01",1,5},{"BOOT2",1,5},{"CD001",1,5},{"CDW02",1,5},{"CD-I",1,4},
        {"FAT16",54,5},{"FAT32",82,5},{"MICROSOFT*XBOX*MEDIA",0,20},
        {"NSR01",1,5},{"NSR02",1,5},{"NSR03",1,5},{"NSR04",1,5},{"NSR05",1,5},
        {"NSR06",1,5},{"NSR07",1,5},{"NSR08",1,5},{"NSR09",1,5},{"TEA01",1,5}
    };
    for (unsigned i = 0; i < ARRAYSIZE(markers); i++) if (!memcmp(sector + markers[i].offset,markers[i].text,markers[i].bytes)) return true;
    return false;
}

static bool mds_tools_read_at(HANDLE file, uint64_t offset, void* data, DWORD bytes) {
    LARGE_INTEGER position; position.QuadPart = (LONGLONG)offset; DWORD got = 0;
    if (!SetFilePointerEx(file,position,NULL,FILE_BEGIN) || !ReadFile(file,data,bytes,&got,NULL)) return false;
    if (got != bytes) { SetLastError(ERROR_READ_FAULT); return false; }
    return true;
}

/* 首卷须独立容纳完整标记扇区；PDI允许标记之后出现非整分卷边界。 */
static bool mds_tools_headed_file(HANDLE file, const wchar_t* path, uint64_t bytes, uint32_t offset, bool splitSectors, MdsToolsInput* input) {
    if (bytes <= offset || (!splitSectors && (bytes - offset) % 2048)) { SetLastError(ERROR_INVALID_DATA); return false; }
    uint64_t sectors = (bytes - offset) / 2048;
    if (sectors > INT32_MAX || sectors < 17) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    static const uint16_t sizes[] = {2048,2054,2064,2332,2336,2352,2448};
    static const uint8_t windows[][3] = {{0,0,0},{6,0,0},{12,0,0},{0,8,0},{0,8,0},{0,16,24},{0,16,24}};
    static const uint8_t counts[] = {1,1,1,2,2,3,3}; uint8_t sector[2448] = {0};
    for (unsigned i = 0; i < ARRAYSIZE(sizes); i++) {
        uint64_t start = (uint64_t)16 * sizes[i]; if (start + sizes[i] > bytes) continue;
        if (!mds_tools_read_at(file,start,sector,sizes[i])) return false;
        for (unsigned j = 0; j < counts[i]; j++) if (mds_tools_volume_marker(sector + windows[i][j])) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    }
    if (!mds_tools_read_at(file,(uint64_t)offset + 16 * 2048,sector,2048)) return false;
    if (memcmp(sector + 1,"CD001",5)) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    DWORD length = GetFullPathNameW(path,MAX_PATH,input->files[0].path,NULL);
    if (!length || length >= MAX_PATH) { SetLastError(ERROR_INVALID_NAME); return false; }
    input->files[0].bytes = bytes; input->fileCount = 1; input->totalSectors = (uint32_t)sectors; input->startByteOffset = offset;
    SetLastError(ERROR_SUCCESS); return true;
}

bool imgburn_mds_load_tools_gi(const wchar_t* descriptionPath, MdsToolsInput* outInput) {
    if (outInput) memset(outInput,0,sizeof(*outInput));
    if (!descriptionPath || !descriptionPath[0] || wcslen(descriptionPath) >= MAX_PATH || !outInput) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    HANDLE file = CreateFileW(descriptionPath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    uint8_t probe[0xB0] = {0}, *raw = NULL; MdsToolsInput* parsed = NULL;
    LARGE_INTEGER size = {0}, beginning = {0}; DWORD error = ERROR_INVALID_DATA, got = 0; bool ok = false;
    if (!GetFileSizeEx(file,&size)) { error = GetLastError(); goto done; }
    if (size.QuadPart < (LONGLONG)sizeof(probe)) { error = ERROR_BAD_FORMAT; goto done; }
    if (!ReadFile(file,probe,sizeof(probe),&got,NULL)) { error = GetLastError(); goto done; }
    if (got != sizeof(probe)) { error = ERROR_READ_FAULT; goto done; }
    unsigned position = probe[0x28] + 0x28; if (position + 4 > sizeof(probe)) position = 0x62;
    uint32_t type = cue_le32(probe + position);
    /* 原版probe只检查这两个签名、两个字节标记和22/88类型，邻接保留字节不加限制。 */
    if (memcmp(probe,"\xDA\xDA\xFE\xFE",4) || cue_le32(probe + 0x14) != 0x11111111 || probe[0x44] != 1 || probe[0x48] != 1 || (type != 0x22222222 && type != 0x88888888)) { error = ERROR_BAD_FORMAT; goto done; }
    DWORD bytes = size.QuadPart > 1024 * 1024 ? 1024 * 1024 : (DWORD)size.QuadPart;
    raw = malloc(bytes); if (!raw) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if (!SetFilePointerEx(file,beginning,NULL,FILE_BEGIN) || !ReadFile(file,raw,bytes,&got,NULL)) { error = GetLastError(); goto done; }
    if (got != bytes) { error = ERROR_READ_FAULT; goto done; }
    /* loader按实际有界读取长度重新选位置，不能复用probe固定B0的回退结果。 */
    position = raw[0x28] + 0x28; if (position + 4 > bytes) position = 0x62;
    if (cue_le32(raw + position) == 0x22222222) {
        parsed = calloc(1,sizeof(*parsed)); if (!parsed) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
        ok = mds_tools_headed_file(file,descriptionPath,(uint64_t)size.QuadPart,0x9C,false,parsed);
        error = GetLastError(); if (ok) *outInput = *parsed; goto done;
    }
    if (cue_le32(raw + position) != 0x88888888) { error = ERROR_NOT_SUPPORTED; goto done; }
    if (position + 0x36 >= bytes) goto done;
    unsigned count = raw[position + 0x36], table = position + 0x4E;
    if (!count || (uint64_t)table + (uint64_t)count * 260 > bytes) goto done;
    parsed = calloc(1,sizeof(*parsed)); if (!parsed) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    for (unsigned i = 0; i < count; i++) {
        const char* slot = (const char*)raw + table + i * 260; const char* end = memchr(slot,0,260);
        if (!end) goto done;
        if (end == slot) continue;
        wchar_t stored[MAX_PATH]; int length = MultiByteToWideChar(CP_ACP,0,slot,(int)(end - slot),stored,MAX_PATH - 1);
        if (!length) goto done;
        stored[length] = 0;
        /* 原版先剥反斜杠和冒号之前的目录，不Trim；空尾名直接跳过。 */
        const wchar_t* name = stored; for (const wchar_t* p = stored; *p; p++) if (*p == L'\\' || *p == L':') name = p + 1;
        if (!*name) continue;
        if (wcschr(name,L'/')) { error = ERROR_NOT_SUPPORTED; goto done; }
        wcscpy(parsed->files[parsed->fileCount++].path,name);
    }
    if (!parsed->fileCount) goto done;
    wchar_t description[MAX_PATH]; DWORD length = GetFullPathNameW(descriptionPath,MAX_PATH,description,NULL);
    if (!length || length >= MAX_PATH) { error = ERROR_INVALID_NAME; goto done; }
    uint64_t total = 0;
    for (unsigned i = 0; i < parsed->fileCount; i++) {
        MdsDataFile* part = &parsed->files[i]; wchar_t path[MAX_PATH]; WIN32_FILE_ATTRIBUTE_DATA attributes = {0};
        if (!mds_resolve(description,part->path,path)) { error = ERROR_INVALID_NAME; goto done; }
        bool exists = GetFileAttributesExW(path,GetFileExInfoStandard,&attributes) != 0;
        DWORD fileError = exists ? ERROR_INVALID_DATA : GetLastError(); exists = exists && !(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (!exists && parsed->fileCount == 1) {
            const wchar_t* extension = wcsrchr(part->path,L'.'); if (!extension) extension = L"";
            const wchar_t* descriptionName = wcsrchr(description,L'\\'); wchar_t changed[MAX_PATH], candidate[MAX_PATH];
            if (descriptionName && mds_tools_dvd_change_ext(descriptionName + 1,extension,changed) && mds_resolve(description,changed,candidate) && !mds_tools_equal(description,wcslen(description),candidate,wcslen(candidate))) {
                WIN32_FILE_ATTRIBUTE_DATA found = {0};
                if (GetFileAttributesExW(candidate,GetFileExInfoStandard,&found) && !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { wcscpy(path,candidate); attributes = found; exists = true; }
            }
        }
        if (!exists) { error = fileError; goto done; }
        uint64_t partBytes = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
        if (!partBytes || partBytes % 2048) goto done;
        if (partBytes / 2048 > (uint64_t)INT32_MAX - total) { error = ERROR_NOT_SUPPORTED; goto done; }
        wcscpy(part->path,path); part->bytes = partBytes; total += partBytes / 2048;
    }
    parsed->totalSectors = (uint32_t)total; *outInput = *parsed; error = ERROR_SUCCESS; ok = true;
done:
    CloseHandle(file); free(parsed); free(raw); SetLastError(error); return ok;
}

bool imgburn_mds_load_tools_pdi(const wchar_t* descriptionPath, MdsToolsInput* outInput) {
    if (outInput) memset(outInput,0,sizeof(*outInput));
    if (!descriptionPath || !descriptionPath[0] || wcslen(descriptionPath) >= MAX_PATH || !outInput) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    HANDLE file = CreateFileW(descriptionPath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {0}, beginning = {0}; uint8_t header[18] = {0}; DWORD got = 0, error = ERROR_INVALID_DATA;
    MdsToolsInput* parsed = NULL; bool ok = false;
    if (!GetFileSizeEx(file,&size)) { error = GetLastError(); goto done; }
    if (size.QuadPart < (LONGLONG)sizeof(header)) { error = ERROR_BAD_FORMAT; goto done; }
    if (!ReadFile(file,header,sizeof(header),&got,NULL)) { error = GetLastError(); goto done; }
    if (got != sizeof(header)) { error = ERROR_READ_FAULT; goto done; }
    if (memcmp(header,"TTAFMPVOBCDIMG",14)) { error = ERROR_BAD_FORMAT; goto done; }
    if (!SetFilePointerEx(file,beginning,NULL,FILE_BEGIN) || !ReadFile(file,header,sizeof(header),&got,NULL)) { error = GetLastError(); goto done; }
    if (got != sizeof(header)) { error = ERROR_READ_FAULT; goto done; }
    unsigned count = (unsigned)header[16] | (unsigned)header[17] << 8;
    if (!count) goto done;
    if (count > MAX_MDS_FILES) { error = ERROR_NOT_SUPPORTED; goto done; }
    parsed = calloc(1,sizeof(*parsed)); if (!parsed) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if (!mds_tools_headed_file(file,descriptionPath,(uint64_t)size.QuadPart,0x130,true,parsed)) { error = GetLastError(); goto done; }
    uint32_t sectors, carry;
    if (!mds_part_geometry(parsed->files[0].bytes,parsed->startByteOffset,0,count == 1,true,&sectors,&carry)) { error = GetLastError(); goto done; }
    parsed->totalSectors = sectors; parsed->splitSectors = true;
    for (unsigned i = 1; i < count; i++) {
        wchar_t path[MAX_PATH]; WIN32_FILE_ATTRIBUTE_DATA attributes = {0};
        /* 原版在完整扩展名后直接附至少两位序号；后卷没有新的304字节偏移。 */
        if (swprintf(path,MAX_PATH,L"%ls%02u",parsed->files[0].path,i) < 0) { error = ERROR_INVALID_NAME; goto done; }
        if (!GetFileAttributesExW(path,GetFileExInfoStandard,&attributes)) { error = GetLastError(); goto done; }
        uint64_t bytes = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
        if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !bytes) { error = ERROR_INVALID_DATA; goto done; }
        if (!mds_part_geometry(bytes,0,carry,i + 1 == count,true,&sectors,&carry)) { error = GetLastError(); goto done; }
        if (sectors > (uint32_t)INT32_MAX - parsed->totalSectors) { error = ERROR_NOT_SUPPORTED; goto done; }
        MdsDataFile* part = &parsed->files[parsed->fileCount++]; wcscpy(part->path,path); part->bytes = bytes;
        parsed->totalSectors += sectors;
    }
    *outInput = *parsed; error = ERROR_SUCCESS; ok = true;
done:
    CloseHandle(file); free(parsed); SetLastError(error); return ok;
}

/* Tools直接描述逻辑文件列表；它与Read的虚拟布局及普通DVD刻录生成规则分别保留。 */
static bool mds_create_tools_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath, MdsToolsMedium medium, bool preserveFullPathnames, bool explicitLayerBreak, bool ansiFilenames, uint64_t startByteOffset, bool splitSectors) {
    if (!mdsFilePath || !mdsFilePath[0] || !dataFiles || !fileCount || fileCount > MAX_MDS_FILES || wcslen(mdsFilePath) >= MAX_PATH || !totalSectors || totalSectors > INT32_MAX || (unsigned)medium > MDS_TOOLS_DVD_PLUS_RW) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    /* Tools显式层位置须在逻辑长度内并按16对齐；不套用新刻录DVD的容量或half限制。 */
    if (layerBreakLba && (layerBreakLba >= totalSectors || (explicitLayerBreak && layerBreakLba % 16))) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    if (!layerBreakLba) oppositeTrackPath = false;
    wchar_t source[MAX_PATH], destination[MAX_PATH], temporary[MAX_PATH];
    DWORD length = GetFullPathNameW(mdsFilePath,MAX_PATH,destination,NULL);
    if (!length || length >= MAX_PATH) { SetLastError(ERROR_INVALID_NAME); return false; }
    const wchar_t* destinationName = destination;
    for (const wchar_t* p = destination; *p; p++) if (*p == L'\\' || *p == L'/') destinationName = p + 1;
    const wchar_t* destinationDot = wcsrchr(destinationName,L'.');
    size_t destinationStem = destinationDot ? (size_t)(destinationDot - destinationName) : wcslen(destinationName);
    size_t destinationDirectory = (size_t)(destinationName - destination);
    wchar_t (*names)[MAX_PATH] = (wchar_t (*)[MAX_PATH])calloc(fileCount,sizeof(*names));
    if (!names) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    uint32_t parts[MAX_MDS_FILES], carries[MAX_MDS_FILES], incomingCarry = 0, footerOffset = layerBreakLba ? 8392 : 4292;
    uint32_t bytes = footerOffset + (uint32_t)fileCount * 16;
    uint64_t sourceSectors = 0; DWORD error = ERROR_SUCCESS;
    for (size_t i = 0; i < fileCount; i++) {
        if (!dataFiles[i] || !dataFiles[i][0] || wcslen(dataFiles[i]) >= MAX_PATH || !mds_resolve(mdsFilePath,dataFiles[i],source) || !_wcsicmp(source,destination)) { error = ERROR_INVALID_PARAMETER; break; }
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(source,GetFileExInfoStandard,&attributes)) { error = GetLastError(); break; }
        uint64_t part = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
        uint64_t skipped = i ? 0 : startByteOffset;
        if (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { error = ERROR_INVALID_DATA; break; }
        if (!mds_part_geometry(part,skipped,incomingCarry,i + 1 == fileCount,splitSectors,&parts[i],&carries[i])) { error = GetLastError(); break; }
        sourceSectors += parts[i]; incomingCarry = carries[i];
        const wchar_t* name = source;
        for (const wchar_t* p = source; *p; p++) if (*p == L'\\' || *p == L'/') name = p + 1;
        if (preserveFullPathnames) name = source;
        else if ((size_t)(name - source) > destinationDirectory && !wcsncmp(source,destination,destinationDirectory)) name = source + destinationDirectory;
        const wchar_t* basename = name;
        for (const wchar_t* p = name; *p; p++) if (*p == L'\\' || *p == L'/') basename = p + 1;
        const wchar_t* dot = wcsrchr(basename,L'.'); size_t stem = dot ? (size_t)(dot - name) : wcslen(name);
        /* 默认子目录保留相对路径，别处只写基本名；完整路径选项绕过同主干通配。 */
        if (!preserveFullPathnames && mds_tools_equal(name,stem,destinationName,destinationStem)) swprintf(names[i],MAX_PATH,L"*%ls",dot ? dot : L"");
        else wcscpy(names[i],name);
        /* 原版ANSI请求长度仍按UTF16码元数加1，并非转换后的字节数。 */
        bytes += (uint32_t)(wcslen(names[i]) + 1) * (ansiFilenames ? 1 : 2);
    }
    if (!error && sourceSectors != totalSectors) error = ERROR_INVALID_DATA;
    if (error) { free(names); SetLastError(error); return false; }
    uint8_t* data = (uint8_t*)calloc(1,bytes);
    if (!data) { free(names); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    static const uint16_t media[] = {0x10,0x12,0x13,0x15,0x14};
    static const uint8_t books[] = {0x01,0x21,0x31,0xA1,0x91};
    MdsHeader* header = (MdsHeader*)data;
    memcpy(header->signature,"MEDIA DESCRIPTOR",16); header->version[0] = 1; header->version[1] = 3; header->mediumType = media[medium]; header->sessionCount = 1; header->dummy1[0] = 2; header->discStructuresOffset = 192; header->sessionsOffset = 88;
    MdsSessionBlock* session = (MdsSessionBlock*)(data + 88);
    session->sessionEndLba = (int32_t)totalSectors; session->sessionNumber = 1; session->totalTracks = 1; session->leadInTracks = 3; session->firstTrack = 1; session->lastTrack = 1; session->tracksOffset = 112;
    MdsTrackBlock* track = (MdsTrackBlock*)(data + 112);
    track->trackMode = 2; track->adrControl = 0x14; track->point = 1; track->extraOffset = totalSectors; track->sectorSize = 2048; track->startByteOffset = startByteOffset; track->fileCount = (uint32_t)fileCount; track->footerOffset = footerOffset;
    uint8_t* pfi = data + 2244;
    pfi[0] = books[medium]; pfi[1] = 2; pfi[2] = layerBreakLba ? (oppositeTrackPath ? 0x31 : 0x21) : 1; pfi[3] = 0x10;
    mds_set_be32(pfi + 4,0x30000);
    mds_set_be32(pfi + 8,(layerBreakLba && oppositeTrackPath ? ((0x30000 + layerBreakLba) ^ 0xFFFFFF) + (totalSectors - layerBreakLba) : 0x30000 + (layerBreakLba ? layerBreakLba : totalSectors) - 1) & 0xFFFFFF);
    if (layerBreakLba && oppositeTrackPath) mds_set_be32(pfi + 12,(0x30000 + layerBreakLba - 1) & 0xFFFFFF);
    if (layerBreakLba) {
        memcpy(data + 4292,data + 192,4100);
        if (!oppositeTrackPath) mds_set_be32(pfi + 4108,(0x30000 + totalSectors - layerBreakLba - 1) & 0xFFFFFF);
    }
    uint32_t nameOffset = footerOffset + (uint32_t)fileCount * 16;
    for (size_t i = 0; i < fileCount; i++) {
        uint32_t footer[4] = {nameOffset,(carries[i] << 4) | (ansiFilenames ? 0u : 1u),parts[i],0}, nameBytes = (uint32_t)(wcslen(names[i]) + 1) * (ansiFilenames ? 1 : 2);
        memcpy(data + footerOffset + i * 16,footer,sizeof(footer));
        if (ansiFilenames) {
            char converted[MAX_PATH * 4];
            int convertedBytes = WideCharToMultiByte(CP_ACP,0,names[i],-1,converted,sizeof(converted),NULL,NULL);
            if (!convertedBytes) { error = GetLastError(); break; }
            /* 短于原版请求长度时不读取转换缓冲区之外的未知字节。 */
            if ((uint32_t)convertedBytes < nameBytes) { error = ERROR_NOT_SUPPORTED; break; }
            memcpy(data + nameOffset,converted,nameBytes);
        } else memcpy(data + nameOffset,names[i],nameBytes);
        nameOffset += nameBytes;
    }
    free(names);
    if (error) { free(data); SetLastError(error); return false; }
    static volatile LONG sequence = 0; HANDLE output = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 16; i++) {
        if (swprintf(temporary,MAX_PATH,L"%ls.partial.%lu.%ld",destination,GetCurrentProcessId(),InterlockedIncrement(&sequence)) < 0) { error = ERROR_FILENAME_EXCED_RANGE; break; }
        output = CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if (output != INVALID_HANDLE_VALUE) break;
        error = GetLastError(); if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
    }
    bool ok = output != INVALID_HANDLE_VALUE; uint32_t offset = 0;
    while (ok && offset < bytes) {
        DWORD actual = 0;
        if (!WriteFile(output,data + offset,bytes - offset,&actual,NULL)) { error = GetLastError(); ok = false; }
        else if (!actual || actual > bytes - offset) { error = ERROR_WRITE_FAULT; ok = false; }
        else offset += actual;
    }
    if (ok && !FlushFileBuffers(output)) { error = GetLastError(); ok = false; }
    if (output != INVALID_HANDLE_VALUE && !CloseHandle(output) && ok) { error = GetLastError(); ok = false; }
    if (ok && !MoveFileExW(temporary,destination,MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { error = GetLastError(); ok = false; }
    if (!ok && output != INVALID_HANDLE_VALUE) DeleteFileW(temporary);
    free(data); SetLastError(ok ? ERROR_SUCCESS : error); return ok;
}

bool imgburn_mds_create_tools_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath, MdsToolsMedium medium, bool preserveFullPathnames) {
    return imgburn_mds_create_tools_files_ex(mdsFilePath,dataFiles,fileCount,totalSectors,layerBreakLba,oppositeTrackPath,medium,preserveFullPathnames,false);
}

bool imgburn_mds_create_tools_files_ex(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath, MdsToolsMedium medium, bool preserveFullPathnames, bool ansiFilenames) {
    return mds_create_tools_files(mdsFilePath,dataFiles,fileCount,totalSectors,layerBreakLba,oppositeTrackPath,medium,preserveFullPathnames,true,ansiFilenames,0,false);
}

bool imgburn_mds_create_tools_input(const wchar_t* mdsFilePath, const MdsToolsInput* input, bool overrideLayerBreak, uint32_t layerBreakLba, bool oppositeTrackPath, MdsToolsMedium medium, bool preserveFullPathnames) {
    return imgburn_mds_create_tools_input_ex(mdsFilePath,input,overrideLayerBreak,layerBreakLba,oppositeTrackPath,medium,preserveFullPathnames,false);
}

bool imgburn_mds_create_tools_input_ex(const wchar_t* mdsFilePath, const MdsToolsInput* input, bool overrideLayerBreak, uint32_t layerBreakLba, bool oppositeTrackPath, MdsToolsMedium medium, bool preserveFullPathnames, bool ansiFilenames) {
    if (!input || !input->fileCount || input->fileCount > MAX_MDS_FILES || !input->totalSectors || input->totalSectors > INT32_MAX) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    const wchar_t* paths[MAX_MDS_FILES]; uint64_t total = 0; uint32_t incomingCarry = 0;
    for (unsigned i = 0; i < input->fileCount; i++) {
        const MdsDataFile* part = &input->files[i]; WIN32_FILE_ATTRIBUTE_DATA attributes;
        uint64_t skipped = i ? 0 : input->startByteOffset;
        if (!wmemchr(part->path,0,MAX_PATH) || !part->path[0]) { SetLastError(ERROR_INVALID_DATA); return false; }
        uint32_t sectors, carry;
        if (!mds_part_geometry(part->bytes,skipped,incomingCarry,i + 1 == input->fileCount,input->splitSectors,&sectors,&carry)) return false;
        /* loader已经展开为绝对路径，避免手工输入在检查与输出时使用不同基目录。 */
        if (!((part->path[0] == L'\\' && part->path[1] == L'\\') || (wcslen(part->path) > 2 && part->path[1] == L':' && (part->path[2] == L'\\' || part->path[2] == L'/')))) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        if (!GetFileAttributesExW(part->path,GetFileExInfoStandard,&attributes)) return false;
        uint64_t actual = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
        if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || actual != part->bytes) { SetLastError(ERROR_INVALID_DATA); return false; }
        paths[i] = part->path; total += sectors; incomingCarry = carry;
    }
    if (total != input->totalSectors) { SetLastError(ERROR_INVALID_DATA); return false; }
    uint32_t chosen = overrideLayerBreak ? layerBreakLba : input->layerBreakLba;
    return mds_create_tools_files(mdsFilePath,paths,input->fileCount,input->totalSectors,chosen,oppositeTrackPath,medium,preserveFullPathnames,overrideLayerBreak,ansiFilenames,input->startByteOffset,input->splitSectors);
}

bool imgburn_mds_create_files_ex(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath) {
    return mds_create_data(mdsFilePath,dataFiles,fileCount,totalSectors,layerBreakLba,oppositeTrackPath,false,false,2048,0);
}
bool imgburn_mds_create_dvd_read(const wchar_t* mdsFilePath, const wchar_t* dataFile, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath) {
    const wchar_t* files[] = {dataFile}; return imgburn_mds_create_dvd_read_files(mdsFilePath,files,1,totalSectors,layerBreakLba,oppositeTrackPath);
}
bool imgburn_mds_create_dvd_read_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath) {
    return mds_create_data(mdsFilePath,dataFiles,fileCount,totalSectors,layerBreakLba,oppositeTrackPath,true,false,2048,0);
}
bool imgburn_mds_create_cd_read_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors) {
    return mds_create_data(mdsFilePath,dataFiles,fileCount,totalSectors,0,false,true,true,2048,0xAA);
}
bool imgburn_mds_create_cd_raw_read(const wchar_t* mdsFilePath, const wchar_t* dataFile, uint32_t totalSectors) {
    const wchar_t* files[] = {dataFile}; return imgburn_mds_create_cd_raw_read_files(mdsFilePath,files,1,totalSectors,0xAA);
}
bool imgburn_mds_create_cd_raw_read_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint8_t mode) {
    if (mode != 0xAA && mode != 0xAB) return false;
    return mds_create_data(mdsFilePath,dataFiles,fileCount,totalSectors,0,false,true,true,2352,mode);
}

/* -------------------------------------------------------------------------
 * DVD-Video IFO Parser & Layer Break Candidate Analyzer
 * ------------------------------------------------------------------------- */
bool imgburn_dvd_find_layer_breaks(const wchar_t* videoTsPath, uint32_t totalImageSectors, DvdLayerBreakCandidate* outCandidates, int maxCandidates, int* outCount) {
    if (outCount) *outCount = 0;
    if (!videoTsPath || !totalImageSectors || !outCandidates || maxCandidates <= 0 || !outCount) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    /* 目录文件不足以确定镜像中的物理LBA；接入实际布局和PGC之前，禁止把VOB相对地址或中点伪报为候选。 */
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}
