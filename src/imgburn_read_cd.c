/* 原始 CD：主通道读取与校验；读取时用 Q 核对轨道和索引。 */
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
    bool present[100], seen, modeKnown, oldGapSeen, newGapSeen, gapModeKnown, scms, sessionFirst;
    unsigned lastIndex, mode, gapMode, copyValues, oldCopyValues, copyPhases, lastCopy;
    char isrc[13], queriedIsrc[13];
} CdReadTrack;
typedef struct {
    MmcCdToc toc; CdReadTrack tracks[99]; unsigned lastTrack; uint32_t lastPosition, sessionStarts[99]; bool positioned; CdText cdText; CueSheet* metadata;
    char catalog[14], queriedCatalog[14], pendingIsrc[13]; unsigned pendingTrack, captureFirstTrack; uint32_t captureStart; bool pendingConflict, idWarning;
} CdReadPlan;
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
static bool cd_identifier_merge(char* target, const char* value) {
    if (!value[0]) return true;
    if (target[0] && strcmp(target,value)) { SetLastError(ERROR_INVALID_DATA); return false; }
    strcpy(target,value); return true;
}
static bool cd_find_session_index00(const ReadConfig* config, const MmcCdFullToc* full, unsigned sessionIndex, PipelineStats* stats, uint32_t* start, PipelineProgressCallback callback, void* context) {
    if (!config || !full || !stats || !start || !sessionIndex || sessionIndex >= full->sessionCount) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    const MmcCdSession* session = &full->sessions[sessionIndex]; const MmcCdTrack* first = &full->tracks[session->firstTrackIndex];
    uint32_t lower = full->sessions[sessionIndex - 1].leadOutLba, found = first->startLba; bool seen = false, unlocated = false; uint8_t data[2368];
    for (uint32_t lba = first->startLba; lba > lower;) {
        lba--;
        if ((first->startLba - lba - 1) % 27 == 0) { snprintf(stats->statusMessage,sizeof(stats->statusMessage),"Locating CD session %u INDEX00 from Q, LBA %u...",session->number,lba); if (callback) callback(stats,context); }
        if (cd_cancelled(config)) { SetLastError(ERROR_CANCELLED); return false; }
        if (!cd_read(config,lba,1,data,true,stats)) { SetLastError(cd_cancelled(config) ? ERROR_CANCELLED : cd_error(config->drive)); return false; }
        const uint8_t* q = data + 2352; unsigned adr = q[0] & 15;
        if (!cd_q_crc(q)) { SetLastError(ERROR_CRC); return false; }
        if (adr == 2 || adr == 3) { unlocated = true; continue; }
        if (adr != 1 || q[6]) { SetLastError(ERROR_INVALID_DATA); return false; }
        /* 导入区的TNO为00，POINT载荷不是普通INDEX与绝对MSF；不能套轨道位置解析。 */
        if (!q[1] || q[1] == 0xAA) {
            if (!q[1] && q[2] != 0xA0 && q[2] != 0xA1 && q[2] != 0xA2) { int point = cd_bcd(q[2]); if (point < 1 || point > 99) { SetLastError(ERROR_NOT_SUPPORTED); return false; } }
            if (q[1] == 0xAA) { uint32_t absolute; if (!cd_msf(q + 7,&absolute) || absolute != lba + 150) { SetLastError(ERROR_INVALID_DATA); return false; } }
            if (!seen || unlocated) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
            *start = found; SetLastError(ERROR_SUCCESS); return true;
        }
        int number = cd_bcd(q[1]), index = cd_bcd(q[2]); uint32_t absolute, relative;
        if (number != first->number || index || !cd_msf(q + 7,&absolute) || !cd_msf(q + 3,&relative) || absolute != lba + 150 || relative != first->startLba - lba || ((q[0] >> 4) & 13) != (first->control & 13)) { SetLastError(ERROR_INVALID_DATA); return false; }
        found = lba; seen = true; unlocated = false;
    }
    /* 只有相邻的已定位Q能证明00起点；读失败或非位置Q都不能充当边界。 */
    SetLastError(ERROR_NOT_SUPPORTED); return false;
}
static bool cd_identifier_query(const ReadConfig* config, CdReadPlan* plan, bool final) {
    for (unsigned i = 0; i <= plan->toc.trackCount; i++) {
        if (i && (plan->toc.tracks[i - 1].control & 4)) continue;
        if (cd_cancelled(config)) { SetLastError(ERROR_CANCELLED); return false; }
        char value[14] = {0}; bool ok = i ? mmc_read_cd_isrc(config->drive,i,value) : mmc_read_cd_catalog(config->drive,value);
        if (!ok) {
            DWORD error = GetLastError();
            /* 编号是可选信息；坏响应不能写入CUE，仍可用实际读取的Q补全。真正设备错误保留。 */
            if (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_DATA) { if (error == ERROR_INVALID_DATA) plan->idWarning = true; continue; }
            return false;
        }
        char* queried = i ? plan->tracks[i - 1].queriedIsrc : plan->queriedCatalog;
        char* target = i ? plan->tracks[i - 1].isrc : plan->catalog;
        if (final && queried[0] && value[0] && strcmp(queried,value)) { SetLastError(ERROR_MEDIA_CHANGED); return false; }
        if (!cd_identifier_merge(target,value)) return false;
        if (!final && value[0]) strcpy(queried,value);
    }
    return true;
}
static bool cd_capture_identifier(CdReadPlan* plan, uint32_t lba, const uint8_t* q, unsigned adr) {
    char value[14] = {0}; bool empty = true;
    for (unsigned i = 1; i <= 8; i++) if (q[i]) empty = false;
    if (empty) return true;
    int frame = cd_bcd(q[9]); if (frame < 0 || frame >= 75 || (unsigned)frame != lba % 75) goto invalid;
    if (adr == 2) {
        if ((q[7] & 15) || q[8]) goto invalid;
        for (unsigned i = 0; i < 13; i++) { unsigned digit = i & 1 ? q[1 + i / 2] & 15 : q[1 + i / 2] >> 4; if (digit > 9) goto invalid; value[i] = (char)('0' + digit); }
        if (!strcmp(value,"0000000000000")) return true;
        return cd_identifier_merge(plan->catalog,value);
    }
    if ((q[4] & 3) || (q[8] & 15)) goto invalid;
    unsigned codes[5] = {q[1] >> 2,((q[1] & 3) << 4) | (q[2] >> 4),((q[2] & 15) << 2) | (q[3] >> 6),q[3] & 63,q[4] >> 2};
    for (unsigned i = 0; i < 5; i++) {
        unsigned code = codes[i]; bool digit = code <= 9, letter = code >= 0x11 && code <= 0x2A;
        if (i < 2 ? !letter : !digit && !letter) goto invalid;
        value[i] = (char)(digit ? '0' + code : 'A' + code - 0x11);
    }
    for (unsigned i = 0; i < 7; i++) { unsigned digit = i & 1 ? q[5 + i / 2] & 15 : q[5 + i / 2] >> 4; if (digit > 9) goto invalid; value[5 + i] = (char)('0' + digit); }
    /* ADR3没有轨号；等待后续位置Q，只有前后位置均属于同轨才采纳。 */
    if (!plan->pendingIsrc[0]) { strcpy(plan->pendingIsrc,value); plan->pendingTrack = plan->positioned ? plan->lastTrack : 0; }
    else if (strcmp(plan->pendingIsrc,value)) plan->pendingConflict = true;
    return true;
invalid:
    plan->idWarning = true; return true;
}
static bool cd_resolve_isrc(CdReadPlan* plan, unsigned track) {
    bool ok = true;
    if (plan->pendingIsrc[0] && plan->pendingTrack && plan->pendingTrack == track) {
        if (plan->toc.tracks[track - 1].control & 4) plan->idWarning = true;
        else if (plan->pendingConflict) { SetLastError(ERROR_INVALID_DATA); ok = false; }
        else ok = cd_identifier_merge(plan->tracks[track - 1].isrc,plan->pendingIsrc);
    }
    plan->pendingIsrc[0] = 0; plan->pendingTrack = 0; plan->pendingConflict = false; return ok;
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
        bool extended = i && !track->sessionFirst && (control & 4) && track->oldGapSeen && (((control ^ plan->toc.tracks[i - 1].control) & 13) || track->mode != plan->tracks[i - 1].mode);
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
    if (adr == 2 || adr == 3) return cd_capture_identifier(plan,lba,q,adr);
    if (adr != 1 || q[6]) goto invalid;
    int number = cd_bcd(q[1]), index = cd_bcd(q[2]); uint32_t absolute, relative;
    if (number < 1 || (unsigned)number > plan->toc.trackCount || index < 0 || !cd_msf(q + 7,&absolute) || !cd_msf(q + 3,&relative) || absolute != lba + 150) goto invalid;
    unsigned track = (unsigned)number - 1; CdReadTrack* current = &plan->tracks[track]; uint32_t start = plan->toc.tracks[track].startLba;
    if ((!plan->positioned && (unsigned)number != (plan->captureFirstTrack ? plan->captureFirstTrack : 1)) || (plan->positioned && ((unsigned)number < plan->lastTrack || (unsigned)number > plan->lastTrack + 1))) goto invalid;
    if ((!index && (lba >= start || relative != start - lba)) || (index && (lba < start || relative != lba - start))) goto invalid;
    if ((current->seen && (unsigned)index < current->lastIndex) || (!current->seen && index > 1)) goto invalid;
    if (!current->present[index]) {
        /* 非位置 Q 可能遮住索引边界；无法确定时拒绝猜测起点。INDEX 01 使用 TOC 的确定地址。 */
        if ((plan->positioned && plan->lastPosition + 1 != lba) || (!plan->positioned && lba != plan->captureStart)) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        current->indices[index] = lba; current->present[index] = true;
    }
    unsigned control = q[0] >> 4, expected = plan->toc.tracks[track].control;
    bool oldGap = false;
    if (!index && track && !current->sessionFirst && (expected & 4) && start - current->indices[0] >= 225 && lba < start - 150) {
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
    if (!cd_resolve_isrc(plan,(unsigned)number)) return false;
    current->seen = true; current->lastIndex = (unsigned)index; plan->lastTrack = (unsigned)number; plan->lastPosition = lba; plan->positioned = true; return true;
invalid:
    SetLastError(ERROR_INVALID_DATA); return false;
unsupported:
    SetLastError(ERROR_NOT_SUPPORTED); return false;
}
static bool cd_target(const wchar_t* input, wchar_t* full, bool replace) {
    full[0] = 0; DWORD length = GetFullPathNameW(input,MAX_PATH,full,NULL); if (!length || length >= MAX_PATH) { SetLastError(ERROR_INVALID_NAME); return false; }
    const wchar_t* dot = wcsrchr(full,L'.');
    if (!dot || (_wcsicmp(dot,L".cue") && _wcsicmp(dot,L".mds")) || !wcsncmp(full,L"\\\\.\\",4) || !wcsncmp(full,L"\\\\?\\",4)) { SetLastError(ERROR_INVALID_NAME); return false; }
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
static bool cd_publish(const ReadConfig* config, const CdReadPlan* plan, const wchar_t* destination, const wchar_t* binary, const wchar_t* cdTextFile, const MmcCdFullToc* full, const wchar_t (*binaries)[MAX_PATH]) {
    const wchar_t* name = wcsrchr(binary,L'\\'); if (!name) { SetLastError(ERROR_INVALID_NAME); return false; }
    char fileName[MAX_PATH * 3]; if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,name + 1,-1,fileName,sizeof(fileName),NULL,NULL)) return false;
    char* text = calloc(512 * 1024,1); if (!text) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; } size_t used = 0;
    bool ok = cd_append(text,&used,"\xEF\xBB\xBF");
    if (ok && plan->catalog[0]) ok = cd_append(text,&used,"CATALOG %s\r\n",plan->catalog);
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
    if (ok && !full) ok = cd_append(text,&used,"REM SESSION 1\r\nFILE \"%s\" BINARY\r\n",fileName);
    unsigned sessionIndex = 0; uint32_t origin = 0;
    for (unsigned i = 0; ok && i < plan->toc.trackCount; i++) {
        if (full && i == full->sessions[sessionIndex].firstTrackIndex) {
            const MmcCdSession* session = &full->sessions[sessionIndex]; origin = plan->sessionStarts[sessionIndex];
            const wchar_t* partName = wcsrchr(binaries[sessionIndex],L'\\');
            if (!partName || !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,partName + 1,-1,fileName,sizeof(fileName),NULL,NULL)) { ok = false; break; }
            ok = cd_append(text,&used,"REM SESSION %u\r\n",session->number);
            if (ok) ok = cd_append(text,&used,"FILE \"%s\" BINARY\r\n",fileName);
        }
        const CdReadTrack* t = &plan->tracks[i]; unsigned control = plan->toc.tracks[i].control;
        if (ok) ok = cd_append(text,&used,"  TRACK %02u %s\r\n",i + 1,t->mode == 1 ? "MODE1/2352" : t->mode == 2 ? "MODE2/2352" : "AUDIO");
        if (ok && plan->metadata) {
            const CueTrack* fields = &plan->metadata->tracks[i];
            ok = cd_append_text(text,&used,"TITLE",fields->title,true) && cd_append_text(text,&used,"PERFORMER",fields->performer,true) && cd_append_text(text,&used,"SONGWRITER",fields->songwriter,true);
            if (ok) ok = cd_append_text(text,&used,"COMPOSER",fields->composer,true) && cd_append_text(text,&used,"ARRANGER",fields->arranger,true) && cd_append_text(text,&used,"MESSAGE",fields->message,true);
        }
        if (ok && ((control & 11) || t->scms)) ok = cd_append(text,&used,"    FLAGS%s%s%s%s\r\n",!t->scms && (control & 2) ? " DCP" : "",control & 1 ? " PRE" : "",control & 8 ? " 4CH" : "",t->scms ? " SCMS" : "");
        if (ok && t->isrc[0]) ok = cd_append(text,&used,"    ISRC %s\r\n",t->isrc);
        for (unsigned j = 0; ok && j < 100; j++) if (t->present[j]) {
            uint32_t lba = t->indices[j] - origin;
            if (config->includeSessionPregaps && full && sessionIndex && i == full->sessions[sessionIndex].firstTrackIndex && !j) {
                /* 每会话自动暂停150帧由原版生成；BIN保留全部00，前150帧不作CUE引用。 */
                if (t->indices[1] - origin == 150) continue; lba = 150;
            }
            ok = cd_append(text,&used,"    INDEX %02u %02u:%02u:%02u\r\n",j,lba / 4500,lba / 75 % 60,lba % 75);
        }
        if (full && i + 1 == full->sessions[sessionIndex].firstTrackIndex + full->sessions[sessionIndex].trackCount) {
            uint32_t end = full->sessions[sessionIndex].leadOutLba - origin;
            if (ok) ok = cd_append(text,&used,"REM LEAD-OUT %02u:%02u:%02u\r\n",end / 4500,end / 75 % 60,end % 75);
            if (sessionIndex + 1 < full->sessionCount) sessionIndex++;
        }
    }
    uint32_t end = plan->toc.leadOutLba; if (ok && !full) ok = cd_append(text,&used,"REM LEAD-OUT %02u:%02u:%02u\r\n",end / 4500,end / 75 % 60,end % 75);
    wchar_t temporary[MAX_PATH] = L"", checked[MAX_PATH]; HANDLE file = INVALID_HANDLE_VALUE; static volatile LONG serial; bool created = false;
    for (unsigned i = 0; ok && i < 64; i++) {
        if (swprintf(temporary,MAX_PATH,L"%ls.partial.%lu.%lu",destination,GetCurrentProcessId(),(unsigned long)InterlockedIncrement(&serial)) < 0) { SetLastError(ERROR_FILENAME_EXCED_RANGE); ok = false; break; }
        file = CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL); if (file != INVALID_HANDLE_VALUE) { created = true; break; }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) { ok = false; break; }
    }
    ok = ok && created;
    if (ok) ok = cd_write(file,text,(DWORD)used) && FlushFileBuffers(file);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError(); if (file != INVALID_HANDLE_VALUE && !CloseHandle(file) && ok) { ok = false; error = GetLastError(); }
    if (ok && full) {
        CueSheet* parsed = calloc(1,sizeof(*parsed));
        if (!parsed) { ok = false; error = ERROR_NOT_ENOUGH_MEMORY; }
        else {
            ok = imgburn_cue_parse(temporary,parsed) && parsed->sessionCount == full->sessionCount && parsed->trackCount == (int)full->trackCount && parsed->totalSectors == full->leadOutLba;
            for (unsigned i = 0; ok && i < full->sessionCount; i++) if (parsed->sessionEndLba[i] != full->sessions[i].leadOutLba) ok = false;
            for (unsigned i = 0; ok && i < full->trackCount; i++) if (parsed->tracks[i].startLba != full->tracks[i].startLba || parsed->tracks[i].sectorSize != 2352) ok = false;
            free(parsed); if (!ok) error = ERROR_INVALID_DATA;
        }
    }
    if (ok && cd_cancelled(config)) { ok = false; error = ERROR_CANCELLED; }
    if (ok && !cd_target(destination,checked,config->replaceExisting)) { ok = false; error = GetLastError(); }
    if (ok && !MoveFileExW(temporary,destination,MOVEFILE_WRITE_THROUGH | (config->replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0))) { ok = false; error = GetLastError(); }
    if (!ok && created) DeleteFileW(temporary); free(text); if (!ok) SetLastError(error ? error : ERROR_WRITE_FAULT); return ok;
}

static bool cd_publish_mds(const ReadConfig* config, const wchar_t* destination, const wchar_t (*binaries)[MAX_PATH], unsigned partCount, uint32_t sectors, unsigned mode) {
    wchar_t temporary[MAX_PATH] = L"", checked[MAX_PATH]; static volatile LONG sequence; bool created = false, ok = false;
    for (unsigned i = 0; i < 64; i++) {
        if (swprintf(temporary,MAX_PATH,L"%ls.partial.%lu.%lu",destination,GetCurrentProcessId(),(unsigned long)InterlockedIncrement(&sequence)) < 0) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
        HANDLE reservation = CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if (reservation != INVALID_HANDLE_VALUE) { CloseHandle(reservation); created = true; break; }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (created) {
        const wchar_t* names[MAX_MDS_FILES]; for (unsigned i = 0; i < partCount; i++) names[i] = wcsrchr(binaries[i],L'\\') + 1;
        ok = imgburn_mds_create_cd_raw_read_files(temporary,names,partCount,sectors,mode == 1 ? 0xAA : 0xAB);
        if (ok && cd_cancelled(config)) { SetLastError(ERROR_CANCELLED); ok = false; }
        if (ok) ok = cd_target(destination,checked,config->replaceExisting);
        if (ok) ok = MoveFileExW(temporary,destination,MOVEFILE_WRITE_THROUGH | (config->replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0)) != FALSE;
    }
    DWORD error = GetLastError(); if (!ok && created) DeleteFileW(temporary); if (!ok) SetLastError(error ? error : ERROR_WRITE_FAULT); return ok;
}

typedef struct { HANDLE file; bool created; } CdReadOutput;

bool pipeline_read_cd(const ReadConfig* config, PipelineProgressCallback callback, void* context) {
    PipelineStats stats = {0}; if (!config || !config->drive || !config->imagePath || !config->imagePath[0] || config->retryCount > 20) return cd_fail(&stats,ERROR_INVALID_PARAMETER,callback,context);
    if (cd_cancelled(config)) return cd_fail(&stats,ERROR_CANCELLED,callback,context);
    wchar_t destination[MAX_PATH] = L"", textPath[MAX_PATH] = L"", parent[MAX_PATH]; if (!cd_target(config->imagePath,destination,config->replaceExisting)) return cd_fail(&stats,GetLastError(),callback,context);
    bool mds = !_wcsicmp(wcsrchr(destination,L'.'),L".mds");
    if (config->splitSectors && !mds) return cd_fail(&stats,ERROR_NOT_SUPPORTED,callback,context);
    if (mds && (config->readCdText || config->includeSessionPregaps)) return cd_fail(&stats,ERROR_NOT_SUPPORTED,callback,context);
    stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Inspecting raw CD layout..."); if (callback) callback(&stats,context);
    OpticalDrive* drive = config->drive; if (!mmc_test_unit_ready(drive)) return cd_fail(&stats,cd_error(drive),callback,context);
    MmcProfile profile = mmc_get_current_profile(drive); if (profile != MMC_PROFILE_CD_ROM && profile != MMC_PROFILE_CD_R && profile != MMC_PROFILE_CD_RW) return cd_fail(&stats,ERROR_NOT_SUPPORTED,callback,context);
    CdReadPlan* plan = calloc(1,sizeof(*plan)); MmcCdFullToc* full = NULL; uint8_t* data = malloc(27 * 2368); HANDLE textOutput = INVALID_HANDLE_VALUE;
    wchar_t (*binaries)[MAX_PATH] = calloc(MAX_MDS_FILES,sizeof(*binaries)); CdReadOutput* outputs = calloc(MAX_MDS_FILES,sizeof(*outputs)); unsigned partCount = 1; bool textCreated = false, success = false, idWarning = false; DWORD error = ERROR_SUCCESS;
    if (!plan || !data || !binaries || !outputs) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if (!mmc_read_cd_toc(drive,&plan->toc)) {
        error = cd_error(drive); if (error != ERROR_NOT_SUPPORTED || mds) goto done;
        full = calloc(2,sizeof(*full)); if (!full) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
        if (!mmc_read_cd_full_toc(drive,full)) { error = cd_error(drive); goto done; }
        if (full->sessionCount < 2 || full->tracks[0].startLba) { error = ERROR_NOT_SUPPORTED; goto done; }
        /* 默认只保存首INDEX01至A2；显式保留会话00时由实际Q确定起点。 */
        for (unsigned i = 1; i < full->sessionCount; i++) {
            uint32_t first = full->tracks[full->sessions[i].firstTrackIndex].startLba, previous = full->sessions[i - 1].leadOutLba;
            if (!config->includeSessionPregaps && first - previous != (i == 1 ? 11400u : 6900u)) { error = ERROR_NOT_SUPPORTED; goto done; }
        }
        plan->toc.trackCount = full->trackCount; plan->toc.leadOutLba = full->leadOutLba; memcpy(plan->toc.tracks,full->tracks,full->trackCount * sizeof(*full->tracks)); partCount = full->sessionCount;
    }
    if (plan->toc.leadOutLba > 100 * 60 * 75 - 150 || (mds && (plan->toc.trackCount != 1 || plan->toc.tracks[0].startLba || plan->toc.tracks[0].control != 4))) { error = ERROR_NOT_SUPPORTED; goto done; }
    if (config->includeSessionPregaps && !full) { error = ERROR_NOT_SUPPORTED; goto done; }
    if (config->readCdText && (!cd_read_text(drive,&plan->cdText) || !cd_text_metadata(plan))) { error = GetLastError(); goto done; }
    if (!cd_identifier_query(config,plan,false)) { error = GetLastError(); goto done; }
    if (mds && plan->catalog[0]) { error = ERROR_NOT_SUPPORTED; goto done; }
    if (cd_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
    if (full) for (unsigned i = 0; i < partCount; i++) {
        const MmcCdSession* session = &full->sessions[i]; uint32_t first = full->tracks[session->firstTrackIndex].startLba;
        plan->sessionStarts[i] = first; plan->tracks[session->firstTrackIndex].sessionFirst = true;
        if (config->includeSessionPregaps && i) {
            snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Locating CD session %u INDEX00 from Q...",session->number); if (callback) callback(&stats,context);
            if (!cd_find_session_index00(config,full,i,&stats,&plan->sessionStarts[i],callback,context)) { error = GetLastError(); goto done; }
            uint32_t expected = full->sessions[i - 1].leadOutLba + (i == 1 ? 6750u : 2250u) + 4500;
            if (plan->sessionStarts[i] != expected || first - plan->sessionStarts[i] < 150) { error = ERROR_NOT_SUPPORTED; goto done; }
        }
        stats.totalSectors += session->leadOutLba - plan->sessionStarts[i];
    }
    else stats.totalSectors = plan->toc.leadOutLba;
    if (mds && config->splitSectors) {
        uint64_t count = ((uint64_t)stats.totalSectors + config->splitSectors - 1) / config->splitSectors;
        if (count > MAX_MDS_FILES) { error = ERROR_NOT_SUPPORTED; goto done; } partCount = (unsigned)count;
    }
    for (unsigned i = 0; i < plan->toc.trackCount; i++) { plan->tracks[i].indices[1] = plan->toc.tracks[i].startLba; plan->tracks[i].present[1] = true; plan->tracks[i].modeKnown = !(plan->toc.tracks[i].control & 4); }
    wcscpy(parent,destination); wchar_t* slash = wcsrchr(parent,L'\\'); if (!slash) { error = ERROR_INVALID_NAME; goto done; } slash[1] = 0;
    ULARGE_INTEGER available; if (!GetDiskFreeSpaceExW(parent,&available,NULL,NULL)) { error = GetLastError(); goto done; }
    if (available.QuadPart < (uint64_t)stats.totalSectors * 2352 + 512 * 1024) { error = ERROR_DISK_FULL; goto done; }
    wcscpy(parent,destination); wchar_t* dot = wcsrchr(parent,L'.'); if (!dot) { error = ERROR_INVALID_NAME; goto done; } *dot = 0;
    /* 整组预留新BIN；任意重名换下一组，旧描述始终引用旧数据。 */
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        bool collision = false;
        for (unsigned i = 0; i < partCount; i++) {
            int chars;
            if (full) chars = attempt ? swprintf(binaries[i],MAX_PATH,L"%ls.%03u.session%02u.bin",parent,attempt,i + 1) : swprintf(binaries[i],MAX_PATH,L"%ls.session%02u.bin",parent,i + 1);
            else if (partCount > 1) chars = attempt ? swprintf(binaries[i],MAX_PATH,L"%ls.%03u.i%02u",parent,attempt,i) : swprintf(binaries[i],MAX_PATH,L"%ls.i%02u",parent,i);
            else chars = attempt ? swprintf(binaries[i],MAX_PATH,L"%ls.%03u.bin",parent,attempt) : swprintf(binaries[i],MAX_PATH,L"%ls.bin",parent);
            if (chars < 0) { error = ERROR_FILENAME_EXCED_RANGE; goto done; }
            outputs[i].file = CreateFileW(binaries[i],GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
            if (outputs[i].file != INVALID_HANDLE_VALUE) { outputs[i].created = true; continue; }
            error = GetLastError(); if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) goto done;
            collision = true; break;
        }
        if (!collision) break;
        for (unsigned i = 0; i < partCount; i++) if (outputs[i].created) { CloseHandle(outputs[i].file); outputs[i].file = INVALID_HANDLE_VALUE; DeleteFileW(binaries[i]); outputs[i].created = false; }
    }
    if (!outputs[partCount - 1].created) goto done;
    stats.state = PIPELINE_STATE_READING; ULONGLONG started = GetTickCount64(); snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Reading raw CD and Q indexes..."); if (callback) callback(&stats,context);
    for (unsigned part = 0; part < partCount; part++) {
        uint32_t first = full ? plan->sessionStarts[part] : partCount > 1 ? part * config->splitSectors : 0;
        uint32_t end = full ? full->sessions[part].leadOutLba : plan->toc.leadOutLba;
        if (!full && partCount > 1 && end - first > config->splitSectors) end = first + config->splitSectors;
        if (full) { plan->positioned = false; plan->captureFirstTrack = full->sessions[part].firstTrack; plan->captureStart = first; if (!cd_resolve_isrc(plan,0)) { error = GetLastError(); goto done; } }
        for (uint32_t lba = first; lba < end;) {
            if (full) { snprintf(stats.statusMessage,sizeof(stats.statusMessage),config->includeSessionPregaps && part ? "Reading CD session %u/%u, LBA %u (INDEX00 to lead-out)..." : "Reading CD session %u/%u, LBA %u (INDEX01 to lead-out)...",part + 1,partCount,lba); if (callback) callback(&stats,context); }
            if (cd_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
            uint16_t count = (uint16_t)(end - lba < 27 ? end - lba : 27);
            if (!cd_read(config,lba,count,data,true,&stats)) { error = cd_cancelled(config) ? ERROR_CANCELLED : cd_error(drive); goto done; }
            for (unsigned i = 0; i < count; i++) { if (!cd_capture(plan,lba + i,data + (size_t)i * 2368)) { error = GetLastError(); goto done; } memmove(data + (size_t)i * 2352,data + (size_t)i * 2368,2352); }
            if (!cd_write(outputs[part].file,data,(DWORD)count * 2352)) { error = GetLastError(); goto done; }
            lba += count; stats.currentLba += count; stats.progressPercent = (int)((uint64_t)stats.currentLba * 99 / stats.totalSectors); ULONGLONG elapsed = GetTickCount64() - started;
            stats.elapsedTimeSec = (uint32_t)(elapsed / 1000); if (elapsed) { stats.currentSpeedKBps = (double)stats.currentLba * 2296.875 / elapsed; stats.speedMultiplier = stats.currentSpeedKBps / 150.0; stats.remainingTimeSec = (uint32_t)((stats.totalSectors - stats.currentLba) * 2.296875 / stats.currentSpeedKBps); } if (callback) callback(&stats,context);
        }
    }
    if (!cd_capture_complete(plan)) { error = GetLastError(); goto done; }
    if (mds) {
        bool extra = (plan->tracks[0].mode != 1 && plan->tracks[0].mode != 2) || plan->tracks[0].scms || plan->catalog[0];
        for (unsigned i = 0; i < 100; i++) if (i != 1 && plan->tracks[0].present[i]) extra = true;
        if (extra) { error = ERROR_NOT_SUPPORTED; goto done; }
    }
    if (cd_cancelled(config)) { error = ERROR_CANCELLED; goto done; }
    if (!mmc_test_unit_ready(drive)) { error = cd_error(drive); goto done; }
    bool changed;
    if (full) {
        if (!mmc_read_cd_full_toc(drive,full + 1)) { error = cd_error(drive); goto done; }
        changed = full->sessionCount != full[1].sessionCount || full->trackCount != full[1].trackCount || full->leadOutLba != full[1].leadOutLba || memcmp(full->sessions,full[1].sessions,full->sessionCount * sizeof(*full->sessions)) || memcmp(full->tracks,full[1].tracks,full->trackCount * sizeof(*full->tracks));
    } else {
        MmcCdToc finalToc; if (!mmc_read_cd_toc(drive,&finalToc)) { error = cd_error(drive); goto done; }
        changed = finalToc.trackCount != plan->toc.trackCount || finalToc.leadOutLba != plan->toc.leadOutLba || memcmp(finalToc.tracks,plan->toc.tracks,finalToc.trackCount * sizeof(MmcCdTrack));
    }
    if (mmc_get_current_profile(drive) != profile || changed) { error = ERROR_MEDIA_CHANGED; goto done; }
    if (!cd_identifier_query(config,plan,true)) { error = GetLastError(); goto done; }
    if (mds && plan->catalog[0]) { error = ERROR_NOT_SUPPORTED; goto done; }
    if (config->readCdText) {
        uint8_t* finalText = NULL; uint32_t finalBytes = 0;
        if (!mmc_read_cd_text(drive,&finalText,&finalBytes)) { error = GetLastError(); goto done; }
        bool textChanged = finalBytes != plan->cdText.bytes || (finalBytes && memcmp(finalText,plan->cdText.packs,finalBytes)); free(finalText);
        if (textChanged) { error = ERROR_MEDIA_CHANGED; goto done; }
    }
    for (unsigned i = 0; i < partCount; i++) {
        if (!FlushFileBuffers(outputs[i].file)) { error = GetLastError(); goto done; }
        if (!CloseHandle(outputs[i].file)) { error = GetLastError(); outputs[i].file = INVALID_HANDLE_VALUE; goto done; } outputs[i].file = INVALID_HANDLE_VALUE;
    }
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
    if (!(mds ? cd_publish_mds(config,destination,binaries,partCount,plan->toc.leadOutLba,plan->tracks[0].mode) : cd_publish(config,plan,destination,binaries[0],textPath,full,binaries))) { error = GetLastError(); goto done; } idWarning = plan->idWarning; success = true;
done:
    for (unsigned i = 0; outputs && i < MAX_MDS_FILES; i++) { if (outputs[i].file && outputs[i].file != INVALID_HANDLE_VALUE) CloseHandle(outputs[i].file); if (!success && outputs[i].created) DeleteFileW(binaries[i]); }
    if (textOutput != INVALID_HANDLE_VALUE) CloseHandle(textOutput); if (!success && textCreated) DeleteFileW(textPath);
    if (plan) { imgburn_cdtext_free(&plan->cdText); free(plan->metadata); } free(data); free(outputs); free(binaries); free(full); free(plan);
    if (!success) return cd_fail(&stats,error,callback,context);
    stats.state = PIPELINE_STATE_COMPLETED; stats.progressPercent = 100; stats.remainingTimeSec = 0;
    if (!mds && partCount > 1) snprintf(stats.statusMessage,sizeof(stats.statusMessage),config->includeSessionPregaps ? "CD sessions and INDEX00 read; each session BIN retains 150 unreferenced pause frames.%s" : "CD session data read; gaps and session-first INDEX00 excluded.%s",idWarning ? " Invalid MCN/ISRC ignored." : "");
    else snprintf(stats.statusMessage,sizeof(stats.statusMessage),idWarning ? "Raw CD image read; invalid MCN/ISRC samples ignored." : "Raw CD image read successfully.");
    if (callback) callback(&stats,context); SetLastError(ERROR_SUCCESS); return true;
}

typedef struct {
    HANDLE file;
    uint32_t start, end, payloadStart, payloadEnd, extendedUntil;
    uint64_t offset, sourceBytes;
    TrackDataMode mode, previousMode;
    bool audio, swap, saoData;
} CdVerifySpan;
typedef struct { wchar_t path[MAX_PATH]; CueFileType type; AudioPcmSource source; } CdVerifyAudio;
typedef struct { HANDLE file; uint64_t offset, start, bytes; } CdVerifyPart;
typedef struct { CueSheet sheet; CdVerifySpan spans[99]; bool saoData; HANDLE cancelEvent; CdVerifyAudio audio[99]; unsigned audioCount; DWORD prepareError; CdVerifyPart* parts; unsigned partCount; } CdVerifyImage;

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
    if (!sheet->sessionCount || sheet->sessionCount > 99) { SetLastError(ERROR_INVALID_DATA); return false; }
    if (sheet->sessionCount > 1) for (unsigned i = 0; i < sheet->sessionCount; i++) if ((i && sheet->sessionLeadIn[i] != 4500) || sheet->sessionLeadOut[i] != (i ? 2250u : 6750u)) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
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
        bool newSession = i && track->sessionNumber != sheet->tracks[i - 1].sessionNumber;
        if (!track->sessionNumber || track->sessionNumber > sheet->sessionCount || (!i && track->sessionNumber != 1) || (newSession && track->sessionNumber != sheet->tracks[i - 1].sessionNumber + 1)) { SetLastError(ERROR_INVALID_DATA); return false; }
        if (track->sourceByteOffset < prefix || span->end <= span->start || (i ? newSession ? span->start < image->spans[i - 1].end || image->spans[i - 1].end != sheet->sessionEndLba[sheet->tracks[i - 1].sessionNumber - 1] : span->start != image->spans[i - 1].end : span->start != 0)) { SetLastError(ERROR_INVALID_DATA); return false; }
        span->offset = track->sourceByteOffset - prefix; span->audio = track->mode == TRACK_MODE_AUDIO; span->swap = track->sourceType == CUE_FILE_MOTOROLA;
        span->mode = track->mode; span->previousMode = i ? sheet->tracks[i - 1].mode : track->mode; span->saoData = image->saoData;
        if (image->saoData && i && !newSession && !span->audio && span->mode != span->previousMode) {
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
    if (sheet->tracks[sheet->trackCount - 1].sessionNumber != sheet->sessionCount || image->spans[sheet->trackCount - 1].end != sheet->totalSectors || sheet->sessionEndLba[sheet->sessionCount - 1] != sheet->totalSectors) { SetLastError(ERROR_INVALID_DATA); return false; } return true;
}

static bool cd_verify_mds(CdVerifyImage* image, const wchar_t* path) {
    MdsSheet* sheet = calloc(1,sizeof(*sheet)); DWORD error = ERROR_SUCCESS;
    if (!sheet) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    if (!imgburn_mds_parse(path,sheet)) { error = ERROR_INVALID_DATA; goto done; }
    const MdsTrack* track = &sheet->tracks[0];
    if (sheet->mediumType || sheet->sessionCount != 1 || sheet->trackCount != 1 || track->sessionNumber != 1 || track->trackNumber != 1 || track->startLba || track->sectorSize != 2352 || (track->mode != 0xAA && track->mode != 0xAB) || track->subChannel || (track->control & 15) != 4 || track->sectorCount != sheet->totalSectors) { error = ERROR_NOT_SUPPORTED; goto done; }
    if (sheet->sessions[0].totalTracks != 1 || sheet->sessions[0].leadInTracks != 3 || sheet->sessions[0].sessionStartLba != -150 || track->pregapSectors != 150) { error = ERROR_NOT_SUPPORTED; goto done; }
    image->partCount = track->fileCount; image->parts = calloc(image->partCount,sizeof(*image->parts));
    if (!image->parts) { image->partCount = 0; error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    uint64_t remaining = (uint64_t)track->sectorCount * 2352, logical = 0;
    for (unsigned i = 0; i < image->partCount; i++) {
        CdVerifyPart* part = &image->parts[i]; const MdsDataFile* metadata = &sheet->files[track->firstFile + i];
        part->file = CreateFileW(metadata->path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if (part->file == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
        LARGE_INTEGER length;
        if (!GetFileSizeEx(part->file,&length)) { error = GetLastError(); goto done; }
        if (length.QuadPart < 0 || (uint64_t)length.QuadPart != metadata->bytes) { error = ERROR_INVALID_DATA; goto done; }
        part->offset = i ? 0 : track->startByteOffset; part->start = logical;
        if (part->offset > (uint64_t)length.QuadPart) { error = ERROR_INVALID_DATA; goto done; }
        uint64_t available = (uint64_t)length.QuadPart - part->offset;
        if (i + 1 < image->partCount && available % 2352) { error = ERROR_NOT_SUPPORTED; goto done; }
        part->bytes = available < remaining ? available : remaining; remaining -= part->bytes; logical += part->bytes;
    }
    if (remaining) { error = ERROR_HANDLE_EOF; goto done; }
    image->sheet.sessionCount = 1; image->sheet.trackCount = 1; image->sheet.totalSectors = track->sectorCount; image->sheet.sessionEndLba[0] = track->sectorCount;
    image->sheet.tracks[0].sessionNumber = 1; image->sheet.tracks[0].trackNumber = 1; image->sheet.tracks[0].sectorCount = track->sectorCount;
    image->spans[0].end = image->spans[0].payloadEnd = track->sectorCount; image->spans[0].mode = track->mode == 0xAA ? TRACK_MODE_MODE1_2352 : TRACK_MODE_MODE2_2352;
done:
    free(sheet); SetLastError(error); return error == ERROR_SUCCESS;
}

static bool cd_verify_mds_read(const CdVerifyImage* image, uint32_t lba, uint16_t count, uint8_t* data) {
    uint64_t position = (uint64_t)lba * 2352; DWORD remaining = (DWORD)count * 2352;
    for (unsigned i = 0; i < image->partCount && remaining; i++) {
        const CdVerifyPart* part = &image->parts[i]; if (position < part->start || position >= part->start + part->bytes) continue;
        uint64_t inside = position - part->start, available = part->bytes - inside; DWORD wanted = available < remaining ? (DWORD)available : remaining;
        LARGE_INTEGER offset; offset.QuadPart = (LONGLONG)(part->offset + inside); if (!SetFilePointerEx(part->file,offset,NULL,FILE_BEGIN)) return false;
        DWORD done = 0;
        while (done < wanted) { DWORD actual = 0; if (!ReadFile(part->file,data + done,wanted - done,&actual,NULL)) return false; if (!actual || actual > wanted - done) { SetLastError(ERROR_HANDLE_EOF); return false; } done += actual; }
        data += wanted; position += wanted; remaining -= wanted;
    }
    if (remaining) { SetLastError(ERROR_HANDLE_EOF); return false; } return true;
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

static bool cd_full_toc_equal(const MmcCdFullToc* first, const MmcCdFullToc* second) {
    return first->sessionCount == second->sessionCount && first->trackCount == second->trackCount && first->leadOutLba == second->leadOutLba && !memcmp(first->sessions,second->sessions,first->sessionCount * sizeof(*first->sessions)) && !memcmp(first->tracks,second->tracks,first->trackCount * sizeof(*first->tracks));
}
static bool cd_verify_sessions(const VerifyConfig* config, MmcProfile profile, CdVerifyImage* image, PipelineProgressCallback callback, void* context) {
    PipelineStats stats = {0}; MmcCdFullToc* toc = calloc(2,sizeof(*toc)); uint8_t* data = malloc(27 * 2352); uint8_t* reference = image ? malloc(27 * 2352) : NULL; DWORD error = ERROR_SUCCESS; bool success = false;
    uint32_t starts[99] = {0};
    if (!toc || !data || (image && !reference)) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if (!mmc_read_cd_full_toc(config->drive,toc)) { error = cd_error(config->drive); goto done; }
    if (toc->sessionCount < 2) { error = ERROR_MEDIA_CHANGED; goto done; }
    if (image) {
        const CueSheet* sheet = &image->sheet;
        if (sheet->sessionCount != toc->sessionCount || sheet->trackCount != (int)toc->trackCount || sheet->totalSectors != toc->leadOutLba) { error = ERROR_INVALID_DATA; goto done; }
        for (unsigned i = 0; i < toc->sessionCount; i++) {
            const MmcCdSession* session = &toc->sessions[i];
            if (sheet->sessionEndLba[i] != session->leadOutLba || image->spans[session->firstTrackIndex + session->trackCount - 1].end != session->leadOutLba) { error = ERROR_INVALID_DATA; goto done; }
            for (unsigned j = session->firstTrackIndex; j < session->firstTrackIndex + session->trackCount; j++) {
                const CueTrack* track = &sheet->tracks[j]; const CdVerifySpan* span = &image->spans[j];
                if (track->sessionNumber != session->number || track->trackNumber != toc->tracks[j].number || track->startLba != toc->tracks[j].startLba || span->audio == ((toc->tracks[j].control & 4) != 0) || span->start > track->startLba || span->end <= track->startLba) { error = ERROR_INVALID_DATA; goto done; }
            }
        }
    }
    ReadConfig reader = {0}; reader.drive = config->drive; reader.cancelEvent = config->cancelEvent; reader.retryCount = config->retryCount;
    /* Full TOC 只有 INDEX01；首00必须用实际 Q 确认，探测帧不计入完成进度。 */
    for (unsigned i = 0; i < toc->sessionCount; i++) {
        starts[i] = toc->tracks[toc->sessions[i].firstTrackIndex].startLba;
        if (config->includeSessionPregaps && i) {
            stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Locating session %u INDEX00 using Q...",i + 1); if (callback) callback(&stats,context);
            if (!cd_find_session_index00(&reader,toc,i,&stats,&starts[i],callback,context)) { error = GetLastError(); goto done; }
            if (image && (starts[i] > UINT32_MAX - 150 || starts[i] + 150 != image->spans[toc->sessions[i].firstTrackIndex].start)) { error = ERROR_INVALID_DATA; goto done; }
        }
        stats.totalSectors += toc->sessions[i].leadOutLba - starts[i];
    }
    ULONGLONG started = GetTickCount64(); stats.state = PIPELINE_STATE_VERIFYING;
    for (unsigned i = 0; i < toc->sessionCount; i++) {
        const MmcCdSession* session = &toc->sessions[i]; uint32_t start = starts[i]; unsigned track = session->firstTrackIndex;
        for (uint32_t lba = start; lba < session->leadOutLba;) {
            snprintf(stats.statusMessage,sizeof(stats.statusMessage),"%s CD session %u/%u, LBA %u (%s to lead-out)...",image ? "Comparing" : "Checking",session->number,toc->sessionCount,lba,config->includeSessionPregaps && i ? "Q-confirmed INDEX00" : "INDEX01"); if (callback) callback(&stats,context);
            if (cd_cancelled(&reader)) { error = ERROR_CANCELLED; goto done; }
            uint32_t end = session->leadOutLba; bool referenced = image != NULL;
            if (image) {
                while (track + 1 < session->firstTrackIndex + session->trackCount && lba >= image->spans[track].end) track++;
                const CdVerifySpan* span = &image->spans[track]; if (lba >= span->end) { error = ERROR_INVALID_DATA; goto done; }
                /* 自动150帧不由普通CUE引用；只检查可读性，不把任意源前缀当参考内容。 */
                if (lba < span->start) { referenced = false; end = span->start; }
                else { if (span->end < end) end = span->end; if (lba < span->extendedUntil && span->extendedUntil < end) end = span->extendedUntil; }
            }
            uint16_t count = (uint16_t)(end - lba < 27 ? end - lba : 27);
            if (!cd_read(&reader,lba,count,data,false,&stats)) { error = cd_cancelled(&reader) ? ERROR_CANCELLED : cd_error(config->drive); goto done; }
            if (referenced && (cd_verify_mode(&image->spans[track],lba) != TRACK_MODE_AUDIO || config->compareAudio)) {
                if (!cd_verify_read(&image->spans[track],lba,count,reference)) { error = GetLastError(); goto done; }
                for (unsigned j = 0; j < count; j++) if (memcmp(data + (size_t)j * 2352,reference + (size_t)j * 2352,2352)) stats.miscomparesCount++;
                stats.comparedSectors += count;
            } else stats.readableOnlySectors += count;
            lba += count; stats.currentLba = stats.readableOnlySectors + stats.comparedSectors; stats.progressPercent = (int)((uint64_t)stats.currentLba * 99 / stats.totalSectors);
            ULONGLONG elapsed = GetTickCount64() - started; stats.elapsedTimeSec = (uint32_t)(elapsed / 1000);
            if (elapsed) { stats.currentSpeedKBps = (double)stats.currentLba * 2296.875 / elapsed; stats.speedMultiplier = stats.currentSpeedKBps / 150; stats.remainingTimeSec = (uint32_t)((stats.totalSectors - stats.currentLba) * 2.296875 / stats.currentSpeedKBps); }
            if (callback) callback(&stats,context);
        }
    }
    if (cd_cancelled(&reader)) { error = ERROR_CANCELLED; goto done; }
    if (!mmc_test_unit_ready(config->drive) || !mmc_read_cd_full_toc(config->drive,toc + 1)) { error = cd_error(config->drive); goto done; }
    if (mmc_get_current_profile(config->drive) != profile || !cd_full_toc_equal(toc,toc + 1)) { error = ERROR_MEDIA_CHANGED; goto done; }
    if (stats.miscomparesCount) { error = ERROR_CRC; goto done; } success = true;
done:
    free(toc); free(data); free(reference); stats.state = success ? PIPELINE_STATE_COMPLETED : error == ERROR_CANCELLED ? PIPELINE_STATE_ABORTED : PIPELINE_STATE_FAILED;
    if (success) { stats.progressPercent = 100; stats.remainingTimeSec = 0; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"CD sessions verified: %u compared, %u readability-only sectors%s.",stats.comparedSectors,stats.readableOnlySectors,config->includeSessionPregaps ? "; automatic pauses are readability-only" : "; session gaps excluded"); }
    else snprintf(stats.statusMessage,sizeof(stats.statusMessage),"CD session Verify did not complete (error %lu).",error);
    if (callback) callback(&stats,context); SetLastError(success ? ERROR_SUCCESS : error); return success;
}

bool pipeline_verify_cd(const VerifyConfig* config, PipelineProgressCallback callback, void* context) {
    PipelineStats stats = {0}; DWORD error = ERROR_SUCCESS; bool success = false, sessionVerify = false; HANDLE descriptor = INVALID_HANDLE_VALUE; CdVerifyImage* image = NULL; uint8_t* data = NULL; uint8_t* reference = NULL;
    if (!config || !config->drive || config->retryCount > 20) { error = ERROR_INVALID_PARAMETER; goto verify_done; }
    ReadConfig reader = {0}; reader.drive = config->drive; reader.cancelEvent = config->cancelEvent; reader.retryCount = config->retryCount;
    if (cd_cancelled(&reader)) { error = ERROR_CANCELLED; goto verify_done; }
    bool compare = config->imagePath && config->imagePath[0];
    if (compare) {
        const wchar_t* extension = wcsrchr(config->imagePath,L'.'); if (!extension || (_wcsicmp(extension,L".cue") && _wcsicmp(extension,L".mds"))) { error = ERROR_NOT_SUPPORTED; goto verify_done; }
        descriptor = CreateFileW(config->imagePath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL); if (descriptor == INVALID_HANDLE_VALUE) { error = GetLastError(); goto verify_done; }
        image = calloc(1,sizeof(*image)); if (!image) { error = ERROR_NOT_ENOUGH_MEMORY; goto verify_done; } image->saoData = config->saoData; image->cancelEvent = config->cancelEvent;
        stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Preparing and checking CD reference audio..."); if (callback) callback(&stats,context);
        if (cd_cancelled(&reader)) { error = ERROR_CANCELLED; goto verify_done; }
        SetLastError(ERROR_SUCCESS);
        if (!_wcsicmp(extension,L".mds")) { if (!cd_verify_mds(image,config->imagePath)) { error = GetLastError(); goto verify_done; } }
        else {
            if (!imgburn_cue_parse_ex(config->imagePath,&image->sheet,cd_verify_resolve,image)) { error = image->prepareError ? image->prepareError : GetLastError() == ERROR_NOT_SUPPORTED ? ERROR_NOT_SUPPORTED : ERROR_INVALID_DATA; goto verify_done; }
            if (!cd_verify_plan(image)) { error = GetLastError(); goto verify_done; }
        }
        if (config->includeSessionPregaps && (image->parts || image->sheet.sessionCount < 2)) { error = ERROR_NOT_SUPPORTED; goto verify_done; }
    }
    if (cd_cancelled(&reader)) { error = ERROR_CANCELLED; goto verify_done; }
    OpticalDrive* drive = config->drive; if (!mmc_test_unit_ready(drive)) { error = cd_error(drive); goto verify_done; }
    MmcProfile profile = mmc_get_current_profile(drive); if (profile != MMC_PROFILE_CD_ROM && profile != MMC_PROFILE_CD_R && profile != MMC_PROFILE_CD_RW) { error = ERROR_NOT_SUPPORTED; goto verify_done; }
    if (image && image->sheet.sessionCount > 1) { sessionVerify = true; success = cd_verify_sessions(config,profile,image,callback,context); error = GetLastError(); goto verify_done; }
    MmcCdToc discToc; if (!mmc_read_cd_toc(drive,&discToc)) {
        error = cd_error(drive); if (!compare && error == ERROR_NOT_SUPPORTED) return cd_verify_sessions(config,profile,NULL,callback,context); goto verify_done;
    } stats.totalSectors = discToc.leadOutLba;
    if (config->includeSessionPregaps) { error = ERROR_NOT_SUPPORTED; goto verify_done; }
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
            if (!(image->parts ? cd_verify_mds_read(image,lba,count,reference) : cd_verify_read(&image->spans[track],lba,count,reference))) { error = GetLastError(); goto verify_done; }
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
        for (unsigned i = 0; i < image->partCount; i++) if (image->parts[i].file && image->parts[i].file != INVALID_HANDLE_VALUE) CloseHandle(image->parts[i].file);
        free(image->parts);
    }
    if (descriptor != INVALID_HANDLE_VALUE) CloseHandle(descriptor); free(image); free(data); free(reference);
    if (sessionVerify) { SetLastError(error); return success; }
    stats.state = success ? PIPELINE_STATE_COMPLETED : error == ERROR_CANCELLED ? PIPELINE_STATE_ABORTED : PIPELINE_STATE_FAILED;
    if (success) { stats.progressPercent = 100; stats.remainingTimeSec = 0; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"CD verify passed: %u compared, %u readability-only sectors.",stats.comparedSectors,stats.readableOnlySectors); }
    else { if (!error) error = ERROR_READ_FAULT; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"CD verify did not complete (error %lu).",error); }
    if (callback) callback(&stats,context); if (!success) SetLastError(error); return success;
}
