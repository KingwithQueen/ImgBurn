/* 成品DVD-Video镜像只读分析：ISO目录提供真实extent，导航读取共用文件夹解析器。 */
#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_fs_internal.h"
#include "imgburn_data_source.h"
#include "imgburn_udf.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define DVD_IMAGE_BLOCK 2048u
#define DVD_IMAGE_MAX_FILE 0x3FFFF800u

static uint16_t dvd_image_le16(const uint8_t* p) { return (uint16_t)((uint16_t)p[1] << 8 | p[0]); }
static uint32_t dvd_image_le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static uint32_t dvd_image_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static bool dvd_image_error(DWORD error) { SetLastError(error); return false; }
static bool dvd_image_both32(const uint8_t* p, uint32_t* result) { *result = dvd_image_le32(p); return *result == dvd_image_be32(p + 4); }
static bool dvd_image_extent(DataImageSource* source, uint32_t lba, uint64_t bytes) { return bytes && lba < source->sectors && (bytes + DVD_IMAGE_BLOCK - 1) / DVD_IMAGE_BLOCK <= source->sectors - lba; }

static bool dvd_image_read(DataImageSource* source, uint64_t position, void* output, size_t bytes) {
    if (position > (uint64_t)source->sectors * DVD_IMAGE_BLOCK || bytes > (uint64_t)source->sectors * DVD_IMAGE_BLOCK - position) return dvd_image_error(ERROR_INVALID_DATA);
    uint8_t* target = output;
    while (bytes) {
        uint32_t lba = (uint32_t)(position / DVD_IMAGE_BLOCK), inside = (uint32_t)(position % DVD_IMAGE_BLOCK);
        if (!inside && bytes >= DVD_IMAGE_BLOCK) {
            uint16_t count = (uint16_t)(bytes / DVD_IMAGE_BLOCK > 32 ? 32 : bytes / DVD_IMAGE_BLOCK);
            if (!imgburn_data_source_read(source,lba,count,target)) return false;
            size_t transferred = (size_t)count * DVD_IMAGE_BLOCK; target += transferred; position += transferred; bytes -= transferred;
        } else {
            uint8_t block[DVD_IMAGE_BLOCK]; if (!imgburn_data_source_read(source,lba,1,block)) return false;
            size_t count = bytes < DVD_IMAGE_BLOCK - inside ? bytes : DVD_IMAGE_BLOCK - inside; memcpy(target,block + inside,count); target += count; position += count; bytes -= count;
        }
    }
    return true;
}

static bool dvd_image_file_read(const FsFileNode* file, uint64_t offset, void* data, size_t bytes, void* context) {
    if (offset > file->fileSize || bytes > file->fileSize - offset) return dvd_image_error(ERROR_INVALID_DATA);
    return dvd_image_read(context,(uint64_t)file->allocatedLba * DVD_IMAGE_BLOCK + offset,data,bytes);
}

static bool dvd_image_tag(const uint8_t* data, uint16_t id, uint32_t lba) {
    uint16_t bytes = dvd_image_le16(data + 10); uint8_t sum = 0;
    for (unsigned i = 0; i < 16; i++) if (i != 4) sum = (uint8_t)(sum + data[i]);
    return dvd_image_le16(data) == id && dvd_image_le16(data + 2) == 2 && sum == data[4] && dvd_image_le32(data + 12) == lba && bytes <= DVD_IMAGE_BLOCK - 16 && udf_crc16(data + 16,bytes) == dvd_image_le16(data + 8);
}

static bool dvd_image_udf102(DataImageSource* source, uint32_t vrs) {
    uint8_t data[DVD_IMAGE_BLOCK];
    const char* identifiers[] = {"BEA01","NSR02","TEA01"};
    for (unsigned i = 0; i < 3; i++) {
        if (!imgburn_data_source_read(source,vrs + i,1,data)) return false;
        if (data[0] || memcmp(data + 1,identifiers[i],5) || data[6] != 1) return dvd_image_error(ERROR_NOT_SUPPORTED);
    }
    if (!imgburn_data_source_read(source,256,1,data)) return false;
    if (!dvd_image_tag(data,UDF_TAG_AVDP,256)) return dvd_image_error(ERROR_INVALID_DATA);
    uint32_t length = dvd_image_le32(data + 16), first = dvd_image_le32(data + 20);
    if (!length || length % DVD_IMAGE_BLOCK || length / DVD_IMAGE_BLOCK > 16 || !dvd_image_extent(source,first,length)) return dvd_image_error(ERROR_NOT_SUPPORTED);
    bool partition = false, logical = false, terminated = false;
    for (uint32_t i = 0; i < length / DVD_IMAGE_BLOCK; i++) {
        uint32_t lba = first + i; if (!imgburn_data_source_read(source,lba,1,data)) return false;
        uint16_t id = dvd_image_le16(data); if (!dvd_image_tag(data,id,lba)) return dvd_image_error(ERROR_INVALID_DATA);
        if (id == UDF_TAG_PD) {
            const UdfPartitionDesc* descriptor = (const UdfPartitionDesc*)data;
            if (partition || descriptor->partitionNum || memcmp(descriptor->partitionContents.ident,"+NSR02",6) || !dvd_image_extent(source,descriptor->partitionStart,(uint64_t)descriptor->partitionLen * DVD_IMAGE_BLOCK)) return dvd_image_error(ERROR_NOT_SUPPORTED);
            partition = true;
        } else if (id == UDF_TAG_LVD) {
            const UdfLogicalVolDesc* descriptor = (const UdfLogicalVolDesc*)data;
            if (logical || descriptor->logicalBlockSize != DVD_IMAGE_BLOCK || memcmp(descriptor->domainIdent.ident,"*OSTA UDF Compliant",19) || dvd_image_le16((const uint8_t*)descriptor->domainIdent.identSuffix) != UDF_REV_102 || descriptor->mapTableLength != 6 || descriptor->numPartitionMaps != 1 || memcmp(descriptor->partitionMaps,"\1\6\1\0\0\0",6)) return dvd_image_error(ERROR_NOT_SUPPORTED);
            logical = true;
        } else if (id == UDF_TAG_TD) { terminated = true; break; }
    }
    return partition && logical && terminated ? true : dvd_image_error(ERROR_INVALID_DATA);
}

static bool dvd_image_record(DataImageSource* source, const uint8_t* record, uint32_t* lba, uint32_t* bytes) {
    if (record[0] < 34 || record[1] || record[32] > record[0] - 33 || record[26] || record[27] || dvd_image_le16(record + 28) != 1 || record[30] || record[31] != 1 || !dvd_image_both32(record + 2,lba) || !dvd_image_both32(record + 10,bytes)) return dvd_image_error(ERROR_INVALID_DATA);
    if (!dvd_image_extent(source,*lba,*bytes)) return dvd_image_error(ERROR_INVALID_DATA);
    return true;
}

static bool dvd_image_name(const uint8_t* record, wchar_t name[128]) {
    unsigned bytes = record[32]; if (!bytes || bytes >= 128) return false;
    if (bytes >= 2 && record[33 + bytes - 2] == ';' && record[33 + bytes - 1] == '1') bytes -= 2;
    for (unsigned i = 0; i < bytes; i++) { if (record[33 + i] < 32 || record[33 + i] >= 127 || record[33 + i] == '/' || record[33 + i] == '\\') return false; name[i] = record[33 + i]; }
    name[bytes] = 0; return true;
}

static bool dvd_image_video_name(const wchar_t* name, unsigned* group, unsigned* order) {
    if (!_wcsicmp(name,L"VIDEO_TS.IFO")) { *group = 0; *order = 0; return true; }
    if (!_wcsicmp(name,L"VIDEO_TS.VOB")) { *group = 0; *order = 1; return true; }
    if (!_wcsicmp(name,L"VIDEO_TS.BUP")) { *group = 0; *order = 11; return true; }
    if (wcslen(name) != 12 || _wcsnicmp(name,L"VTS_",4) || name[6] != '_' || name[8] != '.' || name[4] < '0' || name[4] > '9' || name[5] < '0' || name[5] > '9' || name[7] < '0' || name[7] > '9') return false;
    *group = (unsigned)(name[4] - '0') * 10 + name[5] - '0'; if (!*group) return false;
    if (name[7] == '0' && !_wcsicmp(name + 9,L"IFO")) *order = 0;
    else if (name[7] == '0' && !_wcsicmp(name + 9,L"BUP")) *order = 11;
    else if (!_wcsicmp(name + 9,L"VOB")) *order = (unsigned)(name[7] - '0') + 1;
    else return false;
    return true;
}

static FsFileNode* dvd_image_find(FsDirectoryNode* directory, const wchar_t* name) {
    for (FsFileNode* file = directory->firstFile; file; file = file->next) if (!_wcsicmp(file->nameW,name)) return file;
    return NULL;
}

static bool dvd_image_directory(DataImageSource* source, FsDirectoryNode* directory, uint32_t lba, uint32_t bytes, bool root) {
    if (bytes % DVD_IMAGE_BLOCK) return dvd_image_error(ERROR_NOT_SUPPORTED);
    uint8_t block[DVD_IMAGE_BLOCK]; bool found = false;
    for (uint32_t sector = 0; sector < bytes / DVD_IMAGE_BLOCK; sector++) {
        if (!imgburn_data_source_read(source,lba + sector,1,block)) return false;
        for (unsigned position = 0; position < DVD_IMAGE_BLOCK;) {
            const uint8_t* record = block + position; unsigned length = record[0]; if (!length) break;
            if (length < 34 || length > DVD_IMAGE_BLOCK - position || record[32] > length - 33) return dvd_image_error(ERROR_INVALID_DATA);
            position += length; if (record[32] == 1 && record[33] <= 1) continue;
            wchar_t name[128]; if (!dvd_image_name(record,name)) return dvd_image_error(ERROR_NOT_SUPPORTED);
            unsigned group, order;
            if (root ? _wcsicmp(name,L"VIDEO_TS") != 0 : !dvd_image_video_name(name,&group,&order)) continue;
            uint32_t extent, size; if (!dvd_image_record(source,record,&extent,&size)) return false;
            if (record[25] != (root ? 2 : 0)) return dvd_image_error(ERROR_NOT_SUPPORTED);
            if (root) {
                if (found) return dvd_image_error(ERROR_INVALID_DATA);
                FsDirectoryNode* video = calloc(1,sizeof(*video)); if (!video) return dvd_image_error(ERROR_NOT_ENOUGH_MEMORY);
                wcscpy(video->nameW,L"VIDEO_TS"); strcpy(video->nameA,"VIDEO_TS"); video->parent = directory; video->allocatedLbaIso = extent; video->sectorCountIso = size / DVD_IMAGE_BLOCK; directory->firstChildDir = video;
                if (!dvd_image_directory(source,video,extent,size,false)) return false; found = true;
            } else {
                if (!size || size % DVD_IMAGE_BLOCK || size > DVD_IMAGE_MAX_FILE || dvd_image_find(directory,name)) return dvd_image_error(ERROR_INVALID_DATA);
                FsFileNode* file = calloc(1,sizeof(*file)); if (!file) return dvd_image_error(ERROR_NOT_ENOUGH_MEMORY);
                wcscpy(file->nameW,name); file->fileSize = size; file->allocatedLba = extent; file->sectorCount = size / DVD_IMAGE_BLOCK; file->parent = directory; file->next = directory->firstFile; directory->firstFile = file; found = true;
            }
        }
    }
    return found ? true : dvd_image_error(ERROR_INVALID_DATA);
}

static bool dvd_image_equal(DataImageSource* source, const FsFileNode* first, const FsFileNode* second) {
    if (first->fileSize != second->fileSize) return dvd_image_error(ERROR_INVALID_DATA);
    uint8_t left[16384], right[16384];
    for (uint64_t offset = 0; offset < first->fileSize;) {
        size_t bytes = first->fileSize - offset < sizeof(left) ? (size_t)(first->fileSize - offset) : sizeof(left);
        if (!dvd_image_file_read(first,offset,left,bytes,source) || !dvd_image_file_read(second,offset,right,bytes,source)) return false;
        if (memcmp(left,right,bytes)) return dvd_image_error(ERROR_INVALID_DATA); offset += bytes;
    }
    return true;
}

static bool dvd_image_navigation_files(DataImageSource* source, FsDirectoryNode* directory) {
    if (!dvd_image_find(directory,L"VIDEO_TS.IFO") || !dvd_image_find(directory,L"VIDEO_TS.BUP")) return dvd_image_error(ERROR_INVALID_DATA);
    for (FsFileNode* file = directory->firstFile; file; file = file->next) {
        unsigned group, order; if (!dvd_image_video_name(file->nameW,&group,&order)) continue;
        wchar_t name[16];
        if (group) {
            swprintf(name,16,L"VTS_%02u_0.IFO",group); if (!dvd_image_find(directory,name)) return dvd_image_error(ERROR_INVALID_DATA);
            if (order >= 3 && order <= 10) { swprintf(name,16,L"VTS_%02u_%u.VOB",group,order - 2); if (!dvd_image_find(directory,name)) return dvd_image_error(ERROR_INVALID_DATA); }
        }
        if (order) continue;
        wcscpy(name,file->nameW); wcscpy(name + wcslen(name) - 3,L"BUP"); FsFileNode* backup = dvd_image_find(directory,name); uint8_t header[DVD_IMAGE_BLOCK];
        if (!backup) return dvd_image_error(ERROR_INVALID_DATA);
        if (!dvd_image_equal(source,file,backup) || !dvd_image_file_read(file,0,header,sizeof(header),source)) return false;
        if (memcmp(header,group ? "DVDVIDEO-VTS" : "DVDVIDEO-VMG",12) || backup->allocatedLba < file->allocatedLba || dvd_image_be32(header + 12) != backup->allocatedLba + backup->sectorCount - file->allocatedLba - 1 || dvd_image_be32(header + 28) != file->sectorCount - 1) return dvd_image_error(ERROR_INVALID_DATA);
        if (group) swprintf(name,16,L"VTS_%02u_0.VOB",group); else wcscpy(name,L"VIDEO_TS.VOB"); FsFileNode* menu = dvd_image_find(directory,name);
        if ((menu && menu->allocatedLba < file->allocatedLba) || dvd_image_be32(header + 0xC0) != (menu ? menu->allocatedLba - file->allocatedLba : 0)) return dvd_image_error(ERROR_INVALID_DATA);
        if (group) {
            swprintf(name,16,L"VTS_%02u_1.VOB",group); FsFileNode* title = dvd_image_find(directory,name);
            if (!title || title->allocatedLba < file->allocatedLba || dvd_image_be32(header + 0xC4) != title->allocatedLba - file->allocatedLba) return dvd_image_error(ERROR_INVALID_DATA);
        } else {
            uint64_t table = (uint64_t)dvd_image_be32(header + 0xC4) * DVD_IMAGE_BLOCK;
            if (table) {
                uint8_t tableHeader[8]; if (!dvd_image_file_read(file,table,tableHeader,sizeof(tableHeader),source)) return false;
                uint16_t count = (uint16_t)((uint16_t)tableHeader[0] << 8 | tableHeader[1]); uint64_t bytes = (uint64_t)dvd_image_be32(tableHeader + 4) + 1;
                if (!count || count > 99 || bytes < 8 + (uint64_t)count * 12 || table > file->fileSize || bytes > file->fileSize - table) return dvd_image_error(ERROR_INVALID_DATA);
                for (unsigned i = 0; i < count; i++) {
                    uint8_t entry[12]; if (!dvd_image_file_read(file,table + 8 + i * 12,entry,sizeof(entry),source)) return false;
                    if (!entry[6] || entry[6] > 99) return dvd_image_error(ERROR_INVALID_DATA); swprintf(name,16,L"VTS_%02u_0.IFO",entry[6]); FsFileNode* title = dvd_image_find(directory,name);
                    if (!title || title->allocatedLba < file->allocatedLba || dvd_image_be32(entry + 8) != title->allocatedLba - file->allocatedLba) return dvd_image_error(ERROR_INVALID_DATA);
                }
            }
        }
    }
    return true;
}

bool imgburn_dvd_image_layer_break_candidates(const wchar_t* path, const DvdMediaProfile* profile, DvdLayerBreakCandidate* candidates, size_t capacity, size_t* count, uint32_t* imageSectors) {
    if (count) *count = 0; if (imageSectors) *imageSectors = 0;
    if (!path || !path[0] || !count || (!candidates && capacity) || !profile || !profile->maximumL0 || profile->minimumL0 > profile->maximumL0 || profile->maximumL0 > 2086912 || !profile->mediaCapacity || profile->mediaCapacity > 4173824) return dvd_image_error(ERROR_INVALID_PARAMETER);
    DataImageSource source; if (!imgburn_data_source_open(&source,path)) return false;
    ImageBuilder builder = {0}; bool initialized = false, ok = false; DWORD error = ERROR_SUCCESS; uint8_t data[DVD_IMAGE_BLOCK]; uint32_t rootLba = 0, rootBytes = 0, volumeSectors = 0, terminator = 0;
    if (source.sectors <= 256) { error = ERROR_NOT_SUPPORTED; goto done; }
    if (!imgburn_data_source_read(&source,16,1,data)) { error = GetLastError(); goto done; }
    if (data[0] != 1 || memcmp(data + 1,"CD001\1",6)) { error = ERROR_NOT_SUPPORTED; goto done; }
    if (!dvd_image_both32(data + 80,&volumeSectors) || volumeSectors != source.sectors || dvd_image_le16(data + 128) != DVD_IMAGE_BLOCK || data[130] != 8 || data[131] || data[156 + 25] != 2 || !dvd_image_record(&source,data + 156,&rootLba,&rootBytes)) { error = ERROR_INVALID_DATA; goto done; }
    for (uint32_t lba = 17; lba < 32; lba++) {
        if (!imgburn_data_source_read(&source,lba,1,data)) { error = GetLastError(); goto done; }
        if (memcmp(data + 1,"CD001\1",6)) { error = ERROR_NOT_SUPPORTED; goto done; }
        if (data[0] == 255) { terminator = lba; break; }
        if (data[0] != 2) { error = ERROR_NOT_SUPPORTED; goto done; }
    }
    if (!terminator) { error = ERROR_NOT_SUPPORTED; goto done; }
    if (!dvd_image_udf102(&source,terminator + 1)) { error = GetLastError(); goto done; }
    if (!image_builder_init(&builder,"DVD_IMAGE_NAVIGATION")) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; } initialized = true;
    builder.config.dvdVideoCompliance = true; builder.config.fsTypes = FS_TYPE_ISO_UDF; builder.config.udfRev = UDF_REV_102; builder.config.totalImageSectors = source.sectors;
    if (!dvd_image_directory(&source,builder.rootDir,rootLba,rootBytes,true) || !dvd_image_navigation_files(&source,builder.rootDir->firstChildDir)) { error = GetLastError(); goto done; }
    ok = fs_dvd_layer_break_candidates_fixed(&builder,profile,candidates,capacity,count,dvd_image_file_read,&source); error = GetLastError();
    if (imageSectors && (ok || error == ERROR_INSUFFICIENT_BUFFER)) *imageSectors = source.sectors;
done:
    if (initialized) image_builder_destroy(&builder); imgburn_data_source_close(&source); SetLastError(ok ? ERROR_SUCCESS : error ? error : ERROR_INVALID_DATA); return ok;
}
