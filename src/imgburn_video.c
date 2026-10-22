#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_fs_internal.h"
#include "imgburn_image.h"
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
    if ((b->config.dvdBreakPgc || b->config.dvdBreakTitleSet) && !b->config.dvdVideoCompliance) return false;
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
    if (!order && b->config.dvdBreakPgc && group == b->config.dvdBreakTitleSet) next += b->config.dvdBreakPadding;
    if (order == 11) {
        wchar_t ifoName[128]; wcscpy(ifoName, file->nameW); wcscpy(ifoName + wcslen(ifoName) - 3, L"IFO");
        FsFileNode* ifo = video_file(file->parent, ifoName);
        if (!ifo) return 0;
        uint64_t minimum = (uint64_t)ifo->allocatedLba + ifo->sectorCount + 16;
        if (next < minimum) next = minimum;
    }
    /* 原版的32KiB填充是IFO末尾到BUP开头的最小距离，并非每个视频文件的对齐。 */
    return next > UINT32_MAX ? 0 : (uint32_t)next;
}

static bool video_patch_add(FsFileNode* file, uint64_t offset, uint32_t value) {
    if (offset > file->fileSize || file->fileSize - offset < 4 || file->patchCount >= SIZE_MAX / sizeof(FsBytePatch) - 1) return false;
    FsBytePatch* grown = (FsBytePatch*)realloc(file->patches, (file->patchCount + 1) * sizeof(*grown));
    if (!grown) return false;
    file->patches = grown; file->patches[file->patchCount++] = (FsBytePatch){offset,value}; return true;
}

static bool dvd_finalize_break(ImageBuilder* b);

bool fs_video_finalize(ImageBuilder* b) {
    if (!b->config.dvdVideoCompliance) return true;
    FsDirectoryNode* dir = video_dir(b->rootDir, L"VIDEO_TS");
    FsFileNode* vmg = video_file(dir, L"VIDEO_TS.IFO");
    if (!dvd_finalize_break(b)) return false;
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

/* IFO内的偏移全部限制在所属表和PGC内，不以主机文件尾代替子表边界。 */
typedef struct { FILE* file; uint64_t bytes; } DvdReader;
static uint16_t dvd_be16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static unsigned dvd_bcd(uint8_t value) { return (value >> 4) * 10 + (value & 15); }
static uint32_t dvd_time_ms(const uint8_t* p) {
    unsigned rate = p[3] >> 6; if (rate != 1 && rate != 3) return 0;
    return ((dvd_bcd(p[0]) * 60 + dvd_bcd(p[1])) * 60 + dvd_bcd(p[2])) * 1000 + dvd_bcd(p[3] & 63) * (rate == 1 ? 40 : 1001) / (rate == 1 ? 1 : 30);
}
static bool dvd_read(DvdReader* r, uint64_t offset, void* data, size_t bytes) {
    return offset <= r->bytes && bytes <= r->bytes - offset && !_fseeki64(r->file, (int64_t)offset, SEEK_SET) && fread(data, 1, bytes, r->file) == bytes;
}
static bool dvd_table(DvdReader* r, uint32_t sector, unsigned headerBytes, uint64_t* begin, uint64_t* end, uint16_t* count) {
    uint8_t header[8]; *begin = (uint64_t)sector * VIDEO_BLOCK;
    if (!sector || !dvd_read(r, *begin, header, headerBytes)) return false;
    uint64_t bytes = (uint64_t)video_be32(header + headerBytes - 4) + 1;
    if (bytes < headerBytes || bytes > r->bytes - *begin) return false;
    *end = *begin + bytes; if (count) *count = dvd_be16(header); return true;
}
static bool dvd_subread(DvdReader* r, uint64_t begin, uint64_t end, uint32_t offset, void* data, size_t bytes) {
    return begin <= end && offset <= end - begin && bytes <= end - begin - offset && dvd_read(r, begin + offset, data, bytes);
}
static bool dvd_map_contains(DvdReader* r, uint64_t begin, uint64_t end, uint32_t sector) {
    uint64_t lo = 0, hi = (end - begin - 4) / 4; uint8_t data[4];
    while (lo < hi) { uint64_t mid = lo + (hi - lo) / 2; if (!dvd_read(r, begin + 4 + mid * 4, data, 4)) return false; if (video_be32(data) < sector) lo = mid + 1; else hi = mid; }
    return begin + 4 + lo * 4 < end && dvd_read(r, begin + 4 + lo * 4, data, 4) && video_be32(data) == sector;
}
static bool dvd_nav_packet(FsFileNode* const* parts, uint32_t sector, const DvdNavigationCell* cell) {
    uint32_t relative = sector; unsigned part = 0;
    while (part < 9 && parts[part] && relative >= parts[part]->sectorCount) { relative -= parts[part]->sectorCount; part++; }
    if (part == 9 || !parts[part]) return false;
    DvdReader r = {_wfopen(parts[part]->sourceDiskPath, L"rb"),parts[part]->fileSize}; if (!r.file) return false;
    uint8_t data[VIDEO_BLOCK]; bool ok = dvd_read(&r, (uint64_t)relative * VIDEO_BLOCK, data, sizeof(data)); fclose(r.file);
    if (!ok || memcmp(data, "\0\0\1\xBA", 4) || memcmp(data + 1024, "\0\0\1\xBF", 4) || data[1030] != 1) return false;
    /* DSI的包地址、VOB/Cell标识及VOBU末尾须与IFO指向的同一物理单元一致。 */
    const uint8_t* dsi = data + 1031;
    if (video_be32(dsi + 4) != sector || dvd_be16(dsi + 24) != cell->vobId || dsi[27] != cell->cellId || video_be32(dsi + 8) > cell->lastSector - sector) return false;
    if ((cell->category & 0x04000000) && sector == cell->firstSector) {
        if ((dvd_be16(dsi + 32) & 0x6000) != 0x6000 || video_be32(dsi + 34) != cell->firstIlvuEndSector - sector) return false;
    }
    return true;
}
static bool dvd_cell_append(DvdNavigation* nav, size_t* capacity, const DvdNavigationCell* cell) {
    if (nav->cellCount == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 256;
        if (next > 1048576 || next > SIZE_MAX / sizeof(*nav->cells)) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
        DvdNavigationCell* grown = (DvdNavigationCell*)realloc(nav->cells, next * sizeof(*grown)); if (!grown) return false;
        nav->cells = grown; *capacity = next;
    }
    nav->cells[nav->cellCount++] = *cell; return true;
}

static bool dvd_pgci_navigation(DvdReader* r, FsFileNode* const* parts, uint32_t vobSectors, uint64_t pgci, uint64_t pgciEnd, uint64_t adt, uint64_t adtEnd, uint64_t map, uint64_t mapEnd, unsigned titleSet, DvdNavigationDomain domain, unsigned languageUnit, uint16_t languageCode, DvdNavigation* nav, size_t* capacity) {
    uint8_t tableHeader[8];
    if (!dvd_subread(r, pgci, pgciEnd, 0, tableHeader, 8)) return false;
    unsigned pgcCount = dvd_be16(tableHeader); uint32_t offsets[999]; uint8_t menuTypes[999];
    if (!pgcCount || pgcCount > 999 || 8 + (uint64_t)pgcCount * 8 > pgciEnd - pgci) return false;
    for (unsigned i = 0; i < pgcCount; i++) {
        uint8_t entry[8]; if (!dvd_read(r, pgci + 8 + i * 8, entry, 8)) return false; offsets[i] = video_be32(entry + 4); menuTypes[i] = domain == DVD_DOMAIN_TITLE || !(entry[0] & 0x80) ? 0 : entry[0] & 15;
        if (offsets[i] < 8 + (uint32_t)pgcCount * 8 || offsets[i] > pgciEnd - pgci || pgciEnd - pgci - offsets[i] < 236) return false;
    }
    for (unsigned i = 0; i < pgcCount; i++) {
        uint64_t begin = pgci + offsets[i], end = pgciEnd;
        for (unsigned j = 0; j < pgcCount; j++) if (offsets[j] > offsets[i] && pgci + offsets[j] < end) end = pgci + offsets[j];
        uint8_t pgc[236], programs[255]; if (!dvd_subread(r, begin, end, 0, pgc, sizeof(pgc))) return false;
        unsigned programCount = pgc[2], cellCount = pgc[3], commands = dvd_be16(pgc + 0xE4), programMap = dvd_be16(pgc + 0xE6), playback = dvd_be16(pgc + 0xE8), positions = dvd_be16(pgc + 0xEA), cellCommands = 0;
        if (!cellCount && !programCount) {
            /* 只含命令的虚拟PGC不提供可切换单元，但仍检查其命令表边界。 */
            if (commands) {
                uint8_t counts[8]; if (commands < 236 || !dvd_subread(r, begin, end, commands, counts, 8)) return false;
                unsigned total = dvd_be16(counts) + dvd_be16(counts + 2) + dvd_be16(counts + 4);
                if (total > 128 || dvd_be16(counts + 6) + 1u < 8 + total * 8 || commands + (uint64_t)dvd_be16(counts + 6) + 1 > end - begin) return false;
            }
            continue;
        }
        if (!cellCount || !programCount || programCount > cellCount || programMap < 236 || playback < programMap + programCount || positions < playback + cellCount * 24 || !dvd_subread(r, begin, end, programMap, programs, programCount) || positions + (uint64_t)cellCount * 4 > end - begin) return false;
        if (programs[0] != 1) return false;
        for (unsigned j = 0; j < programCount; j++) if (programs[j] > cellCount || (j && programs[j] <= programs[j - 1])) return false;
        if (commands) {
            uint8_t counts[8]; if (commands < 236 || !dvd_subread(r, begin, end, commands, counts, 8)) return false;
            cellCommands = dvd_be16(counts + 4); unsigned total = dvd_be16(counts) + dvd_be16(counts + 2) + cellCommands;
            if (total > 128 || dvd_be16(counts + 6) + 1u < 8 + total * 8 || commands + (uint64_t)dvd_be16(counts + 6) + 1 > programMap) return false;
        }
        unsigned program = 0, angleCells = 0, angleFirst = 0; bool previousCommand = false;
        for (unsigned j = 0; j < cellCount; j++) {
            uint8_t cp[24], position[4];
            if (!dvd_subread(r, begin, end, playback + j * 24, cp, 24) || !dvd_subread(r, begin, end, positions + j * 4, position, 4) || cp[3] > cellCommands) return false;
            /* 角度块必须以首单元开始并以末单元闭合，最多9个角度。 */
            unsigned blockMode = cp[0] >> 6, blockType = (cp[0] >> 4) & 3;
            if (!blockType) { if (blockMode || angleCells) return false; }
            else if (blockType != 1 || !blockMode) return false;
            else if (blockMode == 1) { if (angleCells) return false; angleCells = 1; angleFirst = j + 1; }
            else { if (!angleCells || ++angleCells > 9) return false; if (blockMode == 3) angleCells = 0; }
            while (program + 1 < programCount && programs[program + 1] <= j + 1) program++;
            DvdNavigationCell cell = {0}; cell.titleSet = (uint16_t)titleSet; cell.domain = domain; cell.languageUnit = (uint16_t)languageUnit; cell.languageCode = languageCode; cell.menuType = menuTypes[i]; cell.pgc = (uint16_t)(i + 1); cell.cell = (uint16_t)(j + 1); cell.program = (uint16_t)(program + 1);
            cell.angleBlockFirstCell = blockType ? (uint16_t)angleFirst : 0;
            cell.playbackMilliseconds = dvd_time_ms(cp + 4);
            cell.vobId = dvd_be16(position); cell.cellId = position[3]; cell.category = video_be32(cp); cell.ifoOffset = (uint32_t)(begin + playback + j * 24); cell.previousCellCommand = previousCommand;
            cell.firstIlvuEndSector = video_be32(cp + 12); cell.firstSector = video_be32(cp + 8); cell.lastVobuSector = video_be32(cp + 16); cell.lastSector = video_be32(cp + 20);
            if (!cell.vobId || !cell.cellId || cell.firstSector > cell.lastVobuSector || cell.lastVobuSector > cell.lastSector || cell.lastSector >= vobSectors || parts[0]->allocatedLba > UINT32_MAX - cell.firstSector) return false;
            if ((cp[0] & 4) && (cell.firstIlvuEndSector < cell.firstSector || cell.firstIlvuEndSector > cell.lastSector)) return false;
            cell.lba = parts[0]->allocatedLba + cell.firstSector; previousCommand = cp[3] != 0;
            bool found = false;
            for (uint64_t pos = adt + 8; pos < adtEnd; pos += 12) {
                uint8_t entry[12]; if (!dvd_read(r, pos, entry, 12)) return false;
                if (dvd_be16(entry) == cell.vobId && entry[2] == cell.cellId && video_be32(entry + 4) == cell.firstSector && video_be32(entry + 8) == cell.lastSector) { found = true; break; }
            }
            if (!found || !dvd_map_contains(r, map, mapEnd, cell.firstSector) || !dvd_map_contains(r, map, mapEnd, cell.lastVobuSector) || !dvd_nav_packet(parts, cell.firstSector, &cell) || !dvd_nav_packet(parts, cell.lastVobuSector, &cell) || !dvd_cell_append(nav, capacity, &cell)) return false;
        }
        if (angleCells) return false;
    }
    return true;
}

static bool dvd_title_chapters(DvdReader* r, const uint8_t* header, DvdNavigation* nav, size_t firstCell) {
    uint64_t ptt, pttEnd; uint16_t titles;
    if (!dvd_table(r, video_be32(header + 0xC8), 8, &ptt, &pttEnd, &titles) || !titles || titles > 99 || 8 + (uint64_t)titles * 4 > pttEnd - ptt) return false;
    for (unsigned i = 0; i < titles; i++) {
        uint8_t entry[4]; if (!dvd_read(r, ptt + 8 + i * 4, entry, 4)) return false;
        uint32_t offset = video_be32(entry); uint64_t end = pttEnd;
        if (i + 1 < titles) { if (!dvd_read(r, ptt + 12 + i * 4, entry, 4)) return false; end = ptt + video_be32(entry); }
        if (offset < 8 + (uint32_t)titles * 4 || end > pttEnd || end <= ptt + offset || (end - ptt - offset) % 4 || (end - ptt - offset) / 4 > 999) return false;
        for (uint64_t pos = ptt + offset; pos < end; pos += 4) {
            if (!dvd_read(r, pos, entry, 4)) return false; unsigned pgc = dvd_be16(entry), program = dvd_be16(entry + 2); bool found = false;
            for (size_t j = firstCell; j < nav->cellCount; j++) {
                DvdNavigationCell* cell = &nav->cells[j];
                if (cell->pgc != pgc || cell->program != program) continue;
                if (!cell->chapterStart) { cell->chapterStart = true; cell->title = (uint16_t)(i + 1); cell->chapter = (uint16_t)((pos - ptt - offset) / 4 + 1); }
                found = true; break;
            }
            if (!found) return false;
        }
    }
    return true;
}

static bool dvd_domain_navigation(FsDirectoryNode* dir, FsFileNode* ifo, unsigned titleSet, DvdNavigationDomain domain, DvdNavigation* nav, size_t* capacity) {
    DvdReader r = {_wfopen(ifo->sourceDiskPath, L"rb"),ifo->fileSize}; if (!r.file) return false;
    bool ok = false; uint8_t header[2048]; FsFileNode* parts[9] = {0}; uint64_t vobSectors = 0;
    uint64_t pgci, pgciEnd, adt = 0, adtEnd = 0, map = 0, mapEnd = 0; uint16_t count;
    bool menu = domain != DVD_DOMAIN_TITLE; size_t firstCell = nav->cellCount;
    if (!dvd_read(&r, 0, header, sizeof(header)) || memcmp(header, titleSet ? "DVDVIDEO-VTS" : "DVDVIDEO-VMG", 12)) goto done;
    uint32_t tableSector = video_be32(header + (domain == DVD_DOMAIN_TITLE ? 0xCC : titleSet ? 0xD0 : 0xC8));
    if (menu && !tableSector) { ok = true; goto done; }
    for (unsigned i = 0; i < (menu ? 1u : 9u); i++) {
        wchar_t name[16];
        if (!titleSet) wcscpy(name, L"VIDEO_TS.VOB"); else swprintf(name, 16, L"VTS_%02u_%u.VOB", titleSet, menu ? 0 : i + 1);
        parts[i] = video_file(dir, name); if (!parts[i]) break;
        if (i && parts[i]->allocatedLba != parts[i - 1]->allocatedLba + parts[i - 1]->sectorCount) goto done;
        vobSectors += parts[i]->sectorCount;
    }
    if (vobSectors > UINT32_MAX || (!menu && !vobSectors)) goto done;
    if (vobSectors) {
        if (!dvd_table(&r, video_be32(header + (menu ? 0xD8 : 0xE0)), 8, &adt, &adtEnd, NULL) || (adtEnd - adt - 8) % 12 || !dvd_table(&r, video_be32(header + (menu ? 0xDC : 0xE4)), 4, &map, &mapEnd, NULL) || (mapEnd - map - 4) % 4) goto done;
        uint32_t previous = 0;
        for (uint64_t pos = map + 4; pos < mapEnd; pos += 4) {
            uint8_t entry[4]; if (!dvd_read(&r, pos, entry, 4)) goto done; uint32_t sector = video_be32(entry);
            if (sector >= vobSectors || (pos > map + 4 && sector <= previous)) goto done; previous = sector;
        }
    }
    if (!dvd_table(&r, tableSector, 8, &pgci, &pgciEnd, &count)) goto done;
    if (!menu) {
        if (!dvd_pgci_navigation(&r, parts, (uint32_t)vobSectors, pgci, pgciEnd, adt, adtEnd, map, mapEnd, titleSet, domain, 0, 0, nav, capacity) || !dvd_title_chapters(&r, header, nav, firstCell)) goto done;
    } else {
        uint32_t offsets[99]; uint16_t languages[99];
        if (!count || count > 99 || 8 + (uint64_t)count * 8 > pgciEnd - pgci) goto done;
        for (unsigned i = 0; i < count; i++) {
            uint8_t entry[8]; if (!dvd_read(&r, pgci + 8 + i * 8, entry, 8)) goto done;
            offsets[i] = video_be32(entry + 4); languages[i] = dvd_be16(entry);
            if (offsets[i] < 8 + (uint32_t)count * 8 || offsets[i] > pgciEnd - pgci || pgciEnd - pgci - offsets[i] < 8) goto done;
        }
        for (unsigned i = 0; i < count; i++) {
            uint64_t begin = pgci + offsets[i], limit = pgciEnd; uint8_t entry[8];
            for (unsigned j = 0; j < count; j++) if (offsets[j] > offsets[i] && pgci + offsets[j] < limit) limit = pgci + offsets[j];
            if (!dvd_subread(&r, begin, limit, 0, entry, 8)) goto done;
            uint64_t bytes = (uint64_t)video_be32(entry + 4) + 1;
            /* LU的PGC表还受下一个LU起点约束，不能跨语言单元读取。 */
            if (bytes < 8 || bytes > limit - begin || !dvd_pgci_navigation(&r, parts, (uint32_t)vobSectors, begin, begin + bytes, adt, adtEnd, map, mapEnd, titleSet, domain, i + 1, languages[i], nav, capacity)) goto done;
        }
    }
    ok = true;
done:
    fclose(r.file); return ok;
}

void imgburn_dvd_free_navigation(DvdNavigation* navigation) { if (navigation) { free(navigation->cells); memset(navigation, 0, sizeof(*navigation)); } }

bool imgburn_dvd_read_navigation(const ImageBuilder* b, DvdNavigation* navigation) {
    if (!navigation) { SetLastError(ERROR_INVALID_PARAMETER); return false; } memset(navigation, 0, sizeof(*navigation));
    if (!b || !b->rootDir || !b->config.dvdVideoCompliance || !b->config.totalImageSectors) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    FsDirectoryNode* dir = video_dir(b->rootDir, L"VIDEO_TS"); size_t capacity = 0;
    if (!dir) { SetLastError(ERROR_INVALID_DATA); return false; }
    for (FsFileNode* file = dir->firstFile; file; file = file->next) if (!file->excluded) {
        unsigned group, order;
        if (dvd_name(file->nameW, &group, &order) && !order && ((!dvd_domain_navigation(dir, file, group, group ? DVD_DOMAIN_VTS_MENU : DVD_DOMAIN_VMG_MENU, navigation, &capacity)) || (group && !dvd_domain_navigation(dir, file, group, DVD_DOMAIN_TITLE, navigation, &capacity)))) { imgburn_dvd_free_navigation(navigation); SetLastError(ERROR_INVALID_DATA); return false; }
    }
    if (!navigation->cellCount) { SetLastError(ERROR_INVALID_DATA); return false; }
    return true;
}

static bool dvd_profile_valid(const DvdMediaProfile* p) { return p && p->maximumL0 && p->minimumL0 <= p->maximumL0 && p->maximumL0 <= 2086912 && p->mediaCapacity && p->mediaCapacity <= 4173824; }
bool imgburn_dvd_layer_break_candidates(const ImageBuilder* b, const DvdMediaProfile* profile, DvdLayerBreakCandidate* candidates, size_t capacity, size_t* count) {
    return imgburn_dvd_layer_break_candidates_ex(b, profile, 0, candidates, capacity, count);
}

bool imgburn_dvd_layer_break_candidates_ex(const ImageBuilder* b, const DvdMediaProfile* profile, unsigned options, DvdLayerBreakCandidate* candidates, size_t capacity, size_t* count) {
    if (count) *count = 0;
    if (!count || (!candidates && capacity) || !dvd_profile_valid(profile) || !b || b->config.dvdBreakPgc || (options & ~DVD_LAYER_SHOW_INTERLEAVED)) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    DvdNavigation nav; if (!imgburn_dvd_read_navigation(b, &nav)) return false;
    size_t found = 0;
    for (size_t i = 0; i < nav.cellCount; i++) {
        const DvdNavigationCell* cell = &nav.cells[i];
        /* 与原版的“Show Interleaved Cells”一致，默认隐藏角度及交错单元。 */
        if ((((cell->category >> 24) & 0xF4) && !(options & DVD_LAYER_SHOW_INTERLEAVED)) || !cell->lba || cell->lba >= b->config.totalImageSectors) continue;
        /* 同一角度块只列首角度；其余角度的交错起点不是独立切换候选。 */
        if ((cell->category & 0x30000000) && (cell->category >> 30) != 1) continue;
        uint32_t l1 = b->config.totalImageSectors - cell->lba;
        uint64_t l0 = cell->lba; if (l0 < l1) l0 = l1; if (l0 < profile->minimumL0) l0 = profile->minimumL0; l0 = (l0 + 15) & ~15ull;
        uint64_t total = l0 + l1;
        if (l0 > profile->maximumL0 || total > profile->mediaCapacity) continue;
        if (found < capacity) {
            DvdLayerBreakCandidate candidate = {0}; candidate.lba = (uint32_t)l0; candidate.vobId = cell->vobId; candidate.cellId = cell->cellId;
            candidate.isSeamless = (cell->category & 0x08000000) != 0; candidate.isChapterStart = cell->chapterStart; candidate.titleSet = cell->titleSet; candidate.pgc = cell->pgc; candidate.cell = cell->cell; candidate.program = cell->program; candidate.title = cell->title; candidate.chapter = cell->chapter;
            candidate.vobSector = cell->firstSector; candidate.paddingSectors = (uint32_t)l0 - cell->lba; candidate.imageSectors = (uint32_t)total; candidate.seamlessAllowed = !cell->previousCellCommand;
            candidate.domain = cell->domain; candidate.languageUnit = cell->languageUnit; candidate.languageCode = cell->languageCode; candidate.menuType = cell->menuType;
            candidate.isInterleaved = (cell->category & 0x04000000) != 0; candidate.isAngleBlock = (cell->category & 0x30000000) != 0;
            sprintf(candidate.description, "VTS_%02u %s LU %u PGC %u Cell %u", cell->titleSet, cell->domain == DVD_DOMAIN_TITLE ? "Title" : "Menu", cell->languageUnit, cell->pgc, cell->cell); candidates[found] = candidate;
        }
        found++;
    }
    imgburn_dvd_free_navigation(&nav); *count = found;
    if (found > capacity) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return false; } return true;
}

bool imgburn_dvd_select_layer_break(ImageBuilder* b, const DvdMediaProfile* profile, const DvdLayerBreakCandidate* candidate, bool seamless) {
    if (!b || !candidate || !dvd_profile_valid(profile) || (unsigned)candidate->domain > DVD_DOMAIN_VMG_MENU || candidate->titleSet > 99 || (!candidate->titleSet && candidate->domain != DVD_DOMAIN_VMG_MENU) || (candidate->titleSet && candidate->domain == DVD_DOMAIN_VMG_MENU) || (candidate->domain == DVD_DOMAIN_TITLE ? candidate->languageUnit != 0 : !candidate->languageUnit) || !candidate->pgc || !candidate->cell || (seamless && !candidate->seamlessAllowed)) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    b->config.dvdBreakTitleSet = candidate->titleSet; b->config.dvdBreakPgc = candidate->pgc; b->config.dvdBreakCell = candidate->cell; b->config.dvdBreakSeamless = seamless;
    b->config.dvdBreakDomain = candidate->domain; b->config.dvdBreakLanguageUnit = candidate->languageUnit; b->config.dvdBreakInterleaved = candidate->isInterleaved || candidate->isAngleBlock;
    b->config.dvdBreakPadding = candidate->paddingSectors; b->config.dvdLayerBreakLba = candidate->lba; b->config.dvdMinimumL0 = profile->minimumL0; b->config.dvdMaximumL0 = profile->maximumL0; b->config.dvdMediaCapacity = profile->mediaCapacity; return true;
}

static bool dvd_finalize_break(ImageBuilder* b) {
    if (!b->config.dvdBreakPgc) return !b->config.dvdBreakTitleSet && !b->config.dvdBreakCell && !b->config.dvdBreakLanguageUnit && !b->config.dvdBreakDomain && !b->config.dvdBreakInterleaved && !b->config.dvdBreakPadding && !b->config.dvdLayerBreakLba;
    DvdMediaProfile profile = {b->config.dvdMinimumL0,b->config.dvdMaximumL0,b->config.dvdMediaCapacity};
    uint32_t l0 = b->config.dvdLayerBreakLba;
    if (!dvd_profile_valid(&profile) || !l0 || l0 % 16 || l0 < profile.minimumL0 || l0 > profile.maximumL0 || b->config.totalImageSectors <= l0 || b->config.totalImageSectors - l0 > l0 || b->config.totalImageSectors > profile.mediaCapacity) return false;
    DvdNavigation nav; if (!imgburn_dvd_read_navigation(b, &nav)) return false;
    const DvdNavigationCell* selected = NULL; bool ok = true;
    for (size_t i = 0; i < nav.cellCount; i++) if (nav.cells[i].titleSet == b->config.dvdBreakTitleSet && nav.cells[i].domain == b->config.dvdBreakDomain && nav.cells[i].languageUnit == b->config.dvdBreakLanguageUnit && nav.cells[i].pgc == b->config.dvdBreakPgc && nav.cells[i].cell == b->config.dvdBreakCell) selected = &nav.cells[i];
    if (!selected || selected->lba != l0 || (((selected->category >> 24) & 0xF4) && !b->config.dvdBreakInterleaved)) ok = false;
    if (selected && (selected->category & 0x30000000) && (selected->category >> 30) != 1) ok = false;
    const DvdNavigationCell* affected[9]; size_t affectedCount = 0;
    if (ok) {
        affected[affectedCount++] = selected;
        /* 原版在角度块的首单元切换时，同步更新整个角度块以及它们的其他PGC引用。 */
        if (selected->angleBlockFirstCell) for (size_t i = 0; i < nav.cellCount; i++) {
            const DvdNavigationCell* cell = &nav.cells[i];
            if (cell != selected && cell->domain == selected->domain && cell->titleSet == selected->titleSet && cell->languageUnit == selected->languageUnit && cell->pgc == selected->pgc && cell->angleBlockFirstCell == selected->angleBlockFirstCell) {
                if (affectedCount == 9) { ok = false; break; } affected[affectedCount++] = cell;
            }
        }
    }
    for (size_t i = 0; ok && i < nav.cellCount; i++) {
        const DvdNavigationCell* cell = &nav.cells[i];
        if (cell->titleSet != selected->titleSet || cell->domain != selected->domain) continue;
        bool matches = false;
        for (size_t j = 0; j < affectedCount; j++) if (cell->vobId == affected[j]->vobId && cell->cellId == affected[j]->cellId && cell->firstSector == affected[j]->firstSector) { matches = true; break; }
        if (!matches) continue;
        if (b->config.dvdBreakSeamless && cell->previousCellCommand) { ok = false; break; }
        wchar_t name[16]; if (cell->titleSet) swprintf(name, 16, L"VTS_%02u_0.IFO", cell->titleSet); else wcscpy(name, L"VIDEO_TS.IFO"); FsFileNode* ifo = video_file(video_dir(b->rootDir, L"VIDEO_TS"), name);
        uint32_t category = b->config.dvdBreakSeamless ? cell->category | 0x08000000 : cell->category & ~0x08000000u;
        if (!ifo || !video_patch_add(ifo, cell->ifoOffset, category)) ok = false;
    }
    imgburn_dvd_free_navigation(&nav); return ok;
}

bool imgburn_dvd_preview_plan(const ImageBuilder* b, DvdNavigationDomain domain, unsigned titleSet, unsigned languageUnit, unsigned pgc, unsigned cellNumber, DvdPreviewPlan* plan) {
    if (!plan) { SetLastError(ERROR_INVALID_PARAMETER); return false; } memset(plan, 0, sizeof(*plan));
    DvdNavigation nav; if (!imgburn_dvd_read_navigation(b, &nav)) return false;
    const DvdNavigationCell* selected = NULL; const DvdNavigationCell* previous = NULL;
    for (size_t i = 0; i < nav.cellCount; i++) {
        const DvdNavigationCell* c = &nav.cells[i];
        if (c->domain == domain && c->titleSet == titleSet && c->languageUnit == languageUnit && c->pgc == pgc && c->cell == cellNumber) selected = c;
    }
    if (!selected) { imgburn_dvd_free_navigation(&nav); SetLastError(ERROR_NOT_FOUND); return false; }
    unsigned previousNumber = selected->angleBlockFirstCell ? selected->angleBlockFirstCell - 1 : selected->cell - 1;
    for (size_t i = 0; i < nav.cellCount; i++) {
        const DvdNavigationCell* c = &nav.cells[i];
        if (c->domain == domain && c->titleSet == titleSet && c->languageUnit == languageUnit && c->pgc == pgc && c->cell == previousNumber) previous = c;
    }
    if (previous && previous->angleBlockFirstCell) for (size_t i = 0; i < nav.cellCount; i++) {
        const DvdNavigationCell* c = &nav.cells[i];
        if (c->domain == domain && c->titleSet == titleSet && c->languageUnit == languageUnit && c->pgc == pgc && c->cell == previous->angleBlockFirstCell) { previous = c; break; }
    }
    if (previous) plan->cells[plan->cellCount++] = *previous;
    plan->selectedIndex = plan->cellCount; plan->cells[plan->cellCount++] = *selected;
    wchar_t name[16]; if (titleSet) swprintf(name, 16, L"VTS_%02u_0.IFO", titleSet); else wcscpy(name, L"VIDEO_TS.IFO");
    FsFileNode* ifo = video_file(video_dir(b->rootDir, L"VIDEO_TS"), name); uint8_t header[2048];
    bool ok = ifo && video_header(ifo, header, sizeof(header));
    if (ok) {
        const uint8_t* attrs = header + (domain == DVD_DOMAIN_TITLE ? 0x200 : 0x100);
        bool pal = ((attrs[0] >> 4) & 3) == 1; unsigned resolution = (attrs[1] >> 3) & 7;
        if (((attrs[0] >> 4) & 3) > 1 || resolution > 3) ok = false;
        plan->width = resolution == 0 ? 720 : resolution == 1 ? 704 : 352; plan->height = pal ? 576 : 480; if (resolution == 3) plan->height /= 2;
        plan->aspectNumerator = (attrs[0] & 12) == 12 ? 16 : 4; plan->aspectDenominator = (attrs[0] & 12) == 12 ? 9 : 3;
        plan->frameRateNumerator = pal ? 25 : 30000; plan->frameRateDenominator = pal ? 1 : 1001;
    }
    imgburn_dvd_free_navigation(&nav); if (!ok) { memset(plan, 0, sizeof(*plan)); SetLastError(ERROR_INVALID_DATA); } return ok;
}

typedef struct { HANDLE handles[9]; FsFileNode* parts[9]; unsigned count; } DvdVideoInput;
static bool dvd_input_read(DvdVideoInput* input, uint64_t offset, void* data, DWORD bytes) {
    uint8_t* target = (uint8_t*)data; unsigned part = 0;
    while (part < input->count && offset >= input->parts[part]->fileSize) { offset -= input->parts[part]->fileSize; part++; }
    while (bytes) {
        if (part == input->count) return false;
        DWORD take = input->parts[part]->fileSize - offset < bytes ? (DWORD)(input->parts[part]->fileSize - offset) : bytes, got; LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)offset;
        if (!take || !SetFilePointerEx(input->handles[part], pos, NULL, FILE_BEGIN) || !ReadFile(input->handles[part], target, take, &got, NULL) || got != take) return false;
        target += take; bytes -= take; part++; offset = 0;
    }
    return true;
}

bool imgburn_dvd_read_cell(const ImageBuilder* b, const DvdNavigationCell* requested, DvdVideoWriteCallback write, void* context, uint64_t* bytesWritten) {
    if (bytesWritten) *bytesWritten = 0;
    if (!requested || !write) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    DvdNavigation nav; if (!imgburn_dvd_read_navigation(b, &nav)) return false;
    const DvdNavigationCell* cell = NULL;
    for (size_t i = 0; i < nav.cellCount; i++) {
        const DvdNavigationCell* c = &nav.cells[i];
        if (c->domain == requested->domain && c->titleSet == requested->titleSet && c->languageUnit == requested->languageUnit && c->pgc == requested->pgc && c->cell == requested->cell) { cell = c; break; }
    }
    if (!cell) { imgburn_dvd_free_navigation(&nav); SetLastError(ERROR_NOT_FOUND); return false; }
    DvdVideoInput input = {0}; bool ok = true; uint64_t written = 0; uint8_t* buffer = (uint8_t*)malloc(65536);
    FsDirectoryNode* dir = video_dir(b->rootDir, L"VIDEO_TS");
    for (unsigned i = 0; ok && i < (cell->domain == DVD_DOMAIN_TITLE ? 9u : 1u); i++) {
        wchar_t name[16]; if (!cell->titleSet) wcscpy(name, L"VIDEO_TS.VOB"); else swprintf(name, 16, L"VTS_%02u_%u.VOB", cell->titleSet, cell->domain == DVD_DOMAIN_TITLE ? i + 1 : 0);
        FsFileNode* part = video_file(dir, name); if (!part) break;
        HANDLE h = CreateFileW(part->sourceDiskPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (h == INVALID_HANDLE_VALUE) { ok = false; break; }
        input.handles[input.count] = h; input.parts[input.count++] = part; BY_HANDLE_FILE_INFORMATION info;
        ok = GetFileInformationByHandle(h, &info) && ((uint64_t)info.nFileSizeHigh << 32 | info.nFileSizeLow) == part->fileSize && CompareFileTime(&info.ftLastWriteTime, &part->modTime) == 0;
    }
    uint32_t sector = cell->firstSector;
    if (!buffer || !input.count) ok = false;
    while (ok) {
        if (!dvd_input_read(&input, (uint64_t)sector * VIDEO_BLOCK, buffer, VIDEO_BLOCK) || memcmp(buffer, "\0\0\1\xBA", 4) || memcmp(buffer + 1024, "\0\0\1\xBF", 4) || buffer[1030] != 1) { ok = false; break; }
        const uint8_t* dsi = buffer + 1031; uint32_t end = video_be32(dsi + 8), next = video_be32(dsi + 0x13A) & 0x3FFFFFFF;
        if (video_be32(dsi + 4) != sector || dvd_be16(dsi + 24) != cell->vobId || dsi[27] != cell->cellId || end > cell->lastSector - sector || (sector == cell->lastVobuSector && sector + end != cell->lastSector)) { ok = false; break; }
        uint64_t offset = (uint64_t)sector * VIDEO_BLOCK, left = ((uint64_t)end + 1) * VIDEO_BLOCK;
        while (ok && left) {
            DWORD take = left < 65536 ? (DWORD)left : 65536;
            ok = dvd_input_read(&input, offset, buffer, take) && write(buffer, take, context); if (!ok) break;
            written += take; offset += take; left -= take;
        }
        if (!ok || sector == cell->lastVobuSector) break;
        if (cell->category & 0x04000000) {
            /* SRI指向同一角度/故事的下一个VOBU，跳过物理上交错的其他单元。 */
            if (!next || next == 0x3FFFFFFF || next > cell->lastVobuSector - sector) { ok = false; break; } sector += next;
        } else {
            if (end >= cell->lastVobuSector - sector) { ok = false; break; } sector += end + 1;
        }
    }
    for (unsigned i = 0; i < 9; i++) if (input.handles[i]) CloseHandle(input.handles[i]); free(buffer); imgburn_dvd_free_navigation(&nav);
    if (ok && bytesWritten) *bytesWritten = written; if (!ok && !GetLastError()) SetLastError(ERROR_INVALID_DATA); return ok;
}
