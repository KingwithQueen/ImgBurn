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

bool pipeline_read_disc(const ReadConfig* config, PipelineProgressCallback callback, void* context) {
    PipelineStats stats = {0}; HANDLE output = INVALID_HANDLE_VALUE; uint8_t* buffer = NULL; wchar_t destination[MAX_PATH], temporary[MAX_PATH] = L"", descriptor[MAX_PATH] = L"";
    DWORD error = ERROR_SUCCESS; bool success = false, temporaryCreated = false, descriptorCreated = false;
    if (!config || !config->drive || !config->imagePath || !config->imagePath[0] || config->retryCount > 20) return read_failure(&stats,ERROR_INVALID_PARAMETER,"Invalid read configuration",callback,context);
    if (config->readCdText) return read_failure(&stats,ERROR_NOT_SUPPORTED,"CD-TEXT requires raw CD CUE output",callback,context);
    if (read_cancelled(config)) return read_failure(&stats,ERROR_CANCELLED,"Read cancelled",callback,context);
    if (!read_target(config->imagePath,destination,config->replaceExisting)) return read_failure(&stats,GetLastError(),"Invalid image destination",callback,context);
    bool mdsOutput = !_wcsicmp(wcsrchr(destination,L'.'),L".mds");
    stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Inspecting source disc..."); if (callback) callback(&stats,context);
    OpticalDrive* drive = config->drive;
    if (!mmc_test_unit_ready(drive)) return read_failure(&stats,read_device_error(drive),"Source disc is not ready",callback,context);
    MmcProfile profile = mmc_get_current_profile(drive);
    if (!read_profile(profile)) return read_failure(&stats,ERROR_NOT_SUPPORTED,"Unsupported source media profile",callback,context);
    uint32_t last = 0, bytes = 0;
    if (!mmc_read_capacity(drive,&last,&bytes)) return read_failure(&stats,read_device_error(drive),"Cannot read disc capacity",callback,context);
    if (last == UINT32_MAX || bytes != 2048) return read_failure(&stats,ERROR_NOT_SUPPORTED,"Require bounded 2048-byte data sectors",callback,context);
    stats.totalSectors = last + 1;
    if (mdsOutput && (profile < MMC_PROFILE_DVD_ROM || profile > MMC_PROFILE_DVD_PLUS_R_DL)) return read_failure(&stats,ERROR_NOT_SUPPORTED,"MDS Read requires supported DVD media",callback,context);
    MmcDvdLayout dvdLayout = {0};
    if (mdsOutput) {
        if (!mmc_read_dvd_layout(drive,stats.totalSectors,&dvdLayout)) return read_failure(&stats,read_device_error(drive),"Cannot read matching DVD physical layout",callback,context);
        if (dvdLayout.layerCount == 2) {
            MmcDiscInfo disc; if (!mmc_read_disc_information(drive,&disc)) return read_failure(&stats,read_device_error(drive),"Cannot inspect dual-layer DVD session",callback,context);
            if (disc.numSessions != 1 || disc.discStatus != DISC_STATUS_COMPLETE || disc.lastSessionStatus != 3) return read_failure(&stats,ERROR_NOT_SUPPORTED,"Dual-layer MDS Read requires a finalized single-session DVD",callback,context);
        }
    }
    if (profile <= MMC_PROFILE_CD_RW && !mmc_check_iso_cd(drive,stats.totalSectors)) return read_failure(&stats,read_device_error(drive),"CD layout requires a raw track image",callback,context);
    wchar_t parent[MAX_PATH]; wcscpy(parent,destination); wchar_t* slash = wcsrchr(parent,L'\\'); if (!slash) return read_failure(&stats,ERROR_INVALID_NAME,"Invalid image directory",callback,context); slash[1] = 0;
    ULARGE_INTEGER available;
    if (!GetDiskFreeSpaceExW(parent,&available,NULL,NULL)) return read_failure(&stats,GetLastError(),"Cannot query destination space",callback,context);
    if (available.QuadPart < (uint64_t)stats.totalSectors * 2048) return read_failure(&stats,ERROR_DISK_FULL,"Not enough destination space",callback,context);
    buffer = (uint8_t*)malloc(32 * 2048); if (!buffer) return read_failure(&stats,ERROR_NOT_ENOUGH_MEMORY,"Cannot allocate read buffer",callback,context);
    static volatile LONG serial;
    wchar_t dataBase[MAX_PATH]; wcscpy(dataBase,destination); *wcsrchr(dataBase,L'.') = 0;
    for (unsigned attempt = 0; attempt < (mdsOutput ? 1000u : 64u); attempt++) {
        int chars = mdsOutput ? (attempt ? swprintf(temporary,MAX_PATH,L"%ls.%03u.iso",dataBase,attempt) : swprintf(temporary,MAX_PATH,L"%ls.iso",dataBase)) : swprintf(temporary,MAX_PATH,L"%ls.partial.%lu.%lu",destination,GetCurrentProcessId(),(unsigned long)InterlockedIncrement(&serial));
        if (chars < 0) { error = ERROR_FILENAME_EXCED_RANGE; goto done; }
        output = CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if (output != INVALID_HANDLE_VALUE) { temporaryCreated = true; break; }
        error = GetLastError(); if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) goto done;
    }
    if (!temporaryCreated) goto done;
    ULONGLONG started = GetTickCount64(), previousTime = started; uint32_t previousLba = 0;
    stats.state = PIPELINE_STATE_READING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Reading disc to image..."); if (callback) callback(&stats,context);
    for (uint32_t lba = 0; lba < stats.totalSectors;) {
        if (dvdLayout.layerCount == 2) snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Reading DVD layer %u to image (L0 %u sectors)...",lba < dvdLayout.layerBreakLba ? 0 : 1,dvdLayout.layerBreakLba);
        if (read_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
        uint16_t count = (uint16_t)(stats.totalSectors - lba < 32 ? stats.totalSectors - lba : 32);
        if (dvdLayout.layerCount == 2 && lba < dvdLayout.layerBreakLba && count > dvdLayout.layerBreakLba - lba) count = (uint16_t)(dvdLayout.layerBreakLba - lba);
        if (!read_sectors(config,lba,count,buffer,&stats)) { error = read_cancelled(config) ? ERROR_CANCELLED : read_device_error(drive); goto done; }
        DWORD wanted = (DWORD)count * 2048, offset = 0;
        while (offset < wanted) { DWORD written = 0; if (!WriteFile(output,buffer + offset,wanted - offset,&written,NULL)) { error = GetLastError(); if (!error) error = ERROR_WRITE_FAULT; goto done; } if (!written || written > wanted - offset) { error = ERROR_WRITE_FAULT; goto done; } offset += written; }
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
    if (mdsOutput) {
        MmcDvdLayout finalLayout;
        if (!mmc_read_dvd_layout(drive,stats.totalSectors,&finalLayout)) { error = read_device_error(drive); goto done; }
        if (finalLayout.layerCount != dvdLayout.layerCount || finalLayout.oppositeTrackPath != dvdLayout.oppositeTrackPath || finalLayout.layerBreakLba != dvdLayout.layerBreakLba) { error = ERROR_MEDIA_CHANGED; goto done; }
    }
    if (!FlushFileBuffers(output)) { error = GetLastError(); goto done; }
    if (!CloseHandle(output)) { error = GetLastError(); output = INVALID_HANDLE_VALUE; goto done; } output = INVALID_HANDLE_VALUE;
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
        const wchar_t* fileName = wcsrchr(temporary,L'\\') + 1;
        if (!imgburn_mds_create_dvd_read(descriptor,fileName,stats.totalSectors,dvdLayout.layerBreakLba,dvdLayout.oppositeTrackPath)) { error = GetLastError(); goto done; }
        if (read_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
    }
    if (!read_target(destination,parent,config->replaceExisting)) { error = GetLastError(); goto done; }
    if (!MoveFileExW(mdsOutput ? descriptor : temporary,destination,MOVEFILE_WRITE_THROUGH | (config->replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0))) { error = GetLastError(); goto done; }
    success = true;
done:
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output); if (temporaryCreated && !success) DeleteFileW(temporary); if (descriptorCreated && !success) DeleteFileW(descriptor); free(buffer);
    if (!success) return read_failure(&stats,error,"Disc read did not complete",callback,context);
    stats.state = PIPELINE_STATE_COMPLETED; stats.progressPercent = 100; stats.remainingTimeSec = 0; snprintf(stats.statusMessage,sizeof(stats.statusMessage),mdsOutput ? "DVD read successfully to MDS and ISO." : "Disc image read successfully."); if (callback) callback(&stats,context); return true;
}

bool pipeline_verify_disc_ex(const VerifyConfig* config, PipelineProgressCallback callback, void* context) {
    PipelineStats stats = {0}; DataImageSource image = {0}; uint8_t* data = NULL; uint8_t* reference = NULL; DWORD error = ERROR_SUCCESS; bool success = false, imageOpen = false;
    if (!config || !config->drive || config->retryCount > 20) return read_failure(&stats,ERROR_INVALID_PARAMETER,"Invalid verify configuration",callback,context);
    ReadConfig reader = {0}; reader.drive = config->drive; reader.cancelEvent = config->cancelEvent; reader.retryCount = config->retryCount;
    if (read_cancelled(&reader)) return read_failure(&stats,ERROR_CANCELLED,"Verify cancelled",callback,context);
    bool zeroSource = config->zeroSectors != 0, compare = zeroSource || (config->imagePath && config->imagePath[0]);
    if (zeroSource) stats.totalSectors = config->zeroSectors;
    else if (compare) {
        const wchar_t* extension = wcsrchr(config->imagePath,L'.');
        if (extension && !_wcsicmp(extension,L".cue")) return pipeline_verify_cd(config,callback,context);
        if (!imgburn_data_source_open(&image,config->imagePath)) return read_failure(&stats,GetLastError(),"Cannot open comparison image",callback,context);
        imageOpen = true; stats.totalSectors = image.sectors;
    }
    OpticalDrive* drive = config->drive; if (!mmc_test_unit_ready(drive)) { error = read_device_error(drive); goto verify_done; }
    MmcProfile profile = mmc_get_current_profile(drive); if (!read_profile(profile)) { error = ERROR_NOT_SUPPORTED; goto verify_done; }
    if (!compare && profile <= MMC_PROFILE_CD_RW) return pipeline_verify_cd(config,callback,context);
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
