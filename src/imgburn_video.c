#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_fs_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define VIDEO_BLOCK 2048u
#define DVD_MAX_FILE 0x3FFFF800u

static uint32_t video_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static FsDirectoryNode* video_dir(FsDirectoryNode* parent, const wchar_t* name) {
    if (!parent) return NULL;
    for (FsDirectoryNode* dir = parent->firstChildDir; dir; dir = dir->nextSibling) if (!_wcsicmp(dir->nameW, name)) return dir;
    return NULL;
}
static FsFileNode* video_file(FsDirectoryNode* dir, const wchar_t* name) {
    if (!dir) return NULL;
    for (FsFileNode* file = dir->firstFile; file; file = file->next) if (!file->excluded && !_wcsicmp(file->nameW, name)) return file;
    return NULL;
}
static void video_upper(wchar_t* text) { for (; *text; text++) *text = towupper(*text); }
static bool video_header(const FsFileNode* file, uint8_t* data, size_t bytes) {
    FILE* input = _wfopen(file->sourceDiskPath, L"rb");
    if (!input) return false;
    bool ok = fread(data, 1, bytes, input) == bytes; fclose(input); return ok;
}
static bool video_equal(const FsFileNode* left, const FsFileNode* right) {
    FILE* a = _wfopen(left->sourceDiskPath, L"rb"); FILE* b = _wfopen(right->sourceDiskPath, L"rb");
    bool ok = a && b; uint64_t remaining = left->fileSize; uint8_t first[16384], second[16384];
    while (ok && remaining) {
        size_t bytes = remaining < sizeof(first) ? (size_t)remaining : sizeof(first);
        ok = fread(first, 1, bytes, a) == bytes && fread(second, 1, bytes, b) == bytes && !memcmp(first, second, bytes); remaining -= bytes;
    }
    if (a) fclose(a); if (b) fclose(b); return ok;
}

/* 排序键只影响物理顺序，三种文件系统的目录名称顺序仍各自生成。 */
static bool dvd_name(const wchar_t* name, unsigned* group, unsigned* order) {
    if (!_wcsicmp(name, L"VIDEO_TS.IFO")) { *group = 0; *order = 0; return true; }
    if (!_wcsicmp(name, L"VIDEO_TS.VOB")) { *group = 0; *order = 1; return true; }
    if (!_wcsicmp(name, L"VIDEO_TS.BUP")) { *group = 0; *order = 11; return true; }
    if (wcslen(name) != 12 || _wcsnicmp(name, L"VTS_", 4) || name[6] != '_' || name[8] != '.' || name[4] < '0' || name[4] > '9' || name[5] < '0' || name[5] > '9' || name[7] < '0' || name[7] > '9') return false;
    *group = (unsigned)(name[4] - '0') * 10 + name[5] - '0';
    if (!*group) return false;
    if (name[7] == '0' && !_wcsicmp(name + 9, L"IFO")) *order = 0;
    else if (name[7] == '0' && !_wcsicmp(name + 9, L"BUP")) *order = 11;
    else if (!_wcsicmp(name + 9, L"VOB")) *order = (unsigned)(name[7] - '0') + 1;
    else return false;
    return true;
}

static void video_reset(FsDirectoryNode* dir) {
    dir->excluded = false;
    for (FsFileNode* file = dir->firstFile; file; file = file->next) file->excluded = false;
    for (FsDirectoryNode* child = dir->firstChildDir; child; child = child->nextSibling) video_reset(child);
}

static bool video_stats(ImageBuilder* b, FsDirectoryNode* dir) {
    if (dir->excluded) return true;
    if (dir->parent) { if (b->config.totalDirectories == UINT32_MAX) return false; b->config.totalDirectories++; }
    for (FsFileNode* file = dir->firstFile; file; file = file->next) if (!file->excluded) {
        if (b->config.totalFiles == UINT32_MAX || b->config.totalSourceBytes > UINT64_MAX - file->fileSize) return false;
        b->config.totalFiles++; b->config.totalSourceBytes += file->fileSize;
    }
    for (FsDirectoryNode* child = dir->firstChildDir; child; child = child->nextSibling) if (!video_stats(b, child)) return false;
    return true;
}

static bool dvd_prepare(ImageBuilder* b) {
    FsDirectoryNode* dir = video_dir(b->rootDir, L"VIDEO_TS");
    if (!dir || b->config.udfRev != UDF_REV_102 || b->config.fsTypes != FS_TYPE_ISO_UDF) return false;
    video_upper(dir->nameW);
    for (FsDirectoryNode* child = dir->firstChildDir; child; child = child->nextSibling) child->excluded = true;
    for (FsFileNode* file = dir->firstFile; file; file = file->next) {
        unsigned group, order;
        file->excluded = !dvd_name(file->nameW, &group, &order);
        if (file->excluded) continue;
        video_upper(file->nameW);
        if (!file->fileSize || file->fileSize % VIDEO_BLOCK || file->fileSize > DVD_MAX_FILE) return false;
        if (order == 0) {
            uint8_t header[2048];
            if (!video_header(file, header, sizeof(header)) || memcmp(header, group ? "DVDVIDEO-VTS" : "DVDVIDEO-VMG", 12)) return false;
            wchar_t backup[128]; wcscpy(backup, file->nameW); wcscpy(backup + wcslen(backup) - 3, L"BUP");
            FsFileNode* bup = video_file(dir, backup);
            if (!bup || bup->fileSize != file->fileSize || !video_equal(file, bup)) return false;
        }
    }
    if (!video_file(dir, L"VIDEO_TS.IFO") || !video_file(dir, L"VIDEO_TS.BUP")) return false;
    for (FsFileNode* file = dir->firstFile; file; file = file->next) if (!file->excluded) {
        unsigned group, order; dvd_name(file->nameW, &group, &order);
        if (group) {
            wchar_t ifo[16]; swprintf(ifo, 16, L"VTS_%02u_0.IFO", group);
            if (!video_file(dir, ifo)) return false;
            if (order >= 3 && order <= 10) { wchar_t previous[16]; swprintf(previous, 16, L"VTS_%02u_%u.VOB", group, order - 2); if (!video_file(dir, previous)) return false; }
        }
    }
    return fs_ensure_directory(b, L"AUDIO_TS") != NULL;
}

static bool bd_prepare(ImageBuilder* b) {
    if (b->config.udfRev != UDF_REV_250 || b->config.fsTypes != FS_TYPE_UDF) return false;
    FsDirectoryNode* bdmv = video_dir(b->rootDir, L"BDMV");
    FsDirectoryNode* bdav = video_dir(b->rootDir, L"BDAV");
    if (!bdmv && !bdav) return false;
    if (bdmv) {
        video_upper(bdmv->nameW);
        FsFileNode* index = video_file(bdmv, L"index.bdmv");
        FsFileNode* movie = video_file(bdmv, L"MovieObject.bdmv");
        uint8_t data[8];
        if (!index || !movie || !video_header(index, data, 8) || memcmp(data, "INDX", 4) || !video_header(movie, data, 8) || memcmp(data, "MOBJ", 4)) return false;
        const wchar_t* paths[] = {L"BDMV/AUXDATA",L"BDMV/BACKUP",L"BDMV/BDJO",L"BDMV/CLIPINF",L"BDMV/JAR",L"BDMV/META",L"BDMV/PLAYLIST",L"BDMV/STREAM",L"BDMV/BACKUP/BDJO",L"BDMV/BACKUP/CLIPINF",L"BDMV/BACKUP/JAR",L"BDMV/BACKUP/PLAYLIST"};
        for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); i++) {
            FsDirectoryNode* child = fs_ensure_directory(b, paths[i]);
            if (!child) return false;
            video_upper(child->nameW);
        }
        wcscpy(index->nameW, L"index.bdmv"); wcscpy(movie->nameW, L"MovieObject.bdmv");
    }
    if (bdav) {
        video_upper(bdav->nameW);
        if (!video_file(bdav, L"info.bdav") || !fs_ensure_directory(b, L"BDAV/PLAYLIST") || !fs_ensure_directory(b, L"BDAV/CLIPINF") || !fs_ensure_directory(b, L"BDAV/STREAM")) return false;
    }
    return true;
}

bool fs_video_prepare(ImageBuilder* b) {
    video_reset(b->rootDir);
    if (b->config.dvdVideoCompliance && b->config.blurayCompliance) return false;
    if (b->config.dvdVideoCompliance && !dvd_prepare(b)) return false;
    if (b->config.blurayCompliance && !bd_prepare(b)) return false;
    b->config.totalFiles = 0; b->config.totalDirectories = 0; b->config.totalSourceBytes = 0;
    return video_stats(b, b->rootDir);
}

static unsigned video_rank(const FsFileNode* file) {
    unsigned group, order;
    if (file->parent && file->parent->parent && !file->parent->parent->parent && !_wcsicmp(file->parent->nameW, L"VIDEO_TS") && dvd_name(file->nameW, &group, &order)) return group * 16 + order;
    return 10000;
}

int fs_video_file_order(const void* a, const void* b) {
    const FsFileNode* left = *(const FsFileNode* const*)a;
    const FsFileNode* right = *(const FsFileNode* const*)b;
    unsigned first = video_rank(left), second = video_rank(right);
    if (first != second) return first < second ? -1 : 1;
    int names = _wcsicmp(left->parent ? left->parent->nameW : L"", right->parent ? right->parent->nameW : L"");
    return names ? names : _wcsicmp(left->nameW, right->nameW);
}

uint32_t fs_video_align_file(const ImageBuilder* b, const FsFileNode* file, uint32_t lba) {
    if (b->config.blurayCompliance) return lba > UINT32_MAX - 31 ? 0 : (lba + 31u) & ~31u;
    if (!b->config.dvdVideoCompliance || video_rank(file) == 10000) return lba;
    unsigned group, order; dvd_name(file->nameW, &group, &order);
    /* 同一标题VOBS的分片必须物理连续，不能在第2片之后插入对齐空隙。 */
    if (order >= 3 && order <= 10) return lba;
    uint64_t next = lba;
    if (order == 11) {
        wchar_t ifoName[128]; wcscpy(ifoName, file->nameW); wcscpy(ifoName + wcslen(ifoName) - 3, L"IFO");
        FsFileNode* ifo = video_file(file->parent, ifoName);
        if (!ifo) return 0;
        uint64_t minimum = (uint64_t)ifo->allocatedLba + ifo->sectorCount + 16;
        if (next < minimum) next = minimum;
    }
    return next > UINT32_MAX - 15 ? 0 : ((uint32_t)next + 15u) & ~15u;
}

static bool video_patch_add(FsFileNode* file, uint64_t offset, uint32_t value) {
    if (offset > file->fileSize || file->fileSize - offset < 4 || file->patchCount >= SIZE_MAX / sizeof(FsBytePatch) - 1) return false;
    FsBytePatch* grown = (FsBytePatch*)realloc(file->patches, (file->patchCount + 1) * sizeof(*grown));
    if (!grown) return false;
    file->patches = grown; file->patches[file->patchCount++] = (FsBytePatch){offset,value}; return true;
}

bool fs_video_finalize(ImageBuilder* b) {
    if (!b->config.dvdVideoCompliance) return true;
    FsDirectoryNode* dir = video_dir(b->rootDir, L"VIDEO_TS");
    FsFileNode* vmg = video_file(dir, L"VIDEO_TS.IFO");
    for (FsFileNode* file = dir->firstFile; file; file = file->next) if (!file->excluded) {
        unsigned group, order; dvd_name(file->nameW, &group, &order);
        if (order) continue;
        wchar_t name[128]; wcscpy(name, file->nameW); wcscpy(name + wcslen(name) - 3, L"BUP");
        FsFileNode* bup = video_file(dir, name);
        if (!bup || bup->allocatedLba < file->allocatedLba || !video_patch_add(file, 12, bup->allocatedLba + bup->sectorCount - file->allocatedLba - 1) || !video_patch_add(file, 28, file->sectorCount - 1)) return false;
        if (group) swprintf(name, 128, L"VTS_%02u_0.VOB", group); else wcscpy(name, L"VIDEO_TS.VOB");
        FsFileNode* menu = video_file(dir, name);
        if (!video_patch_add(file, 0xC0, menu ? menu->allocatedLba - file->allocatedLba : 0)) return false;
        if (group) {
            swprintf(name, 128, L"VTS_%02u_1.VOB", group); FsFileNode* titles = video_file(dir, name);
            if (!titles || !video_patch_add(file, 0xC4, titles->allocatedLba - file->allocatedLba)) return false;
        } else {
            uint8_t header[2048]; if (!video_header(file, header, sizeof(header))) return false;
            uint64_t table = (uint64_t)video_be32(header + 0xC4) * VIDEO_BLOCK;
            if (table) {
                FILE* input = _wfopen(file->sourceDiskPath, L"rb"); if (!input) return false;
                uint8_t tableHeader[8]; bool ok = table <= file->fileSize - 8 && !_fseeki64(input, (int64_t)table, SEEK_SET) && fread(tableHeader, 1, 8, input) == 8;
                uint32_t count = ok ? (uint32_t)tableHeader[0] << 8 | tableHeader[1] : 0;
                uint32_t last = ok ? video_be32(tableHeader + 4) : 0;
                if (!count || count > 99 || last + (uint64_t)1 > file->fileSize - table || last + 1u < 8 + count * 12) ok = false;
                for (uint32_t i = 0; ok && i < count; i++) {
                    uint8_t entry[12]; ok = fread(entry, 1, sizeof(entry), input) == sizeof(entry);
                    if (!ok || !entry[6] || entry[6] > 99) { ok = false; break; }
                    swprintf(name, 128, L"VTS_%02u_0.IFO", entry[6]); FsFileNode* vts = video_file(dir, name);
                    ok = vts && vts->allocatedLba >= vmg->allocatedLba && video_patch_add(file, table + 8 + i * 12 + 8, vts->allocatedLba - vmg->allocatedLba);
                }
                fclose(input); if (!ok) return false;
            }
        }
        /* IFO与BUP均使用同一组扇区修正，主机上的原文件保持只读。 */
        bup->patches = (FsBytePatch*)malloc(file->patchCount * sizeof(*bup->patches));
        if (!bup->patches) return false;
        memcpy(bup->patches, file->patches, file->patchCount * sizeof(*bup->patches)); bup->patchCount = file->patchCount;
    }
    return true;
}

void fs_video_patch(const FsFileNode* file, uint64_t offset, uint8_t* data, uint32_t bytes) {
    for (size_t i = 0; i < file->patchCount; i++) {
        const FsBytePatch* patch = &file->patches[i];
        for (unsigned j = 0; j < 4; j++) if (patch->offset + j >= offset && patch->offset + j - offset < bytes) data[patch->offset + j - offset] = (uint8_t)(patch->value >> (24 - 8 * j));
    }
}
