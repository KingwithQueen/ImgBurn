/**
 * @file imgburn_fs.c
 * @brief ImgBurn Core C - Optical Disc Filesystem Generator Implementation
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_fs.h"
#include "imgburn_udf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* CRC32 lookup table for file deduplication */
static uint32_t CRC32_TABLE[256];
static bool s_crc32_init = false;

static void init_crc32(void) {
    if (s_crc32_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320L ^ (c >> 1)) : (c >> 1);
        }
        CRC32_TABLE[i] = c;
    }
    s_crc32_init = true;
}

static uint32_t compute_file_crc32(const wchar_t* path, uint64_t maxBytes) {
    init_crc32();
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    uint32_t crc = 0xFFFFFFFF;
    BYTE buf[65536];
    DWORD bytesRead = 0;
    uint64_t totalRead = 0;

    while (ReadFile(hFile, buf, sizeof(buf), &bytesRead, NULL) && bytesRead > 0) {
        for (DWORD i = 0; i < bytesRead; i++) {
            crc = CRC32_TABLE[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
        }
        totalRead += bytesRead;
        if (maxBytes > 0 && totalRead >= maxBytes) break;
    }

    CloseHandle(hFile);
    return crc ^ 0xFFFFFFFF;
}

/* CRC32仅用于筛选候选；逐字节相同才能共享数据区。 */
static bool source_matches_snapshot(HANDLE file, const FsFileNode* node) {
    LARGE_INTEGER size;
    FILETIME written;
    return GetFileSizeEx(file, &size) && (uint64_t)size.QuadPart == node->fileSize &&
           GetFileTime(file, NULL, NULL, &written) && CompareFileTime(&written, &node->modTime) == 0;
}

static bool files_equal(const FsFileNode* left, const FsFileNode* right) {
    HANDLE a = CreateFileW(left->sourceDiskPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE b = CreateFileW(right->sourceDiskPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    bool equal = false;
    if (a == INVALID_HANDLE_VALUE || b == INVALID_HANDLE_VALUE) goto cleanup;
    if (!source_matches_snapshot(a, left) || !source_matches_snapshot(b, right)) goto cleanup;
    uint8_t first[65536], second[65536];
    uint64_t remaining = left->fileSize;
    while (remaining > 0) {
        DWORD wanted = remaining < sizeof(first) ? (DWORD)remaining : (DWORD)sizeof(first), readA = 0, readB = 0;
        if (!ReadFile(a, first, wanted, &readA, NULL) || !ReadFile(b, second, wanted, &readB, NULL) ||
            readA != wanted || readB != wanted || memcmp(first, second, wanted) != 0) goto cleanup;
        remaining -= wanted;
    }
    equal = true;
cleanup:
    if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
    if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
    return equal;
}

/* 同步写入也可能短写，完整送出全部数据后才算成功。 */
static bool write_bytes(HANDLE file, const void* buffer, DWORD bytes) {
    const uint8_t* cursor = (const uint8_t*)buffer;
    while (bytes > 0) {
        DWORD actual = 0;
        if (!WriteFile(file, cursor, bytes, &actual, NULL) || actual == 0 || actual > bytes) return false;
        cursor += actual;
        bytes -= actual;
    }
    return true;
}

/* Helpers for ISO9660 multi-byte integers (Little-Endian & Big-Endian together) */
static inline void set_both_endian_16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

static inline void set_both_endian_32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)((v >> 16) & 0xFF);
    p[6] = (uint8_t)((v >> 8) & 0xFF);
    p[7] = (uint8_t)(v & 0xFF);
}

static void convert_to_iso_name(const wchar_t* src, char* dst, size_t maxLen, bool isDir) {
    char temp[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, src, -1, temp, sizeof(temp), NULL, NULL);

    size_t i = 0, j = 0;
    while (temp[i] && j < maxLen - 4) {
        char c = temp[i++];
        if ((c >= 'a' && c <= 'z')) c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.') {
            dst[j++] = c;
        } else {
            dst[j++] = '_';
        }
    }
    dst[j] = '\0';
    if (!isDir && strchr(dst, '.') == NULL) {
        strncat(dst, ".DAT", maxLen - strlen(dst) - 1);
    }
    if (!isDir) {
        strncat(dst, ";1", maxLen - strlen(dst) - 1);
    }
}

static void utf16_to_be(const wchar_t* src, uint8_t* dst, size_t lenChars) {
    for (size_t i = 0; i < lenChars; i++) {
        uint16_t w = (uint16_t)src[i];
        dst[i * 2] = (uint8_t)(w >> 8);
        dst[i * 2 + 1] = (uint8_t)(w & 0xFF);
    }
}

static FsDirectoryNode* create_dir_node(const char* nameA, const wchar_t* nameW, FsDirectoryNode* parent) {
    FsDirectoryNode* d = (FsDirectoryNode*)calloc(1, sizeof(FsDirectoryNode));
    if (!d) return NULL;
    strncpy(d->nameA, nameA, sizeof(d->nameA) - 1);
    wcsncpy(d->nameW, nameW, (sizeof(d->nameW) / sizeof(wchar_t)) - 1);
    d->parent = parent;
    return d;
}

static void free_dir_node(FsDirectoryNode* dir) {
    if (!dir) return;

    /* Free files */
    FsFileNode* f = dir->firstFile;
    while (f) {
        FsFileNode* next = f->next;
        free(f);
        f = next;
    }

    /* Free child dirs */
    FsDirectoryNode* child = dir->firstChildDir;
    while (child) {
        FsDirectoryNode* next = child->nextSibling;
        free_dir_node(child);
        child = next;
    }

    free(dir);
}

bool image_builder_init(ImageBuilder* b, const char* volumeLabel) {
    if (!b) return false;
    memset(b, 0, sizeof(*b));

    const char* label = (volumeLabel && volumeLabel[0]) ? volumeLabel : "IMGBURN_DISC";
    strncpy(b->config.volumeLabelIso, label, 32);
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, label, -1, NULL, 0);
    if (count <= 0) return false;
    wchar_t* wideLabel = (wchar_t*)calloc((size_t)count, sizeof(wchar_t));
    if (!wideLabel) return false;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, label, -1, wideLabel, count)) { free(wideLabel); return false; }
    wcsncpy(b->config.volumeLabelJoliet, wideLabel, 16);
    wcsncpy(b->config.volumeLabelUdf, wideLabel, 126);
    free(wideLabel);

    b->config.fsTypes = FS_TYPE_ALL;
    b->config.udfRev = UDF_REV_102;
    b->config.optimizeDuplicateFiles = true;
    b->config.dvdVideoCompliance = true;

    b->rootDir = create_dir_node("", L"", NULL);
    return b->rootDir != NULL;
}

void image_builder_destroy(ImageBuilder* b) {
    if (!b) return;
    if (b->rootDir) {
        free_dir_node(b->rootDir);
        b->rootDir = NULL;
    }
}

static FsDirectoryNode* get_or_create_dir(ImageBuilder* b, const wchar_t* relDirPath) {
    FsDirectoryNode* root = b->rootDir;
    if (relDirPath && wcslen(relDirPath) >= MAX_PATH) return NULL;
    if (!relDirPath || !relDirPath[0] || wcscmp(relDirPath, L"\\") == 0 || wcscmp(relDirPath, L"/") == 0) {
        return root;
    }

    wchar_t pathCopy[MAX_PATH] = {0};
    wcsncpy(pathCopy, relDirPath, MAX_PATH - 1);

    FsDirectoryNode* curr = root;
    wchar_t* context = NULL;
    wchar_t* token = wcstok(pathCopy, L"\\/", &context);
    while (token) {
        if (wcslen(token) > 110 || wcscmp(token, L".") == 0 || wcscmp(token, L"..") == 0) return NULL;
        /* Search child dirs */
        FsDirectoryNode* found = NULL;
        FsDirectoryNode* c = curr->firstChildDir;
        while (c) {
            if (_wcsicmp(c->nameW, token) == 0) {
                found = c;
                break;
            }
            c = c->nextSibling;
        }

        if (!found) {
            char nameA[128];
            convert_to_iso_name(token, nameA, sizeof(nameA), true);
            found = create_dir_node(nameA, token, curr);
            if (!found) return NULL;
            b->config.totalDirectories++;
            found->nextSibling = curr->firstChildDir;
            curr->firstChildDir = found;
        }

        curr = found;
        token = wcstok(NULL, L"\\/", &context);
    }

    return curr;
}

bool image_builder_add_file(ImageBuilder* b,
                            const wchar_t* sourceFilePath,
                            const wchar_t* discRelativePath) {
    if (!b || !b->rootDir || !sourceFilePath || !discRelativePath || wcslen(sourceFilePath) >= MAX_PATH || wcslen(discRelativePath) >= MAX_PATH) return false;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(sourceFilePath, GetFileExInfoStandard, &fad)) {
        return false;
    }

    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    /* Extract directory and filename from discRelativePath */
    wchar_t dirPart[MAX_PATH] = { 0 };
    wchar_t filePart[MAX_PATH] = { 0 };

    const wchar_t* lastSlash = wcsrchr(discRelativePath, L'\\');
    const wchar_t* forwardSlash = wcsrchr(discRelativePath, L'/');
    if (!lastSlash || (forwardSlash && forwardSlash > lastSlash)) lastSlash = forwardSlash;

    if (lastSlash) {
        wcsncpy(dirPart, discRelativePath, (size_t)(lastSlash - discRelativePath));
        wcscpy(filePart, lastSlash + 1);
    } else {
        wcscpy(filePart, discRelativePath);
    }

    if (!filePart[0] || wcslen(filePart) > 110) return false;
    FsDirectoryNode* dir = get_or_create_dir(b, dirPart);
    if (!dir) return false;

    FsFileNode* f = (FsFileNode*)calloc(1, sizeof(FsFileNode));
    if (!f) return false;

    convert_to_iso_name(filePart, f->nameA, sizeof(f->nameA), false);
    wcsncpy(f->nameW, filePart, (sizeof(f->nameW) / sizeof(wchar_t)) - 1);
    wcsncpy(f->sourceDiskPath, sourceFilePath, MAX_PATH - 1);
    f->fileSize = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    f->modTime = fad.ftLastWriteTime;
    f->sectorCount = (uint32_t)((f->fileSize + FS_SECTOR_SIZE - 1) / FS_SECTOR_SIZE);

    /* Append to dir */
    f->next = dir->firstFile;
    dir->firstFile = f;

    b->config.totalFiles++;
    b->config.totalSourceBytes += f->fileSize;
    return true;
}

bool image_builder_add_folder(ImageBuilder* b, const wchar_t* hostFolderPath, const wchar_t* discDestFolder) {
    if (!b || !b->rootDir || !hostFolderPath || wcslen(hostFolderPath) + 3 > MAX_PATH) return false;
    DWORD attributes = GetFileAttributesW(hostFolderPath);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    if (!get_or_create_dir(b, discDestFolder)) return false;
    wchar_t search[MAX_PATH];
    if (swprintf(search, MAX_PATH, L"%ls\\*", hostFolderPath) < 0) return false;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(search, &data);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool success = true;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        wchar_t childHost[MAX_PATH], childDisc[MAX_PATH];
        if (swprintf(childHost, MAX_PATH, L"%ls\\%ls", hostFolderPath, data.cFileName) < 0 ||
            (discDestFolder && discDestFolder[0] ? swprintf(childDisc, MAX_PATH, L"%ls/%ls", discDestFolder, data.cFileName) :
             swprintf(childDisc, MAX_PATH, L"%ls", data.cFileName)) < 0) { success = false; break; }
        /* 暂不跟随重解析点，避免遗漏、越界或递归环；遇到时明确失败。 */
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { success = false; break; }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) success = image_builder_add_folder(b, childHost, childDisc);
        else success = image_builder_add_file(b, childHost, childDisc);
        if (!success) break;
    } while (FindNextFileW(find, &data));
    if (success && GetLastError() != ERROR_NO_MORE_FILES) success = false;
    FindClose(find);
    return success;
}

/* Collect all files into a flat list for deduplication */
static bool collect_files(FsDirectoryNode* dir, FsFileNode*** list, int* count, int* cap) {
    FsFileNode* f = dir->firstFile;
    while (f) {
        if (*count >= *cap) {
            if (*cap > INT_MAX / 2) return false;
            int newCapacity = *cap == 0 ? 256 : *cap * 2;
            if ((size_t)newCapacity > SIZE_MAX / sizeof(FsFileNode*)) return false;
            FsFileNode** grown = (FsFileNode**)realloc(*list, (size_t)newCapacity * sizeof(FsFileNode*));
            if (!grown) return false;
            *cap = newCapacity;
            *list = grown;
        }
        (*list)[(*count)++] = f;
        f = f->next;
    }

    FsDirectoryNode* c = dir->firstChildDir;
    while (c) {
        if (!collect_files(c, list, count, cap)) return false;
        c = c->nextSibling;
    }
    return true;
}

static bool deduplicate_files(ImageBuilder* b) {

    FsFileNode** allFiles = NULL;
    int count = 0, cap = 0;
    if (!collect_files(b->rootDir, &allFiles, &count, &cap)) { free(allFiles); return false; }
    b->config.deduplicatedBytesSaved = 0;
    for (int i = 0; i < count; i++) { allFiles[i]->isDuplicate = false; allFiles[i]->duplicateOf = NULL; allFiles[i]->crc32 = 0; }
    if (!b->config.optimizeDuplicateFiles) { free(allFiles); return true; }

    for (int i = 0; i < count; i++) {
        FsFileNode* fi = allFiles[i];
        if (fi->fileSize == 0 || fi->isDuplicate) continue;

        for (int j = i + 1; j < count; j++) {
            FsFileNode* fj = allFiles[j];
            if (fj->isDuplicate || fj->fileSize != fi->fileSize) continue;

            /* Same size: compute CRC32 */
            if (fi->crc32 == 0) fi->crc32 = compute_file_crc32(fi->sourceDiskPath, 0);
            if (fj->crc32 == 0) fj->crc32 = compute_file_crc32(fj->sourceDiskPath, 0);

            if (fi->crc32 == fj->crc32 && files_equal(fi, fj)) {
                fj->isDuplicate = true;
                fj->duplicateOf = fi;
                b->config.deduplicatedBytesSaved += fj->fileSize;
            }
        }
    }

    free(allFiles);
    return true;
}

/* Calculate directory record size in bytes */
static uint32_t calculate_dir_extent_size(FsDirectoryNode* dir, bool isJoliet) {
    /* Dot (.) and DotDot (..) entries */
    uint32_t size = 34 + 34;

    /* Child directories */
    FsDirectoryNode* c = dir->firstChildDir;
    while (c) {
        uint32_t nameLen = isJoliet ? (uint32_t)(wcslen(c->nameW) * 2) : (uint32_t)strlen(c->nameA);
        uint32_t recLen = 33 + nameLen;
        if (recLen % 2 != 0) recLen++;
        size += recLen;
        c = c->nextSibling;
    }

    /* Files */
    FsFileNode* f = dir->firstFile;
    while (f) {
        uint32_t nameLen = isJoliet ? (uint32_t)(wcslen(f->nameW) * 2) : (uint32_t)strlen(f->nameA);
        uint32_t recLen = 33 + nameLen;
        if (recLen % 2 != 0) recLen++;
        size += recLen;
        f = f->next;
    }

    return (size + FS_SECTOR_SIZE - 1) & ~(FS_SECTOR_SIZE - 1);
}

/* Assign directory LBAs */
static void allocate_dir_lbas(FsDirectoryNode* dir, uint32_t* currentLba, bool isJoliet) {
    uint32_t extentBytes = calculate_dir_extent_size(dir, isJoliet);
    uint32_t sectors = extentBytes / FS_SECTOR_SIZE;

    if (isJoliet) {
        dir->allocatedLbaJoliet = *currentLba;
        dir->sectorCountJoliet = sectors;
    } else {
        dir->allocatedLbaIso = *currentLba;
        dir->sectorCountIso = sectors;
    }
    *currentLba += sectors;

    FsDirectoryNode* c = dir->firstChildDir;
    while (c) {
        allocate_dir_lbas(c, currentLba, isJoliet);
        c = c->nextSibling;
    }
}

/* Assign file data LBAs */
static void allocate_file_lbas(FsDirectoryNode* dir, uint32_t* currentLba, bool dvdPadding) {
    /* DVD-Video: Check if this is VIDEO_TS and pad IFO/BUP */
    bool isVideoTs = (_stricmp(dir->nameA, "VIDEO_TS") == 0 || _wcsicmp(dir->nameW, L"VIDEO_TS") == 0);

    FsFileNode* f = dir->firstFile;
    while (f) {
        if (f->isDuplicate) {
            f->allocatedLba = f->duplicateOf->allocatedLba;
        } else {
            /* DVD-Video: If this is a .BUP file, ensure >= 16 sectors from its .IFO */
            if (dvdPadding && isVideoTs && strstr(f->nameA, ".BUP")) {
                char ifoName[128];
                strncpy(ifoName, f->nameA, sizeof(ifoName) - 1);
                char* bupExt = strstr(ifoName, ".BUP");
                if (bupExt) memcpy(bupExt, ".IFO", 4);

                /* Search matching IFO in dir */
                FsFileNode* ifo = dir->firstFile;
                while (ifo) {
                    if (strcmp(ifo->nameA, ifoName) == 0) {
                        if (*currentLba - ifo->allocatedLba < 16) {
                            /* Pad to at least 16 sectors (32 KB ECC block) */
                            *currentLba = ifo->allocatedLba + 16;
                        }
                        break;
                    }
                    ifo = ifo->next;
                }
            }

            f->allocatedLba = *currentLba;
            *currentLba += f->sectorCount;
        }
        f = f->next;
    }

    FsDirectoryNode* c = dir->firstChildDir;
    while (c) {
        allocate_file_lbas(c, currentLba, dvdPadding);
        c = c->nextSibling;
    }
}

uint32_t image_builder_calculate_layout(ImageBuilder* b) {
    if (!b || !b->rootDir) return 0;

    /* 1. File deduplication */
    if (!deduplicate_files(b)) { b->config.totalImageSectors = 0; return 0; }

    /* 2. System area: LBA 0..15 (32 KB reserved) */
    uint32_t lba = 16;
    b->pvdLba = lba++;
    b->svdLba = lba++;
    lba++;

    /* Path tables */
    b->pathTableLbaL = lba++;
    b->pathTableLbaM = lba++;
    b->pathTableLbaLJoliet = lba++;
    b->pathTableLbaMJoliet = lba++;

    /* 3. Allocate ISO directory extents */
    allocate_dir_lbas(b->rootDir, &lba, false);

    /* 4. Allocate Joliet directory extents */
    allocate_dir_lbas(b->rootDir, &lba, true);

    /* 5. If UDF enabled, reserve space for AVDP / MVDS / FSD */
    if (b->config.fsTypes & FS_TYPE_UDF) {
        if (lba < 302) {
            lba = 302; /* FSD at LBA 300, Root FE at LBA 301 */
        }
    }

    /* 6. Allocate File data extents */
    b->dataStartLba = lba;
    allocate_file_lbas(b->rootDir, &lba, b->config.dvdVideoCompliance);

    /* If UDF enabled, ensure volume contains at least 512 sectors */
    if ((b->config.fsTypes & FS_TYPE_UDF) && lba < 512) {
        lba = 512;
    }

    b->config.totalImageSectors = lba;
    return lba;
}

static void write_datetime(uint8_t* p) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[18];
    snprintf(buf, sizeof(buf), "%04u%02u%02u%02u%02u%02u%02u%c",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds / 10, 0);
    memcpy(p, buf, 17);
}

static bool write_pvd(ImageBuilder* b, HANDLE hFile, bool isJoliet) {
    uint8_t sector[FS_SECTOR_SIZE];
    memset(sector, 0, sizeof(sector));

    sector[0] = isJoliet ? 0x02 : 0x01; /* 1 = PVD, 2 = SVD (Joliet) */
    memcpy(&sector[1], "CD001\x01", 6);

    if (isJoliet) {
        /* Joliet escape sequence: %/E (UCS-2 Level 3) */
        sector[88] = 0x25; sector[89] = 0x2F; sector[90] = 0x45;
        utf16_to_be(b->config.volumeLabelJoliet, &sector[40], wcslen(b->config.volumeLabelJoliet));
    } else {
        memcpy(&sector[40], b->config.volumeLabelIso, strlen(b->config.volumeLabelIso));
    }

    set_both_endian_32(&sector[80], b->config.totalImageSectors);
    set_both_endian_16(&sector[120], 1); /* Volume set size */
    set_both_endian_16(&sector[124], 1); /* Volume seq number */
    set_both_endian_16(&sector[128], FS_SECTOR_SIZE);

    /* Path tables */
    uint32_t ptLba = isJoliet ? b->pathTableLbaLJoliet : b->pathTableLbaL;
    uint32_t ptLbaM = isJoliet ? b->pathTableLbaMJoliet : b->pathTableLbaM;
    *(uint32_t*)&sector[140] = ptLba;
    sector[148] = (uint8_t)(ptLbaM >> 24); sector[149] = (uint8_t)(ptLbaM >> 16);
    sector[150] = (uint8_t)(ptLbaM >> 8);  sector[151] = (uint8_t)(ptLbaM & 0xFF);

    /* Root directory record (34 bytes at sector 156) */
    uint8_t* rootRec = &sector[156];
    rootRec[0] = 34; /* Length */
    set_both_endian_32(&rootRec[2], isJoliet ? b->rootDir->allocatedLbaJoliet : b->rootDir->allocatedLbaIso);
    set_both_endian_32(&rootRec[10], isJoliet ? b->rootDir->sectorCountJoliet * FS_SECTOR_SIZE : b->rootDir->sectorCountIso * FS_SECTOR_SIZE);
    rootRec[25] = 0x02; /* Directory flag */
    set_both_endian_16(&rootRec[28], 1);
    rootRec[32] = 1;
    rootRec[33] = 0x00; /* Special root identifier */

    write_datetime(&sector[813]); /* Creation date */
    write_datetime(&sector[830]); /* Mod date */
    sector[881] = 0x01;           /* File structure version */

    return write_bytes(hFile, sector, FS_SECTOR_SIZE);
}

/* Write directory records extent to ISO */
static bool write_dir_extent(HANDLE hFile, FsDirectoryNode* dir, bool isJoliet) {
    uint32_t extentBytes = isJoliet ? (dir->sectorCountJoliet * FS_SECTOR_SIZE) : (dir->sectorCountIso * FS_SECTOR_SIZE);
    uint8_t* buffer = (uint8_t*)calloc(1, extentBytes);
    if (!buffer) return false;
    uint32_t offset = 0;

    /* Entry 1: Current dir '.' */
    buffer[offset] = 34;
    set_both_endian_32(&buffer[offset + 2], isJoliet ? dir->allocatedLbaJoliet : dir->allocatedLbaIso);
    set_both_endian_32(&buffer[offset + 10], extentBytes);
    buffer[offset + 25] = 0x02;
    set_both_endian_16(&buffer[offset + 28], 1);
    buffer[offset + 32] = 1;
    buffer[offset + 33] = 0x00;
    offset += 34;

    /* Entry 2: Parent dir '..' */
    FsDirectoryNode* parent = dir->parent ? dir->parent : dir;
    uint32_t parentBytes = isJoliet ? (parent->sectorCountJoliet * FS_SECTOR_SIZE) : (parent->sectorCountIso * FS_SECTOR_SIZE);
    buffer[offset] = 34;
    set_both_endian_32(&buffer[offset + 2], isJoliet ? parent->allocatedLbaJoliet : parent->allocatedLbaIso);
    set_both_endian_32(&buffer[offset + 10], parentBytes);
    buffer[offset + 25] = 0x02;
    set_both_endian_16(&buffer[offset + 28], 1);
    buffer[offset + 32] = 1;
    buffer[offset + 33] = 0x01;
    offset += 34;

    /* Child Dirs */
    FsDirectoryNode* c = dir->firstChildDir;
    while (c) {
        uint32_t nameLen = isJoliet ? (uint32_t)(wcslen(c->nameW) * 2) : (uint32_t)strlen(c->nameA);
        uint8_t recLen = (uint8_t)(33 + nameLen);
        if (recLen % 2 != 0) recLen++;

        uint32_t cBytes = isJoliet ? (c->sectorCountJoliet * FS_SECTOR_SIZE) : (c->sectorCountIso * FS_SECTOR_SIZE);
        buffer[offset] = recLen;
        set_both_endian_32(&buffer[offset + 2], isJoliet ? c->allocatedLbaJoliet : c->allocatedLbaIso);
        set_both_endian_32(&buffer[offset + 10], cBytes);
        buffer[offset + 25] = 0x02;
        set_both_endian_16(&buffer[offset + 28], 1);
        buffer[offset + 32] = (uint8_t)nameLen;
        if (isJoliet) {
            utf16_to_be(c->nameW, &buffer[offset + 33], wcslen(c->nameW));
        } else {
            memcpy(&buffer[offset + 33], c->nameA, nameLen);
        }

        offset += recLen;
        c = c->nextSibling;
    }

    /* Files */
    FsFileNode* f = dir->firstFile;
    while (f) {
        uint32_t nameLen = isJoliet ? (uint32_t)(wcslen(f->nameW) * 2) : (uint32_t)strlen(f->nameA);
        uint8_t recLen = (uint8_t)(33 + nameLen);
        if (recLen % 2 != 0) recLen++;

        buffer[offset] = recLen;
        set_both_endian_32(&buffer[offset + 2], f->allocatedLba);
        set_both_endian_32(&buffer[offset + 10], (uint32_t)f->fileSize);
        buffer[offset + 25] = 0x00; /* Regular file */
        set_both_endian_16(&buffer[offset + 28], 1);
        buffer[offset + 32] = (uint8_t)nameLen;
        if (isJoliet) {
            utf16_to_be(f->nameW, &buffer[offset + 33], wcslen(f->nameW));
        } else {
            memcpy(&buffer[offset + 33], f->nameA, nameLen);
        }

        offset += recLen;
        f = f->next;
    }

    bool success = write_bytes(hFile, buffer, extentBytes);
    free(buffer);
    if (!success) return false;

    /* Recurse child dirs */
    c = dir->firstChildDir;
    while (c) {
        if (!write_dir_extent(hFile, c, isJoliet)) return false;
        c = c->nextSibling;
    }
    return true;
}

/* 任何输入快照变化、短读、写入错误都必须中止镜像生成。 */
static bool write_all_files_data(ImageBuilder* b, HANDLE output, ImageProgressFn progress, void* userData) {
    FsFileNode** files = NULL;
    int count = 0, capacity = 0;
    if (!collect_files(b->rootDir, &files, &count, &capacity)) { free(files); return false; }
    bool success = true;
    uint8_t buffer[65536];
    for (int i = 0; i < count && success; i++) {
        FsFileNode* node = files[i];
        HANDLE source = CreateFileW(node->sourceDiskPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (source == INVALID_HANDLE_VALUE) { success = false; break; }
        success = source_matches_snapshot(source, node);
        if (success && node->isDuplicate) success = files_equal(node, node->duplicateOf);
        if (success && !node->isDuplicate) {
            LARGE_INTEGER position; position.QuadPart = (LONGLONG)node->allocatedLba * FS_SECTOR_SIZE;
            success = SetFilePointerEx(output, position, NULL, FILE_BEGIN) != FALSE;
            uint64_t remaining = node->fileSize;
            while (remaining > 0 && success) {
                DWORD wanted = remaining < sizeof(buffer) ? (DWORD)remaining : (DWORD)sizeof(buffer), actual = 0;
                success = ReadFile(source, buffer, wanted, &actual, NULL) && actual > 0 && actual <= wanted;
                if (success) { success = write_bytes(output, buffer, actual); remaining -= actual; }
            }
            if (success && node->fileSize % FS_SECTOR_SIZE != 0) {
                DWORD padding = FS_SECTOR_SIZE - (DWORD)(node->fileSize % FS_SECTOR_SIZE);
                memset(buffer, 0, padding);
                success = write_bytes(output, buffer, padding);
            }
        }
        CloseHandle(source);
        if (success && progress) progress((int)((uint64_t)(i + 1) * 100 / count), node->allocatedLba, b->config.totalImageSectors, userData);
    }
    free(files);
    return success;
}

bool image_builder_create_iso(ImageBuilder* b,
                              const wchar_t* outputIsoPath,
                              ImageProgressFn progressCallback,
                              void* userData) {
    if (!b || !b->rootDir || !outputIsoPath || b->config.totalImageSectors == 0) return false;

    wchar_t destination[MAX_PATH], temporary[MAX_PATH];
    DWORD length = GetFullPathNameW(outputIsoPath, MAX_PATH, destination, NULL);
    if (length == 0 || length >= MAX_PATH) return false;
    FsFileNode** sources = NULL;
    int sourceCount = 0, sourceCapacity = 0;
    if (!collect_files(b->rootDir, &sources, &sourceCount, &sourceCapacity)) { free(sources); return false; }
    HANDLE existing = CreateFileW(destination, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    BY_HANDLE_FILE_INFORMATION targetInfo = {0};
    bool haveTarget = existing != INVALID_HANDLE_VALUE;
    if (haveTarget && !GetFileInformationByHandle(existing, &targetInfo)) { CloseHandle(existing); free(sources); return false; }
    if (haveTarget) CloseHandle(existing);
    for (int i = 0; i < sourceCount; i++) {
        HANDLE source = CreateFileW(sources[i]->sourceDiskPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        BY_HANDLE_FILE_INFORMATION info = {0};
        bool valid = source != INVALID_HANDLE_VALUE && source_matches_snapshot(source, sources[i]) && GetFileInformationByHandle(source, &info);
        if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
        if (!valid || (haveTarget && info.dwVolumeSerialNumber == targetInfo.dwVolumeSerialNumber &&
            info.nFileIndexHigh == targetInfo.nFileIndexHigh && info.nFileIndexLow == targetInfo.nFileIndexLow)) { free(sources); return false; }
    }
    free(sources);
    static volatile LONG sequence = 0;
    HANDLE hOut = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 16; attempt++) {
        if (swprintf(temporary, MAX_PATH, L"%ls.partial.%lu.%ld", destination, GetCurrentProcessId(), InterlockedIncrement(&sequence)) < 0) return false;
        hOut = CreateFileW(temporary, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hOut != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (hOut == INVALID_HANDLE_VALUE) return false;
    /* Preallocate total image size */
    LARGE_INTEGER totalSize;
    totalSize.QuadPart = (int64_t)b->config.totalImageSectors * FS_SECTOR_SIZE;
    if (!SetFilePointerEx(hOut, totalSize, NULL, FILE_BEGIN) || !SetEndOfFile(hOut)) goto failed;

    /* 1. Write System Area (LBA 0..15 = 32KB zeroes) */
    LARGE_INTEGER zeroPos;
    zeroPos.QuadPart = 0;
    if (!SetFilePointerEx(hOut, zeroPos, NULL, FILE_BEGIN)) goto failed;
    uint8_t zeroSector[FS_SECTOR_SIZE] = { 0 };
    for (int i = 0; i < 16; i++) {
        if (!write_bytes(hOut, zeroSector, FS_SECTOR_SIZE)) goto failed;
    }

    /* 2. Write ISO9660 PVD (LBA 16) */
    if (!write_pvd(b, hOut, false)) goto failed;

    /* 3. Write Joliet SVD (LBA 17) */
    if (!write_pvd(b, hOut, true)) goto failed;

    /* 4. Write Volume Descriptor Set Terminator (LBA 18) */
    memset(zeroSector, 0, sizeof(zeroSector));
    zeroSector[0] = 0xFF; /* Terminator */
    memcpy(&zeroSector[1], "CD001\x01", 6);
    if (!write_bytes(hOut, zeroSector, FS_SECTOR_SIZE)) goto failed;

    /* 5. Write Empty Path Tables (4 sectors) */
    for (int i = 0; i < 4; i++) {
        memset(zeroSector, 0, sizeof(zeroSector));
        if (!write_bytes(hOut, zeroSector, FS_SECTOR_SIZE)) goto failed;
    }

    /* 6. Write ISO9660 Directory Extents */
    LARGE_INTEGER dirPosIso;
    dirPosIso.QuadPart = (int64_t)b->rootDir->allocatedLbaIso * FS_SECTOR_SIZE;
    if (!SetFilePointerEx(hOut, dirPosIso, NULL, FILE_BEGIN)) goto failed;
    if (!write_dir_extent(hOut, b->rootDir, false)) goto failed;

    /* 7. Write Joliet Directory Extents */
    LARGE_INTEGER dirPosJoliet;
    dirPosJoliet.QuadPart = (int64_t)b->rootDir->allocatedLbaJoliet * FS_SECTOR_SIZE;
    if (!SetFilePointerEx(hOut, dirPosJoliet, NULL, FILE_BEGIN)) goto failed;
    if (!write_dir_extent(hOut, b->rootDir, true)) goto failed;

    /* 8. If UDF enabled, write UDF Descriptors (VRS, MVDS, AVDP, RVDS, FSD) */
    if (b->config.fsTypes & FS_TYPE_UDF) {
        bool isUdf250 = (b->config.udfRev >= UDF_REV_250);

        /* Write VRS at LBA 19..21 (BEA01, NSR02/03, TEA01) */
        LARGE_INTEGER vrsPos;
        vrsPos.QuadPart = (int64_t)19 * FS_SECTOR_SIZE;
        if (!SetFilePointerEx(hOut, vrsPos, NULL, FILE_BEGIN)) goto failed;
        uint8_t vrsBuffer[3 * FS_SECTOR_SIZE];
        udf_build_vrs(vrsBuffer, isUdf250);
        if (!write_bytes(hOut, vrsBuffer, sizeof(vrsBuffer))) goto failed;

        /* Write MVDS at LBA 32..47 (16 sectors) */
        LARGE_INTEGER mvdsPos;
        mvdsPos.QuadPart = (int64_t)32 * FS_SECTOR_SIZE;
        if (!SetFilePointerEx(hOut, mvdsPos, NULL, FILE_BEGIN)) goto failed;
        uint8_t mvdsBuffer[16 * FS_SECTOR_SIZE] = {0};
        udf_build_mvds(mvdsBuffer, 32, b->config.volumeLabelUdf, 300, b->config.totalImageSectors - 300, 300, isUdf250);
        if (!write_bytes(hOut, mvdsBuffer, sizeof(mvdsBuffer))) goto failed;

        /* Write AVDP at LBA 256 */
        LARGE_INTEGER avdpPos;
        avdpPos.QuadPart = (int64_t)256 * FS_SECTOR_SIZE;
        if (!SetFilePointerEx(hOut, avdpPos, NULL, FILE_BEGIN)) goto failed;
        uint8_t avdpSector[FS_SECTOR_SIZE];
        udf_build_avdp(avdpSector, 256, 32, 16);
        if (!write_bytes(hOut, avdpSector, FS_SECTOR_SIZE)) goto failed;

        /* Write RVDS at LBA 257..272 (duplicate of MVDS) */
        if (!write_bytes(hOut, mvdsBuffer, sizeof(mvdsBuffer))) goto failed;

        /* Write FSD at LBA 300 */
        LARGE_INTEGER fsdPos;
        fsdPos.QuadPart = (int64_t)300 * FS_SECTOR_SIZE;
        if (!SetFilePointerEx(hOut, fsdPos, NULL, FILE_BEGIN)) goto failed;
        uint8_t fsdSector[FS_SECTOR_SIZE];
        udf_build_fsd(fsdSector, 300, 301);
        if (!write_bytes(hOut, fsdSector, FS_SECTOR_SIZE)) goto failed;
    }

    /* 9. Write File Contents */
    if (!write_all_files_data(b, hOut, progressCallback, userData) || !FlushFileBuffers(hOut)) goto failed;
    if (!CloseHandle(hOut)) { DeleteFileW(temporary); return false; }
    if (MoveFileExW(temporary, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(temporary);
    return false;
failed:
    CloseHandle(hOut);
    DeleteFileW(temporary);
    return false;
}
