#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_cdtext.h"
#include <stdlib.h>
#include <string.h>

static uint16_t cdtext_crc(const uint8_t* data) {
    uint16_t value = 0;
    for (unsigned i = 0; i < 16; i++) {
        value ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; bit++) value = (uint16_t)(((unsigned)value << 1) ^ ((value & 0x8000) ? 0x1021 : 0));
    }
    return (uint16_t)~value;
}

static bool cdtext_error(DWORD error) { SetLastError(error); return false; }

void imgburn_cdtext_free(CdText* text) {
    if (!text) return;
    free(text->packs); memset(text,0,sizeof(*text));
}

static bool cdtext_positions(const uint8_t* packs, size_t bytes, unsigned block, unsigned type, const CdTextBlock* info) {
    unsigned track = 0, position = 0; bool seen = false, finished = false, previousNonempty = false, tab = false;
    for (size_t offset = 0; offset < bytes; offset += 18) {
        const uint8_t* pack = packs + offset;
        if (((unsigned)(pack[3] >> 4) & 7) != block || (unsigned)pack[0] != type) continue;
        if (!seen) { track = pack[1]; seen = true; }
        if (finished || (unsigned)pack[1] != track || ((unsigned)pack[3] & 15) != (position < 15 ? position : 15)) return cdtext_error(ERROR_INVALID_DATA);
        for (unsigned i = 4; i < 16; i++) {
            uint8_t value = pack[i];
            if (finished) { if (value) return cdtext_error(ERROR_INVALID_DATA); continue; }
            if (!value) {
                if (tab && (type > 0x85 || track <= info->firstTrack || !previousNonempty)) return cdtext_error(ERROR_INVALID_DATA);
                previousNonempty = position != 0; position = 0; tab = false;
                track = track ? track + 1 : info->firstTrack; finished = type == 0x86 || track > info->lastTrack;
            } else {
                if (tab || (value == 9 && (position || type > 0x85))) return cdtext_error(ERROR_INVALID_DATA);
                tab = value == 9; position++;
            }
        }
    }
    return !position || cdtext_error(ERROR_INVALID_DATA);
}

bool imgburn_cdtext_parse(const uint8_t* packs, size_t bytes, CdText* text) {
    if (!text) return cdtext_error(ERROR_INVALID_PARAMETER);
    memset(text,0,sizeof(*text));
    if (!packs || !bytes || bytes % 18 || bytes > IMGBURN_CDTEXT_MAX_BYTES) return cdtext_error(ERROR_INVALID_DATA);
    CdText result = {0}; uint16_t counts[8][16] = {{0}}, sequence[8] = {0}; uint8_t sizes[8][36] = {{0}}, sizeParts[8] = {0}, previousType[8] = {0};
    bool present[8] = {0}, dbcc[8] = {0}, extended[8] = {0};
    for (size_t offset = 0; offset < bytes; offset += 18) {
        const uint8_t* pack = packs + offset; unsigned block = (pack[3] >> 4) & 7, type = pack[0] & 15;
        if (pack[0] < 0x80 || pack[0] > 0x8F || sequence[block] >= 256 || pack[2] != sequence[block] || (present[block] && pack[0] < previousType[block])) return cdtext_error(ERROR_INVALID_DATA);
        uint16_t stored = (uint16_t)((unsigned)pack[16] << 8 | pack[17]), expected = cdtext_crc(pack);
        if (stored != expected) { if (stored) return cdtext_error(ERROR_CRC); result.blocks[block].omittedCrcCount++; }
        bool doubleByte = (pack[3] & 0x80) != 0;
        dbcc[block] = dbcc[block] || doubleByte; present[block] = true; previousType[block] = pack[0]; sequence[block]++; counts[block][type]++;
        if (pack[1] & 0x80) extended[block] = true;
        if ((pack[0] == 0x88 || pack[0] == 0x89 || pack[0] == 0x8F) && (pack[3] & 15)) return cdtext_error(ERROR_INVALID_DATA);
        if (pack[0] == 0x8F) {
            if (pack[1] > 2 || (sizeParts[block] & (1 << pack[1]))) return cdtext_error(ERROR_INVALID_DATA);
            sizeParts[block] |= (uint8_t)(1 << pack[1]); memcpy(sizes[block] + pack[1] * 12,pack + 4,12);
        }
    }
    for (unsigned block = 0; block < 8; block++) {
        if (!present[block]) continue;
        if (block != result.blockCount || sizeParts[block] != 7 || counts[block][15] != 3) return cdtext_error(ERROR_INVALID_DATA);
        result.blockCount++; CdTextBlock* info = &result.blocks[block]; const uint8_t* size = sizes[block];
        info->characterCode = size[0]; info->firstTrack = size[1]; info->lastTrack = size[2]; info->language = size[28 + block]; info->packCount = sequence[block]; info->doubleByte = dbcc[block];
        info->supported = (size[0] == 0 || size[0] == 1) && !dbcc[block] && !extended[block];
        if (!size[1] || size[1] > size[2] || size[2] > 99 || (size[3] & 0x38) || ((size[3] & 0x40) && !(size[3] & 0x80))) return cdtext_error(ERROR_INVALID_DATA);
        for (unsigned type = 0; type < 16; type++) {
            if (counts[block][type] != size[4 + type]) return cdtext_error(ERROR_INVALID_DATA);
            if (counts[block][type] && type > 6 && type != 14 && type != 15) info->hasUnmapped = true;
        }
        if (extended[block] || size[3]) info->hasUnmapped = true;
        for (unsigned other = 0; other < 8; other++) if (size[20 + other] != (sequence[other] ? sequence[other] - 1 : 0) || (other && size[28 + other] != sizes[0][28 + other]) || (!present[other] && size[28 + other])) return cdtext_error(ERROR_INVALID_DATA);
        if (block && (size[1] != sizes[0][1] || size[2] != sizes[0][2] || size[28] != sizes[0][28])) return cdtext_error(ERROR_INVALID_DATA);
    }
    for (size_t offset = 0; offset < bytes; offset += 18) {
        const uint8_t* pack = packs + offset; unsigned block = (pack[3] >> 4) & 7;
        if (!(pack[1] & 0x80) && ((pack[0] >= 0x80 && pack[0] <= 0x85) || pack[0] == 0x8E) && pack[1] && (pack[1] < result.blocks[block].firstTrack || pack[1] > result.blocks[block].lastTrack)) return cdtext_error(ERROR_INVALID_DATA);
        if (!(pack[1] & 0x80) && pack[0] == 0x86 && pack[1]) return cdtext_error(ERROR_INVALID_DATA);
    }
    for (unsigned block = 0; block < result.blockCount; block++) if (result.blocks[block].supported) {
        for (unsigned type = 0x80; type <= 0x86; type++) if (!cdtext_positions(packs,bytes,block,type,&result.blocks[block])) return false;
        if (!cdtext_positions(packs,bytes,block,0x8E,&result.blocks[block])) return false;
    }
    result.packs = malloc(bytes);
    if (!result.packs) return cdtext_error(ERROR_NOT_ENOUGH_MEMORY);
    memcpy(result.packs,packs,bytes); result.bytes = bytes; *text = result; SetLastError(ERROR_SUCCESS); return true;
}

static char* cdtext_field(CueSheet* sheet, unsigned type, unsigned track, size_t* capacity) {
    if (!track) {
        *capacity = type == 0x8E ? sizeof(sheet->cdTextUpc) : sizeof(sheet->discTitle);
        return type == 0x80 ? sheet->discTitle : type == 0x81 ? sheet->discPerformer : type == 0x82 ? sheet->songwriter : type == 0x83 ? sheet->composer : type == 0x84 ? sheet->arranger : type == 0x85 ? sheet->message : type == 0x86 ? sheet->discId : sheet->cdTextUpc;
    }
    CueTrack* item = &sheet->tracks[track - (unsigned)sheet->tracks[0].trackNumber];
    *capacity = type == 0x8E ? sizeof(item->isrc) : sizeof(item->title);
    return type == 0x80 ? item->title : type == 0x81 ? item->performer : type == 0x82 ? item->songwriter : type == 0x83 ? item->composer : type == 0x84 ? item->arranger : type == 0x85 ? item->message : item->isrc;
}

static bool cdtext_code(const char* value, bool upc) {
    size_t length = strlen(value); if (!length) return true;
    if (length != (size_t)(upc ? 13 : 12)) return cdtext_error(ERROR_INVALID_DATA);
    for (size_t i = 0; i < length; i++) {
        bool digit = value[i] >= '0' && value[i] <= '9', letter = value[i] >= 'A' && value[i] <= 'Z';
        if (upc ? !digit : i < 2 ? !letter : i < 5 ? !(letter || digit) : !digit) return cdtext_error(ERROR_INVALID_DATA);
    }
    return true;
}

static bool cdtext_extract_type(const CdText* text, unsigned block, unsigned type, CueSheet* sheet) {
    const CdTextBlock* info = &text->blocks[block]; unsigned track = 0; size_t used = 0; bool finished = false, tab = false;
    for (size_t offset = 0; offset < text->bytes; offset += 18) {
        const uint8_t* pack = text->packs + offset;
        if (((unsigned)(pack[3] >> 4) & 7) != block || (unsigned)pack[0] != type) continue;
        if (!used && !finished && !track) track = pack[1];
        for (unsigned i = 4; i < 16; i++) {
            if (finished) continue;
            size_t capacity; char* field = cdtext_field(sheet,type,track,&capacity); uint8_t value = pack[i];
            if (!value) {
                if (tab) { size_t previousCapacity; char* previous = cdtext_field(sheet,type,track - 1,&previousCapacity); size_t length = strlen(previous); if (!length || length >= capacity) return cdtext_error(ERROR_INVALID_DATA); memcpy(field,previous,length + 1); }
                if (type == 0x8E && !cdtext_code(field,!track)) return false;
                track = track ? track + 1 : info->firstTrack; used = 0; tab = false; finished = type == 0x86 || track > info->lastTrack; continue;
            }
            if (value == 9) { tab = true; continue; }
            if (value < 0x20 || value == 0x7F || (value >= 0x80 && value < 0xA0) || (value >= 0x80 && info->characterCode == 1 && type <= 0x85) || value == '"') return cdtext_error(ERROR_NO_UNICODE_TRANSLATION);
            size_t needed = value < 0x80 ? 1 : 2;
            if (used + needed >= capacity) return cdtext_error(ERROR_INSUFFICIENT_BUFFER);
            if (needed == 2) { field[used++] = (char)(0xC0 | (value >> 6)); field[used++] = (char)(0x80 | (value & 0x3F)); }
            else field[used++] = (char)value;
            field[used] = 0;
        }
    }
    return true;
}

bool imgburn_cdtext_extract(const CdText* text, unsigned block, CueSheet* sheet) {
    if (!text || !text->packs || block >= text->blockCount || !sheet || sheet->trackCount < 1 || sheet->trackCount > MAX_CUE_TRACKS) return cdtext_error(ERROR_INVALID_PARAMETER);
    const CdTextBlock* info = &text->blocks[block];
    if (!info->supported) return cdtext_error(ERROR_NOT_SUPPORTED);
    if (sheet->trackCount != info->lastTrack - info->firstTrack + 1) return cdtext_error(ERROR_INVALID_DATA);
    for (int i = 0; i < sheet->trackCount; i++) if (sheet->tracks[i].trackNumber != info->firstTrack + i) return cdtext_error(ERROR_INVALID_DATA);
    CueSheet* copy = malloc(sizeof(*copy));
    if (!copy) return cdtext_error(ERROR_NOT_ENOUGH_MEMORY);
    *copy = *sheet;
    copy->discTitle[0] = copy->discPerformer[0] = copy->songwriter[0] = copy->composer[0] = copy->arranger[0] = copy->message[0] = copy->discId[0] = copy->cdTextUpc[0] = 0;
    for (int i = 0; i < copy->trackCount; i++) copy->tracks[i].title[0] = copy->tracks[i].performer[0] = copy->tracks[i].songwriter[0] = copy->tracks[i].composer[0] = copy->tracks[i].arranger[0] = copy->tracks[i].message[0] = copy->tracks[i].isrc[0] = 0;
    bool ok = true;
    for (unsigned type = 0x80; ok && type <= 0x86; type++) ok = cdtext_extract_type(text,block,type,copy);
    if (ok) ok = cdtext_extract_type(text,block,0x8E,copy);
    DWORD error = GetLastError(); if (ok) *sheet = *copy; free(copy); SetLastError(ok ? ERROR_SUCCESS : error); return ok;
}

typedef struct { uint8_t* packs; unsigned count, used; } CdTextBuilder;

static bool cdtext_byte(CdTextBuilder* builder, unsigned type, unsigned track, unsigned position, uint8_t value) {
    if (builder->count >= 253) return cdtext_error(ERROR_FILE_TOO_LARGE);
    uint8_t* pack = builder->packs + builder->count * 18;
    if (!builder->used) { memset(pack,0,18); pack[0] = (uint8_t)type; pack[1] = (uint8_t)track; pack[2] = (uint8_t)builder->count; pack[3] = (uint8_t)(position < 15 ? position : 15); }
    pack[4 + builder->used++] = value;
    if (builder->used == 12) { uint16_t crc = cdtext_crc(pack); pack[16] = (uint8_t)(crc >> 8); pack[17] = (uint8_t)crc; builder->used = 0; builder->count++; }
    return true;
}

static void cdtext_finish(CdTextBuilder* builder) {
    if (!builder->used) return;
    uint8_t* pack = builder->packs + builder->count * 18; uint16_t crc = cdtext_crc(pack);
    pack[16] = (uint8_t)(crc >> 8); pack[17] = (uint8_t)crc; builder->used = 0; builder->count++;
}

static bool cdtext_latin1(const char* value, size_t capacity, uint8_t latin[128], unsigned* length) {
    size_t bytes = strnlen(value,capacity);
    if (bytes == capacity) return cdtext_error(ERROR_INVALID_DATA);
    wchar_t wide[128]; int count = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,(int)bytes,wide,128);
    if (bytes && !count) return false;
    for (int i = 0; i < count; i++) {
        unsigned character = wide[i];
        if (character < 0x20 || character == 0x7F || (character >= 0x80 && character < 0xA0) || character > 0xFF) return cdtext_error(ERROR_NO_UNICODE_TRANSLATION);
        latin[i] = (uint8_t)character;
    }
    *length = (unsigned)count; return true;
}

bool imgburn_cdtext_generate(const CueSheet* sheet, CdText* text) {
    if (!text) return cdtext_error(ERROR_INVALID_PARAMETER);
    memset(text,0,sizeof(*text));
    if (!sheet || sheet->trackCount < 1 || sheet->trackCount > MAX_CUE_TRACKS) return cdtext_error(ERROR_INVALID_PARAMETER);
    if (sheet->cdTextFile[0]) return cdtext_error(ERROR_NOT_SUPPORTED);
    unsigned first = (unsigned)sheet->tracks[0].trackNumber, last = first + (unsigned)sheet->trackCount - 1;
    if (!first || last > 99) return cdtext_error(ERROR_INVALID_DATA);
    for (int i = 0; i < sheet->trackCount; i++) if ((unsigned)sheet->tracks[i].trackNumber != first + (unsigned)i) return cdtext_error(ERROR_INVALID_DATA);
    CdTextBuilder builder = {0}; builder.packs = calloc(256,18);
    if (!builder.packs) return cdtext_error(ERROR_NOT_ENOUGH_MEMORY);
    bool ok = false; unsigned counts[16] = {0}; unsigned types[8] = {0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x8E};
    for (unsigned category = 0; category < 8; category++) {
        unsigned type = types[category], before = builder.count; bool present = type == 0x80;
        unsigned lastItem = type == 0x86 ? 0 : (unsigned)sheet->trackCount;
        for (unsigned item = 0; item <= lastItem; item++) { size_t capacity; const char* value = cdtext_field((CueSheet*)sheet,type,item ? first + item - 1 : 0,&capacity); if (value[0]) present = true; }
        if (!present) continue;
        for (unsigned item = 0; item <= lastItem; item++) {
            unsigned track = item ? first + item - 1 : 0; size_t capacity; const char* value = cdtext_field((CueSheet*)sheet,type,track,&capacity); uint8_t latin[128]; unsigned length = 0;
            if (!cdtext_latin1(value,capacity,latin,&length)) goto cleanup;
            if (type == 0x8E) {
                char code[16]; if (length >= sizeof(code)) { cdtext_error(ERROR_INVALID_DATA); goto cleanup; } memcpy(code,latin,length); code[length] = 0;
                /* 与SAO的Q子通道一致，只规范化生成的轨道ISRC，不修改CUE和外部原始包。 */
                if (track) for (unsigned i = 0; i < length; i++) if (code[i] >= 'a' && code[i] <= 'z') code[i] = (char)(code[i] - 'a' + 'A');
                if (!cdtext_code(code,!track)) goto cleanup; memcpy(latin,code,length);
            }
            for (unsigned i = 0; i <= length; i++) if (!cdtext_byte(&builder,type,track,i,i < length ? latin[i] : 0)) goto cleanup;
        }
        cdtext_finish(&builder); counts[type & 15] = builder.count - before;
    }
    uint8_t size[36] = {0}; size[1] = (uint8_t)first; size[2] = (uint8_t)last; counts[15] = 3;
    for (unsigned i = 0; i < 16; i++) size[4 + i] = (uint8_t)counts[i];
    size[20] = (uint8_t)(builder.count + 2); size[28] = 9;
    for (unsigned part = 0; part < 3; part++) {
        uint8_t* pack = builder.packs + builder.count * 18; pack[0] = 0x8F; pack[1] = (uint8_t)part; pack[2] = (uint8_t)builder.count; memcpy(pack + 4,size + part * 12,12);
        uint16_t crc = cdtext_crc(pack); pack[16] = (uint8_t)(crc >> 8); pack[17] = (uint8_t)crc; builder.count++;
    }
    ok = imgburn_cdtext_parse(builder.packs,(size_t)builder.count * 18,text);
cleanup:
    { DWORD error = GetLastError(); free(builder.packs); SetLastError(ok ? ERROR_SUCCESS : error); }
    return ok;
}

bool imgburn_cdtext_encode_rw(const CdText* text, uint64_t firstSector, uint16_t sectorCount, uint8_t* output, size_t outputBytes) {
    if (!text || !text->packs || !output || !sectorCount || !text->bytes || text->bytes % 18 || text->bytes > IMGBURN_CDTEXT_MAX_BYTES) return cdtext_error(ERROR_INVALID_PARAMETER);
    size_t required = (size_t)sectorCount * 96;
    if (outputBytes < required) return cdtext_error(ERROR_INSUFFICIENT_BUFFER);
    uintptr_t sourceAddress = (uintptr_t)text->packs, targetAddress = (uintptr_t)output;
    if (sourceAddress > UINTPTR_MAX - text->bytes || targetAddress > UINTPTR_MAX - required || (sourceAddress < targetAddress + required && targetAddress < sourceAddress + text->bytes)) return cdtext_error(ERROR_INVALID_PARAMETER);
    CdText validated = {0};
    if (!imgburn_cdtext_parse(text->packs, text->bytes, &validated)) return false;
    imgburn_cdtext_free(&validated);
    size_t packCount = text->bytes / 18, packIndex = (size_t)(firstSector % packCount) * 4 % packCount;
    for (unsigned sector = 0; sector < sectorCount; sector++) {
        for (unsigned item = 0; item < 4; item++) {
            uint8_t pack[18]; memcpy(pack, text->packs + packIndex * 18, sizeof(pack));
            uint16_t crc = cdtext_crc(pack); pack[16] = (uint8_t)(crc >> 8); pack[17] = (uint8_t)crc;
            for (unsigned group = 0; group < 6; group++) {
                unsigned bits = (unsigned)pack[group * 3] << 16 | (unsigned)pack[group * 3 + 1] << 8 | pack[group * 3 + 2];
                for (unsigned symbol = 0; symbol < 4; symbol++) output[(size_t)sector * 96 + item * 24 + group * 4 + symbol] = (uint8_t)((bits >> (18 - symbol * 6)) & 0x3F);
            }
            if (++packIndex == packCount) packIndex = 0;
        }
    }
    SetLastError(ERROR_SUCCESS); return true;
}
