/* 扇区查看器核心：2048 字节用户数据、2352 字节原始 CD 和 96 字节子通道。 */
#include "imgburn_sector.h"
#include <stdio.h>
#include <string.h>
static bool sector_cancelled(const SectorRequest* request) { return request->cancelEvent && WaitForSingleObject(request->cancelEvent,0) == WAIT_OBJECT_0; }
static bool sector_failure(DWORD error) { SetLastError(error ? error : ERROR_READ_FAULT); return false; }
bool imgburn_sector_read(OpticalDrive* drive, const SectorRequest* request, SectorData* output) {
    if (output) memset(output,0,sizeof(*output)); if (!drive || !request || !output || (unsigned)request->mode > SECTOR_RAW_SUBCHANNEL) return sector_failure(ERROR_INVALID_PARAMETER);
    if (sector_cancelled(request)) return sector_failure(ERROR_CANCELLED);
    if (!mmc_test_unit_ready(drive)) return sector_failure(drive->lastError);
    MmcProfile profile = mmc_get_current_profile(drive); if (profile == MMC_PROFILE_UNKNOWN || profile == MMC_PROFILE_NONE) return sector_failure(drive->lastError ? drive->lastError : ERROR_NOT_SUPPORTED);
    bool cd = profile == MMC_PROFILE_CD_ROM || profile == MMC_PROFILE_CD_R || profile == MMC_PROFILE_CD_RW; uint32_t last = 0, bytes = 0; MmcCdToc toc = {0};
    if (request->mode != SECTOR_USER_DATA && !cd) return sector_failure(ERROR_NOT_SUPPORTED);
    if (cd) { if (!mmc_read_cd_toc(drive,&toc)) return sector_failure(drive->lastError); last = toc.leadOutLba - 1; }
    else if (!mmc_read_capacity(drive,&last,&bytes)) return sector_failure(drive->lastError);
    else if (last == UINT32_MAX || bytes != 2048) return sector_failure(ERROR_NOT_SUPPORTED);
    uint32_t lba = request->useLast ? last : request->lba; if (lba > last) return sector_failure(ERROR_SECTOR_NOT_FOUND);
    if (cd && request->mode == SECTOR_USER_DATA) { unsigned track = 0; while (track + 1 < toc.trackCount && lba >= toc.tracks[track + 1].startLba) track++; if (lba < toc.tracks[track].startLba || !(toc.tracks[track].control & 4)) return sector_failure(ERROR_NOT_SUPPORTED); }
    if (sector_cancelled(request)) return sector_failure(ERROR_CANCELLED); SectorData result = {0}; result.lba = lba; result.lastLba = last; result.profile = profile; result.bytes = request->mode == SECTOR_USER_DATA ? 2048 : request->mode == SECTOR_RAW ? 2352 : 2448;
    bool ok = request->mode == SECTOR_USER_DATA ? mmc_read_blocks(drive,lba,1,result.data) : mmc_read_cd_raw_sector(drive,lba,request->mode == SECTOR_RAW_SUBCHANNEL,result.data);
    if (!ok) return sector_failure(drive->lastError); if (sector_cancelled(request)) return sector_failure(ERROR_CANCELLED);
    if (!mmc_test_unit_ready(drive) || mmc_get_current_profile(drive) != profile) return sector_failure(drive->lastError ? drive->lastError : ERROR_MEDIA_CHANGED);
    if (cd) { MmcCdToc finalToc; if (!mmc_read_cd_toc(drive,&finalToc)) return sector_failure(drive->lastError); if (finalToc.trackCount != toc.trackCount || finalToc.leadOutLba != toc.leadOutLba || memcmp(finalToc.tracks,toc.tracks,toc.trackCount * sizeof(MmcCdTrack))) return sector_failure(ERROR_MEDIA_CHANGED); }
    else { uint32_t finalLast, finalBytes; if (!mmc_read_capacity(drive,&finalLast,&finalBytes)) return sector_failure(drive->lastError); if (last != finalLast || bytes != finalBytes) return sector_failure(ERROR_MEDIA_CHANGED); }
    if (sector_cancelled(request)) return sector_failure(ERROR_CANCELLED); *output = result; return true;
}
bool imgburn_sector_format(const SectorData* sector, wchar_t* text, size_t capacity) {
    if (text && capacity) text[0] = 0; if (!sector || !text || (sector->bytes != 2048 && sector->bytes != 2352 && sector->bytes != 2448)) return sector_failure(ERROR_INVALID_PARAMETER);
    size_t need = ((size_t)sector->bytes + 15) / 16 * 80 + 100; if (capacity < need) return sector_failure(ERROR_INSUFFICIENT_BUFFER);
    size_t used = (size_t)swprintf(text,capacity,L"Offset    00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F   ASCII\r\n");
    for (uint32_t offset = 0; offset < sector->bytes; offset += 16) {
        used += (size_t)swprintf(text + used,capacity - used,L"%04X      ",offset);
        for (unsigned i = 0; i < 16; i++) used += (size_t)swprintf(text + used,capacity - used,offset + i < sector->bytes ? L"%02X " : L"   ",offset + i < sector->bytes ? sector->data[offset + i] : 0);
        text[used++] = L' '; text[used++] = L' ';
        for (unsigned i = 0; i < 16 && offset + i < sector->bytes; i++) { unsigned value = sector->data[offset + i]; text[used++] = value >= 32 && value < 127 ? (wchar_t)value : L'.'; }
        text[used++] = L'\r'; text[used++] = L'\n'; text[used] = 0;
    }
    return true;
}
