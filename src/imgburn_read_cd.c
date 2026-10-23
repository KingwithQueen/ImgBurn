/* 单会话原始 CD：主通道读取与校验；读取时用 Q 核对轨道和索引。 */
#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_pipeline.h"
#include "imgburn_image.h"
#include "imgburn_audio.h"
#include "imgburn_audio_source.h"
#include "imgburn_cd_sector.h"
#include "imgburn_cdtext.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    uint32_t indices[100], oldGapLast, newGapFirst, copySamples, copyTransitions;
    bool present[100], seen, modeKnown, oldGapSeen, newGapSeen, gapModeKnown, scms;
    unsigned lastIndex, mode, gapMode, copyValues, oldCopyValues, copyPhases, lastCopy;
} CdReadTrack;
typedef struct { MmcCdToc toc; CdReadTrack tracks[99]; unsigned lastTrack; uint32_t lastPosition; bool positioned; CdText cdText; CueSheet* metadata; } CdReadPlan;
static bool cd_cancelled(const ReadConfig* config) { return config->cancelEvent && WaitForSingleObject(config->cancelEvent,0) == WAIT_OBJECT_0; }
static bool cd_fail(PipelineStats* stats, DWORD error, PipelineProgressCallback callback, void* context) {
    if (!error) error = ERROR_READ_FAULT; stats->state = error == ERROR_CANCELLED ? PIPELINE_STATE_ABORTED : PIPELINE_STATE_FAILED;
    snprintf(stats->statusMessage,sizeof(stats->statusMessage),"Raw CD read did not complete (error %lu).",error); if (callback) callback(stats,context); SetLastError(error); return false;
}
static DWORD cd_error(const OpticalDrive* drive) { return drive->lastError ? drive->lastError : ERROR_READ_FAULT; }
static bool cd_retryable(const OpticalDrive* drive) { return drive->lastSense.senseKey != 2 && drive->lastSense.senseKey != 5 && drive->lastSense.senseKey != 6; }
static bool cd_read(const ReadConfig* config, uint32_t lba, uint16_t count, uint8_t* data, bool withQ, PipelineStats* stats) {
    OpticalDrive* drive = config->drive; bool (*readBlocks)(OpticalDrive*,uint32_t,uint16_t,void*) = withQ ? mmc_read_cd_blocks : mmc_read_cd_main_blocks;
    if (readBlocks(drive,lba,count,data)) return true; if (!cd_retryable(drive)) return false;
    for (uint16_t i = 0; i < count; i++) {
        bool ok = false;
        for (unsigned attempt = count == 1 ? 1 : 0; attempt <= config->retryCount; attempt++) {
            if (cd_cancelled(config)) { SetLastError(ERROR_CANCELLED); return false; }
            if (stats->retriesCount != UINT32_MAX) stats->retriesCount++;
            if (readBlocks(drive,lba + i,1,data + (size_t)i * (withQ ? 2368 : 2352))) { ok = true; break; } if (!cd_retryable(drive)) return false;
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
static unsigned cd_frame_mode(const uint8_t* data) {
    if (data[0] || data[11] || (data[15] != 1 && data[15] != 2)) return 0;
    for (unsigned i = 1; i < 11; i++) if (data[i] != 0xFF) return 0;
    return data[15];
}
static void cd_capture_copy(CdReadTrack* track, uint32_t lba, unsigned control) {
    unsigned copy = (control >> 1) & 1;
    if (!track->copySamples) track->copyPhases = 0xFF;
    else if (track->lastCopy != copy) track->copyTransitions++;
    track->copyValues |= 1u << copy; track->lastCopy = copy; track->copySamples++;
    /* SCMS每四块翻转；保持全部八个初始相位候选，不从TOC猜测采样相位。 */
    for (unsigned phase = 0; phase < 8; phase++) if (((lba + phase) % 8 >= 4) != (copy != 0)) track->copyPhases &= ~(1u << phase);
}
static bool cd_capture_complete(CdReadPlan* plan) {
    for (unsigned i = 0; i < plan->toc.trackCount; i++) {
        CdReadTrack* track = &plan->tracks[i]; unsigned control = plan->toc.tracks[i].control;
        if (!track->seen || !track->modeKnown || !track->copySamples) goto invalid;
        bool extended = i && (control & 4) && track->oldGapSeen && (((control ^ plan->toc.tracks[i - 1].control) & 13) || track->mode != plan->tracks[i - 1].mode);
        if (extended) {
            uint32_t start = plan->toc.tracks[i].startLba;
            if (!track->present[0] || start - track->indices[0] < 225 || !track->oldGapSeen || !track->newGapSeen || track->oldGapLast != start - 151 || track->newGapFirst != start - 150) goto unsupported;
        }
        unsigned expectedOld = extended ? plan->toc.tracks[i - 1].control : control;
        if (track->oldCopyValues && track->oldCopyValues != (1u << ((expectedOld >> 1) & 1))) goto unsupported;
        if (track->copyValues == 3) {
            /* 普通拷贝位变化不能等同SCMS；只有完整周期证据才能导出该标志。 */
            if ((control & 6) || !track->copyPhases || (track->copyPhases & (track->copyPhases - 1)) || track->copySamples < 16 || track->copyTransitions < 3) goto unsupported;
            track->scms = true;
        } else if (track->copyValues != (1u << ((control >> 1) & 1))) goto invalid;
    }
    return true;
unsupported:
    SetLastError(ERROR_NOT_SUPPORTED); return false;
invalid:
    SetLastError(ERROR_INVALID_DATA); return false;
}
static bool cd_capture(CdReadPlan* plan, uint32_t lba, const uint8_t* data) {
    for (unsigned i = 0; i < plan->toc.trackCount; i++) if (lba == plan->toc.tracks[i].startLba && (plan->toc.tracks[i].control & 4)) {
        unsigned mode = cd_frame_mode(data); if (!mode || (plan->tracks[i].gapModeKnown && mode != plan->tracks[i].gapMode)) goto invalid;
        plan->tracks[i].mode = mode; plan->tracks[i].modeKnown = true;
    }
    const uint8_t* q = data + 2352; unsigned adr = q[0] & 15;
    if (!cd_q_crc(q)) { SetLastError(ERROR_CRC); return false; }
    if (adr == 2 || adr == 3) return true;
    if (adr != 1 || q[6]) goto invalid;
    int number = cd_bcd(q[1]), index = cd_bcd(q[2]); uint32_t absolute, relative;
    if (number < 1 || (unsigned)number > plan->toc.trackCount || index < 0 || !cd_msf(q + 7,&absolute) || !cd_msf(q + 3,&relative) || absolute != lba + 150) goto invalid;
    unsigned track = (unsigned)number - 1; CdReadTrack* current = &plan->tracks[track]; uint32_t start = plan->toc.tracks[track].startLba;
    if ((!plan->positioned && number != 1) || (plan->positioned && ((unsigned)number < plan->lastTrack || (unsigned)number > plan->lastTrack + 1))) goto invalid;
    if ((!index && (lba >= start || relative != start - lba)) || (index && (lba < start || relative != lba - start))) goto invalid;
    if ((current->seen && (unsigned)index < current->lastIndex) || (!current->seen && index > 1)) goto invalid;
    if (!current->present[index]) {
        /* 非位置 Q 可能遮住索引边界；无法确定时拒绝猜测起点。INDEX 01 使用 TOC 的确定地址。 */
        if ((plan->positioned && plan->lastPosition + 1 != lba) || (!plan->positioned && lba)) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        current->indices[index] = lba; current->present[index] = true;
    }
    unsigned control = q[0] >> 4, expected = plan->toc.tracks[track].control;
    bool oldGap = false;
    if (!index && track && (expected & 4) && start - current->indices[0] >= 225 && lba < start - 150) {
        unsigned previous = plan->toc.tracks[track - 1].control;
        /* 只有实际保持前轨结构的帧才进入扩展例外；普通长间隙可以始终保持当前结构。 */
        oldGap = (control & 13) == (previous & 13) && plan->tracks[track - 1].modeKnown && (!(previous & 4) || cd_frame_mode(data) == plan->tracks[track - 1].mode);
    }
    if (oldGap) {
        if (current->newGapSeen) goto unsupported;
        current->oldGapSeen = true; current->oldGapLast = lba; current->oldCopyValues |= 1u << ((control >> 1) & 1);
    } else {
        if ((control & 13) != (expected & 13)) goto invalid;
        if (!index && (expected & 4)) {
            unsigned mode = cd_frame_mode(data); if (!mode || (current->gapModeKnown && mode != current->gapMode)) goto invalid;
            if (!current->newGapSeen) current->newGapFirst = lba;
            current->newGapSeen = true; current->gapModeKnown = true; current->gapMode = mode;
        }
        cd_capture_copy(current,lba,control);
    }
    current->seen = true; current->lastIndex = (unsigned)index; plan->lastTrack = (unsigned)number; plan->lastPosition = lba; plan->positioned = true; return true;
invalid:
    SetLastError(ERROR_INVALID_DATA); return false;
unsupported:
    SetLastError(ERROR_NOT_SUPPORTED); return false;
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
static bool cd_append_text(char* text, size_t* used, const char* command, const char* value, bool track) {
    if (!value[0]) return true;
    /* CUE不能无损表示引号或控制符；这些字段只由原始CDT保留，不能擅自替换字符。 */
    for (const unsigned char* p = (const unsigned char*)value; *p; p++) if (*p < 32 || *p == '"' || *p == 127) return true;
    return cd_append(text,used,"%s%s \"%s\"\r\n",track ? "    " : "",command,value);
}
static bool cd_read_text(OpticalDrive* drive, CdText* text) {
    uint8_t* packs = NULL; uint32_t bytes = 0;
    if (!mmc_read_cd_text(drive,&packs,&bytes)) return false;
    bool result = !bytes || imgburn_cdtext_parse(packs,bytes,text); DWORD error = GetLastError(); free(packs); SetLastError(error); return result;
}
static bool cd_text_metadata(CdReadPlan* plan) {
    if (!plan->cdText.bytes) return true;
    plan->metadata = calloc(1,sizeof(*plan->metadata)); if (!plan->metadata) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    plan->metadata->trackCount = (int)plan->toc.trackCount;
    for (unsigned i = 0; i < plan->toc.trackCount; i++) plan->metadata->tracks[i].trackNumber = (int)i + 1;
    for (unsigned block = 0; block < 8; block++) if (plan->cdText.blocks[block].packCount && plan->cdText.blocks[block].supported && imgburn_cdtext_extract(&plan->cdText,block,plan->metadata)) return true;
    /* 未支持的语言、长字段或额外信息仍完整保存在CDT，明文字段是可选展示。 */
    memset(plan->metadata,0,sizeof(*plan->metadata)); return true;
}
static bool cd_publish(const ReadConfig* config, const CdReadPlan* plan, const wchar_t* destination, const wchar_t* binary, const wchar_t* cdTextFile) {
    const wchar_t* name = wcsrchr(binary,L'\\'); if (!name) { SetLastError(ERROR_INVALID_NAME); return false; }
    char fileName[MAX_PATH * 3]; if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,name + 1,-1,fileName,sizeof(fileName),NULL,NULL)) return false;
    char* text = calloc(512 * 1024,1); if (!text) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; } size_t used = 0;
    bool ok = cd_append(text,&used,"\xEF\xBB\xBF");
    if (ok && cdTextFile[0]) {
        const wchar_t* textName = wcsrchr(cdTextFile,L'\\'); char utf8[MAX_PATH * 3];
        if (!textName || !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,textName + 1,-1,utf8,sizeof(utf8),NULL,NULL)) ok = false;
        else ok = cd_append(text,&used,"CDTEXTFILE \"%s\"\r\n",utf8);
    }
    if (ok && plan->metadata) {
        const CueSheet* fields = plan->metadata;
        ok = cd_append_text(text,&used,"TITLE",fields->discTitle,false) && cd_append_text(text,&used,"PERFORMER",fields->discPerformer,false) && cd_append_text(text,&used,"SONGWRITER",fields->songwriter,false);
        if (ok) ok = cd_append_text(text,&used,"COMPOSER",fields->composer,false) && cd_append_text(text,&used,"ARRANGER",fields->arranger,false) && cd_append_text(text,&used,"MESSAGE",fields->message,false);
        if (ok) ok = cd_append_text(text,&used,"DISCID",fields->discId,false) && cd_append_text(text,&used,"UPC",fields->cdTextUpc,false);
    }
    if (ok) ok = cd_append(text,&used,"REM SESSION 1\r\nFILE \"%s\" BINARY\r\n",fileName);
    for (unsigned i = 0; ok && i < plan->toc.trackCount; i++) {
        const CdReadTrack* t = &plan->tracks[i]; unsigned control = plan->toc.tracks[i].control;
        ok = cd_append(text,&used,"  TRACK %02u %s\r\n",i + 1,t->mode == 1 ? "MODE1/2352" : t->mode == 2 ? "MODE2/2352" : "AUDIO");
        if (ok && plan->metadata) {
            const CueTrack* fields = &plan->metadata->tracks[i];
            ok = cd_append_text(text,&used,"TITLE",fields->title,true) && cd_append_text(text,&used,"PERFORMER",fields->performer,true) && cd_append_text(text,&used,"SONGWRITER",fields->songwriter,true);
            if (ok) ok = cd_append_text(text,&used,"COMPOSER",fields->composer,true) && cd_append_text(text,&used,"ARRANGER",fields->arranger,true) && cd_append_text(text,&used,"MESSAGE",fields->message,true);
        }
        if (ok && ((control & 11) || t->scms)) ok = cd_append(text,&used,"    FLAGS%s%s%s%s\r\n",!t->scms && (control & 2) ? " DCP" : "",control & 1 ? " PRE" : "",control & 8 ? " 4CH" : "",t->scms ? " SCMS" : "");
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
    wchar_t destination[MAX_PATH] = L"", binary[MAX_PATH] = L"", textPath[MAX_PATH] = L"", parent[MAX_PATH]; if (!cd_target(config->imagePath,destination,config->replaceExisting)) return cd_fail(&stats,GetLastError(),callback,context);
    stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Inspecting raw CD layout..."); if (callback) callback(&stats,context);
    OpticalDrive* drive = config->drive; if (!mmc_test_unit_ready(drive)) return cd_fail(&stats,cd_error(drive),callback,context);
    MmcProfile profile = mmc_get_current_profile(drive); if (profile != MMC_PROFILE_CD_ROM && profile != MMC_PROFILE_CD_R && profile != MMC_PROFILE_CD_RW) return cd_fail(&stats,ERROR_NOT_SUPPORTED,callback,context);
    CdReadPlan* plan = calloc(1,sizeof(*plan)); uint8_t* data = malloc(27 * 2368); HANDLE output = INVALID_HANDLE_VALUE, textOutput = INVALID_HANDLE_VALUE; bool created = false, textCreated = false, success = false; DWORD error = ERROR_SUCCESS;
    if (!plan || !data) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if (!mmc_read_cd_toc(drive,&plan->toc)) { error = cd_error(drive); goto done; }
    if (plan->toc.leadOutLba > 100 * 60 * 75 - 150) { error = ERROR_NOT_SUPPORTED; goto done; }
    stats.totalSectors = plan->toc.leadOutLba;
    if (config->readCdText && (!cd_read_text(drive,&plan->cdText) || !cd_text_metadata(plan))) { error = GetLastError(); goto done; }
    if (cd_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
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
        if (!cd_read(config,lba,count,data,true,&stats)) { error = cd_cancelled(config) ? ERROR_CANCELLED : cd_error(drive); goto done; }
        for (unsigned i = 0; i < count; i++) { if (!cd_capture(plan,lba + i,data + (size_t)i * 2368)) { error = GetLastError(); goto done; } memmove(data + (size_t)i * 2352,data + (size_t)i * 2368,2352); }
        if (!cd_write(output,data,(DWORD)count * 2352)) { error = GetLastError(); goto done; }
        lba += count; stats.currentLba = lba; stats.progressPercent = (int)((uint64_t)lba * 99 / stats.totalSectors); ULONGLONG elapsed = GetTickCount64() - started;
        stats.elapsedTimeSec = (uint32_t)(elapsed / 1000); if (elapsed) { stats.currentSpeedKBps = (double)lba * 2296.875 / elapsed; stats.speedMultiplier = stats.currentSpeedKBps / 150.0; stats.remainingTimeSec = (uint32_t)((stats.totalSectors - lba) * 2.296875 / stats.currentSpeedKBps); } if (callback) callback(&stats,context);
    }
    if (!cd_capture_complete(plan)) { error = GetLastError(); goto done; }
    if (cd_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
    MmcCdToc finalToc; if (!mmc_test_unit_ready(drive) || !mmc_read_cd_toc(drive,&finalToc)) { error = cd_error(drive); goto done; }
    if (mmc_get_current_profile(drive) != profile || finalToc.trackCount != plan->toc.trackCount || finalToc.leadOutLba != plan->toc.leadOutLba || memcmp(finalToc.tracks,plan->toc.tracks,finalToc.trackCount * sizeof(MmcCdTrack))) { error = ERROR_MEDIA_CHANGED; goto done; }
    if (config->readCdText) {
        uint8_t* finalText = NULL; uint32_t finalBytes = 0;
        if (!mmc_read_cd_text(drive,&finalText,&finalBytes)) { error = GetLastError(); goto done; }
        bool textChanged = finalBytes != plan->cdText.bytes || (finalBytes && memcmp(finalText,plan->cdText.packs,finalBytes)); free(finalText);
        if (textChanged) { error = ERROR_MEDIA_CHANGED; goto done; }
    }
    if (!FlushFileBuffers(output)) { error = GetLastError(); goto done; }
    if (!CloseHandle(output)) { error = GetLastError(); output = INVALID_HANDLE_VALUE; goto done; } output = INVALID_HANDLE_VALUE;
    if (cd_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
    if (plan->cdText.bytes) {
        for (unsigned i = 0; i < 1000; i++) {
            int chars = i ? swprintf(textPath,MAX_PATH,L"%ls.%03u.cdt",parent,i) : swprintf(textPath,MAX_PATH,L"%ls.cdt",parent);
            if (chars < 0) { error = ERROR_FILENAME_EXCED_RANGE; goto done; }
            textOutput = CreateFileW(textPath,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
            if (textOutput != INVALID_HANDLE_VALUE) { textCreated = true; break; }
            error = GetLastError(); if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) goto done;
        }
        if (!textCreated) goto done;
        if (!cd_write(textOutput,plan->cdText.packs,(DWORD)plan->cdText.bytes) || !FlushFileBuffers(textOutput)) { error = GetLastError(); goto done; }
        if (!CloseHandle(textOutput)) { error = GetLastError(); textOutput = INVALID_HANDLE_VALUE; goto done; } textOutput = INVALID_HANDLE_VALUE;
    }
    if (!cd_publish(config,plan,destination,binary,textPath)) { error = GetLastError(); goto done; } success = true;
done:
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output); if (textOutput != INVALID_HANDLE_VALUE) CloseHandle(textOutput);
    if (!success && created) DeleteFileW(binary); if (!success && textCreated) DeleteFileW(textPath);
    if (plan) { imgburn_cdtext_free(&plan->cdText); free(plan->metadata); } free(data); free(plan);
    if (!success) return cd_fail(&stats,error,callback,context);
    stats.state = PIPELINE_STATE_COMPLETED; stats.progressPercent = 100; stats.remainingTimeSec = 0; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Raw CD image read successfully."); if (callback) callback(&stats,context); return true;
}

typedef struct {
    HANDLE file;
    uint32_t start, end, payloadStart, payloadEnd, extendedUntil;
    uint64_t offset, sourceBytes;
    TrackDataMode mode, previousMode;
    bool audio, swap, saoData;
} CdVerifySpan;
typedef struct { wchar_t path[MAX_PATH]; CueFileType type; AudioPcmSource source; } CdVerifyAudio;
typedef struct { CueSheet sheet; CdVerifySpan spans[99]; bool saoData; HANDLE cancelEvent; CdVerifyAudio audio[99]; unsigned audioCount; DWORD prepareError; } CdVerifyImage;

static AudioPcmSource* cd_verify_audio(CdVerifyImage* image, const wchar_t* path, CueFileType type) {
    for (unsigned i = 0; i < image->audioCount; i++) if (image->audio[i].type == type && !_wcsicmp(image->audio[i].path,path)) return &image->audio[i].source;
    if (image->audioCount >= 99) { SetLastError(ERROR_INVALID_DATA); return NULL; }
    CdVerifyAudio* audio = &image->audio[image->audioCount];
    if (!audio_pcm_source_open(path,image->cancelEvent,&audio->source)) { image->prepareError = GetLastError(); return NULL; }
    wcscpy(audio->path,path); audio->type = type; image->audioCount++; return &audio->source;
}
static bool cd_verify_resolve(const wchar_t* path, CueFileType type, uint64_t* bytes, void* context) {
    AudioPcmSource* source = cd_verify_audio(context,path,type); if (!source) return false; *bytes = source->dataBytes; return true;
}

static TrackDataMode cd_verify_mode(const CdVerifySpan* span, uint32_t lba) { return lba < span->extendedUntil ? span->previousMode : span->mode; }

static bool cd_verify_plan(CdVerifyImage* image) {
    CueSheet* sheet = &image->sheet;
    if (sheet->trackCount < 1 || sheet->trackCount > MAX_CUE_TRACKS) { SetLastError(ERROR_INVALID_DATA); return false; }
    if (sheet->sessionCount != 1) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    for (int i = 0; i < sheet->trackCount; i++) {
        const CueTrack* track = &sheet->tracks[i]; CdVerifySpan* span = &image->spans[i];
        if (track->indexCount < 1 || track->indexCount > MAX_CUE_INDICES || track->sectorCount > UINT32_MAX - track->startLba) { SetLastError(ERROR_INVALID_DATA); return false; }
        if (track->sectorSize != 2352 || (track->mode != TRACK_MODE_AUDIO && track->mode != TRACK_MODE_MODE1_2352 && track->mode != TRACK_MODE_MODE2_2352) || track->sourceType > CUE_FILE_AUDIO || (track->sourceType != CUE_FILE_BINARY && track->mode != TRACK_MODE_AUDIO) || (!image->saoData && (track->pregapSectors || track->postgapSectors) && track->mode != TRACK_MODE_AUDIO)) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        uint32_t indexOne = 0; bool found = false;
        for (int j = 0; j < track->indexCount; j++) {
            const CueIndex* entry = &track->indices[j];
            if (entry->indexNumber < 0 || entry->indexNumber > 99 || (!j && entry->indexNumber > 1) || (j && (entry->indexNumber <= track->indices[j - 1].indexNumber || entry->lba < track->indices[j - 1].lba))) { SetLastError(ERROR_INVALID_DATA); return false; }
            if (entry->indexNumber == 1) { indexOne = entry->lba; found = true; }
        }
        /* 与写入及原版一致：仅整个镜像首轨缺少INDEX00时保留文件零帧起的隐式间隙。 */
        uint32_t fileStart = !i && track->indices[0].indexNumber != 0 ? 0 : track->indices[0].lba;
        if (!found || fileStart > indexOne || indexOne - fileStart > track->startLba || track->indices[track->indexCount - 1].lba - indexOne >= track->sectorCount) { SetLastError(ERROR_INVALID_DATA); return false; }
        span->payloadStart = track->startLba - (indexOne - fileStart); span->payloadEnd = track->startLba + track->sectorCount;
        uint64_t prefix = (uint64_t)(track->startLba - span->payloadStart) * 2352;
        if (track->pregapSectors > span->payloadStart || track->postgapSectors > UINT32_MAX - span->payloadEnd) { SetLastError(ERROR_INVALID_DATA); return false; }
        span->start = span->payloadStart - track->pregapSectors; span->end = span->payloadEnd + track->postgapSectors;
        if (track->sourceByteOffset < prefix || span->end <= span->start || (i ? span->start != image->spans[i - 1].end : span->start != 0)) { SetLastError(ERROR_INVALID_DATA); return false; }
        span->offset = track->sourceByteOffset - prefix; span->audio = track->mode == TRACK_MODE_AUDIO; span->swap = track->sourceType == CUE_FILE_MOTOROLA;
        span->mode = track->mode; span->previousMode = i ? sheet->tracks[i - 1].mode : track->mode; span->saoData = image->saoData;
        if (image->saoData && i && !span->audio && span->mode != span->previousMode) {
            if (track->startLba - span->start < 225) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
            /* 扩展预间隙先沿用前轨数据结构，最后两秒才切换为新数据轨。 */
            span->extendedUntil = track->startLba - 150;
        }
        /* 句柄保持只读共享，校验期间禁止源文件被改写或替换。 */
        uint64_t bytes = (uint64_t)(span->payloadEnd - span->payloadStart) * 2352;
        if (track->sourceType >= CUE_FILE_WAVE) {
            AudioPcmSource* pcm = cd_verify_audio(image,track->sourceFilePath,track->sourceType); if (!pcm) return false;
            uint64_t paddedBytes = (uint64_t)pcm->sectorCount * 2352;
            if ((track->decodedSourceSectors && track->decodedSourceSectors != pcm->sectorCount) || span->offset >= pcm->dataBytes || span->offset > paddedBytes || bytes > paddedBytes - span->offset) { SetLastError(ERROR_INVALID_DATA); return false; }
            if (!DuplicateHandle(GetCurrentProcess(),pcm->pcm,GetCurrentProcess(),&span->file,0,FALSE,DUPLICATE_SAME_ACCESS)) return false;
            span->sourceBytes = pcm->dataBytes - span->offset < bytes ? pcm->dataBytes - span->offset : bytes; span->offset += pcm->dataOffset; continue;
        }
        span->file = CreateFileW(track->sourceFilePath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if (span->file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER length; BY_HANDLE_FILE_INFORMATION info;
        if (GetFileType(span->file) != FILE_TYPE_DISK || !GetFileInformationByHandle(span->file,&info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !GetFileSizeEx(span->file,&length) || length.QuadPart < 0) { SetLastError(ERROR_INVALID_DATA); return false; }
        if (span->offset > (uint64_t)length.QuadPart || bytes > (uint64_t)length.QuadPart - span->offset) { SetLastError(ERROR_INVALID_DATA); return false; }
        span->sourceBytes = bytes;
    }
    if (image->spans[sheet->trackCount - 1].end != sheet->totalSectors) { SetLastError(ERROR_INVALID_DATA); return false; } return true;
}
static bool cd_verify_read(const CdVerifySpan* span, uint32_t lba, uint16_t count, uint8_t* data) {
    if (!count || count > 27 || lba < span->start || lba >= span->end || count > span->end - lba) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    DWORD wanted = (DWORD)count * 2352; memset(data,0,wanted);
    uint32_t first = lba > span->payloadStart ? lba : span->payloadStart, end = lba + count < span->payloadEnd ? lba + count : span->payloadEnd;
    /* PREGAP/POSTGAP 由布局生成，WAVE 末帧也只读取有效 PCM，再保持零填充。 */
    if (first < end) {
        uint64_t sourceOffset = (uint64_t)(first - span->payloadStart) * 2352;
        if (sourceOffset < span->sourceBytes) {
            LARGE_INTEGER position; position.QuadPart = (LONGLONG)(span->offset + sourceOffset);
            if (!SetFilePointerEx(span->file,position,NULL,FILE_BEGIN)) return false;
            DWORD bytes = (end - first) * 2352, offset = 0, destination = (first - lba) * 2352;
            if (span->sourceBytes - sourceOffset < bytes) bytes = (DWORD)(span->sourceBytes - sourceOffset);
            while (offset < bytes) { DWORD actual = 0; if (!ReadFile(span->file,data + destination + offset,bytes - offset,&actual,NULL)) return false; if (!actual || actual > bytes - offset) { SetLastError(ERROR_HANDLE_EOF); return false; } offset += actual; }
        }
    }
    if (span->swap) for (DWORD i = 0; i + 1 < wanted; i += 2) { uint8_t value = data[i]; data[i] = data[i + 1]; data[i + 1] = value; }
    if (span->saoData) for (uint32_t i = 0; i < count; i++) {
        uint32_t current = lba + i; uint8_t* frame = data + (size_t)i * 2352; TrackDataMode mode = cd_verify_mode(span,current);
        bool generated = current < span->payloadStart || current >= span->payloadEnd;
        if (!(generated ? imgburn_cd_zero_sector(mode,current,frame) : imgburn_cd_normalize_sector(mode,current,frame))) return false;
    }
    return true;
}

bool pipeline_verify_cd(const VerifyConfig* config, PipelineProgressCallback callback, void* context) {
    PipelineStats stats = {0}; DWORD error = ERROR_SUCCESS; bool success = false; HANDLE descriptor = INVALID_HANDLE_VALUE; CdVerifyImage* image = NULL; uint8_t* data = NULL; uint8_t* reference = NULL;
    if (!config || !config->drive || config->retryCount > 20) { error = ERROR_INVALID_PARAMETER; goto verify_done; }
    ReadConfig reader = {0}; reader.drive = config->drive; reader.cancelEvent = config->cancelEvent; reader.retryCount = config->retryCount;
    if (cd_cancelled(&reader)) { error = ERROR_CANCELLED; goto verify_done; }
    bool compare = config->imagePath && config->imagePath[0];
    if (compare) {
        const wchar_t* extension = wcsrchr(config->imagePath,L'.'); if (!extension || _wcsicmp(extension,L".cue")) { error = ERROR_NOT_SUPPORTED; goto verify_done; }
        descriptor = CreateFileW(config->imagePath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL); if (descriptor == INVALID_HANDLE_VALUE) { error = GetLastError(); goto verify_done; }
        image = calloc(1,sizeof(*image)); if (!image) { error = ERROR_NOT_ENOUGH_MEMORY; goto verify_done; } image->saoData = config->saoData; image->cancelEvent = config->cancelEvent;
        stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Preparing and checking CD reference audio..."); if (callback) callback(&stats,context);
        if (cd_cancelled(&reader)) { error = ERROR_CANCELLED; goto verify_done; }
        SetLastError(ERROR_SUCCESS);
        if (!imgburn_cue_parse_ex(config->imagePath,&image->sheet,cd_verify_resolve,image)) { error = image->prepareError ? image->prepareError : GetLastError() == ERROR_NOT_SUPPORTED ? ERROR_NOT_SUPPORTED : ERROR_INVALID_DATA; goto verify_done; }
        if (!cd_verify_plan(image)) { error = GetLastError(); goto verify_done; }
    }
    if (cd_cancelled(&reader)) { error = ERROR_CANCELLED; goto verify_done; }
    OpticalDrive* drive = config->drive; if (!mmc_test_unit_ready(drive)) { error = cd_error(drive); goto verify_done; }
    MmcProfile profile = mmc_get_current_profile(drive); if (profile != MMC_PROFILE_CD_ROM && profile != MMC_PROFILE_CD_R && profile != MMC_PROFILE_CD_RW) { error = ERROR_NOT_SUPPORTED; goto verify_done; }
    MmcCdToc discToc; if (!mmc_read_cd_toc(drive,&discToc)) { error = cd_error(drive); goto verify_done; } stats.totalSectors = discToc.leadOutLba;
    if (compare) {
        if (image->sheet.trackCount != (int)discToc.trackCount || image->sheet.totalSectors != discToc.leadOutLba) { error = ERROR_INVALID_DATA; goto verify_done; }
        for (unsigned i = 0; i < discToc.trackCount; i++) if (image->sheet.tracks[i].startLba != discToc.tracks[i].startLba || image->spans[i].audio == ((discToc.tracks[i].control & 4) != 0)) { error = ERROR_INVALID_DATA; goto verify_done; }
    }
    data = malloc(27 * 2352); if (compare) reference = malloc(27 * 2352); if (!data || (compare && !reference)) { error = ERROR_NOT_ENOUGH_MEMORY; goto verify_done; }
    stats.state = PIPELINE_STATE_VERIFYING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),compare ? "Verifying CD (audio: %s)..." : "Checking CD readability...",config->compareAudio ? "compare" : "readability only"); if (callback) callback(&stats,context);
    ULONGLONG started = GetTickCount64(); unsigned track = 0;
    for (uint32_t lba = 0; lba < stats.totalSectors;) {
        if (cd_cancelled(&reader)) { error = ERROR_CANCELLED; goto verify_done; }
        if (compare) while (track + 1 < discToc.trackCount && lba >= image->spans[track].end) track++;
        uint32_t end = compare ? image->spans[track].end : stats.totalSectors;
        if (compare && lba < image->spans[track].extendedUntil && image->spans[track].extendedUntil < end) end = image->spans[track].extendedUntil;
        uint16_t count = (uint16_t)(end - lba < 27 ? end - lba : 27);
        if (!cd_read(&reader,lba,count,data,false,&stats)) { error = cd_cancelled(&reader) ? ERROR_CANCELLED : cd_error(drive); goto verify_done; }
        if (compare && (cd_verify_mode(&image->spans[track],lba) != TRACK_MODE_AUDIO || config->compareAudio)) {
            if (!cd_verify_read(&image->spans[track],lba,count,reference)) { error = GetLastError(); goto verify_done; }
            for (unsigned i = 0; i < count; i++) if (memcmp(data + (size_t)i * 2352,reference + (size_t)i * 2352,2352)) stats.miscomparesCount++;
            stats.comparedSectors += count;
        } else stats.readableOnlySectors += count;
        lba += count; stats.currentLba = lba; stats.progressPercent = (int)((uint64_t)lba * 99 / stats.totalSectors); ULONGLONG elapsed = GetTickCount64() - started;
        stats.elapsedTimeSec = (uint32_t)(elapsed / 1000); if (elapsed) { stats.currentSpeedKBps = (double)lba * 2296.875 / elapsed; stats.speedMultiplier = stats.currentSpeedKBps / 150; stats.remainingTimeSec = (uint32_t)((stats.totalSectors - lba) * 2.296875 / stats.currentSpeedKBps); } if (callback) callback(&stats,context);
    }
    if (cd_cancelled(&reader)) { error = ERROR_CANCELLED; goto verify_done; }
    MmcCdToc finalToc; if (!mmc_test_unit_ready(drive) || !mmc_read_cd_toc(drive,&finalToc)) { error = cd_error(drive); goto verify_done; }
    if (mmc_get_current_profile(drive) != profile || discToc.trackCount != finalToc.trackCount || discToc.leadOutLba != finalToc.leadOutLba || memcmp(discToc.tracks,finalToc.tracks,discToc.trackCount * sizeof(MmcCdTrack))) { error = ERROR_MEDIA_CHANGED; goto verify_done; }
    if (stats.miscomparesCount) { error = ERROR_CRC; goto verify_done; } success = true;
verify_done:
    if (image) {
        for (unsigned i = 0; i < 99; i++) if (image->spans[i].file && image->spans[i].file != INVALID_HANDLE_VALUE) CloseHandle(image->spans[i].file);
        for (unsigned i = 0; i < image->audioCount; i++) audio_pcm_source_close(&image->audio[i].source);
    }
    if (descriptor != INVALID_HANDLE_VALUE) CloseHandle(descriptor); free(image); free(data); free(reference);
    stats.state = success ? PIPELINE_STATE_COMPLETED : error == ERROR_CANCELLED ? PIPELINE_STATE_ABORTED : PIPELINE_STATE_FAILED;
    if (success) { stats.progressPercent = 100; stats.remainingTimeSec = 0; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"CD verify passed: %u compared, %u readability-only sectors.",stats.comparedSectors,stats.readableOnlySectors); }
    else { if (!error) error = ERROR_READ_FAULT; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"CD verify did not complete (error %lu).",error); }
    if (callback) callback(&stats,context); if (!success) SetLastError(error); return success;
}
