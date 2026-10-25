#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_pipeline.h"
#include "imgburn_iso_session.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef struct { MmcDiscInfo disc; MmcTrackInfo track; MmcCdFullToc toc; MmcWriteParameters parameters; } CdSessionSnapshot;

static bool cd_session_snapshot(OpticalDrive* drive, CdSessionSnapshot* snapshot) {
    memset(snapshot,0,sizeof(*snapshot));
    if (!mmc_read_disc_information(drive,&snapshot->disc)) return false;
    if (snapshot->disc.currentProfile != MMC_PROFILE_CD_R || snapshot->disc.isErasable || (snapshot->disc.discStatus != DISC_STATUS_EMPTY && snapshot->disc.discStatus != DISC_STATUS_APPENDABLE)) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (!mmc_read_track_information(drive,255,false,&snapshot->track) || !mmc_get_write_parameters(drive,&snapshot->parameters)) return false;
    if (snapshot->disc.discStatus == DISC_STATUS_APPENDABLE) return mmc_read_cd_full_toc(drive,&snapshot->toc);
    if (snapshot->disc.discStatus != DISC_STATUS_EMPTY) { SetLastError(ERROR_NOT_SUPPORTED); return false; } return true;
}

static bool cd_session_plan(const CdSessionSnapshot* snapshot, uint32_t sectors, bool allowBlank, MmcCdAppendPlan* plan) {
    if (snapshot->disc.discStatus != DISC_STATUS_EMPTY) return mmc_plan_cd_mode1_append(&snapshot->disc,&snapshot->track,&snapshot->toc,&snapshot->parameters,sectors,plan);
    const MmcDiscInfo* disc = &snapshot->disc; const MmcTrackInfo* track = &snapshot->track; const MmcWriteParameters* page = &snapshot->parameters;
    if (!allowBlank) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (disc->lastSessionStatus || disc->numSessions != 1 || disc->firstTrack != 1 || disc->firstTrackLastSession != 1 || disc->lastTrack != 1 || track->trackNumber != 1 || track->sessionNumber != 1 || !track->isBlank || track->isDamage || track->isReserved || track->isPacket || track->fixedPacket || !track->nwaValid || track->trackStartLba || track->nextWritableAddress) { SetLastError(ERROR_INVALID_DATA); return false; }
    if ((page->page[2] & 15) != 1 || (page->page[3] & 15) != 4 || (page->page[4] & 15) != 8 || page->page[8]) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (sectors > track->freeBlocks) { SetLastError(ERROR_DISK_FULL); return false; }
    memset(plan,0,sizeof(*plan)); plan->sessionNumber = 1; plan->trackNumber = 1; plan->dataEndLba = sectors; plan->sourceSectors = sectors;
    plan->availableDataSectors = track->freeBlocks; plan->remainingDataSectors = track->freeBlocks - sectors; plan->sectorBytes = 2048; plan->writeType = 1; plan->trackMode = 4; plan->dataBlockType = 8; return true;
}

static bool cd_session_tao_feature(OpticalDrive* drive) {
    uint8_t cdb[10] = {0x46,2,0,0x2D}, data[16] = {0}; cdb[8] = sizeof(data);
    if (!spti_send_cdb(drive,cdb,10,data,sizeof(data),true,10)) return false;
    /* MMC-5表146：仅接纳当前CD-R TAO功能及零损链接，保持小块传输时的欠载保护。 */
    if (drive->lastTransferBytes != sizeof(data) || data[0] || data[1] || data[2] || data[3] < 12 || data[6] || data[7] != MMC_PROFILE_CD_R || data[8] || data[9] != 0x2D || !(data[10] & 1) || data[11] != 4 || !(data[12] & 0x40)) { SetLastError(ERROR_NOT_SUPPORTED); return false; } return true;
}

static bool cd_session_cancelled(const CdSessionConfig* config) {
    if (config->cancelEvent && WaitForSingleObject(config->cancelEvent,0) == WAIT_OBJECT_0) { SetLastError(ERROR_CANCELLED); return true; } return false;
}

static void cd_session_progress(PipelineStats* stats, PipelineProgressCallback callback, void* userData, PipelineState state, const char* message) {
    stats->state = state; snprintf(stats->statusMessage,sizeof(stats->statusMessage),"%s",message); if (callback) callback(stats,userData);
}

static bool cd_session_read_source(HANDLE source, IsoSessionSource* iso, uint32_t offset, uint16_t count, void* buffer) {
    if (iso) return imgburn_iso_session_read(iso,offset,count,buffer);
    DWORD bytes = (DWORD)count * 2048, actual = 0; if (!ReadFile(source,buffer,bytes,&actual,NULL)) return false;
    if (actual != bytes) { SetLastError(ERROR_READ_FAULT); return false; } return true;
}

static bool cd_session_write(const CdSessionConfig* config, CdSessionResult* result, PipelineProgressCallback callback, void* userData, bool isoInput) {
    if (!result) { SetLastError(ERROR_INVALID_PARAMETER); return false; } memset(result,0,sizeof(*result));
    if (!config || !config->drive || !config->rawDataPath) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    const wchar_t* extension = wcsrchr(config->rawDataPath,L'.'); if (!extension || _wcsicmp(extension,isoInput ? L".iso" : L".bin")) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (cd_session_cancelled(config)) return false;
    IsoSessionSource iso = {0}; HANDLE source;
    if (isoInput) {
        if (!imgburn_iso_session_open(&iso,config->rawDataPath,config->cancelEvent)) return false;
        /* Joliet地址视图不等同旧Joliet规范的MODE1多会话介质支持，先在设备查询前拒绝。 */
        if (iso.joliet) { imgburn_iso_session_close(&iso); SetLastError(ERROR_NOT_SUPPORTED); return false; } source = iso.file;
    } else { source = CreateFileW(config->rawDataPath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL); if (source == INVALID_HANDLE_VALUE) return false; }
    LARGE_INTEGER size; bool success = false, ownedLock = false, parametersChanged = false; DWORD error = ERROR_SUCCESS; uint8_t* buffers = NULL;
    CdSessionSnapshot initial = {0}, locked = {0}, prepared = {0}, after = {0}; MmcWriteParameters selected = {0}; PipelineStats stats = {0};
    if (!GetFileSizeEx(source,&size)) goto cleanup;
    if (size.QuadPart < 300 * 2048 || size.QuadPart % 2048 || size.QuadPart / 2048 > UINT32_MAX) { SetLastError(ERROR_INVALID_DATA); goto cleanup; }
    stats.totalSectors = (uint32_t)(size.QuadPart / 2048); buffers = malloc(2 * 32 * 2048); if (!buffers) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto cleanup; }
    cd_session_progress(&stats,callback,userData,PIPELINE_STATE_INITIALIZING,"Checking experimental CD-R TAO session...");
    if (!cd_session_snapshot(config->drive,&initial) || !cd_session_plan(&initial,stats.totalSectors,config->allowBlank,&result->data) || !cd_session_tao_feature(config->drive)) goto cleanup;
    if (isoInput && !imgburn_iso_session_set_start(&iso,result->data.dataStartLba)) goto cleanup;
    if (!(initial.parameters.page[2] & 0x40) || (initial.parameters.page[2] & 0x10)) { SetLastError(ERROR_NOT_SUPPORTED); goto cleanup; }
    if (cd_session_cancelled(config)) goto cleanup;
    if (!config->drive->isLocked) { if (!spti_lock_drive(config->drive,true)) goto cleanup; ownedLock = true; }
    if (!cd_session_snapshot(config->drive,&locked)) goto cleanup;
    if (memcmp(&initial,&locked,sizeof(initial))) { SetLastError(ERROR_MEDIA_CHANGED); goto cleanup; }
    selected = locked.parameters; selected.page[3] = config->finalizeDisc ? 4 : 0xC4;
    if (selected.page[3] != locked.parameters.page[3]) { parametersChanged = true; if (!mmc_set_write_parameters(config->drive,&selected)) goto cleanup; }
    /* 写参数可能影响NWA解释；重新取得同一介质状态和实际可用用户块后才接受布局。 */
    if (!cd_session_snapshot(config->drive,&prepared)) goto cleanup;
    if (memcmp(&prepared.disc,&locked.disc,sizeof(locked.disc)) || memcmp(&prepared.toc,&locked.toc,sizeof(locked.toc)) || prepared.parameters.size != selected.size || memcmp(prepared.parameters.page,selected.page,selected.size)) { SetLastError(ERROR_MEDIA_CHANGED); goto cleanup; }
    if (!cd_session_plan(&prepared,stats.totalSectors,config->allowBlank,&result->data) || cd_session_cancelled(config)) goto cleanup;
    if (isoInput && !imgburn_iso_session_set_start(&iso,result->data.dataStartLba)) goto cleanup;
    cd_session_progress(&stats,callback,userData,PIPELINE_STATE_WRITING,"Writing experimental MODE1 payload...");
    for (uint32_t offset = 0; offset < stats.totalSectors;) {
        if (cd_session_cancelled(config)) goto cleanup; uint16_t count = (uint16_t)(stats.totalSectors - offset > 32 ? 32 : stats.totalSectors - offset);
        if (!cd_session_read_source(source,isoInput ? &iso : NULL,offset,count,buffers)) goto cleanup;
        result->writeStarted = true; if (!mmc_write_cd_mode1_blocks(config->drive,result->data.dataStartLba + offset,count,buffers)) goto cleanup;
        offset += count; stats.currentLba = result->data.dataStartLba + offset; stats.progressPercent = (int)((uint64_t)offset * 100 / stats.totalSectors); if (callback) callback(&stats,userData);
    }
    cd_session_progress(&stats,callback,userData,PIPELINE_STATE_CLOSING_SESSION,"Closing track and session with the explicit finalization choice...");
    if (!mmc_synchronize_cache(config->drive) || !mmc_close_track_session(config->drive,1,result->data.trackNumber) || !mmc_close_track_session(config->drive,2,0)) goto cleanup;
    result->sessionClosed = true;
    /* 封盘后没有可写不可见轨；只读取盘信息和已录Full TOC，不要求RTI255仍有效。 */
    if (!mmc_read_disc_information(config->drive,&after.disc) || !mmc_read_cd_full_toc(config->drive,&after.toc)) goto cleanup;
    bool finalReadProfile = config->finalizeDisc && after.disc.currentProfile == MMC_PROFILE_CD_ROM;
    if ((after.disc.currentProfile != MMC_PROFILE_CD_R && !finalReadProfile) || after.disc.isErasable || after.toc.sessionCount != locked.toc.sessionCount + 1 || after.toc.trackCount != locked.toc.trackCount + 1 || after.disc.discStatus != (config->finalizeDisc ? DISC_STATUS_COMPLETE : DISC_STATUS_APPENDABLE)) { SetLastError(ERROR_INVALID_DATA); goto cleanup; }
    for (unsigned i = 0; i < locked.toc.sessionCount; i++) if (memcmp(&after.toc.sessions[i],&locked.toc.sessions[i],sizeof(MmcCdSession))) { SetLastError(ERROR_MEDIA_CHANGED); goto cleanup; }
    for (unsigned i = 0; i < locked.toc.trackCount; i++) if (memcmp(&after.toc.tracks[i],&locked.toc.tracks[i],sizeof(MmcCdTrack))) { SetLastError(ERROR_MEDIA_CHANGED); goto cleanup; }
    const MmcCdSession* recorded = &after.toc.sessions[after.toc.sessionCount - 1]; const MmcCdTrack* track = &after.toc.tracks[after.toc.trackCount - 1];
    if (recorded->number != result->data.sessionNumber || recorded->trackCount != 1 || recorded->firstTrack != result->data.trackNumber || recorded->lastTrack != result->data.trackNumber || track->number != result->data.trackNumber || track->control != 4 || track->startLba != result->data.dataStartLba || recorded->leadOutLba < result->data.dataEndLba) { SetLastError(ERROR_INVALID_DATA); goto cleanup; }
    cd_session_progress(&stats,callback,userData,PIPELINE_STATE_VERIFYING,"Reading back the complete session payload...");
    size.QuadPart = 0; if (!SetFilePointerEx(source,size,NULL,FILE_BEGIN)) goto cleanup;
    for (uint32_t offset = 0; offset < stats.totalSectors;) {
        uint16_t count = (uint16_t)(stats.totalSectors - offset > 32 ? 32 : stats.totalSectors - offset); DWORD bytes = (DWORD)count * 2048;
        if (cd_session_cancelled(config) || !cd_session_read_source(source,isoInput ? &iso : NULL,offset,count,buffers)) goto cleanup;
        if (!mmc_read_blocks(config->drive,result->data.dataStartLba + offset,count,buffers + 32 * 2048)) goto cleanup;
        if (memcmp(buffers,buffers + 32 * 2048,bytes)) { SetLastError(ERROR_CRC); goto cleanup; } offset += count;
    }
    result->verified = true; success = true;
cleanup:
    error = success ? ERROR_SUCCESS : GetLastError(); if (!success && !error) error = ERROR_INVALID_DATA;
    /* 失败的部分会话不猜测收尾；仅排空已提交缓存，保留现场供后续明确处理。 */
    if (result->writeStarted && !result->sessionClosed && !success) mmc_synchronize_cache(config->drive);
    if (parametersChanged && (!result->writeStarted || result->sessionClosed) && !mmc_set_write_parameters(config->drive,&initial.parameters) && success) { success = false; error = GetLastError(); }
    if (ownedLock && !spti_lock_drive(config->drive,false) && success) { success = false; error = GetLastError(); }
    if (isoInput) imgburn_iso_session_close(&iso); else CloseHandle(source); free(buffers); cd_session_progress(&stats,callback,userData,success ? PIPELINE_STATE_COMPLETED : error == ERROR_CANCELLED ? PIPELINE_STATE_ABORTED : PIPELINE_STATE_FAILED,success ? "Experimental CD-R session verified." : "Experimental CD-R session stopped; inspect the recorded state.");
    SetLastError(error); return success;
}

static bool cd_session_verify(OpticalDrive* drive, const wchar_t* rawDataPath, const MmcCdAppendPlan* data, HANDLE cancelEvent, bool isoInput) {
    if (!drive || !rawDataPath || !data || !data->sourceSectors || !data->sessionNumber || !data->trackNumber || data->sectorBytes != 2048 || (uint64_t)data->dataStartLba + data->sourceSectors != data->dataEndLba) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    if (cancelEvent && WaitForSingleObject(cancelEvent,0) == WAIT_OBJECT_0) { SetLastError(ERROR_CANCELLED); return false; }
    IsoSessionSource iso = {0}; HANDLE source;
    if (isoInput) {
        if (!imgburn_iso_session_open(&iso,rawDataPath,cancelEvent)) return false;
        if (iso.joliet || !imgburn_iso_session_set_start(&iso,data->dataStartLba)) { DWORD error = iso.joliet ? ERROR_NOT_SUPPORTED : GetLastError(); imgburn_iso_session_close(&iso); SetLastError(error); return false; } source = iso.file;
    } else { source = CreateFileW(rawDataPath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL); if (source == INVALID_HANDLE_VALUE) return false; }
    bool success = false; DWORD error; LARGE_INTEGER size; MmcDiscInfo disc = {0}; MmcCdFullToc toc = {0}, after = {0}; uint8_t* buffer = NULL;
    if (!GetFileSizeEx(source,&size)) goto cleanup;
    if (size.QuadPart < 0 || (uint64_t)size.QuadPart != (uint64_t)data->sourceSectors * 2048) { SetLastError(ERROR_INVALID_DATA); goto cleanup; }
    if (!mmc_read_disc_information(drive,&disc) || !mmc_read_cd_full_toc(drive,&toc)) goto cleanup;
    if ((disc.currentProfile != MMC_PROFILE_CD_R && disc.currentProfile != MMC_PROFILE_CD_ROM) || disc.isErasable || data->sessionNumber > toc.sessionCount) { SetLastError(ERROR_NOT_SUPPORTED); goto cleanup; }
    const MmcCdSession* session = &toc.sessions[data->sessionNumber - 1];
    if (session->trackCount != 1 || session->firstTrack != data->trackNumber || session->firstTrackIndex >= toc.trackCount || toc.tracks[session->firstTrackIndex].startLba != data->dataStartLba || toc.tracks[session->firstTrackIndex].control != 4 || session->leadOutLba < data->dataEndLba) { SetLastError(ERROR_INVALID_DATA); goto cleanup; }
    buffer = malloc(2 * 32 * 2048); if (!buffer) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto cleanup; }
    for (uint32_t offset = 0; offset < data->sourceSectors;) {
        if (cancelEvent && WaitForSingleObject(cancelEvent,0) == WAIT_OBJECT_0) { SetLastError(ERROR_CANCELLED); goto cleanup; }
        uint16_t count = (uint16_t)(data->sourceSectors - offset > 32 ? 32 : data->sourceSectors - offset); DWORD bytes = (DWORD)count * 2048;
        if (!cd_session_read_source(source,isoInput ? &iso : NULL,offset,count,buffer)) goto cleanup;
        if (!mmc_read_blocks(drive,data->dataStartLba + offset,count,buffer + 32 * 2048)) goto cleanup;
        if (memcmp(buffer,buffer + 32 * 2048,bytes)) { SetLastError(ERROR_CRC); goto cleanup; } offset += count;
    }
    if (!mmc_read_cd_full_toc(drive,&after)) goto cleanup;
    if (memcmp(&toc,&after,sizeof(toc))) { SetLastError(ERROR_MEDIA_CHANGED); goto cleanup; } success = true;
cleanup:
    error = success ? ERROR_SUCCESS : GetLastError(); if (isoInput) imgburn_iso_session_close(&iso); else CloseHandle(source); free(buffer); SetLastError(error); return success;
}

bool pipeline_write_cd_mode1_session(const CdSessionConfig* config, CdSessionResult* result, PipelineProgressCallback callback, void* userData) { return cd_session_write(config,result,callback,userData,false); }
bool pipeline_write_cd_iso_session(const CdIsoSessionConfig* config, CdSessionResult* result, PipelineProgressCallback callback, void* userData) {
    if (!config) return cd_session_write(NULL,result,callback,userData,true);
    CdSessionConfig common = {config->drive,config->isoPath,config->cancelEvent,config->allowBlank,config->finalizeDisc}; return cd_session_write(&common,result,callback,userData,true);
}
bool pipeline_verify_cd_mode1_session(OpticalDrive* drive, const wchar_t* rawDataPath, const MmcCdAppendPlan* data, HANDLE cancelEvent) { return cd_session_verify(drive,rawDataPath,data,cancelEvent,false); }
bool pipeline_verify_cd_iso_session(OpticalDrive* drive, const wchar_t* isoPath, const MmcCdAppendPlan* data, HANDLE cancelEvent) { return cd_session_verify(drive,isoPath,data,cancelEvent,true); }
