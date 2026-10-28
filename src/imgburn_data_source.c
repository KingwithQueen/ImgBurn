/* 将分卷、首卷偏移和逻辑轨道长度映射到同一个只读数据流。 */
#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_data_source.h"
#include "imgburn_image.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct DataImagePart { HANDLE file; uint64_t offset, start, bytes; } DataImagePart;

void imgburn_data_source_close(DataImageSource* source) {
    if (!source) return;
    for (unsigned i = 0; i < source->partCount; i++) if (source->parts[i].file && source->parts[i].file != INVALID_HANDLE_VALUE) CloseHandle(source->parts[i].file);
    if (source->descriptor && source->descriptor != INVALID_HANDLE_VALUE) CloseHandle(source->descriptor);
    free(source->parts); memset(source,0,sizeof(*source));
}

bool imgburn_data_source_open(DataImageSource* source, const wchar_t* path) {
    if (!source || !path || !path[0]) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    memset(source,0,sizeof(*source));
    const wchar_t* extension = wcsrchr(path,L'.');
    if (!extension || (_wcsicmp(extension,L".mds") && _wcsicmp(extension,L".iso") && _wcsicmp(extension,L".bin"))) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    source->mds = !_wcsicmp(extension,L".mds"); MdsSheet* sheet = NULL; DWORD error = ERROR_SUCCESS;
    if (source->mds) {
        source->descriptor = CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if (source->descriptor == INVALID_HANDLE_VALUE) return false;
        sheet = calloc(1,sizeof(*sheet)); if (!sheet) { error = ERROR_NOT_ENOUGH_MEMORY; goto failed; }
        if (!imgburn_mds_parse_source(path,sheet)) { error = ERROR_INVALID_DATA; goto failed; }
        const MdsTrack* track = &sheet->tracks[0];
        if (sheet->sessionCount != 1 || sheet->trackCount != 1 || track->startLba || track->trackNumber != 1 || track->sectorSize != 2048 || track->subChannel || (track->mode != 0x02 && track->mode != 0xAA) || (track->control & 15) != 4 || track->sectorCount != sheet->totalSectors) { error = ERROR_NOT_SUPPORTED; goto failed; }
        source->sectors = track->sectorCount; source->layerBreakLba = sheet->layerBreakLba; source->layerCount = sheet->layerCount; source->oppositeTrackPath = sheet->oppositeTrackPath; source->partCount = track->fileCount;
    } else source->partCount = 1;
    source->parts = calloc(source->partCount,sizeof(*source->parts));
    if (!source->parts) { source->partCount = 0; error = ERROR_NOT_ENOUGH_MEMORY; goto failed; }
    uint64_t remaining = (uint64_t)source->sectors * 2048, logical = 0;
    for (unsigned i = 0; i < source->partCount; i++) {
        DataImagePart* part = &source->parts[i]; const MdsDataFile* metadata = sheet ? &sheet->files[sheet->tracks[0].firstFile + i] : NULL;
        part->file = CreateFileW(metadata ? metadata->path : path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if (part->file == INVALID_HANDLE_VALUE) { error = GetLastError(); goto failed; }
        LARGE_INTEGER size; if (!GetFileSizeEx(part->file,&size)) { error = GetLastError(); goto failed; }
        if (size.QuadPart < 0 || (metadata && (uint64_t)size.QuadPart != metadata->bytes)) { error = ERROR_INVALID_DATA; goto failed; }
        if (!sheet) {
            if (!size.QuadPart || size.QuadPart % 2048 || (uint64_t)size.QuadPart / 2048 > UINT32_MAX) { error = ERROR_INVALID_DATA; goto failed; }
            source->sectors = (uint32_t)((uint64_t)size.QuadPart / 2048); remaining = (uint64_t)size.QuadPart;
        }
        part->offset = sheet && !i ? sheet->tracks[0].startByteOffset : 0; part->start = logical;
        if (part->offset > (uint64_t)size.QuadPart) { error = ERROR_INVALID_DATA; goto failed; }
        uint64_t available = (uint64_t)size.QuadPart - part->offset; part->bytes = available < remaining ? available : remaining;
        logical += part->bytes; remaining -= part->bytes;
    }
    if (remaining) { error = ERROR_HANDLE_EOF; goto failed; }
    free(sheet); SetLastError(ERROR_SUCCESS); return true;
failed:
    free(sheet); imgburn_data_source_close(source); SetLastError(error); return false;
}

bool imgburn_data_source_read(DataImageSource* source, uint32_t lba, uint16_t sectors, void* buffer) {
    if (!source || !source->parts || !buffer || !sectors || lba >= source->sectors || sectors > source->sectors - lba) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    uint64_t position = (uint64_t)lba * 2048; DWORD remaining = (DWORD)sectors * 2048; uint8_t* output = buffer;
    for (unsigned i = 0; i < source->partCount && remaining; i++) {
        DataImagePart* part = &source->parts[i]; if (position < part->start || position >= part->start + part->bytes) continue;
        uint64_t inside = position - part->start, available = part->bytes - inside; DWORD wanted = available < remaining ? (DWORD)available : remaining;
        LARGE_INTEGER offset; offset.QuadPart = (LONGLONG)(part->offset + inside); if (!SetFilePointerEx(part->file,offset,NULL,FILE_BEGIN)) return false;
        DWORD done = 0;
        while (done < wanted) {
            DWORD actual = 0; if (!ReadFile(part->file,output + done,wanted - done,&actual,NULL)) return false;
            if (!actual || actual > wanted - done) { SetLastError(ERROR_HANDLE_EOF); return false; } done += actual;
        }
        output += wanted; position += wanted; remaining -= wanted;
    }
    if (remaining) { SetLastError(ERROR_HANDLE_EOF); return false; } return true;
}
