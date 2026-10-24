/* Read 数据镜像流程：有界重试、取消、完整传输检查及原子发布。 */
#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_pipeline.h"
#include "imgburn_image.h"
#include "imgburn_data_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static bool read_cancelled(const ReadConfig* config) { return config->cancelEvent && WaitForSingleObject(config->cancelEvent,0) == WAIT_OBJECT_0; }
static bool read_failure(PipelineStats* stats, DWORD error, const char* text, PipelineProgressCallback callback, void* context) {
    if (!error) error = ERROR_READ_FAULT;
    stats->state = error == ERROR_CANCELLED ? PIPELINE_STATE_ABORTED : PIPELINE_STATE_FAILED;
    snprintf(stats->statusMessage,sizeof(stats->statusMessage),"%s (error %lu)",text,error);
    if (callback) callback(stats,context); SetLastError(error); return false;
}
static bool read_profile(MmcProfile profile) {
    switch (profile) {
        case MMC_PROFILE_CD_ROM: case MMC_PROFILE_CD_R: case MMC_PROFILE_CD_RW:
        case MMC_PROFILE_DVD_ROM: case MMC_PROFILE_DVD_R_SEQ: case MMC_PROFILE_DVD_RAM: case MMC_PROFILE_DVD_RW_RO: case MMC_PROFILE_DVD_RW_SEQ:
        case MMC_PROFILE_DVD_R_DL_SEQ: case MMC_PROFILE_DVD_R_DL_JUMP: case MMC_PROFILE_DVD_RW_DL: case MMC_PROFILE_DVD_PLUS_RW: case MMC_PROFILE_DVD_PLUS_R:
        case MMC_PROFILE_DVD_PLUS_RW_DL: case MMC_PROFILE_DVD_PLUS_R_DL: case MMC_PROFILE_BD_ROM: case MMC_PROFILE_BD_R_SRM: case MMC_PROFILE_BD_R_RRM: case MMC_PROFILE_BD_RE: return true;
        default: return false;
    }
}
static bool read_target(const wchar_t* input, wchar_t* full, bool replace) {
    DWORD length = GetFullPathNameW(input,MAX_PATH,full,NULL);
    const wchar_t* extension = length && length < MAX_PATH ? wcsrchr(full,L'.') : NULL;
    if (!extension || (_wcsicmp(extension,L".iso") && _wcsicmp(extension,L".mds")) || !wcsncmp(full,L"\\\\.\\",4) || !wcsncmp(full,L"\\\\?\\",4)) { SetLastError(ERROR_INVALID_NAME); return false; }
    const wchar_t* name = wcsrchr(full,L'\\'); if (!name || wcschr(name,L':')) { SetLastError(ERROR_INVALID_NAME); return false; }
    DWORD attributes = GetFileAttributesW(full);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) { SetLastError(ERROR_ACCESS_DENIED); return false; }
        if (!replace) { SetLastError(ERROR_FILE_EXISTS); return false; }
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return false;
    return true;
}
static DWORD read_device_error(OpticalDrive* drive) { return drive->lastError ? drive->lastError : ERROR_READ_FAULT; }
static bool read_retryable(const OpticalDrive* drive) { return drive->lastSense.senseKey != 2 && drive->lastSense.senseKey != 5 && drive->lastSense.senseKey != 6; }
static bool read_sectors(const ReadConfig* config, uint32_t lba, uint16_t count, uint8_t* buffer, PipelineStats* stats) {
    OpticalDrive* drive = config->drive;
    if (mmc_read_blocks(drive,lba,count,buffer)) return true;
    if (!read_retryable(drive)) return false;
    for (uint16_t i = 0; i < count; i++) {
        unsigned attempt = count == 1 ? 1 : 0; bool ok = false;
        for (; attempt <= config->retryCount; attempt++) {
            if (read_cancelled(config)) { SetLastError(ERROR_CANCELLED); return false; }
            if (stats->retriesCount != UINT32_MAX) stats->retriesCount++;
            if (mmc_read_blocks(drive,lba + i,1,buffer + (size_t)i * 2048)) { ok = true; break; }
            if (!read_retryable(drive)) return false;
        }
        if (!ok) { SetLastError(read_device_error(drive)); return false; }
    }
    return true;
}

typedef struct { wchar_t path[MAX_PATH]; HANDLE file; bool created; } ReadDataPart;
bool pipeline_read_resolve_split(const wchar_t* path, uint32_t sectorBytes, uint32_t requestedSectors, bool automatic, uint32_t* sectors) {
    if (!path || !path[0] || !sectors || (sectorBytes != 2048 && sectorBytes != 2352) || (automatic && requestedSectors)) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    *sectors = requestedSectors; if (!automatic) return true;
    wchar_t volume[MAX_PATH], system[MAX_PATH];
    if (!GetVolumePathNameW(path,volume,MAX_PATH) || !GetVolumeInformationW(volume,NULL,0,NULL,NULL,NULL,system,MAX_PATH)) return false;
    /* FAT系列采用文件上限内的完整扇区，4GiB预设不能代替此处的Auto。 */
    if (!_wcsicmp(system,L"FAT") || !_wcsicmp(system,L"FAT32") || !_wcsicmp(system,L"FAT12") || !_wcsicmp(system,L"FAT16")) *sectors = UINT32_MAX / sectorBytes;
    return true;
}
static void read_release_parts(ReadDataPart* parts, unsigned count, bool remove) {
    for (unsigned i = 0; parts && i < count; i++) {
        if (parts[i].file && parts[i].file != INVALID_HANDLE_VALUE) CloseHandle(parts[i].file);
        if (remove && parts[i].created) DeleteFileW(parts[i].path);
        parts[i].file = INVALID_HANDLE_VALUE; parts[i].created = false;
    }
}
static bool read_create_parts(const wchar_t* destination, ReadDataPart* parts, unsigned count) {
    wchar_t base[MAX_PATH]; wcscpy(base,destination); *wcsrchr(base,L'.') = 0;
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        DWORD error = ERROR_SUCCESS;
        for (unsigned i = 0; i < count; i++) {
            int length;
            if (count == 1) length = attempt ? swprintf(parts[i].path,MAX_PATH,L"%ls.%03u.iso",base,attempt) : swprintf(parts[i].path,MAX_PATH,L"%ls.iso",base);
            else length = attempt ? swprintf(parts[i].path,MAX_PATH,L"%ls.%03u.i%02u",base,attempt,i) : swprintf(parts[i].path,MAX_PATH,L"%ls.i%02u",base,i);
            if (length < 0) { error = ERROR_FILENAME_EXCED_RANGE; break; }
            parts[i].file = CreateFileW(parts[i].path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
            if (parts[i].file == INVALID_HANDLE_VALUE) { error = GetLastError(); break; } parts[i].created = true;
        }
        if (!error) return true;
        /* 任一卷冲突就释放本次预留并换整组名称，绝不替换旧数据卷。 */
        read_release_parts(parts,count,true);
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) { SetLastError(error); return false; }
    }
    SetLastError(ERROR_FILE_EXISTS); return false;
}

bool pipeline_read_disc(const ReadConfig* config, PipelineProgressCallback callback, void* context) {
    if (config && config->rawCdMds) return pipeline_read_cd(config,callback,context);
    PipelineStats stats = {0}; HANDLE output = INVALID_HANDLE_VALUE; uint8_t* buffer = NULL; wchar_t destination[MAX_PATH], temporary[MAX_PATH] = L"", descriptor[MAX_PATH] = L"";
    ReadDataPart* parts = NULL; unsigned partCount = 0;
    DWORD error = ERROR_SUCCESS; bool success = false, temporaryCreated = false, descriptorCreated = false;
    if (!config || !config->drive || !config->imagePath || !config->imagePath[0] || config->retryCount > 20) return read_failure(&stats,ERROR_INVALID_PARAMETER,"Invalid read configuration",callback,context);
    if (config->includeSessionPregaps) return read_failure(&stats,ERROR_NOT_SUPPORTED,"Session INDEX00 requires multi-session CUE output",callback,context);
    if (config->readCdText) return read_failure(&stats,ERROR_NOT_SUPPORTED,"CD-TEXT requires raw CD CUE output",callback,context);
    if (read_cancelled(config)) return read_failure(&stats,ERROR_CANCELLED,"Read cancelled",callback,context);
    if (!read_target(config->imagePath,destination,config->replaceExisting)) return read_failure(&stats,GetLastError(),"Invalid image destination",callback,context);
    bool mdsOutput = !_wcsicmp(wcsrchr(destination,L'.'),L".mds");
    if (config->splitSectors && !mdsOutput) return read_failure(&stats,ERROR_NOT_SUPPORTED,"Split data output requires an MDS destination",callback,context);
    uint32_t splitSectors = 0; if (!pipeline_read_resolve_split(destination,2048,config->splitSectors,config->autoSplit,&splitSectors)) return read_failure(&stats,GetLastError(),"Cannot resolve image splitting",callback,context);
    stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Inspecting source disc..."); if (callback) callback(&stats,context);
    OpticalDrive* drive = config->drive;
    if (!mmc_test_unit_ready(drive)) return read_failure(&stats,read_device_error(drive),"Source disc is not ready",callback,context);
    MmcProfile profile = mmc_get_current_profile(drive);
    if (!read_profile(profile)) return read_failure(&stats,ERROR_NOT_SUPPORTED,"Unsupported source media profile",callback,context);
    uint32_t last = 0, bytes = 0;
    if (!mmc_read_capacity(drive,&last,&bytes)) return read_failure(&stats,read_device_error(drive),"Cannot read disc capacity",callback,context);
    if (last == UINT32_MAX || bytes != 2048) return read_failure(&stats,ERROR_NOT_SUPPORTED,"Require bounded 2048-byte data sectors",callback,context);
    stats.totalSectors = last + 1;
    if (!mdsOutput && splitSectors && stats.totalSectors > splitSectors) {
        wcscpy(wcsrchr(destination,L'.'),L".mds"); wchar_t checked[MAX_PATH]; if (!read_target(destination,checked,config->replaceExisting)) return read_failure(&stats,GetLastError(),"Cannot publish automatic split descriptor",callback,context); mdsOutput = true;
    }
    bool cdMds = mdsOutput && profile <= MMC_PROFILE_CD_RW;
    if (mdsOutput && !cdMds && (profile < MMC_PROFILE_DVD_ROM || profile > MMC_PROFILE_DVD_PLUS_R_DL)) return read_failure(&stats,ERROR_NOT_SUPPORTED,"MDS Read requires supported DVD or MODE1 CD media",callback,context);
    MmcDvdLayout dvdLayout = {0};
    if (mdsOutput && !cdMds) {
        if (!mmc_read_dvd_layout(drive,stats.totalSectors,&dvdLayout)) return read_failure(&stats,read_device_error(drive),"Cannot read matching DVD physical layout",callback,context);
        if (dvdLayout.layerCount == 2) {
            MmcDiscInfo disc; if (!mmc_read_disc_information(drive,&disc)) return read_failure(&stats,read_device_error(drive),"Cannot inspect dual-layer DVD session",callback,context);
            if (disc.numSessions != 1 || disc.discStatus != DISC_STATUS_COMPLETE || disc.lastSessionStatus != 3) return read_failure(&stats,ERROR_NOT_SUPPORTED,"Dual-layer MDS Read requires a finalized single-session DVD",callback,context);
        }
    }
    if (profile <= MMC_PROFILE_CD_RW && !mmc_check_iso_cd(drive,stats.totalSectors)) return read_failure(&stats,read_device_error(drive),"CD layout requires a raw track image",callback,context);
    if (mdsOutput) {
        uint64_t count = splitSectors ? ((uint64_t)stats.totalSectors + splitSectors - 1) / splitSectors : 1;
        if (count > MAX_MDS_FILES) return read_failure(&stats,ERROR_NOT_SUPPORTED,"Split image exceeds the supported MDS file count",callback,context); partCount = (unsigned)count;
    }
    wchar_t parent[MAX_PATH]; wcscpy(parent,destination); wchar_t* slash = wcsrchr(parent,L'\\'); if (!slash) return read_failure(&stats,ERROR_INVALID_NAME,"Invalid image directory",callback,context); slash[1] = 0;
    ULARGE_INTEGER available;
    if (!GetDiskFreeSpaceExW(parent,&available,NULL,NULL)) return read_failure(&stats,GetLastError(),"Cannot query destination space",callback,context);
    if (available.QuadPart < (uint64_t)stats.totalSectors * 2048) return read_failure(&stats,ERROR_DISK_FULL,"Not enough destination space",callback,context);
    buffer = (uint8_t*)malloc(32 * 2048); if (!buffer) return read_failure(&stats,ERROR_NOT_ENOUGH_MEMORY,"Cannot allocate read buffer",callback,context);
    if (!mmc_set_read_speed(drive,profile,config->readSpeedKbps)) { error = read_device_error(drive); goto done; }
    static volatile LONG serial;
    if (mdsOutput) {
        parts = calloc(partCount,sizeof(*parts)); if (!parts) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
        if (!read_create_parts(destination,parts,partCount)) { error = GetLastError(); goto done; }
    } else {
        for (unsigned attempt = 0; attempt < 64; attempt++) {
            int chars = swprintf(temporary,MAX_PATH,L"%ls.partial.%lu.%lu",destination,GetCurrentProcessId(),(unsigned long)InterlockedIncrement(&serial));
            if (chars < 0) { error = ERROR_FILENAME_EXCED_RANGE; goto done; }
            output = CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
            if (output != INVALID_HANDLE_VALUE) { temporaryCreated = true; break; }
            error = GetLastError(); if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) goto done;
        }
        if (!temporaryCreated) goto done;
    }
    ULONGLONG started = GetTickCount64(), previousTime = started; uint32_t previousLba = 0;
    stats.state = PIPELINE_STATE_READING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Reading disc to image..."); if (callback) callback(&stats,context);
    for (uint32_t lba = 0; lba < stats.totalSectors;) {
        if (dvdLayout.layerCount == 2) snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Reading DVD layer %u to image (L0 %u sectors)...",lba < dvdLayout.layerBreakLba ? 0 : 1,dvdLayout.layerBreakLba);
        if (read_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
        uint16_t count = (uint16_t)(stats.totalSectors - lba < 32 ? stats.totalSectors - lba : 32);
        if (dvdLayout.layerCount == 2 && lba < dvdLayout.layerBreakLba && count > dvdLayout.layerBreakLba - lba) count = (uint16_t)(dvdLayout.layerBreakLba - lba);
        unsigned part = partCount > 1 ? lba / splitSectors : 0;
        if (partCount > 1 && count > splitSectors - lba % splitSectors) count = (uint16_t)(splitSectors - lba % splitSectors);
        if (!read_sectors(config,lba,count,buffer,&stats)) { error = read_cancelled(config) ? ERROR_CANCELLED : read_device_error(drive); goto done; }
        DWORD wanted = (DWORD)count * 2048, offset = 0;
        HANDLE target = mdsOutput ? parts[part].file : output;
        while (offset < wanted) { DWORD written = 0; if (!WriteFile(target,buffer + offset,wanted - offset,&written,NULL)) { error = GetLastError(); if (!error) error = ERROR_WRITE_FAULT; goto done; } if (!written || written > wanted - offset) { error = ERROR_WRITE_FAULT; goto done; } offset += written; }
        lba += count; stats.currentLba = lba; stats.progressPercent = (int)((uint64_t)lba * 99 / stats.totalSectors);
        ULONGLONG now = GetTickCount64(), elapsed = now - previousTime;
        if (elapsed) { stats.currentSpeedKBps = (double)(lba - previousLba) * 2000.0 / elapsed; previousTime = now; previousLba = lba; }
        stats.speedMultiplier = stats.currentSpeedKBps / (profile <= MMC_PROFILE_CD_RW ? 150.0 : profile >= MMC_PROFILE_BD_ROM ? 4495.5 : 1385.0);
        stats.elapsedTimeSec = (uint32_t)((now - started) / 1000); if (stats.currentSpeedKBps > 0) stats.remainingTimeSec = (uint32_t)((stats.totalSectors - lba) * 2.0 / stats.currentSpeedKBps);
        if (callback) callback(&stats,context);
    }
    if (read_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
    if (!mmc_test_unit_ready(drive) || !mmc_read_capacity(drive,&last,&bytes)) { error = read_device_error(drive); goto done; }
    if (last != stats.totalSectors - 1 || bytes != 2048 || mmc_get_current_profile(drive) != profile) { error = ERROR_MEDIA_CHANGED; goto done; }
    if (mdsOutput && !cdMds) {
        MmcDvdLayout finalLayout;
        if (!mmc_read_dvd_layout(drive,stats.totalSectors,&finalLayout)) { error = read_device_error(drive); goto done; }
        if (finalLayout.layerCount != dvdLayout.layerCount || finalLayout.oppositeTrackPath != dvdLayout.oppositeTrackPath || finalLayout.layerBreakLba != dvdLayout.layerBreakLba) { error = ERROR_MEDIA_CHANGED; goto done; }
    }
    if (cdMds && !mmc_check_iso_cd(drive,stats.totalSectors)) { error = read_device_error(drive); goto done; }
    if (mdsOutput) {
        for (unsigned i = 0; i < partCount; i++) {
            if (read_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
            if (!FlushFileBuffers(parts[i].file)) { error = GetLastError(); goto done; }
            if (!CloseHandle(parts[i].file)) { error = GetLastError(); parts[i].file = INVALID_HANDLE_VALUE; goto done; } parts[i].file = INVALID_HANDLE_VALUE;
        }
    } else {
        if (!FlushFileBuffers(output)) { error = GetLastError(); goto done; }
        if (!CloseHandle(output)) { error = GetLastError(); output = INVALID_HANDLE_VALUE; goto done; } output = INVALID_HANDLE_VALUE;
    }
    if (read_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
    if (mdsOutput) {
        /* 旧 MDS 继续引用旧数据；新 ISO 完整落盘后才发布新的描述文件。 */
        for (unsigned attempt = 0; attempt < 64; attempt++) {
            if (swprintf(descriptor,MAX_PATH,L"%ls.partial.%lu.%lu",destination,GetCurrentProcessId(),(unsigned long)InterlockedIncrement(&serial)) < 0) { error = ERROR_FILENAME_EXCED_RANGE; goto done; }
            HANDLE reservation = CreateFileW(descriptor,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
            if (reservation != INVALID_HANDLE_VALUE) { CloseHandle(reservation); descriptorCreated = true; break; }
            error = GetLastError(); if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) goto done;
        }
        if (!descriptorCreated) goto done;
        const wchar_t* names[MAX_MDS_FILES]; for (unsigned i = 0; i < partCount; i++) names[i] = wcsrchr(parts[i].path,L'\\') + 1;
        bool created = cdMds ? imgburn_mds_create_cd_read_files(descriptor,names,partCount,stats.totalSectors) : imgburn_mds_create_dvd_read_files(descriptor,names,partCount,stats.totalSectors,dvdLayout.layerBreakLba,dvdLayout.oppositeTrackPath);
        if (!created) { error = GetLastError(); goto done; }
        if (read_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
    }
    if (!read_target(destination,parent,config->replaceExisting)) { error = GetLastError(); goto done; }
    if (!MoveFileExW(mdsOutput ? descriptor : temporary,destination,MOVEFILE_WRITE_THROUGH | (config->replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0))) { error = GetLastError(); goto done; }
    success = true;
done:
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output); if (temporaryCreated && !success) DeleteFileW(temporary); if (descriptorCreated && !success) DeleteFileW(descriptor); read_release_parts(parts,partCount,!success); free(parts); free(buffer);
    if (!success) return read_failure(&stats,error,"Disc read did not complete",callback,context);
    if (config->ejectWhenDone && !mmc_load_eject(drive,true)) return read_failure(&stats,read_device_error(drive),"Image published, but disc ejection failed",callback,context);
    stats.state = PIPELINE_STATE_COMPLETED; stats.progressPercent = 100; stats.remainingTimeSec = 0; snprintf(stats.statusMessage,sizeof(stats.statusMessage),mdsOutput ? "Disc read successfully to MDS and data files." : "Disc image read successfully."); if (callback) callback(&stats,context); return true;
}

bool pipeline_verify_disc_ex(const VerifyConfig* config, PipelineProgressCallback callback, void* context) {
    PipelineStats stats = {0}; DataImageSource image = {0}; uint8_t* data = NULL; uint8_t* reference = NULL; DWORD error = ERROR_SUCCESS; bool success = false, imageOpen = false;
    if (!config || !config->drive || config->retryCount > 20) return read_failure(&stats,ERROR_INVALID_PARAMETER,"Invalid verify configuration",callback,context);
    ReadConfig reader = {0}; reader.drive = config->drive; reader.cancelEvent = config->cancelEvent; reader.retryCount = config->retryCount;
    if (read_cancelled(&reader)) return read_failure(&stats,ERROR_CANCELLED,"Verify cancelled",callback,context);
    bool zeroSource = config->zeroSectors != 0, compare = zeroSource || (config->imagePath && config->imagePath[0]);
    if (config->includeSessionPregaps && (zeroSource || (compare && (!wcsrchr(config->imagePath,L'.') || _wcsicmp(wcsrchr(config->imagePath,L'.'),L".cue"))))) return read_failure(&stats,ERROR_NOT_SUPPORTED,"Session INDEX00 comparison requires a multi-session CUE",callback,context);
    if (zeroSource) stats.totalSectors = config->zeroSectors;
    else if (compare) {
        const wchar_t* extension = wcsrchr(config->imagePath,L'.');
        if (extension && !_wcsicmp(extension,L".cue")) return pipeline_verify_cd(config,callback,context);
        if (extension && !_wcsicmp(extension,L".mds")) {
            MdsSheet* metadata = calloc(1,sizeof(*metadata)); if (!metadata) return read_failure(&stats,ERROR_NOT_ENOUGH_MEMORY,"Cannot prepare comparison image",callback,context);
            bool parsed = imgburn_mds_parse(config->imagePath,metadata), raw = parsed && metadata->trackCount && metadata->tracks[0].sectorSize == 2352; free(metadata);
            if (raw) return pipeline_verify_cd(config,callback,context);
        }
        if (!imgburn_data_source_open(&image,config->imagePath)) return read_failure(&stats,GetLastError(),"Cannot open comparison image",callback,context);
        imageOpen = true; stats.totalSectors = image.sectors;
    }
    OpticalDrive* drive = config->drive; if (!mmc_test_unit_ready(drive)) { error = read_device_error(drive); goto verify_done; }
    MmcProfile profile = mmc_get_current_profile(drive); if (!read_profile(profile)) { error = ERROR_NOT_SUPPORTED; goto verify_done; }
    if (!compare && profile <= MMC_PROFILE_CD_RW) return pipeline_verify_cd(config,callback,context);
    if (config->includeSessionPregaps) { error = ERROR_NOT_SUPPORTED; goto verify_done; }
    uint32_t last = 0, bytes = 0;
    if (!mmc_read_capacity(drive,&last,&bytes)) { error = read_device_error(drive); goto verify_done; }
    if (last == UINT32_MAX || bytes != 2048 || (compare && stats.totalSectors > last + 1)) { error = ERROR_INVALID_DATA; goto verify_done; }
    uint32_t capacity = last + 1; if (!compare) stats.totalSectors = capacity;
    if (profile <= MMC_PROFILE_CD_RW && !mmc_check_iso_cd(drive,capacity)) { error = read_device_error(drive); goto verify_done; }
    data = malloc(32 * 2048); if (compare) reference = zeroSource ? calloc(32,2048) : malloc(32 * 2048); if (!data || (compare && !reference)) { error = ERROR_NOT_ENOUGH_MEMORY; goto verify_done; }
    stats.state = PIPELINE_STATE_VERIFYING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),zeroSource ? "Comparing Discovery disc data against in-memory zero data..." : compare ? "Comparing disc data against image..." : "Checking disc data readability..."); ULONGLONG started = GetTickCount64();
    if (callback) callback(&stats,context);
    for (uint32_t lba = 0; lba < stats.totalSectors;) {
        if (read_cancelled(&reader)) { error = ERROR_CANCELLED; goto verify_done; }
        uint16_t count = (uint16_t)(stats.totalSectors - lba < 32 ? stats.totalSectors - lba : 32);
        if (!read_sectors(&reader,lba,count,data,&stats)) { error = read_cancelled(&reader) ? ERROR_CANCELLED : read_device_error(drive); goto verify_done; }
        if (compare) {
            if (!zeroSource && !imgburn_data_source_read(&image,lba,count,reference)) { error = GetLastError(); goto verify_done; }
            for (unsigned i = 0; i < count; i++) if (memcmp(data + (size_t)i * 2048,reference + (size_t)i * 2048,2048)) stats.miscomparesCount++;
            stats.comparedSectors += count;
        } else stats.readableOnlySectors += count;
        lba += count; stats.currentLba = lba; stats.progressPercent = (int)((uint64_t)lba * 99 / stats.totalSectors); ULONGLONG elapsed = GetTickCount64() - started;
        stats.elapsedTimeSec = (uint32_t)(elapsed / 1000); if (elapsed) { stats.currentSpeedKBps = (double)lba * 2000 / elapsed; stats.speedMultiplier = stats.currentSpeedKBps / (profile <= MMC_PROFILE_CD_RW ? 150.0 : profile >= MMC_PROFILE_BD_ROM ? 4495.5 : 1385.0); stats.remainingTimeSec = (uint32_t)((stats.totalSectors - lba) * 2.0 / stats.currentSpeedKBps); }
        if (callback) callback(&stats,context);
    }
    if (read_cancelled(&reader)) { error = ERROR_CANCELLED; goto verify_done; }
    if (!mmc_test_unit_ready(drive) || !mmc_read_capacity(drive,&last,&bytes)) { error = read_device_error(drive); goto verify_done; }
    if (last != capacity - 1 || bytes != 2048 || mmc_get_current_profile(drive) != profile) { error = ERROR_MEDIA_CHANGED; goto verify_done; }
    if (stats.miscomparesCount) { error = ERROR_CRC; goto verify_done; } success = true;
verify_done:
    if (imageOpen) imgburn_data_source_close(&image); free(data); free(reference);
    if (!success) return read_failure(&stats,error,"Verify did not complete",callback,context);
    stats.state = PIPELINE_STATE_COMPLETED; stats.progressPercent = 100; stats.remainingTimeSec = 0; snprintf(stats.statusMessage,sizeof(stats.statusMessage),zeroSource ? "Discovery data matches the zero source." : compare ? "Disc data matches image." : "Disc data readability check passed."); if (callback) callback(&stats,context); return true;
}
