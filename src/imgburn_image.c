/**
 * @file imgburn_image.c
 * @brief Optical Disc Image Parsers & Generators (CUE, BIN, MDS, ISO, DVD-Video IFO)
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_image.h"
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
    output[count] = 0; *cursor = p; return count != 0;
}

static bool cue_end(const char* text) { while (*text && isspace((unsigned char)*text)) text++; return *text == 0; }

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

/* 只接受已有完整计数实现的BINARY和原生PCM WAVE，避免猜测压缩音频长度。 */
static bool cue_source_size(const wchar_t* path, bool wave, uint32_t sectorSize, uint32_t* sectors) {
    FILE* file = _wfopen(path, L"rb");
    if (!file) return false;
    bool ok = false;
    if (_fseeki64(file, 0, SEEK_END) != 0) goto done;
    int64_t size = _ftelli64(file);
    if (size <= 0) goto done;
    uint64_t bytes = (uint64_t)size;
    if (wave) {
        uint8_t header[12];
        if (sectorSize != 2352 || !cue_read(file, 0, header, 12) || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) goto done;
        uint64_t end = (uint64_t)cue_le32(header + 4) + 8, offset = 12;
        bool format = false, data = false;
        if (end > bytes || end < 12) goto done;
        while (offset + 8 <= end) {
            uint8_t chunk[24] = {0};
            if (!cue_read(file, offset, chunk, 8)) goto done;
            uint32_t length = cue_le32(chunk + 4);
            if (length > end - offset - 8) goto done;
            if (!memcmp(chunk, "fmt ", 4)) {
                if (format || length < 16 || !cue_read(file, offset + 8, chunk + 8, 16) || chunk[8] != 1 || chunk[9] || chunk[10] != 2 || chunk[11] || cue_le32(chunk + 12) != 44100 || cue_le32(chunk + 16) != 176400 || chunk[20] != 4 || chunk[21] || chunk[22] != 16 || chunk[23]) goto done;
                format = true;
            } else if (!memcmp(chunk, "data", 4)) { if (data || length % 4) goto done; bytes = length; data = true; }
            offset += 8 + (uint64_t)length + (length & 1u);
            if (offset > end) goto done;
        }
        if (!format || !data || !bytes || offset != end) goto done;
    } else if (bytes % sectorSize) goto done;
    if ((bytes + sectorSize - 1) / sectorSize > UINT32_MAX) goto done;
    *sectors = (uint32_t)((bytes + sectorSize - 1) / sectorSize); ok = true;
done:
    fclose(file); return ok;
}

bool imgburn_cue_parse(const wchar_t* cueFilePath, CueSheet* outCue) {
    if (!outCue) return false;
    memset(outCue, 0, sizeof(*outCue));
    if (!cueFilePath || wcslen(cueFilePath) >= MAX_PATH) return false;
    FILE* file = _wfopen(cueFilePath, L"rb");
    if (!file) return false;
    CueSheet* sheet = (CueSheet*)calloc(1, sizeof(*sheet));
    if (!sheet) { fclose(file); return false; }
    wchar_t absolute[MAX_PATH], directory[MAX_PATH], currentFile[MAX_PATH] = L"";
    DWORD chars = GetFullPathNameW(cueFilePath, MAX_PATH, absolute, NULL);
    bool ok = false, wave = false, firstLine = true;
    unsigned fileId = 0, groups[MAX_CUE_TRACKS] = {0};
    bool waves[MAX_CUE_TRACKS] = {0}, pregaps[MAX_CUE_TRACKS] = {0}, postgaps[MAX_CUE_TRACKS] = {0};
    if (!chars || chars >= MAX_PATH) goto done;
    wcscpy(directory, absolute);
    wchar_t* slash = wcsrchr(directory, L'\\'); if (!slash) goto done; slash[1] = 0;
    wcscpy(sheet->cueFilePath, absolute);
    CueTrack* track = NULL;
    char line[2048];
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) goto done;
        char* cursor = line;
        if (firstLine && !memcmp(line, "\xEF\xBB\xBF", 3)) cursor += 3;
        firstLine = false;
        if (cue_end(cursor)) continue;
        char command[32], value[1024], second[32];
        if (!cue_token(&cursor, command, sizeof(command))) goto done;
        if (!_stricmp(command, "REM")) continue;
        if (!cue_token(&cursor, value, sizeof(value))) goto done;
        if (!_stricmp(command, "FILE")) {
            if (!cue_token(&cursor, second, sizeof(second)) || !cue_end(cursor) || (_stricmp(second, "BINARY") && _stricmp(second, "WAVE"))) goto done;
            wchar_t wide[MAX_PATH], combined[MAX_PATH];
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, wide, MAX_PATH)) goto done;
            if (wide[0] == L'\\' || wide[0] == L'/' || (wcslen(wide) > 1 && wide[1] == L':')) {
                if (swprintf(combined, MAX_PATH, L"%ls", wide) < 0) goto done;
            } else if (swprintf(combined, MAX_PATH, L"%ls%ls", directory, wide) < 0) goto done;
            chars = GetFullPathNameW(combined, MAX_PATH, currentFile, NULL);
            if (!chars || chars >= MAX_PATH || ++fileId > MAX_CUE_TRACKS) goto done;
            wave = !_stricmp(second, "WAVE"); track = NULL; continue;
        }
        if (!_stricmp(command, "TRACK")) {
            unsigned number;
            if (!currentFile[0] || sheet->trackCount == MAX_CUE_TRACKS || !cue_number(value, 99, &number) || number != (unsigned)sheet->trackCount + 1 || !cue_token(&cursor, second, sizeof(second)) || !cue_end(cursor)) goto done;
            track = &sheet->tracks[sheet->trackCount]; groups[sheet->trackCount] = fileId; waves[sheet->trackCount++] = wave;
            track->trackNumber = (int)number; wcscpy(track->sourceFilePath, currentFile);
            track->sourceIsWave = wave;
            if (!_stricmp(second, "AUDIO")) track->mode = TRACK_MODE_AUDIO;
            else if (!_stricmp(second, "MODE1/2048")) track->mode = TRACK_MODE_MODE1_2048;
            else if (!_stricmp(second, "MODE1/2352")) track->mode = TRACK_MODE_MODE1_2352;
            else if (!_stricmp(second, "MODE2/2336")) track->mode = TRACK_MODE_MODE2_2336;
            else if (!_stricmp(second, "MODE2/2352")) track->mode = TRACK_MODE_MODE2_2352;
            else goto done;
            if (wave && track->mode != TRACK_MODE_AUDIO) goto done;
            continue;
        }
        if (!_stricmp(command, "INDEX")) {
            unsigned index;
            uint32_t address;
            if (!track || track->indexCount == MAX_CUE_INDICES || !cue_number(value, 99, &index) || !cue_token(&cursor, second, sizeof(second)) || !cue_end(cursor) || !cue_msf(second, &address)) goto done;
            if (track->indexCount && (index <= (unsigned)track->indices[track->indexCount - 1].indexNumber || address < track->indices[track->indexCount - 1].lba)) goto done;
            CueIndex* entry = &track->indices[track->indexCount++]; entry->indexNumber = (int)index; entry->lba = address; strcpy(entry->msfStr, second); continue;
        }
        if (!cue_end(cursor)) goto done;
        if (!_stricmp(command, "TITLE") || !_stricmp(command, "PERFORMER")) {
            if (strlen(value) >= 128) goto done;
            char* destination = !_stricmp(command, "TITLE") ? (track ? track->title : sheet->discTitle) : (track ? track->performer : sheet->discPerformer);
            strcpy(destination, value);
        } else if (!_stricmp(command, "PREGAP") || !_stricmp(command, "POSTGAP")) {
            uint32_t duration;
            if (!track || !cue_msf(value, &duration)) goto done;
            unsigned i = (unsigned)sheet->trackCount - 1;
            if (!_stricmp(command, "PREGAP")) { if (pregaps[i]) goto done; pregaps[i] = true; track->pregapSectors = duration; }
            else { if (postgaps[i]) goto done; postgaps[i] = true; track->postgapSectors = duration; }
        } else if (!_stricmp(command, "CATALOG")) {
            if (strlen(value) != 13 || strspn(value, "0123456789") != 13 || track) goto done;
            strcpy(sheet->catalogNumber, value);
        } else if (!_stricmp(command, "ISRC")) {
            if (!track || strlen(value) != 12) goto done;
            for (int i = 0; i < 12; i++) if (!isalnum((unsigned char)value[i])) goto done;
            strcpy(track->isrc, value);
        } else goto done;
    }
    if (ferror(file) || !sheet->trackCount || groups[sheet->trackCount - 1] != fileId) goto done;
    uint64_t total = 0;
    for (int first = 0; first < sheet->trackCount;) {
        int last = first + 1;
        while (last < sheet->trackCount && groups[last] == groups[first]) last++;
        if (groups[first] != (first ? groups[first - 1] + 1 : 1)) goto done;
        uint32_t sectorSize = sheet->tracks[first].mode == TRACK_MODE_MODE1_2048 ? 2048 : sheet->tracks[first].mode == TRACK_MODE_MODE2_2336 ? 2336 : 2352;
        uint32_t fileSectors;
        if (!cue_source_size(sheet->tracks[first].sourceFilePath, waves[first], sectorSize, &fileSectors)) goto done;
        uint64_t gaps = 0;
        for (int i = first; i < last; i++) {
            CueTrack* current = &sheet->tracks[i];
            uint32_t currentSize = current->mode == TRACK_MODE_MODE1_2048 ? 2048 : current->mode == TRACK_MODE_MODE2_2336 ? 2336 : 2352;
            if (currentSize != sectorSize) goto done;
            bool indexOne = false;
            uint32_t start = 0;
            for (int j = 0; j < current->indexCount; j++) {
                if (current->indices[j].lba >= fileSectors) goto done;
                if (current->indices[j].indexNumber == 1) { indexOne = true; start = current->indices[j].lba; }
            }
            uint32_t end = i + 1 < last && sheet->tracks[i + 1].indexCount ? sheet->tracks[i + 1].indices[0].lba : fileSectors;
            if (!indexOne || end <= start || (current->indexCount && current->indices[current->indexCount - 1].lba >= end)) goto done;
            gaps += current->pregapSectors;
            if (total + start + gaps > UINT32_MAX) goto done;
            current->startLba = (uint32_t)(total + start + gaps); current->sectorCount = end - start; gaps += current->postgapSectors;
        }
        total += fileSectors + gaps; if (total > UINT32_MAX) goto done; first = last;
    }
    sheet->totalSectors = (uint32_t)total; memcpy(outCue, sheet, sizeof(*sheet)); ok = true;
done:
    free(sheet); fclose(file); return ok;
}

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

/* 当前只支持可完整表达的MDS 1.x单会话、单文件DVD；其它布局明确失败。 */
bool imgburn_mds_parse(const wchar_t* mdsFilePath, MdsSheet* outMds) {
    if (!outMds) return false;
    memset(outMds, 0, sizeof(*outMds));
    if (!mdsFilePath || wcslen(mdsFilePath) >= MAX_PATH) return false;
    FILE* file = _wfopen(mdsFilePath, L"rb");
    if (!file) return false;
    bool ok = false;
    uint8_t* data = NULL;
    if (_fseeki64(file, 0, SEEK_END)) goto done;
    int64_t size = _ftelli64(file);
    if (size < 88 || size > 16 * 1024 * 1024) goto done;
    data = (uint8_t*)malloc((size_t)size);
    if (!data || !cue_read(file, 0, data, (size_t)size)) goto done;
    const MdsHeader* header = (const MdsHeader*)data;
    if (memcmp(header->signature, "MEDIA DESCRIPTOR", 16) || header->version[0] != 1 || header->sessionCount != 1 || (header->mediumType != 0x10 && header->mediumType != 0x12) || !mds_range(size, header->sessionsOffset, sizeof(MdsSessionBlock))) goto done;
    const MdsSessionBlock* session = (const MdsSessionBlock*)(data + header->sessionsOffset);
    if (session->sessionStartLba != 0 || session->sessionEndLba <= 0 || session->sessionNumber != 1 || session->totalTracks != 1 || session->leadInTracks || session->firstTrack != 1 || session->lastTrack != 1 || !mds_range(size, session->tracksOffset, sizeof(MdsTrackBlock))) goto done;
    const MdsTrackBlock* track = (const MdsTrackBlock*)(data + session->tracksOffset);
    if (track->trackMode != 0xAA || track->subChannel || track->point != 1 || track->sectorSize != 2048 || track->startLba || track->startByteOffset || track->fileCount != 1 || track->extraOffset != (uint32_t)session->sessionEndLba || !mds_range(size, track->footerOffset, 16)) goto done;
    const uint8_t* footer = data + track->footerOffset;
    uint32_t nameOffset = cue_le32(footer), wide = cue_le32(footer + 4);
    wchar_t name[MAX_PATH] = {0};
    if (wide > 1 || !mds_range(size, nameOffset, wide ? 2 : 1)) goto done;
    size_t count = 0;
    if (wide) {
        while (count < MAX_PATH && nameOffset + (uint64_t)count * 2 + 2 <= (uint64_t)size) {
            uint16_t value = (uint16_t)(data[nameOffset + count * 2] | (uint16_t)data[nameOffset + count * 2 + 1] << 8);
            if (!value) break;
            name[count++] = (wchar_t)value;
        }
        if (count == MAX_PATH || nameOffset + (uint64_t)count * 2 + 2 > (uint64_t)size) goto done;
    } else {
        while (nameOffset + count < (uint64_t)size && data[nameOffset + count] && count < MAX_PATH * 3) count++;
        if (nameOffset + count >= (uint64_t)size || count >= MAX_PATH * 3 || !MultiByteToWideChar(CP_ACP, 0, (const char*)data + nameOffset, (int)count + 1, name, MAX_PATH)) goto done;
    }
    MdsSheet result = {0};
    if (!_wcsicmp(name, L"*.mdf")) {
        wcscpy(name, mdsFilePath); wchar_t* dot = wcsrchr(name, L'.'); if (!dot || (size_t)(dot - name) > MAX_PATH - 5) goto done; wcscpy(dot, L".mdf");
        DWORD length = GetFullPathNameW(name, MAX_PATH, result.dataFilePath, NULL); if (!length || length >= MAX_PATH) goto done;
    } else if (!mds_resolve(mdsFilePath, name, result.dataFilePath)) goto done;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(result.dataFilePath, GetFileExInfoStandard, &attributes) || (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) goto done;
    uint64_t sourceBytes = (uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow;
    if (sourceBytes != (uint64_t)session->sessionEndLba * 2048) goto done;
    result.totalSectors = (uint32_t)session->sessionEndLba; result.trackCount = 1; result.mediumType = header->mediumType;
    if (header->discStructuresOffset) {
        if (!mds_range(size, header->discStructuresOffset, 4100)) goto done;
        const uint8_t* pfi = data + header->discStructuresOffset + 2052;
        unsigned layers = (pfi[2] >> 5) & 3;
        if (layers > 1) goto done;
        if (layers == 1) {
            if (!(pfi[2] & 0x10) || !mds_range(size, header->discStructuresOffset, 8200)) goto done;
            uint32_t start = mds_be32(pfi + 4) & 0xFFFFFF, end = mds_be32(pfi + 12) & 0xFFFFFF;
            if (start != 0x30000 || end < start) goto done;
            result.layerBreakLba = end - start + 1;
            if (result.layerBreakLba >= result.totalSectors || result.layerBreakLba < (result.totalSectors + 1) / 2) goto done;
        }
    }
    wcscpy(result.mdsFilePath, mdsFilePath); *outMds = result; ok = true;
done:
    free(data); fclose(file); return ok;
}

bool imgburn_mds_create(const wchar_t* mdsFilePath, const wchar_t* dataFileName, uint32_t totalSectors, uint32_t layerBreakLba) {
    if (!mdsFilePath || !dataFileName || !dataFileName[0] || wcslen(mdsFilePath) >= MAX_PATH || wcslen(dataFileName) >= MAX_PATH || !totalSectors || totalSectors > INT32_MAX) return false;
    if (layerBreakLba && (layerBreakLba >= totalSectors || layerBreakLba < (totalSectors + 1) / 2 || layerBreakLba > 2086912 || layerBreakLba % 16)) return false;
    if (!layerBreakLba && totalSectors > 2295104) return false;
    wchar_t source[MAX_PATH], destination[MAX_PATH], temporary[MAX_PATH];
    if (!mds_resolve(mdsFilePath, dataFileName, source)) return false;
    DWORD length = GetFullPathNameW(mdsFilePath, MAX_PATH, destination, NULL);
    if (!length || length >= MAX_PATH || !_wcsicmp(source, destination)) return false;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(source, GetFileExInfoStandard, &attributes) || (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || ((uint64_t)attributes.nFileSizeHigh << 32 | attributes.nFileSizeLow) != (uint64_t)totalSectors * 2048) return false;
    uint32_t nameBytes = (uint32_t)(wcslen(dataFileName) + 1) * 2, structures = (208 + nameBytes + 3) & ~3u;
    uint32_t bytes = structures + (layerBreakLba ? 8200 : 4100);
    uint8_t* data = (uint8_t*)calloc(1, bytes);
    if (!data) return false;
    MdsHeader* header = (MdsHeader*)data;
    memcpy(header->signature, "MEDIA DESCRIPTOR", 16); header->version[0] = 1; header->version[1] = 3; header->mediumType = 0x10; header->sessionCount = 1; header->sessionsOffset = 88; header->discStructuresOffset = structures;
    MdsSessionBlock* session = (MdsSessionBlock*)(data + 88);
    session->sessionEndLba = (int32_t)totalSectors; session->sessionNumber = 1; session->totalTracks = 1; session->firstTrack = 1; session->lastTrack = 1; session->tracksOffset = 112;
    MdsTrackBlock* track = (MdsTrackBlock*)(data + 112);
    track->trackMode = 0xAA; track->adrControl = 0x14; track->point = 1; track->time[5] = 2; track->extraOffset = totalSectors; track->sectorSize = 2048; track->fileCount = 1; track->footerOffset = 192;
    uint32_t footer[4] = {208, 1, 0, 0}; memcpy(data + 192, footer, sizeof(footer)); memcpy(data + 208, dataFileName, nameBytes);
    uint8_t* pfi = data + structures + 2052;
    pfi[0] = 1; pfi[1] = 0x0F; pfi[2] = layerBreakLba ? 0x30 : 0;
    mds_set_be32(pfi + 4, 0x30000);
    mds_set_be32(pfi + 8, layerBreakLba ? ((~(0x30000 + layerBreakLba - 1) + totalSectors - layerBreakLba - 1) & 0xFFFFFF) : 0x30000 + totalSectors - 1);
    if (layerBreakLba) { mds_set_be32(pfi + 12, 0x30000 + layerBreakLba - 1); memcpy(data + structures + 4100, data + structures, 4100); }
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
