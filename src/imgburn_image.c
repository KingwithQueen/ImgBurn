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

static bool mds_filename(const uint8_t* data, uint64_t size, uint32_t footer, const wchar_t* descriptor, MdsDataFile* result) {
    if (!mds_range(size, footer, 16)) return false;
    uint32_t offset = cue_le32(data + footer), wide = cue_le32(data + footer + 4);
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
    if (name[0] == '*' && name[1] == '.') {
        if (wcslen(name) >= 16 || wcschr(name + 1, L'*') || wcschr(name, L'\\') || wcschr(name, L'/')) return false;
        wchar_t local[MAX_PATH]; if (wcslen(descriptor) >= MAX_PATH) return false; wcscpy(local, descriptor);
        wchar_t* dot = wcsrchr(local, L'.'); if (!dot || (size_t)(dot - local) + wcslen(name + 1) >= MAX_PATH) return false; wcscpy(dot, name + 1);
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

bool imgburn_mds_parse(const wchar_t* mdsFilePath, MdsSheet* outMds) {
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
    bool dvd = header->mediumType == 0x10 || header->mediumType == 0x12;
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
            uint64_t available = 0;
            for (uint32_t f = 0; f < track.fileCount; f++) {
                MdsDataFile* part = &result->files[result->dataFileCount++];
                if (!mds_filename(data, size, track.footerOffset + f * 16, mdsFilePath, part)) goto done;
                uint64_t skipped = f ? 0 : track.startByteOffset;
                if (skipped > part->bytes || available > UINT64_MAX - (part->bytes - skipped)) goto done;
                available += part->bytes - skipped;
                if (f + 1 < track.fileCount && (part->bytes - skipped) % track.sectorSize) goto done;
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
            if (start != 0x30000 || end < start) goto done;
            result->layerBreakLba = end - start + 1;
            if (result->layerBreakLba >= result->totalSectors) goto done;
            if (result->oppositeTrackPath) { if (result->layerBreakLba < ((uint64_t)result->totalSectors + 1) / 2) goto done; }
            else {
                /* PTP每层均从内圈开始，L0长度来自第一层的数据区末尾。 */
                const uint8_t* second = pfi + 4100; uint32_t secondStart = mds_be32(second + 4) & 0xFFFFFF, secondEnd = mds_be32(second + 8) & 0xFFFFFF;
                if ((second[2] & 0x70) != 0x20 || secondStart != 0x30000 || secondEnd < secondStart || (uint64_t)result->layerBreakLba + secondEnd - secondStart + 1 != result->totalSectors) goto done;
            }
        }
    }
    wcscpy(result->mdsFilePath, mdsFilePath); if (result->dataFileCount) wcscpy(result->dataFilePath, result->files[0].path);
    memcpy(outMds, result, sizeof(*result)); ok = true;
done:
    free(result); free(data); fclose(file); return ok;
}

bool imgburn_mds_create(const wchar_t* mdsFilePath, const wchar_t* dataFileName, uint32_t totalSectors, uint32_t layerBreakLba) {
    const wchar_t* files[] = {dataFileName};
    return imgburn_mds_create_files(mdsFilePath, files, 1, totalSectors, layerBreakLba);
}

bool imgburn_mds_create_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba) {
    return imgburn_mds_create_files_ex(mdsFilePath, dataFiles, fileCount, totalSectors, layerBreakLba, true);
}

static bool mds_create_data(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath, bool sourceLayout, bool cd, uint16_t sectorSize) {
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
    track->trackMode = cd ? 0xAA : 0x02; track->adrControl = 0x14; track->point = 1; track->time[5] = 2; track->extraOffset = cd ? structures : totalSectors; track->sectorSize = sectorSize; track->fileCount = (uint32_t)fileCount; track->footerOffset = 192;
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

bool imgburn_mds_create_files_ex(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath) {
    return mds_create_data(mdsFilePath,dataFiles,fileCount,totalSectors,layerBreakLba,oppositeTrackPath,false,false,2048);
}
bool imgburn_mds_create_dvd_read(const wchar_t* mdsFilePath, const wchar_t* dataFile, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath) {
    const wchar_t* files[] = {dataFile}; return imgburn_mds_create_dvd_read_files(mdsFilePath,files,1,totalSectors,layerBreakLba,oppositeTrackPath);
}
bool imgburn_mds_create_dvd_read_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors, uint32_t layerBreakLba, bool oppositeTrackPath) {
    return mds_create_data(mdsFilePath,dataFiles,fileCount,totalSectors,layerBreakLba,oppositeTrackPath,true,false,2048);
}
bool imgburn_mds_create_cd_read_files(const wchar_t* mdsFilePath, const wchar_t* const* dataFiles, size_t fileCount, uint32_t totalSectors) {
    return mds_create_data(mdsFilePath,dataFiles,fileCount,totalSectors,0,false,true,true,2048);
}
bool imgburn_mds_create_cd_raw_read(const wchar_t* mdsFilePath, const wchar_t* dataFile, uint32_t totalSectors) {
    const wchar_t* files[] = {dataFile}; return mds_create_data(mdsFilePath,files,1,totalSectors,0,false,true,true,2352);
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
