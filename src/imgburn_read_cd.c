/* 单会话原始 CD：保留 2352 字节主通道，用 Q 子通道核对轨道和索引。 */
#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

typedef struct { uint32_t indices[100]; bool present[100], seen, modeKnown; unsigned lastIndex, mode; } CdReadTrack;
typedef struct { MmcCdToc toc; CdReadTrack tracks[99]; unsigned lastTrack; uint32_t lastPosition; bool positioned; } CdReadPlan;
static bool cd_cancelled(const ReadConfig* config) { return config->cancelEvent && WaitForSingleObject(config->cancelEvent,0) == WAIT_OBJECT_0; }
static bool cd_fail(PipelineStats* stats, DWORD error, PipelineProgressCallback callback, void* context) {
    if (!error) error = ERROR_READ_FAULT; stats->state = error == ERROR_CANCELLED ? PIPELINE_STATE_ABORTED : PIPELINE_STATE_FAILED;
    snprintf(stats->statusMessage,sizeof(stats->statusMessage),"Raw CD read did not complete (error %lu).",error); if (callback) callback(stats,context); SetLastError(error); return false;
}
static DWORD cd_error(const OpticalDrive* drive) { return drive->lastError ? drive->lastError : ERROR_READ_FAULT; }
static bool cd_retryable(const OpticalDrive* drive) { return drive->lastSense.senseKey != 2 && drive->lastSense.senseKey != 5 && drive->lastSense.senseKey != 6; }
static bool cd_read(const ReadConfig* config, uint32_t lba, uint16_t count, uint8_t* data, PipelineStats* stats) {
    OpticalDrive* drive = config->drive; if (mmc_read_cd_blocks(drive,lba,count,data)) return true; if (!cd_retryable(drive)) return false;
    for (uint16_t i = 0; i < count; i++) {
        bool ok = false;
        for (unsigned attempt = count == 1 ? 1 : 0; attempt <= config->retryCount; attempt++) {
            if (cd_cancelled(config)) { SetLastError(ERROR_CANCELLED); return false; }
            if (stats->retriesCount != UINT32_MAX) stats->retriesCount++;
            if (mmc_read_cd_blocks(drive,lba + i,1,data + (size_t)i * 2368)) { ok = true; break; } if (!cd_retryable(drive)) return false;
        }
        if (!ok) { SetLastError(cd_error(drive)); return false; }
    }
    return true;
}
static int cd_bcd(uint8_t value) { return (value & 15) > 9 || (value >> 4) > 9 ? -1 : (value >> 4) * 10 + (value & 15); }
static bool cd_msf(const uint8_t* data, uint32_t* result) {
    int m = cd_bcd(data[0]), s = cd_bcd(data[1]), f = cd_bcd(data[2]); if (m < 0 || s < 0 || s >= 60 || f < 0 || f >= 75) return false;
    *result = (uint32_t)((m * 60 + s) * 75 + f); return true;
}
static bool cd_q_crc(const uint8_t* q) {
    if (!q[10] && !q[11]) return true;
    uint16_t crc = 0; for (unsigned i = 0; i < 10; i++) { crc ^= (uint16_t)q[i] << 8; for (unsigned bit = 0; bit < 8; bit++) crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1); }
    return (uint16_t)~crc == ((uint16_t)q[10] << 8 | q[11]);
}
static bool cd_capture(CdReadPlan* plan, uint32_t lba, const uint8_t* data) {
    for (unsigned i = 0; i < plan->toc.trackCount; i++) if (lba == plan->toc.tracks[i].startLba && (plan->toc.tracks[i].control & 4)) {
        if (data[0] || data[11] || (data[15] != 1 && data[15] != 2)) goto invalid;
        for (unsigned j = 1; j < 11; j++) if (data[j] != 0xFF) goto invalid;
        plan->tracks[i].mode = data[15]; plan->tracks[i].modeKnown = true;
    }
    const uint8_t* q = data + 2352; unsigned adr = q[0] & 15;
    if (!cd_q_crc(q)) { SetLastError(ERROR_CRC); return false; }
    if (adr == 2 || adr == 3) return true;
    if (adr != 1 || q[6]) goto invalid;
    int number = cd_bcd(q[1]), index = cd_bcd(q[2]); uint32_t absolute, relative;
    if (number < 1 || (unsigned)number > plan->toc.trackCount || index < 0 || !cd_msf(q + 7,&absolute) || !cd_msf(q + 3,&relative) || absolute != lba + 150) goto invalid;
    unsigned track = (unsigned)number - 1; CdReadTrack* current = &plan->tracks[track]; uint32_t start = plan->toc.tracks[track].startLba;
    if ((q[0] >> 4) != plan->toc.tracks[track].control || (!plan->positioned && number != 1) || (plan->positioned && ((unsigned)number < plan->lastTrack || (unsigned)number > plan->lastTrack + 1))) goto invalid;
    if ((!index && (lba >= start || relative != start - lba)) || (index && (lba < start || relative != lba - start))) goto invalid;
    if ((current->seen && (unsigned)index < current->lastIndex) || (!current->seen && index > 1)) goto invalid;
    if (!current->present[index]) {
        /* 非位置 Q 可能遮住索引边界；无法确定时拒绝猜测起点。INDEX 01 使用 TOC 的确定地址。 */
        if ((plan->positioned && plan->lastPosition + 1 != lba) || (!plan->positioned && lba)) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        current->indices[index] = lba; current->present[index] = true;
    }
    current->seen = true; current->lastIndex = (unsigned)index; plan->lastTrack = (unsigned)number; plan->lastPosition = lba; plan->positioned = true; return true;
invalid:
    SetLastError(ERROR_INVALID_DATA); return false;
}
static bool cd_target(const wchar_t* input, wchar_t* full, bool replace) {
    full[0] = 0; DWORD length = GetFullPathNameW(input,MAX_PATH,full,NULL); if (!length || length >= MAX_PATH) { SetLastError(ERROR_INVALID_NAME); return false; }
    const wchar_t* dot = wcsrchr(full,L'.');
    if (!dot || _wcsicmp(dot,L".cue") || !wcsncmp(full,L"\\\\.\\",4) || !wcsncmp(full,L"\\\\?\\",4)) { SetLastError(ERROR_INVALID_NAME); return false; }
    const wchar_t* name = wcsrchr(full,L'\\'); if (!name || wcschr(name,L':') || wcschr(name,L'"') || wcschr(name,L'\r') || wcschr(name,L'\n')) { SetLastError(ERROR_INVALID_NAME); return false; }
    DWORD attributes = GetFileAttributesW(full);
    if (attributes != INVALID_FILE_ATTRIBUTES) { if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) { SetLastError(ERROR_ACCESS_DENIED); return false; } if (!replace) { SetLastError(ERROR_FILE_EXISTS); return false; } }
    else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return false;
    return true;
}
static bool cd_write(HANDLE output, const void* data, DWORD length) {
    const uint8_t* bytes = data; DWORD offset = 0;
    while (offset < length) { DWORD written = 0; if (!WriteFile(output,bytes + offset,length - offset,&written,NULL)) return false; if (!written || written > length - offset) { SetLastError(ERROR_WRITE_FAULT); return false; } offset += written; }
    return true;
}
static bool cd_append(char* text, size_t* used, const char* format, ...) {
    va_list args; va_start(args,format); int bytes = _vsnprintf_s(text + *used,512 * 1024 - *used,_TRUNCATE,format,args); va_end(args);
    if (bytes < 0) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return false; } *used += (size_t)bytes; return true;
}
static bool cd_publish(const ReadConfig* config, const CdReadPlan* plan, const wchar_t* destination, const wchar_t* binary) {
    const wchar_t* name = wcsrchr(binary,L'\\'); if (!name) { SetLastError(ERROR_INVALID_NAME); return false; }
    char fileName[MAX_PATH * 3]; if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,name + 1,-1,fileName,sizeof(fileName),NULL,NULL)) return false;
    char* text = calloc(512 * 1024,1); if (!text) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; } size_t used = 0;
    bool ok = cd_append(text,&used,"\xEF\xBB\xBFREM SESSION 1\r\nFILE \"%s\" BINARY\r\n",fileName);
    for (unsigned i = 0; ok && i < plan->toc.trackCount; i++) {
        const CdReadTrack* t = &plan->tracks[i]; unsigned control = plan->toc.tracks[i].control;
        ok = cd_append(text,&used,"  TRACK %02u %s\r\n",i + 1,t->mode == 1 ? "MODE1/2352" : t->mode == 2 ? "MODE2/2352" : "AUDIO");
        if (ok && (control & 11)) ok = cd_append(text,&used,"    FLAGS%s%s%s\r\n",control & 2 ? " DCP" : "",control & 1 ? " PRE" : "",control & 8 ? " 4CH" : "");
        for (unsigned j = 0; ok && j < 100; j++) if (t->present[j]) { uint32_t lba = t->indices[j]; ok = cd_append(text,&used,"    INDEX %02u %02u:%02u:%02u\r\n",j,lba / 4500,lba / 75 % 60,lba % 75); }
    }
    uint32_t end = plan->toc.leadOutLba; if (ok) ok = cd_append(text,&used,"REM LEAD-OUT %02u:%02u:%02u\r\n",end / 4500,end / 75 % 60,end % 75);
    wchar_t temporary[MAX_PATH] = L"", full[MAX_PATH]; HANDLE file = INVALID_HANDLE_VALUE; static volatile LONG serial; bool created = false;
    for (unsigned i = 0; ok && i < 64; i++) {
        if (swprintf(temporary,MAX_PATH,L"%ls.partial.%lu.%lu",destination,GetCurrentProcessId(),(unsigned long)InterlockedIncrement(&serial)) < 0) { SetLastError(ERROR_FILENAME_EXCED_RANGE); ok = false; break; }
        file = CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL); if (file != INVALID_HANDLE_VALUE) { created = true; break; }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) { ok = false; break; }
    }
    ok = ok && created;
    if (ok) ok = cd_write(file,text,(DWORD)used) && FlushFileBuffers(file);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError(); if (file != INVALID_HANDLE_VALUE && !CloseHandle(file) && ok) { ok = false; error = GetLastError(); }
    if (ok && cd_cancelled(config)) { ok = false; error = ERROR_CANCELLED; }
    if (ok && !cd_target(destination,full,config->replaceExisting)) { ok = false; error = GetLastError(); }
    if (ok && !MoveFileExW(temporary,destination,MOVEFILE_WRITE_THROUGH | (config->replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0))) { ok = false; error = GetLastError(); }
    if (!ok && created) DeleteFileW(temporary); free(text); if (!ok) SetLastError(error ? error : ERROR_WRITE_FAULT); return ok;
}

bool pipeline_read_cd(const ReadConfig* config, PipelineProgressCallback callback, void* context) {
    PipelineStats stats = {0}; if (!config || !config->drive || !config->imagePath || !config->imagePath[0] || config->retryCount > 20) return cd_fail(&stats,ERROR_INVALID_PARAMETER,callback,context);
    if (cd_cancelled(config)) return cd_fail(&stats,ERROR_CANCELLED,callback,context);
    wchar_t destination[MAX_PATH] = L"", binary[MAX_PATH] = L"", parent[MAX_PATH]; if (!cd_target(config->imagePath,destination,config->replaceExisting)) return cd_fail(&stats,GetLastError(),callback,context);
    stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Inspecting raw CD layout..."); if (callback) callback(&stats,context);
    OpticalDrive* drive = config->drive; if (!mmc_test_unit_ready(drive)) return cd_fail(&stats,cd_error(drive),callback,context);
    MmcProfile profile = mmc_get_current_profile(drive); if (profile != MMC_PROFILE_CD_ROM && profile != MMC_PROFILE_CD_R && profile != MMC_PROFILE_CD_RW) return cd_fail(&stats,ERROR_NOT_SUPPORTED,callback,context);
    CdReadPlan* plan = calloc(1,sizeof(*plan)); uint8_t* data = malloc(27 * 2368); HANDLE output = INVALID_HANDLE_VALUE; bool created = false, success = false; DWORD error = ERROR_SUCCESS;
    if (!plan || !data) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if (!mmc_read_cd_toc(drive,&plan->toc)) { error = cd_error(drive); goto done; }
    if (plan->toc.leadOutLba > 100 * 60 * 75 - 150) { error = ERROR_NOT_SUPPORTED; goto done; }
    stats.totalSectors = plan->toc.leadOutLba;
    for (unsigned i = 0; i < plan->toc.trackCount; i++) { plan->tracks[i].indices[1] = plan->toc.tracks[i].startLba; plan->tracks[i].present[1] = true; plan->tracks[i].modeKnown = !(plan->toc.tracks[i].control & 4); }
    wcscpy(parent,destination); wchar_t* slash = wcsrchr(parent,L'\\'); if (!slash) { error = ERROR_INVALID_NAME; goto done; } slash[1] = 0;
    ULARGE_INTEGER available; if (!GetDiskFreeSpaceExW(parent,&available,NULL,NULL)) { error = GetLastError(); goto done; }
    if (available.QuadPart < (uint64_t)stats.totalSectors * 2352 + 512 * 1024) { error = ERROR_DISK_FULL; goto done; }
    wcscpy(parent,destination); wchar_t* dot = wcsrchr(parent,L'.'); if (!dot) { error = ERROR_INVALID_NAME; goto done; } *dot = 0;
    /* 新数据文件从不覆盖旧 BIN；最后原子发布 CUE，保证原描述文件始终引用原数据。 */
    for (unsigned i = 0; i < 1000; i++) {
        int chars = i ? swprintf(binary,MAX_PATH,L"%ls.%03u.bin",parent,i) : swprintf(binary,MAX_PATH,L"%ls.bin",parent);
        if (chars < 0) { error = ERROR_FILENAME_EXCED_RANGE; goto done; }
        output = CreateFileW(binary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL); if (output != INVALID_HANDLE_VALUE) { created = true; break; }
        error = GetLastError(); if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) goto done;
    }
    if (!created) goto done;
    stats.state = PIPELINE_STATE_READING; ULONGLONG started = GetTickCount64(); snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Reading raw CD and Q indexes..."); if (callback) callback(&stats,context);
    for (uint32_t lba = 0; lba < stats.totalSectors;) {
        if (cd_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
        uint16_t count = (uint16_t)(stats.totalSectors - lba < 27 ? stats.totalSectors - lba : 27);
        if (!cd_read(config,lba,count,data,&stats)) { error = cd_cancelled(config) ? ERROR_CANCELLED : cd_error(drive); goto done; }
        for (unsigned i = 0; i < count; i++) { if (!cd_capture(plan,lba + i,data + (size_t)i * 2368)) { error = GetLastError(); goto done; } memmove(data + (size_t)i * 2352,data + (size_t)i * 2368,2352); }
        if (!cd_write(output,data,(DWORD)count * 2352)) { error = GetLastError(); goto done; }
        lba += count; stats.currentLba = lba; stats.progressPercent = (int)((uint64_t)lba * 99 / stats.totalSectors); ULONGLONG elapsed = GetTickCount64() - started;
        stats.elapsedTimeSec = (uint32_t)(elapsed / 1000); if (elapsed) { stats.currentSpeedKBps = (double)lba * 2296.875 / elapsed; stats.speedMultiplier = stats.currentSpeedKBps / 150.0; stats.remainingTimeSec = (uint32_t)((stats.totalSectors - lba) * 2.296875 / stats.currentSpeedKBps); } if (callback) callback(&stats,context);
    }
    for (unsigned i = 0; i < plan->toc.trackCount; i++) if (!plan->tracks[i].seen || !plan->tracks[i].modeKnown) { error = ERROR_INVALID_DATA; goto done; }
    if (cd_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
    MmcCdToc finalToc; if (!mmc_test_unit_ready(drive) || !mmc_read_cd_toc(drive,&finalToc)) { error = cd_error(drive); goto done; }
    if (mmc_get_current_profile(drive) != profile || finalToc.trackCount != plan->toc.trackCount || finalToc.leadOutLba != plan->toc.leadOutLba || memcmp(finalToc.tracks,plan->toc.tracks,finalToc.trackCount * sizeof(MmcCdTrack))) { error = ERROR_MEDIA_CHANGED; goto done; }
    if (!FlushFileBuffers(output)) { error = GetLastError(); goto done; }
    if (!CloseHandle(output)) { error = GetLastError(); output = INVALID_HANDLE_VALUE; goto done; } output = INVALID_HANDLE_VALUE;
    if (!cd_publish(config,plan,destination,binary)) { error = GetLastError(); goto done; } success = true;
done:
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output); if (!success && created) DeleteFileW(binary); free(data); free(plan);
    if (!success) return cd_fail(&stats,error,callback,context);
    stats.state = PIPELINE_STATE_COMPLETED; stats.progressPercent = 100; stats.remainingTimeSec = 0; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Raw CD image read successfully."); if (callback) callback(&stats,context); return true;
}
