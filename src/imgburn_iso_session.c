#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_iso_session.h"
#include <stdlib.h>
#include <string.h>

#define ISO_BLOCK 2048u
typedef struct IsoSessionPatch { uint64_t offset; uint32_t original; bool big; } IsoSessionPatch;
typedef struct { uint32_t lba, bytes; uint16_t parent; uint8_t length, name[255]; bool seen; } IsoDirectory;
typedef struct { uint32_t lba, sectors; } IsoSpan;
typedef struct { IsoSessionSource* source; IsoSpan* metadata; size_t metadataCount; IsoSpan* files; size_t fileCount; } IsoParser;

static bool iso_error(DWORD error) { SetLastError(error); return false; }
static uint16_t iso_u16(const uint8_t* p, bool big) { return big ? (uint16_t)((uint16_t)p[0] << 8 | p[1]) : (uint16_t)((uint16_t)p[1] << 8 | p[0]); }
static uint32_t iso_u32(const uint8_t* p, bool big) { return big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static bool iso_both32(const uint8_t* p, uint32_t* value) { *value = iso_u32(p,false); return *value == iso_u32(p + 4,true) || iso_error(ERROR_INVALID_DATA); }
static bool iso_both16(const uint8_t* p, uint16_t* value) { *value = iso_u16(p,false); return *value == iso_u16(p + 2,true) || iso_error(ERROR_INVALID_DATA); }
static bool iso_cancelled(IsoSessionSource* source) { return source->cancelEvent && WaitForSingleObject(source->cancelEvent,0) == WAIT_OBJECT_0; }

static bool iso_read_at(IsoSessionSource* source, uint64_t offset, void* buffer, DWORD bytes) {
    if (iso_cancelled(source)) return iso_error(ERROR_CANCELLED);
    if (offset + bytes > (uint64_t)source->sectors * ISO_BLOCK) return iso_error(ERROR_INVALID_DATA);
    LARGE_INTEGER position; position.QuadPart = (LONGLONG)offset; DWORD actual = 0;
    if (!SetFilePointerEx(source->file,position,NULL,FILE_BEGIN) || !ReadFile(source->file,buffer,bytes,&actual,NULL)) return false;
    return actual == bytes || iso_error(ERROR_READ_FAULT);
}

static bool iso_patch(IsoSessionSource* source, uint64_t offset, uint32_t original, bool big) {
    if (source->patchCount == SIZE_MAX / sizeof(IsoSessionPatch)) return iso_error(ERROR_NOT_ENOUGH_MEMORY);
    IsoSessionPatch* grown = realloc(source->patches,(source->patchCount + 1) * sizeof(*grown)); if (!grown) return iso_error(ERROR_NOT_ENOUGH_MEMORY);
    source->patches = grown; source->patches[source->patchCount++] = (IsoSessionPatch){offset,original,big}; return true;
}

static bool iso_span_add(IsoSpan** list, size_t* count, uint32_t lba, uint32_t bytes) {
    if (!bytes) return true;
    if (*count == SIZE_MAX / sizeof(IsoSpan)) return iso_error(ERROR_NOT_ENOUGH_MEMORY);
    IsoSpan* grown = realloc(*list,(*count + 1) * sizeof(*grown)); if (!grown) return iso_error(ERROR_NOT_ENOUGH_MEMORY);
    *list = grown; grown[(*count)++] = (IsoSpan){lba,(uint32_t)(((uint64_t)bytes + ISO_BLOCK - 1) / ISO_BLOCK)}; return true;
}

static bool iso_extent(IsoSessionSource* source, uint32_t lba, uint32_t bytes) {
    uint64_t count = ((uint64_t)bytes + ISO_BLOCK - 1) / ISO_BLOCK;
    return (lba <= source->volumeSectors && count <= source->volumeSectors - lba) || iso_error(ERROR_INVALID_DATA);
}

static bool iso_record(IsoParser* parser, const uint8_t* record, unsigned available, uint64_t offset, uint32_t* lba, uint32_t* bytes) {
    unsigned length = record[0], name = available > 32 ? record[32] : 0;
    if (length < 34 || length > available || !name || 33 + name + ((name & 1) ? 0 : 1) > length) return iso_error(ERROR_INVALID_DATA);
    if (record[1] || record[26] || record[27] || (record[25] & 0x78) || length != 33 + name + ((name & 1) ? 0 : 1)) return iso_error(ERROR_NOT_SUPPORTED);
    uint16_t sequence; if (!iso_both32(record + 2,lba) || !iso_both32(record + 10,bytes) || !iso_both16(record + 28,&sequence)) return false;
    if (sequence != 1) return iso_error(ERROR_NOT_SUPPORTED);
    if (!iso_extent(parser->source,*lba,*bytes)) return false;
    /* 空文件extent为0时按本接口约定保留；非零空extent仍按地址重定位。 */
    if (*lba && (!iso_patch(parser->source,offset + 2,*lba,false) || !iso_patch(parser->source,offset + 6,*lba,true))) return false;
    return true;
}

static bool iso_paths(IsoParser* parser, uint32_t lba, uint32_t bytes, bool big, bool joliet, IsoDirectory** output, size_t* count) {
    *output = NULL; *count = 0; if (!lba || !bytes) return iso_error(ERROR_INVALID_DATA);
    if (!iso_extent(parser->source,lba,bytes) || !iso_span_add(&parser->metadata,&parser->metadataCount,lba,bytes)) return false;
    for (uint32_t position = 0; position < bytes;) {
        uint8_t entry[264] = {0}; uint64_t offset = (uint64_t)lba * ISO_BLOCK + position;
        if (bytes - position < 8) goto invalid; if (!iso_read_at(parser->source,offset,entry,8)) goto failed;
        unsigned length = entry[0], size = 8 + length + (length & 1); uint32_t extent = iso_u32(entry + 2,big); uint16_t parent = iso_u16(entry + 6,big);
        if (!length || size > bytes - position || *count >= UINT16_MAX || !extent || extent >= parser->source->volumeSectors) goto invalid;
        if (entry[1]) { SetLastError(ERROR_NOT_SUPPORTED); goto failed; }
        if (!iso_read_at(parser->source,offset + 8,entry + 8,size - 8)) goto failed;
        if (!*count ? (length != 1 || entry[8] || parent != 1) : (!parent || parent > *count || (joliet && (length & 1)))) goto invalid;
        IsoDirectory* grown = realloc(*output,(*count + 1) * sizeof(*grown)); if (!grown) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto failed; }
        *output = grown; IsoDirectory* dir = &grown[(*count)++]; memset(dir,0,sizeof(*dir)); dir->lba = extent; dir->parent = parent; dir->length = (uint8_t)length; memcpy(dir->name,entry + 8,length);
        if (!iso_patch(parser->source,offset + 2,extent,big)) goto failed; position += size;
    }
    return true;
invalid:
    SetLastError(ERROR_INVALID_DATA);
failed:
    free(*output); *output = NULL; *count = 0; return false;
}

static int iso_directory_key(const void* a, const void* b) {
    const IsoDirectory* x = *(const IsoDirectory* const*)a; const IsoDirectory* y = *(const IsoDirectory* const*)b;
    if (x->parent != y->parent) return x->parent < y->parent ? -1 : 1;
    unsigned length = x->length < y->length ? x->length : y->length; int compared = memcmp(x->name,y->name,length);
    return compared ? compared : x->length < y->length ? -1 : x->length > y->length;
}

static bool iso_directories(IsoParser* parser, IsoDirectory* dirs, size_t count, uint32_t rootLba, uint32_t rootBytes, bool joliet) {
    if (!count || dirs[0].lba != rootLba) return iso_error(ERROR_INVALID_DATA);
    IsoDirectory** sorted = malloc(count * sizeof(*sorted)); if (!sorted) return iso_error(ERROR_NOT_ENOUGH_MEMORY);
    bool ok = false; uint8_t block[ISO_BLOCK];
    for (size_t i = 0; i < count; i++) {
        sorted[i] = &dirs[i]; uint32_t lba, bytes;
        if (!iso_read_at(parser->source,(uint64_t)dirs[i].lba * ISO_BLOCK,block,ISO_BLOCK) || !iso_record(parser,block,ISO_BLOCK,(uint64_t)dirs[i].lba * ISO_BLOCK,&lba,&bytes)) goto done;
        if (block[25] != 2 || block[32] != 1 || block[33] || lba != dirs[i].lba || !bytes || bytes % ISO_BLOCK) { SetLastError(ERROR_INVALID_DATA); goto done; }
        dirs[i].bytes = bytes; if (!iso_span_add(&parser->metadata,&parser->metadataCount,lba,bytes)) goto done;
    }
    if (dirs[0].bytes != rootBytes) { SetLastError(ERROR_INVALID_DATA); goto done; }
    qsort(sorted,count,sizeof(*sorted),iso_directory_key);
    for (size_t i = 1; i < count; i++) if (!iso_directory_key(&sorted[i - 1],&sorted[i])) { SetLastError(ERROR_INVALID_DATA); goto done; }
    dirs[0].seen = true;
    for (size_t i = 0; i < count; i++) {
        uint8_t pending[255]; unsigned pendingLength = 0; unsigned recordNumber = 0;
        for (uint32_t part = 0; part < dirs[i].bytes / ISO_BLOCK; part++) {
            uint64_t base = ((uint64_t)dirs[i].lba + part) * ISO_BLOCK;
            if (!iso_read_at(parser->source,base,block,ISO_BLOCK)) goto done;
            for (unsigned offset = 0; offset < ISO_BLOCK;) {
                const uint8_t* record = block + offset;
                if (!record[0]) { for (unsigned j = offset; j < ISO_BLOCK; j++) if (block[j]) { SetLastError(ERROR_INVALID_DATA); goto done; } break; }
                uint32_t extent, bytes; if (!iso_record(parser,record,ISO_BLOCK - offset,base + offset,&extent,&bytes)) goto done;
                bool directory = (record[25] & 2) != 0; unsigned length = record[32];
                if (recordNumber < 2) {
                    size_t expected = recordNumber ? dirs[i].parent - 1u : i;
                    if (!directory || record[25] != 2 || length != 1 || record[33] != recordNumber || extent != dirs[expected].lba || bytes != dirs[expected].bytes) { SetLastError(ERROR_INVALID_DATA); goto done; }
                } else if (directory) {
                    if ((record[25] & 0x80) || pendingLength) { SetLastError(ERROR_INVALID_DATA); goto done; }
                    IsoDirectory key = {0}; key.parent = (uint16_t)(i + 1); key.length = (uint8_t)length; memcpy(key.name,record + 33,length); IsoDirectory* pointer = &key;
                    IsoDirectory** found = bsearch(&pointer,sorted,count,sizeof(*sorted),iso_directory_key);
                    if (!found || (*found)->seen || (*found)->lba != extent || (*found)->bytes != bytes) { SetLastError(ERROR_INVALID_DATA); goto done; } (*found)->seen = true;
                } else {
                    if (joliet && (length & 1)) { SetLastError(ERROR_INVALID_DATA); goto done; }
                    if (pendingLength && (length != pendingLength || memcmp(record + 33,pending,length))) { SetLastError(ERROR_INVALID_DATA); goto done; }
                    pendingLength = record[25] & 0x80 ? length : 0; if (pendingLength) memcpy(pending,record + 33,length);
                    if (!iso_span_add(&parser->files,&parser->fileCount,extent,bytes)) goto done;
                }
                recordNumber++; offset += record[0];
            }
        }
        if (recordNumber < 2 || pendingLength) { SetLastError(ERROR_INVALID_DATA); goto done; }
    }
    for (size_t i = 0; i < count; i++) if (!dirs[i].seen) { SetLastError(ERROR_INVALID_DATA); goto done; } ok = true;
done:
    free(sorted); return ok;
}

static bool iso_namespace(IsoParser* parser, const uint8_t* vd, uint32_t descriptor, bool joliet) {
    IsoSessionSource* source = parser->source; uint32_t volume, tableBytes, rootLba, rootBytes; uint16_t setSize, sequence, blockSize;
    if (!iso_both32(vd + 80,&volume) || !iso_both32(vd + 132,&tableBytes) || !iso_both16(vd + 120,&setSize) || !iso_both16(vd + 124,&sequence) || !iso_both16(vd + 128,&blockSize)) return false;
    if (setSize != 1 || sequence != 1 || blockSize != ISO_BLOCK || vd[881] != 1) return iso_error(ERROR_NOT_SUPPORTED);
    if (!volume || volume > source->sectors || volume != source->volumeSectors || !tableBytes) return iso_error(ERROR_INVALID_DATA);
    if (joliet && (memcmp(vd + 88,"%/@",3) && memcmp(vd + 88,"%/C",3) && memcmp(vd + 88,"%/E",3))) return iso_error(ERROR_NOT_SUPPORTED);
    if (!iso_record(parser,vd + 156,34,(uint64_t)descriptor * ISO_BLOCK + 156,&rootLba,&rootBytes)) return false;
    if (vd[181] != 2 || vd[188] != 1 || vd[189]) return iso_error(ERROR_INVALID_DATA);
    IsoDirectory* dirs = NULL; size_t count = 0; bool ok = false;
    for (unsigned i = 0; i < 4; i++) {
        unsigned position = i < 2 ? 140 + i * 4 : 148 + (i - 2) * 4; bool big = i >= 2; uint32_t lba = iso_u32(vd + position,big);
        if (!lba) { if (i == 0 || i == 2) { SetLastError(ERROR_INVALID_DATA); goto done; } continue; }
        if (!iso_patch(source,(uint64_t)descriptor * ISO_BLOCK + position,lba,big)) goto done;
        IsoDirectory* copy = NULL; size_t copied = 0; if (!iso_paths(parser,lba,tableBytes,big,joliet,&copy,&copied)) goto done;
        if (!dirs) { dirs = copy; count = copied; }
        else {
            bool equal = copied == count;
            for (size_t j = 0; equal && j < count; j++) equal = dirs[j].lba == copy[j].lba && dirs[j].parent == copy[j].parent && dirs[j].length == copy[j].length && !memcmp(dirs[j].name,copy[j].name,dirs[j].length);
            free(copy); if (!equal) { SetLastError(ERROR_INVALID_DATA); goto done; }
        }
    }
    ok = iso_directories(parser,dirs,count,rootLba,rootBytes,joliet);
done:
    free(dirs); return ok;
}

static int iso_patch_order(const void* a, const void* b) { const IsoSessionPatch* x = a; const IsoSessionPatch* y = b; return x->offset < y->offset ? -1 : x->offset > y->offset; }
static int iso_span_order(const void* a, const void* b) { const IsoSpan* x = a; const IsoSpan* y = b; return x->lba < y->lba ? -1 : x->lba > y->lba ? 1 : x->sectors < y->sectors ? -1 : x->sectors > y->sectors; }

static bool iso_finish(IsoParser* parser) {
    IsoSessionSource* source = parser->source; qsort(source->patches,source->patchCount,sizeof(*source->patches),iso_patch_order); size_t used = 0;
    for (size_t i = 0; i < source->patchCount; i++) {
        IsoSessionPatch item = source->patches[i];
        if (used && item.offset < source->patches[used - 1].offset + 4) {
            IsoSessionPatch* last = &source->patches[used - 1]; if (item.offset != last->offset || item.original != last->original || item.big != last->big) return iso_error(ERROR_NOT_SUPPORTED); continue;
        }
        source->patches[used++] = item;
    }
    source->patchCount = used; qsort(parser->metadata,parser->metadataCount,sizeof(*parser->metadata),iso_span_order);
    for (size_t i = 1; i < parser->metadataCount; i++) {
        IsoSpan a = parser->metadata[i - 1], b = parser->metadata[i];
        if ((uint64_t)a.lba + a.sectors > b.lba && (a.lba != b.lba || a.sectors != b.sectors)) return iso_error(ERROR_INVALID_DATA);
    }
    for (size_t i = 0; i < parser->fileCount; i++) {
        IsoSpan file = parser->files[i]; size_t low = 0, high = parser->metadataCount;
        while (low < high) { size_t mid = low + (high - low) / 2; if (parser->metadata[mid].lba < (uint64_t)file.lba + file.sectors) low = mid + 1; else high = mid; }
        if (low && (uint64_t)parser->metadata[low - 1].lba + parser->metadata[low - 1].sectors > file.lba) return iso_error(ERROR_INVALID_DATA);
    }
    return true;
}

bool imgburn_iso_session_open(IsoSessionSource* source, const wchar_t* path, HANDLE cancelEvent) {
    if (!source || !path) return iso_error(ERROR_INVALID_PARAMETER); memset(source,0,sizeof(*source)); source->file = INVALID_HANDLE_VALUE; source->cancelEvent = cancelEvent;
    source->file = CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL); if (source->file == INVALID_HANDLE_VALUE) return false;
    IsoParser parser = {0}; parser.source = source; bool ok = false; LARGE_INTEGER size; uint8_t block[ISO_BLOCK], primary[ISO_BLOCK] = {0}, supplementary[ISO_BLOCK] = {0}; uint32_t primaryLba = 0, supplementaryLba = 0, terminator = 0;
    if (!GetFileSizeEx(source->file,&size)) goto done;
    if (size.QuadPart < 18 * ISO_BLOCK || size.QuadPart % ISO_BLOCK || size.QuadPart / ISO_BLOCK > UINT32_MAX) { SetLastError(ERROR_INVALID_DATA); goto done; } source->sectors = (uint32_t)(size.QuadPart / ISO_BLOCK);
    if (!iso_read_at(source,0,block,ISO_BLOCK)) goto done;
    if ((block[510] == 0x55 && block[511] == 0xAA) || !memcmp(block + 512,"EFI PART",8)) { SetLastError(ERROR_NOT_SUPPORTED); goto done; }
    for (uint32_t lba = 16; lba < source->sectors; lba++) {
        if (!iso_read_at(source,(uint64_t)lba * ISO_BLOCK,block,ISO_BLOCK)) goto done;
        if (memcmp(block + 1,"CD001",5) || block[6] != 1) { SetLastError(ERROR_NOT_SUPPORTED); goto done; }
        if (block[0] == 1 && !primaryLba) { memcpy(primary,block,ISO_BLOCK); primaryLba = lba; }
        else if (block[0] == 2 && !supplementaryLba) { memcpy(supplementary,block,ISO_BLOCK); supplementaryLba = lba; source->joliet = true; }
        else if (block[0] == 255) { terminator = lba; break; }
        else { SetLastError(ERROR_NOT_SUPPORTED); goto done; }
    }
    if (!primaryLba || !terminator) { SetLastError(ERROR_INVALID_DATA); goto done; }
    if (!iso_both32(primary + 80,&source->volumeSectors)) goto done;
    /* 标准VRS桥接不能只修ISO而留下UDF绝对位置；这些来源明确留待独立支持。 */
    for (uint32_t lba = 16; lba < 32 && lba < source->sectors; lba++) {
        if (!iso_read_at(source,(uint64_t)lba * ISO_BLOCK,block,ISO_BLOCK)) goto done;
        if (!block[0] && block[6] == 1 && (!memcmp(block + 1,"BEA01",5) || !memcmp(block + 1,"NSR02",5) || !memcmp(block + 1,"NSR03",5) || !memcmp(block + 1,"TEA01",5))) { SetLastError(ERROR_NOT_SUPPORTED); goto done; }
    }
    if (!iso_span_add(&parser.metadata,&parser.metadataCount,0,(terminator + 1) * ISO_BLOCK) || !iso_namespace(&parser,primary,primaryLba,false) || (supplementaryLba && !iso_namespace(&parser,supplementary,supplementaryLba,true)) || !iso_finish(&parser)) goto done;
    ok = true;
done:;
    DWORD error = ok ? ERROR_SUCCESS : GetLastError(); free(parser.metadata); free(parser.files); if (!ok) imgburn_iso_session_close(source); SetLastError(error); return ok;
}

bool imgburn_iso_session_set_start(IsoSessionSource* source, uint32_t startLba) {
    if (!source || !source->file || source->file == INVALID_HANDLE_VALUE) return iso_error(ERROR_INVALID_PARAMETER);
    if ((uint64_t)startLba + source->sectors > UINT32_MAX) return iso_error(ERROR_ARITHMETIC_OVERFLOW);
    source->startLba = startLba; return true;
}

bool imgburn_iso_session_read(IsoSessionSource* source, uint32_t sourceLba, uint16_t sectors, void* buffer) {
    if (!source || !buffer || !sectors || sourceLba >= source->sectors || sectors > source->sectors - sourceLba) return iso_error(ERROR_INVALID_PARAMETER);
    uint64_t begin = (uint64_t)sourceLba * ISO_BLOCK, end = begin + (uint32_t)sectors * ISO_BLOCK;
    if (!iso_read_at(source,begin,buffer,(DWORD)(end - begin))) return false;
    size_t low = 0, high = source->patchCount; uint64_t earliest = begin > 3 ? begin - 3 : 0;
    while (low < high) { size_t mid = low + (high - low) / 2; if (source->patches[mid].offset < earliest) low = mid + 1; else high = mid; }
    for (size_t i = low; i < source->patchCount && source->patches[i].offset < end; i++) {
        const IsoSessionPatch* patch = &source->patches[i]; uint32_t value = patch->original + source->startLba;
        for (unsigned j = 0; j < 4; j++) if (patch->offset + j >= begin && patch->offset + j < end) ((uint8_t*)buffer)[(size_t)(patch->offset + j - begin)] = (uint8_t)(value >> ((patch->big ? 3 - j : j) * 8));
    }
    return true;
}

void imgburn_iso_session_close(IsoSessionSource* source) {
    if (!source) return; if (source->file && source->file != INVALID_HANDLE_VALUE) CloseHandle(source->file); free(source->patches); memset(source,0,sizeof(*source)); source->file = INVALID_HANDLE_VALUE;
}
