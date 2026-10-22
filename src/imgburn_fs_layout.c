#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_fs_internal.h"
#include "imgburn_udf.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define BLOCK 2048u
#define MAX_EXTENT 0x3FFFF800u
#define ISO_SECTION 0xFFFFF800u
_Static_assert(sizeof(UdfExtendedFileEntry) == 216, "UDF extended file entry size");

typedef struct {
    FsDirectoryNode* dir;
    FsFileNode* file;
    const wchar_t* original;
    char* iso;
    wchar_t* joliet;
} TreeEntry;

typedef struct {
    FsDirectoryNode** dirs;
    FsDirectoryNode** jolietDirs;
    FsFileNode** files;
    size_t dirCount, fileCount;
    uint32_t terminator, partition, partitionBlocks, metadata, metadataBlocks, mirrorFe, mirrorData;
    bool metadataPartition;
    FILETIME created;
    ImageBuildConfig configuration;
    uint32_t reserveVds;
} Layout;

static uint32_t blocks(uint64_t bytes) { return (uint32_t)((bytes + BLOCK - 1) / BLOCK); }
static uint32_t align32(uint32_t value) { return (value + 31u) & ~31u; }
static void le16(uint8_t* out, uint16_t value) { out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8); }
static void le32(uint8_t* out, uint32_t value) { for (int i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (i * 8)); }
static void be16(uint8_t* out, uint16_t value) { out[0] = (uint8_t)(value >> 8); out[1] = (uint8_t)value; }
static void be32(uint8_t* out, uint32_t value) { for (int i = 0; i < 4; i++) out[i] = (uint8_t)(value >> (24 - i * 8)); }
static void both16(uint8_t* out, uint16_t value) { le16(out, value); be16(out + 2, value); }
static void both32(uint8_t* out, uint32_t value) { le32(out, value); be32(out + 4, value); }
static void ucs2(uint8_t* out, const wchar_t* text) { for (size_t i = 0; text[i]; i++) be16(out + i * 2, (uint16_t)text[i]); }

static bool put(HANDLE output, uint32_t lba, const void* buffer, uint32_t length) {
    LARGE_INTEGER offset; offset.QuadPart = (LONGLONG)lba * BLOCK;
    if (!SetFilePointerEx(output, offset, NULL, FILE_BEGIN)) return false;
    const uint8_t* cursor = (const uint8_t*)buffer;
    while (length) {
        DWORD actual = 0;
        if (!WriteFile(output, cursor, length, &actual, NULL) || !actual || actual > length) return false;
        cursor += actual; length -= actual;
    }
    return true;
}

void fs_layout_free(ImageBuilder* builder) {
    Layout* plan = (Layout*)builder->layout;
    if (plan) {
        for (size_t i = 0; plan->files && i < plan->fileCount; i++) if (plan->files[i]) { free(plan->files[i]->patches); plan->files[i]->patches = NULL; plan->files[i]->patchCount = 0; }
        free(plan->dirs); free(plan->jolietDirs); free(plan->files); free(plan);
    }
    builder->layout = NULL;
    builder->config.totalImageSectors = 0;
}

static int original_order(const void* a, const void* b) { return wcscmp(((const TreeEntry*)a)->original, ((const TreeEntry*)b)->original); }
static int iso_order(const void* a, const void* b) { return strcmp(((const TreeEntry*)a)->iso, ((const TreeEntry*)b)->iso); }
static int joliet_order(const void* a, const void* b) { return wcscmp(((const TreeEntry*)a)->joliet, ((const TreeEntry*)b)->joliet); }

static TreeEntry* entries(FsDirectoryNode* dir, size_t* count) {
    *count = 0;
    for (FsDirectoryNode* d = dir->firstChildDir; d; d = d->nextSibling) if (!d->excluded) (*count)++;
    for (FsFileNode* f = dir->firstFile; f; f = f->next) if (!f->excluded) (*count)++;
    if (*count > SIZE_MAX / sizeof(TreeEntry)) return NULL;
    TreeEntry* result = (TreeEntry*)calloc(*count ? *count : 1, sizeof(TreeEntry));
    if (!result) return NULL;
    size_t index = 0;
    for (FsDirectoryNode* d = dir->firstChildDir; d; d = d->nextSibling) if (!d->excluded) result[index++] = (TreeEntry){d, NULL, d->nameW, d->nameA, d->jolietNameW};
    for (FsFileNode* f = dir->firstFile; f; f = f->next) if (!f->excluded) result[index++] = (TreeEntry){NULL, f, f->nameW, f->nameA, f->jolietNameW};
    return result;
}

/* ISO Level 2与Joliet分别生成有界且唯一的别名，UDF保留原名。 */
static bool name_entries(FsDirectoryNode* dir) {
    size_t count;
    TreeEntry* list = entries(dir, &count);
    if (!list) return false;
    qsort(list, count, sizeof(*list), original_order);
    for (size_t i = 0; i < count; i++) {
        uint8_t encoded[255];
        if (!list[i].original[0] || !udf_encode_name(list[i].original, encoded, sizeof(encoded)) || (i && wcscmp(list[i - 1].original, list[i].original) == 0)) { free(list); return false; }
        char base[128];
        size_t length = wcslen(list[i].original);
        if (length >= sizeof(base)) { free(list); return false; }
        for (size_t j = 0; j < length; j++) {
            wchar_t c = list[i].original[j];
            if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
            base[j] = (char)((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || (c == L'.' && list[i].file) ? c : L'_');
        }
        base[length] = 0;
        /* 多个点只保留最后一个作为文件扩展名分隔符。 */
        char* dot = strrchr(base, '.');
        for (char* c = base; *c; c++) if (*c == '.' && c != dot) *c = '_';
        char extension[16] = "";
        if (dot && list[i].file) { strncpy(extension, dot + 1, 12); extension[12] = 0; *dot = 0; }
        if (!base[0]) strcpy(base, "_");
        for (size_t serial = 0;; serial++) {
            char suffix[24] = "";
            if (serial) snprintf(suffix, sizeof(suffix), "_%zu", serial);
            size_t extra = strlen(suffix) + (extension[0] ? strlen(extension) + 1 : 0);
            if (extra >= 31) { free(list); return false; }
            snprintf(list[i].iso, 128, "%.*s%s%s%s%s", (int)(31 - extra), base, suffix, extension[0] ? "." : "", extension, list[i].file ? ";1" : "");
            bool collision = false;
            for (size_t j = 0; j < i; j++) if (strcmp(list[j].iso, list[i].iso) == 0) collision = true;
            if (!collision) break;
        }
        for (size_t serial = 0;; serial++) {
            wchar_t suffix[24] = L"";
            if (serial) swprintf(suffix, 24, L"_%zu", serial);
            size_t tail = wcslen(suffix), available = 64 - tail;
            if (tail >= 64) { free(list); return false; }
            wcsncpy(list[i].joliet, list[i].original, available);
            list[i].joliet[available] = 0;
            wcscat(list[i].joliet, suffix);
            bool collision = false;
            for (size_t j = 0; j < i; j++) if (_wcsicmp(list[j].joliet, list[i].joliet) == 0) collision = true;
            if (!collision) break;
        }
    }
    free(list);
    return true;
}

static bool enumerate(Layout* plan, FsDirectoryNode* root, bool joliet) {
    FsDirectoryNode** dirs = joliet ? plan->jolietDirs : plan->dirs;
    size_t used = 1, fileIndex = 0;
    dirs[0] = root;
    for (size_t index = 0; index < used; index++) {
        FsDirectoryNode* dir = dirs[index];
        if (!joliet && !name_entries(dir)) return false;
        if (joliet) dir->dirIndexJoliet = (uint16_t)(index + 1);
        else dir->dirIndex = (uint16_t)(index + 1);
        size_t count;
        TreeEntry* list = entries(dir, &count);
        if (!list) return false;
        qsort(list, count, sizeof(*list), joliet ? joliet_order : iso_order);
        for (size_t i = 0; i < count; i++) {
            if (list[i].dir) {
                if (used >= plan->dirCount) { free(list); return false; }
                dirs[used++] = list[i].dir;
            } else if (!joliet) {
                if (fileIndex >= plan->fileCount) { free(list); return false; }
                plan->files[fileIndex++] = list[i].file;
            }
        }
        free(list);
    }
    return used == plan->dirCount && (joliet || fileIndex == plan->fileCount);
}

static uint32_t record_offset(uint32_t position, uint32_t length) {
    if (position % BLOCK + length > BLOCK) position += BLOCK - position % BLOCK;
    return position;
}

static uint32_t dir_size(FsDirectoryNode* dir, bool joliet) {
    size_t count;
    TreeEntry* list = entries(dir, &count);
    if (!list) return 0;
    qsort(list, count, sizeof(*list), joliet ? joliet_order : iso_order);
    uint64_t position = 68;
    for (size_t i = 0; i < count; i++) {
        uint32_t length = (uint32_t)(33 + (joliet ? wcslen(list[i].joliet) * 2 : strlen(list[i].iso)));
        length = (length + 1) & ~1u;
        uint64_t records = list[i].file && list[i].file->fileSize ? (list[i].file->fileSize + ISO_SECTION - 1) / ISO_SECTION : 1;
        while (records--) {
            if (position > UINT32_MAX - BLOCK) { free(list); return 0; }
            position = record_offset((uint32_t)position, length) + length;
        }
    }
    free(list);
    return blocks(position);
}

/* FID允许跨块，描述符标签的16字节必须完整落在同一逻辑块内。 */
static uint32_t fid_size(uint32_t position, uint8_t nameBytes, uint16_t* implementationBytes) {
    uint32_t length = (38u + nameBytes + 3u) & ~3u;
    uint32_t next = (position + length) % BLOCK;
    *implementationBytes = next > BLOCK - 16 ? 32 : 0;
    return length + *implementationBytes;
}

static bool udf_dir_size(FsDirectoryNode* dir) {
    uint16_t extra;
    uint64_t size = fid_size(0, 0, &extra);
    size_t count;
    TreeEntry* list = entries(dir, &count);
    if (!list) return false;
    qsort(list, count, sizeof(*list), original_order);
    for (size_t i = 0; i < count; i++) {
        uint8_t name[255], length = udf_encode_name(list[i].original, name, sizeof(name));
        if (!length || size > UINT32_MAX - 512) { free(list); return false; }
        size += fid_size((uint32_t)size, length, &extra);
    }
    free(list);
    dir->udfBytes = (uint32_t)size;
    return true;
}

bool fs_layout_prepare(ImageBuilder* builder) {
    fs_layout_free(builder);
    unsigned flags = builder->config.fsTypes;
    if (!flags || (flags & ~FS_TYPE_ALL) || builder->config.boot.isBootable) return false;
    if ((flags & FS_TYPE_UDF) && builder->config.udfRev != UDF_REV_102 && builder->config.udfRev != UDF_REV_250) return false;
    Layout* plan = (Layout*)calloc(1, sizeof(Layout));
    if (!plan) return false;
    builder->layout = plan;
    plan->dirCount = (size_t)builder->config.totalDirectories + 1;
    plan->fileCount = builder->config.totalFiles;
    if (plan->dirCount > 65535 || plan->fileCount > SIZE_MAX / sizeof(FsFileNode*)) goto failed;
    plan->dirs = (FsDirectoryNode**)calloc(plan->dirCount, sizeof(*plan->dirs));
    plan->jolietDirs = (FsDirectoryNode**)calloc(plan->dirCount, sizeof(*plan->dirs));
    plan->files = (FsFileNode**)calloc(plan->fileCount ? plan->fileCount : 1, sizeof(*plan->files));
    if (!plan->dirs || !plan->jolietDirs || !plan->files || !enumerate(plan, builder->rootDir, false) || !enumerate(plan, builder->rootDir, true)) goto failed;
    if (builder->config.dvdVideoCompliance || builder->config.blurayCompliance) qsort(plan->files, plan->fileCount, sizeof(*plan->files), fs_video_file_order);
    plan->reserveVds = 48;
    GetSystemTimeAsFileTime(&plan->created);
    /* Joliet需要ISO主卷；纯UDF则从扇区16直接开始VRS。 */
    bool isoBridge = (flags & (FS_TYPE_ISO9660 | FS_TYPE_JOLIET)) != 0;
    builder->pvdLba = isoBridge ? 16 : 0;
    builder->svdLba = (flags & FS_TYPE_JOLIET) ? 17 : 0;
    plan->terminator = isoBridge ? (builder->svdLba ? 18 : 17) : 15;
    uint64_t cursor = (flags & FS_TYPE_UDF) ? 257 : plan->terminator + 1;
    builder->pathTableSizeIso = 0; builder->pathTableSizeJoliet = 0;
    for (size_t i = 0; isoBridge && i < plan->dirCount; i++) {
        uint32_t isoBytes = i ? (uint32_t)strlen(plan->dirs[i]->nameA) : 1;
        uint32_t jolietBytes = i ? (uint32_t)wcslen(plan->jolietDirs[i]->jolietNameW) * 2 : 1;
        builder->pathTableSizeIso += 8 + ((isoBytes + 1) & ~1u);
        builder->pathTableSizeJoliet += 8 + ((jolietBytes + 1) & ~1u);
    }
    builder->pathTableLbaL = isoBridge ? (uint32_t)cursor : 0; cursor += blocks(builder->pathTableSizeIso);
    builder->pathTableLbaM = isoBridge ? (uint32_t)cursor : 0; cursor += blocks(builder->pathTableSizeIso);
    if (flags & FS_TYPE_JOLIET) {
        builder->pathTableLbaLJoliet = (uint32_t)cursor; cursor += blocks(builder->pathTableSizeJoliet);
        builder->pathTableLbaMJoliet = (uint32_t)cursor; cursor += blocks(builder->pathTableSizeJoliet);
    }
    for (size_t i = 0; isoBridge && i < plan->dirCount; i++) {
        FsDirectoryNode* dir = plan->dirs[i];
        dir->sectorCountIso = dir_size(dir, false);
        dir->allocatedLbaIso = (uint32_t)cursor; cursor += dir->sectorCountIso;
        if (!dir->sectorCountIso) goto failed;
        if (flags & FS_TYPE_JOLIET) {
            dir->sectorCountJoliet = dir_size(dir, true);
            dir->allocatedLbaJoliet = (uint32_t)cursor; cursor += dir->sectorCountJoliet;
            if (!dir->sectorCountJoliet) goto failed;
        }
        if (cursor > UINT32_MAX - 4096) goto failed;
    }
    if (flags & FS_TYPE_UDF) {
        plan->partition = align32((uint32_t)cursor);
        plan->metadataPartition = builder->config.udfRev == UDF_REV_250;
        plan->metadata = 0;
        cursor = 2;
        for (size_t i = 0; i < plan->dirCount; i++) {
            plan->dirs[i]->udfIcb = (uint32_t)cursor++;
            plan->dirs[i]->udfUniqueId = i ? 15 + i : 0;
        }
        for (size_t i = 0; i < plan->fileCount; i++) {
            plan->files[i]->udfIcb = (uint32_t)cursor++;
            plan->files[i]->udfUniqueId = 15 + plan->dirCount + i;
            uint64_t descriptors = (plan->files[i]->fileSize + MAX_EXTENT - 1) / MAX_EXTENT;
            plan->files[i]->udfAllocationLba = (uint32_t)cursor;
            unsigned capacity = builder->config.udfRev == UDF_REV_250 ? 114 : 117;
            plan->files[i]->udfAllocationBlocks = descriptors > capacity ? (uint32_t)((descriptors - capacity + 124) / 125) : 0;
            cursor += plan->files[i]->udfAllocationBlocks;
            if (cursor > UINT32_MAX - 4096) goto failed;
        }
        for (size_t i = 0; i < plan->dirCount; i++) {
            FsDirectoryNode* dir = plan->dirs[i];
            if (!udf_dir_size(dir)) goto failed;
            dir->udfData = (uint32_t)cursor; cursor += blocks(dir->udfBytes);
            if (cursor > UINT32_MAX - 4096) goto failed;
        }
        plan->metadataBlocks = plan->metadataPartition ? align32((uint32_t)cursor) : (uint32_t)cursor;
        if (plan->metadataPartition) {
            /* 元数据副本位于物理分区前部，尾部描述区和锚点独立安排。 */
            plan->mirrorFe = 1; plan->mirrorData = 32; plan->metadata = 32 + plan->metadataBlocks;
        }
        cursor = (uint64_t)plan->partition + plan->metadata + plan->metadataBlocks;
    }
    builder->dataStartLba = (uint32_t)cursor;
    for (size_t i = 0; i < plan->fileCount; i++) {
        FsFileNode* file = plan->files[i];
        if (file->isDuplicate) continue;
        uint32_t aligned = fs_video_align_file(builder, file, (uint32_t)cursor);
        if (aligned < cursor) goto failed;
        cursor = aligned;
        file->allocatedLba = (uint32_t)cursor; cursor += file->sectorCount;
        if (cursor > UINT32_MAX - 4096) goto failed;
    }
    for (size_t i = 0; i < plan->fileCount; i++) if (plan->files[i]->isDuplicate) plan->files[i]->allocatedLba = plan->files[i]->duplicateOf->allocatedLba;
    if (flags & FS_TYPE_UDF) {
        plan->partitionBlocks = (uint32_t)cursor - plan->partition;
        if (builder->config.blurayCompliance) { plan->reserveVds = align32((uint32_t)cursor); cursor = (uint64_t)plan->reserveVds + 32; }
        cursor += 257;
    }
    if (cursor > UINT32_MAX) goto failed;
    builder->config.totalImageSectors = (uint32_t)cursor;
    if (!fs_video_finalize(builder)) goto failed;
    memcpy(&plan->configuration, &builder->config, sizeof(plan->configuration));
    return true;
failed:
    fs_layout_free(builder);
    return false;
}

static void iso_time(uint8_t* record, const FILETIME* time) {
    SYSTEMTIME date;
    if (!FileTimeToSystemTime(time, &date)) GetSystemTime(&date);
    record[0] = (uint8_t)(date.wYear - 1900); record[1] = (uint8_t)date.wMonth; record[2] = (uint8_t)date.wDay;
    record[3] = (uint8_t)date.wHour; record[4] = (uint8_t)date.wMinute; record[5] = (uint8_t)date.wSecond; record[6] = 0;
}

static void iso_record(uint8_t* out, uint32_t length, uint32_t lba, uint32_t bytes, bool directory, const uint8_t* name, uint8_t nameLength, const FILETIME* time) {
    out[0] = (uint8_t)length; both32(out + 2, lba); both32(out + 10, bytes); iso_time(out + 18, time);
    out[25] = directory ? 2 : 0; both16(out + 28, 1); out[32] = nameLength; memcpy(out + 33, name, nameLength);
}

static bool iso_volume(ImageBuilder* b, HANDLE output, bool joliet) {
    Layout* plan = (Layout*)b->layout;
    uint8_t data[BLOCK] = {0};
    data[0] = joliet ? 2 : 1; memcpy(data + 1, "CD001\1", 6);
    memset(data + 8, ' ', 64); memset(data + 190, ' ', 623);
    if (joliet) {
        for (size_t i = 8; i < 72; i += 2) data[i] = 0;
        for (size_t i = 190; i < 813; i++) data[i] = 0;
        ucs2(data + 40, b->config.volumeLabelJoliet); memcpy(data + 88, "%/E", 3);
    } else {
        memcpy(data + 40, b->config.volumeLabelIso, strlen(b->config.volumeLabelIso));
    }
    both32(data + 80, b->config.totalImageSectors); both16(data + 120, 1); both16(data + 124, 1); both16(data + 128, BLOCK);
    both32(data + 132, joliet ? b->pathTableSizeJoliet : b->pathTableSizeIso);
    le32(data + 140, joliet ? b->pathTableLbaLJoliet : b->pathTableLbaL);
    be32(data + 148, joliet ? b->pathTableLbaMJoliet : b->pathTableLbaM);
    FsDirectoryNode* root = b->rootDir;
    uint8_t name = 0;
    iso_record(data + 156, 34, joliet ? root->allocatedLbaJoliet : root->allocatedLbaIso, (joliet ? root->sectorCountJoliet : root->sectorCountIso) * BLOCK, true, &name, 1, &plan->created);
    SYSTEMTIME date; FileTimeToSystemTime(&plan->created, &date);
    char timestamp[18];
    snprintf(timestamp, sizeof(timestamp), "%04u%02u%02u%02u%02u%02u00", date.wYear, date.wMonth, date.wDay, date.wHour, date.wMinute, date.wSecond);
    memcpy(data + 813, timestamp, 17); memcpy(data + 830, timestamp, 17);
    memset(data + 847, '0', 16); memset(data + 864, '0', 16); data[881] = 1;
    return put(output, joliet ? b->svdLba : b->pvdLba, data, BLOCK);
}

static bool iso_paths(ImageBuilder* b, HANDLE output, bool joliet, bool bigEndian) {
    Layout* plan = (Layout*)b->layout;
    uint32_t size = joliet ? b->pathTableSizeJoliet : b->pathTableSizeIso;
    uint8_t* data = (uint8_t*)calloc(blocks(size), BLOCK);
    if (!data) return false;
    uint32_t offset = 0;
    FsDirectoryNode** dirs = joliet ? plan->jolietDirs : plan->dirs;
    for (size_t i = 0; i < plan->dirCount; i++) {
        FsDirectoryNode* dir = dirs[i];
        uint8_t length = i ? (uint8_t)(joliet ? wcslen(dir->jolietNameW) * 2 : strlen(dir->nameA)) : 1;
        uint32_t lba = joliet ? dir->allocatedLbaJoliet : dir->allocatedLbaIso;
        uint16_t parent = dir->parent ? (joliet ? dir->parent->dirIndexJoliet : dir->parent->dirIndex) : 1;
        data[offset] = length;
        if (bigEndian) { be32(data + offset + 2, lba); be16(data + offset + 6, parent); }
        else { le32(data + offset + 2, lba); le16(data + offset + 6, parent); }
        if (i && joliet) ucs2(data + offset + 8, dir->jolietNameW);
        else if (i) memcpy(data + offset + 8, dir->nameA, length);
        offset += 8 + ((length + 1u) & ~1u);
    }
    uint32_t lba = joliet ? (bigEndian ? b->pathTableLbaMJoliet : b->pathTableLbaLJoliet) : (bigEndian ? b->pathTableLbaM : b->pathTableLbaL);
    bool ok = put(output, lba, data, blocks(size) * BLOCK);
    free(data); return ok;
}

static bool iso_directory(ImageBuilder* b, HANDLE output, FsDirectoryNode* dir, bool joliet) {
    Layout* plan = (Layout*)b->layout;
    uint32_t bytes = (joliet ? dir->sectorCountJoliet : dir->sectorCountIso) * BLOCK;
    uint8_t* data = (uint8_t*)calloc(1, bytes);
    if (!data) return false;
    FsDirectoryNode* parent = dir->parent ? dir->parent : dir;
    uint8_t name[256] = {0};
    iso_record(data, 34, joliet ? dir->allocatedLbaJoliet : dir->allocatedLbaIso, bytes, true, name, 1, &plan->created);
    name[0] = 1;
    iso_record(data + 34, 34, joliet ? parent->allocatedLbaJoliet : parent->allocatedLbaIso, (joliet ? parent->sectorCountJoliet : parent->sectorCountIso) * BLOCK, true, name, 1, &plan->created);
    size_t count;
    TreeEntry* list = entries(dir, &count);
    if (!list) { free(data); return false; }
    qsort(list, count, sizeof(*list), joliet ? joliet_order : iso_order);
    uint32_t offset = 68;
    for (size_t i = 0; i < count; i++) {
        TreeEntry* entry = &list[i];
        uint8_t nameLength = (uint8_t)(joliet ? wcslen(entry->joliet) * 2 : strlen(entry->iso));
        uint32_t length = (33u + nameLength + 1u) & ~1u;
        if (joliet) ucs2(name, entry->joliet); else memcpy(name, entry->iso, nameLength);
        uint32_t lba = entry->file ? entry->file->allocatedLba : (joliet ? entry->dir->allocatedLbaJoliet : entry->dir->allocatedLbaIso);
        uint64_t remaining = entry->file ? entry->file->fileSize : (joliet ? entry->dir->sectorCountJoliet : entry->dir->sectorCountIso) * (uint64_t)BLOCK;
        do {
            uint32_t section = remaining > ISO_SECTION ? ISO_SECTION : (uint32_t)remaining;
            offset = record_offset(offset, length);
            iso_record(data + offset, length, lba, section, entry->dir != NULL, name, nameLength, entry->file ? &entry->file->modTime : &plan->created);
            if (remaining > section) data[offset + 25] |= 0x80;
            offset += length; remaining -= section; lba += blocks(section);
        } while (remaining);
    }
    bool ok = put(output, joliet ? dir->allocatedLbaJoliet : dir->allocatedLbaIso, data, bytes);
    free(list); free(data); return ok;
}

static void entity(UdfEntityId* id, const char* text, uint16_t revision) {
    memset(id, 0, sizeof(*id)); strncpy(id->ident, text, sizeof(id->ident));
    if (revision) { le16((uint8_t*)id->identSuffix, revision); id->identSuffix[2] = 6; }
}

static void domain(UdfEntityId* id, uint16_t revision) { entity(id, "*OSTA UDF Compliant", revision); id->identSuffix[2] = 3; }

static void tag(ImageBuilder* b, void* data, uint16_t id, uint32_t location, uint16_t size) {
    udf_finalize_tag_revision((UdfDescriptorTag*)data, id, location, size, (uint16_t)b->config.udfRev);
}

static bool udf_volume_sequence(ImageBuilder* b, HANDLE output, uint32_t base) {
    Layout* plan = (Layout*)b->layout;
    uint8_t data[6 * BLOCK] = {0};
    UdfPrimaryVolDesc* pvd = (UdfPrimaryVolDesc*)data;
    pvd->vdsNum = 1; pvd->volumeSeqNum = 1; pvd->maxVolumeSeqNum = 1; pvd->interchangeLevel = 2; pvd->maxInterchangeLevel = 3;
    pvd->charSetList = 1; pvd->maxCharSetList = 1;
    udf_encode_dstring(b->config.volumeLabelUdf, (uint8_t*)pvd->volumeIdent, sizeof(pvd->volumeIdent));
    udf_encode_dstring(L"0000000000000000IMGBURN", (uint8_t*)pvd->volumeSetIdent, sizeof(pvd->volumeSetIdent));
    udf_set_charset(pvd->descriptorCharSet); udf_set_charset(pvd->explanatoryCharSet);
    entity(&pvd->implementationIdent, "*ImgBurn C", 0); udf_filetime_to_timestamp(&plan->created, &pvd->recordingDateAndTime);
    tag(b, pvd, UDF_TAG_PVD, base, 512);
    UdfPartitionDesc* pd = (UdfPartitionDesc*)(data + BLOCK);
    pd->vdsNum = 2; pd->partitionFlags = 1; pd->accessType = 1;
    entity(&pd->partitionContents, plan->metadataPartition ? "+NSR03" : "+NSR02", 0);
    pd->partitionStart = plan->partition; pd->partitionLen = plan->partitionBlocks; entity(&pd->implementationIdent, "*ImgBurn C", 0);
    tag(b, pd, UDF_TAG_PD, base + 1, 512);
    UdfLogicalVolDesc* lvd = (UdfLogicalVolDesc*)(data + 2 * BLOCK);
    lvd->vdsNum = 3; udf_set_charset(lvd->descriptorCharSet);
    udf_encode_dstring(b->config.volumeLabelUdf, (uint8_t*)lvd->logicalVolumeIdent, sizeof(lvd->logicalVolumeIdent));
    lvd->logicalBlockSize = BLOCK; domain(&lvd->domainIdent, (uint16_t)b->config.udfRev);
    UdfLongAd* fsd = (UdfLongAd*)lvd->logicalVolumeContentsUse;
    fsd->length = BLOCK; fsd->partitionRefNum = plan->metadataPartition ? 1 : 0;
    lvd->mapTableLength = plan->metadataPartition ? 70 : 6; lvd->numPartitionMaps = plan->metadataPartition ? 2 : 1;
    entity(&lvd->implementationIdent, "*ImgBurn C", 0); lvd->integritySequenceExtent = (UdfExtentAd){4 * BLOCK, 64};
    lvd->partitionMaps[0] = 1; lvd->partitionMaps[1] = 6; le16(lvd->partitionMaps + 2, 1);
    if (plan->metadataPartition) {
        uint8_t* map = lvd->partitionMaps + 6;
        map[0] = 2; map[1] = 64; entity((UdfEntityId*)(map + 4), "*UDF Metadata Partition", (uint16_t)b->config.udfRev);
        le16(map + 36, 1); le32(map + 40, 0); le32(map + 44, plan->mirrorFe); le32(map + 48, UINT32_MAX);
        le32(map + 52, 32); le16(map + 56, 32); map[58] = 1;
    }
    tag(b, lvd, UDF_TAG_LVD, base + 2, (uint16_t)(440 + lvd->mapTableLength));
    uint8_t* iuvd = data + 3 * BLOCK;
    le32(iuvd + 16, 4); entity((UdfEntityId*)(iuvd + 20), "*UDF LV Info", (uint16_t)b->config.udfRev);
    udf_set_charset(iuvd + 52); udf_encode_dstring(b->config.volumeLabelUdf, iuvd + 116, 128); entity((UdfEntityId*)(iuvd + 352), "*ImgBurn C", 0);
    tag(b, iuvd, UDF_TAG_IUVD, base + 3, 512);
    uint8_t* usd = data + 4 * BLOCK; le32(usd + 16, 5); tag(b, usd, UDF_TAG_USD, base + 4, 24);
    tag(b, data + 5 * BLOCK, UDF_TAG_TD, base + 5, 512);
    return put(output, base, data, sizeof(data));
}

static bool udf_integrity(ImageBuilder* b, HANDLE output) {
    Layout* plan = (Layout*)b->layout;
    uint8_t data[2 * BLOCK] = {0};
    udf_filetime_to_timestamp(&plan->created, (UdfTimestamp*)(data + 16)); le32(data + 28, 1);
    uint64_t nextId = 15 + plan->dirCount + plan->fileCount; memcpy(data + 40, &nextId, 8);
    uint32_t count = plan->metadataPartition ? 2 : 1;
    le32(data + 72, count); le32(data + 76, 46);
    le32(data + 80 + count * 4, plan->partitionBlocks);
    if (count == 2) le32(data + 80 + count * 4 + 4, plan->metadataBlocks);
    uint8_t* impl = data + 80 + count * 8;
    entity((UdfEntityId*)impl, "*ImgBurn C", 0); le32(impl + 32, (uint32_t)plan->fileCount); le32(impl + 36, (uint32_t)plan->dirCount);
    le16(impl + 40, (uint16_t)b->config.udfRev); le16(impl + 42, (uint16_t)b->config.udfRev); le16(impl + 44, (uint16_t)b->config.udfRev);
    tag(b, data, UDF_TAG_LVID, 64, (uint16_t)(80 + count * 8 + 46)); tag(b, data + BLOCK, UDF_TAG_TD, 65, 512);
    return put(output, 64, data, sizeof(data));
}

static bool udf_tree_put(ImageBuilder* b, HANDLE output, uint32_t logical, const void* data, uint32_t length) {
    Layout* plan = (Layout*)b->layout;
    if (!put(output, plan->partition + plan->metadata + logical, data, length)) return false;
    return !plan->metadataPartition || put(output, plan->partition + plan->mirrorData + logical, data, length);
}

static bool udf_fe(ImageBuilder* b, HANDLE output, uint32_t block, uint8_t type, uint64_t unique, uint64_t bytes, uint32_t extent, uint16_t reference, uint16_t links, const FILETIME* time, uint32_t allocationLba) {
    Layout* plan = (Layout*)b->layout;
    uint8_t data[BLOCK] = {0};
    UdfFileEntry* fe = (UdfFileEntry*)data;
    bool regular = type == UDF_FILE_TYPE_REGULAR;
    fe->strategyType = 4; fe->maxNumEntries = 1; fe->fileType = type; fe->icbFlags = regular ? 1 : 0;
    fe->uid = UINT32_MAX; fe->gid = UINT32_MAX; fe->permissions = type == UDF_FILE_TYPE_DIRECTORY ? 0x14A5 : 0x1084;
    fe->fileLinkCount = links; fe->infoLength = bytes; fe->logicalBlocksRecorded = blocks(bytes); fe->checkpoint = 1; fe->uniqueId = unique;
    udf_filetime_to_timestamp(time, &fe->accessDateAndTime); fe->modDateAndTime = fe->accessDateAndTime; fe->attrDateAndTime = fe->accessDateAndTime;
    entity(&fe->implementationIdent, "*ImgBurn C", 0);
    bool extended = b->config.udfRev == UDF_REV_250;
    if (extended) {
        UdfExtendedFileEntry* efe = (UdfExtendedFileEntry*)data;
        /* 头部前64字节保持一致，其余字段按EFE偏移重新填写。 */
        memset(data + 64, 0, BLOCK - 64);
        efe->objectSize = bytes; efe->logicalBlocksRecorded = blocks(bytes); efe->checkpoint = 1; efe->uniqueId = unique;
        udf_filetime_to_timestamp(time, &efe->accessDateAndTime); efe->modDateAndTime = efe->accessDateAndTime; efe->creationDateAndTime = efe->accessDateAndTime; efe->attrDateAndTime = efe->accessDateAndTime;
        entity(&efe->implementationIdent, "*ImgBurn C", 0);
    }
    bool first = true;
    do {
        uint32_t header = first ? (extended ? sizeof(UdfExtendedFileEntry) : sizeof(*fe)) : 24, offset = header, adSize = regular ? 16 : 8;
        uint64_t needed = (bytes + MAX_EXTENT - 1) / MAX_EXTENT;
        uint32_t slots = (BLOCK - header) / adSize;
        bool continuation = needed > slots;
        if (continuation && (!regular || !allocationLba)) return false;
        uint32_t count = continuation ? slots - 1 : (uint32_t)needed;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t length = bytes > MAX_EXTENT ? MAX_EXTENT : (uint32_t)bytes;
            if (regular) { UdfLongAd ad = {0}; ad.length = length; ad.logicalBlockNum = extent; ad.partitionRefNum = reference; memcpy(data + offset, &ad, sizeof(ad)); }
            else { UdfShortAd ad = {length, extent}; memcpy(data + offset, &ad, sizeof(ad)); }
            offset += adSize; bytes -= length; extent += blocks(length);
        }
        if (continuation) {
            UdfLongAd ad = {0}; ad.length = 0xC0000000u | BLOCK; ad.logicalBlockNum = allocationLba; ad.partitionRefNum = plan->metadataPartition ? 1 : 0;
            memcpy(data + offset, &ad, sizeof(ad)); offset += sizeof(ad);
        }
        if (first && extended) ((UdfExtendedFileEntry*)data)->lengthOfAllocationDescriptors = offset - header;
        else if (first) ((UdfFileEntry*)data)->lengthOfAllocationDescriptors = offset - header;
        else le32(data + 20, offset - header);
        tag(b, data, first ? (extended ? UDF_TAG_EXTENDED_FE : UDF_TAG_FE) : UDF_TAG_ALLOC_EXTENT, block, (uint16_t)(!first && b->config.udfRev == UDF_REV_102 ? 24 : offset));
        if (type == UDF_FILE_TYPE_METADATA_MAIN || type == UDF_FILE_TYPE_METADATA_MIRR) return put(output, plan->partition + block, data, BLOCK);
        if (!udf_tree_put(b, output, block, data, BLOCK)) return false;
        if (!continuation) return true;
        block = allocationLba++; first = false; memset(data, 0, sizeof(data));
    } while (true);
}

static uint32_t write_fid(ImageBuilder* b, uint8_t* target, uint32_t position, uint32_t dataBlock, const wchar_t* name, uint32_t icb, uint64_t unique, uint8_t flags) {
    Layout* plan = (Layout*)b->layout;
    uint8_t encoded[255]; uint8_t length = name ? udf_encode_name(name, encoded, sizeof(encoded)) : 0;
    uint16_t extra;
    uint32_t bytes = fid_size(position, length, &extra);
    UdfFileIdDesc* fid = (UdfFileIdDesc*)(target + position);
    fid->fileVersionNum = 1; fid->fileCharacteristics = flags; fid->lengthOfFileIdent = length;
    fid->icb.length = BLOCK; fid->icb.logicalBlockNum = icb; fid->icb.partitionRefNum = plan->metadataPartition ? 1 : 0;
    le32(fid->icb.implementationUse + 2, (uint32_t)unique); fid->lengthOfImplementationUse = extra;
    if (extra) entity((UdfEntityId*)(target + position + sizeof(*fid)), "*ImgBurn C", 0);
    if (length) memcpy(target + position + sizeof(*fid) + extra, encoded, length);
    tag(b, fid, UDF_TAG_FID, dataBlock + position / BLOCK, (uint16_t)bytes);
    return bytes;
}

static bool udf_directory(ImageBuilder* b, HANDLE output, FsDirectoryNode* dir) {
    Layout* plan = (Layout*)b->layout;
    uint8_t* data = (uint8_t*)calloc(blocks(dir->udfBytes), BLOCK);
    if (!data) return false;
    FsDirectoryNode* parent = dir->parent ? dir->parent : dir;
    uint32_t offset = write_fid(b, data, 0, dir->udfData, NULL, parent->udfIcb, parent->udfUniqueId, 10);
    size_t count; TreeEntry* list = entries(dir, &count);
    if (!list) { free(data); return false; }
    qsort(list, count, sizeof(*list), original_order);
    uint16_t links = 1;
    for (size_t i = 0; i < count; i++) {
        TreeEntry* e = &list[i];
        if (e->dir) { if (links == UINT16_MAX) { free(list); free(data); return false; } links++; }
        offset += write_fid(b, data, offset, dir->udfData, e->original, e->dir ? e->dir->udfIcb : e->file->udfIcb, e->dir ? e->dir->udfUniqueId : e->file->udfUniqueId, e->dir ? 2 : 0);
    }
    bool ok = offset == dir->udfBytes && udf_tree_put(b, output, dir->udfData, data, blocks(dir->udfBytes) * BLOCK);
    if (ok) ok = udf_fe(b, output, dir->udfIcb, UDF_FILE_TYPE_DIRECTORY, dir->udfUniqueId, dir->udfBytes, dir->udfData, plan->metadataPartition ? 1 : 0, links, &plan->created, 0);
    free(list); free(data); return ok;
}

static bool udf_tree(ImageBuilder* b, HANDLE output) {
    Layout* plan = (Layout*)b->layout;
    uint8_t data[2 * BLOCK] = {0};
    UdfFileSetDesc* fsd = (UdfFileSetDesc*)data;
    udf_filetime_to_timestamp(&plan->created, &fsd->recordingDateAndTime); fsd->interchangeLevel = 3; fsd->maxInterchangeLevel = 3; fsd->charSetList = 1; fsd->maxCharSetList = 1;
    udf_set_charset(fsd->logicalVolumeIdentCharSet); udf_set_charset(fsd->fileSetCharSet);
    udf_encode_dstring(b->config.volumeLabelUdf, (uint8_t*)fsd->logicalVolumeIdent, sizeof(fsd->logicalVolumeIdent));
    udf_encode_dstring(b->config.volumeLabelUdf, (uint8_t*)fsd->fileSetIdent, sizeof(fsd->fileSetIdent));
    fsd->rootDirectoryIcb.length = BLOCK; fsd->rootDirectoryIcb.logicalBlockNum = b->rootDir->udfIcb; fsd->rootDirectoryIcb.partitionRefNum = plan->metadataPartition ? 1 : 0;
    domain(&fsd->domainIdent, (uint16_t)b->config.udfRev); tag(b, fsd, UDF_TAG_FSD, 0, 512); tag(b, data + BLOCK, UDF_TAG_TD, 1, 512);
    if (!udf_tree_put(b, output, 0, data, sizeof(data))) return false;
    for (size_t i = 0; i < plan->dirCount; i++) if (!udf_directory(b, output, plan->dirs[i])) return false;
    for (size_t i = 0; i < plan->fileCount; i++) {
        FsFileNode* file = plan->files[i];
        if (!udf_fe(b, output, file->udfIcb, UDF_FILE_TYPE_REGULAR, file->udfUniqueId, file->fileSize, file->allocatedLba - plan->partition, 0, 1, &file->modTime, file->udfAllocationLba)) return false;
    }
    if (plan->metadataPartition) {
        if (!udf_fe(b, output, 0, UDF_FILE_TYPE_METADATA_MAIN, 0, (uint64_t)plan->metadataBlocks * BLOCK, plan->metadata, 0, 0, &plan->created, 0) ||
            !udf_fe(b, output, plan->mirrorFe, UDF_FILE_TYPE_METADATA_MIRR, 0, (uint64_t)plan->metadataBlocks * BLOCK, plan->mirrorData, 0, 0, &plan->created, 0)) return false;
    }
    return true;
}

bool fs_layout_write(ImageBuilder* b, HANDLE output) {
    Layout* plan = (Layout*)b->layout;
    if (!plan || !b->config.totalImageSectors || memcmp(&plan->configuration, &b->config, sizeof(plan->configuration))) return false;
    uint8_t data[3 * BLOCK] = {0};
    if (b->pvdLba) {
        if (!iso_volume(b, output, false) || !iso_paths(b, output, false, false) || !iso_paths(b, output, false, true)) return false;
        if (b->svdLba && (!iso_volume(b, output, true) || !iso_paths(b, output, true, false) || !iso_paths(b, output, true, true))) return false;
        data[0] = 255; memcpy(data + 1, "CD001\1", 6);
        if (!put(output, plan->terminator, data, BLOCK)) return false;
        for (size_t i = 0; i < plan->dirCount; i++) {
            if (!iso_directory(b, output, plan->dirs[i], false)) return false;
            if (b->svdLba && !iso_directory(b, output, plan->dirs[i], true)) return false;
        }
    }
    if (!(b->config.fsTypes & FS_TYPE_UDF)) return true;
    udf_build_vrs(data, plan->metadataPartition);
    if (!put(output, plan->terminator + 1, data, sizeof(data)) || !udf_volume_sequence(b, output, 32) || !udf_volume_sequence(b, output, plan->reserveVds) || !udf_integrity(b, output) || !udf_tree(b, output)) return false;
    uint32_t anchors[3] = {256, b->config.totalImageSectors - 257, b->config.totalImageSectors - 1};
    for (int i = 0; i < 3; i++) {
        memset(data, 0, BLOCK);
        UdfAnchorVolDescPtr* anchor = (UdfAnchorVolDescPtr*)data;
        anchor->mainVdsExtent = (UdfExtentAd){16 * BLOCK, 32}; anchor->reserveVdsExtent = (UdfExtentAd){16 * BLOCK, plan->reserveVds};
        tag(b, anchor, UDF_TAG_AVDP, anchors[i], 512);
        if (!put(output, anchors[i], data, BLOCK)) return false;
    }
    return true;
}
