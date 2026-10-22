/**
 * @file imgburn_fs.c
 * @brief ImgBurn Core C - Optical Disc Filesystem Generator Implementation
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_fs.h"
#include "imgburn_udf.h"
#include "imgburn_fs_internal.h"
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
        free(f->patches);
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
    for (int i = 0; i < count - 1; i++) {
        uint16_t value = (uint16_t)wideLabel[i];
        if (value == 0xFFFE || value == 0xFEFF || (value >= 0xD800 && value <= 0xDFFF)) { free(wideLabel); return false; }
        if (i < 32) {
            if (value >= 'a' && value <= 'z') value -= 'a' - 'A';
            b->config.volumeLabelIso[i] = (char)((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '_' ? value : '_');
        }
    }
    b->config.volumeLabelIso[count - 1 < 32 ? count - 1 : 32] = 0;
    wcsncpy(b->config.volumeLabelJoliet, wideLabel, 16);
    wcsncpy(b->config.volumeLabelUdf, wideLabel, 126);
    free(wideLabel);

    b->config.fsTypes = FS_TYPE_ALL;
    b->config.udfRev = UDF_REV_102;
    b->config.optimizeDuplicateFiles = true;
    b->config.dvdVideoCompliance = false;

    b->rootDir = create_dir_node("", L"", NULL);
    return b->rootDir != NULL;
}

void image_builder_destroy(ImageBuilder* b) {
    if (!b) return;
    fs_layout_free(b);
    if (b->rootDir) {
        free_dir_node(b->rootDir);
        b->rootDir = NULL;
    }
}

FsDirectoryNode* fs_ensure_directory(ImageBuilder* b, const wchar_t* relDirPath) {
    fs_layout_free(b);
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
        if (wcslen(token) > 127 || wcscmp(token, L".") == 0 || wcscmp(token, L"..") == 0) return NULL;
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
            for (FsFileNode* file = curr->firstFile; file; file = file->next) if (_wcsicmp(file->nameW, token) == 0) return NULL;
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

    if (fad.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    uint64_t fileSize = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (fileSize > (uint64_t)(UINT32_MAX - 4096u) * FS_SECTOR_SIZE) return false;
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

    if (!filePart[0] || wcslen(filePart) > 127) return false;
    FsDirectoryNode* dir = fs_ensure_directory(b, dirPart);
    if (!dir) return false;

    for (FsFileNode* existing = dir->firstFile; existing; existing = existing->next) if (_wcsicmp(existing->nameW, filePart) == 0) return false;
    for (FsDirectoryNode* child = dir->firstChildDir; child; child = child->nextSibling) if (_wcsicmp(child->nameW, filePart) == 0) return false;
    FsFileNode* f = (FsFileNode*)calloc(1, sizeof(FsFileNode));
    if (!f) return false;

    convert_to_iso_name(filePart, f->nameA, sizeof(f->nameA), false);
    wcsncpy(f->nameW, filePart, (sizeof(f->nameW) / sizeof(wchar_t)) - 1);
    wcsncpy(f->sourceDiskPath, sourceFilePath, MAX_PATH - 1);
    f->fileSize = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    f->modTime = fad.ftLastWriteTime;
    f->parent = dir;
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
    if (!fs_ensure_directory(b, discDestFolder)) return false;
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
    if (dir->excluded) return true;
    FsFileNode* f = dir->firstFile;
    while (f) {
        if (f->excluded) { f = f->next; continue; }
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
    if (!b->config.optimizeDuplicateFiles || b->config.dvdVideoCompliance || b->config.blurayCompliance) { free(allFiles); return true; }

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

uint32_t image_builder_calculate_layout(ImageBuilder* b) {
    if (!b || !b->rootDir) return 0;
    fs_layout_free(b);
    if (!fs_video_prepare(b) || !deduplicate_files(b) || !fs_layout_prepare(b)) { fs_layout_free(b); return 0; }
    return b->config.totalImageSectors;
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
                if (success) { fs_video_patch(node, node->fileSize - remaining, buffer, actual); success = write_bytes(output, buffer, actual); remaining -= actual; }
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

    if (!fs_layout_write(b, hOut)) goto failed;

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
