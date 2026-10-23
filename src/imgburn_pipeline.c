/* 刻录与校验流水线：完整传递输入、设备和收尾错误。 */
#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CHUNK_SECTORS 32
#define CHUNK_BYTES (CHUNK_SECTORS * 2048)

typedef struct {
    HANDLE hSourceFile;
    RingBuffer* rb;
    uint64_t expectedBytes;
    volatile LONG abortRequested;
    volatile LONG readError;
    uint8_t* buffer;
} ProducerContext;

static bool pipeline_failure(PipelineStats* stats, const char* message, PipelineProgressCallback callback, void* userData) {
    DWORD error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE;
    stats->state = PIPELINE_STATE_FAILED;
    snprintf(stats->statusMessage, sizeof(stats->statusMessage), "%s", message);
    if (callback) callback(stats, userData);
    SetLastError(error);
    return false;
}

static HANDLE open_image(const wchar_t* path, uint32_t* sectors) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return file;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart % 2048 != 0 || (uint64_t)size.QuadPart / 2048 > UINT32_MAX) {
        CloseHandle(file);
        SetLastError(ERROR_INVALID_DATA);
        return INVALID_HANDLE_VALUE;
    }
    *sectors = (uint32_t)(size.QuadPart / 2048);
    return file;
}

static DWORD WINAPI producer_thread(LPVOID param) {
    ProducerContext* ctx = (ProducerContext*)param;
    uint8_t* buffer = ctx->buffer;
    uint64_t remaining = ctx->expectedBytes;
    while (remaining > 0 && InterlockedCompareExchange(&ctx->abortRequested, 0, 0) == 0) {
        DWORD requested = remaining < CHUNK_BYTES ? (DWORD)remaining : CHUNK_BYTES;
        DWORD actual = 0;
        BOOL ok = ReadFile(ctx->hSourceFile,buffer,requested,&actual,NULL);
        if (!ok || actual == 0 || actual > requested) {
            DWORD error = !ok ? GetLastError() : actual == 0 ? ERROR_HANDLE_EOF : ERROR_INVALID_DATA;
            InterlockedExchange(&ctx->readError, (LONG)(error ? error : ERROR_HANDLE_EOF));
            break;
        }
        size_t offset = 0;
        while (offset < actual && InterlockedCompareExchange(&ctx->abortRequested, 0, 0) == 0) {
            /* 超时或部分写入后继续同一块，不能丢弃还没送入缓冲区的数据。 */
            offset += ringbuffer_write(ctx->rb, buffer + offset, actual - offset, 500);
            if (ringbuffer_is_finished(ctx->rb)) break;
        }
        if (offset != actual) break;
        remaining -= actual;
    }
    ringbuffer_set_finished(ctx->rb);
    return 0;
}

static double calculate_multiplier(double kbps, MmcProfile profile) {
    if (profile >= MMC_PROFILE_BD_ROM && profile <= MMC_PROFILE_BD_RE) return kbps / 4495.5;
    if (profile >= MMC_PROFILE_DVD_ROM && profile <= MMC_PROFILE_DVD_PLUS_R_DL) return kbps / 1385.0;
    return kbps / 150.0;
}

static void update_progress(PipelineStats* stats, uint32_t lba, DWORD start, DWORD* previousTime, uint32_t* previousLba) {
    DWORD now = GetTickCount(), duration = now - *previousTime;
    if (duration > 0) stats->currentSpeedKBps = (double)(lba - *previousLba) * 2000.0 / duration;
    stats->currentLba = lba;
    stats->progressPercent = (int)((uint64_t)lba * 100 / stats->totalSectors);
    if (stats->progressPercent == 100) stats->progressPercent = 99;
    stats->elapsedTimeSec = (now - start) / 1000;
    if (stats->currentSpeedKBps > 0) stats->remainingTimeSec = (uint32_t)((double)(stats->totalSectors - lba) * 2.0 / stats->currentSpeedKBps);
    *previousTime = now;
    *previousLba = lba;
}

static bool verify_image_handle(OpticalDrive* drive, HANDLE source, PipelineStats* stats, PipelineProgressCallback callback, void* userData) {
    LARGE_INTEGER zero = {0};
    if (!SetFilePointerEx(source, zero, NULL, FILE_BEGIN)) return pipeline_failure(stats, "Failed to rewind source image.", callback, userData);
    stats->state = PIPELINE_STATE_VERIFYING;
    stats->currentLba = 0;
    stats->progressPercent = 0;
    snprintf(stats->statusMessage, sizeof(stats->statusMessage), "Verifying disc against source image...");
    if (callback) callback(stats, userData);
    uint8_t driveChunk[CHUNK_BYTES], imageChunk[CHUNK_BYTES];
    DWORD start = GetTickCount(), previousTime = start;
    uint32_t previousLba = 0;
    bool allMatch = true;
    for (uint32_t lba = 0; lba < stats->totalSectors;) {
        uint16_t count = (uint16_t)((stats->totalSectors - lba < CHUNK_SECTORS) ? stats->totalSectors - lba : CHUNK_SECTORS);
        DWORD bytes = count * 2048, offset = 0;
        while (offset < bytes) {
            DWORD actual = 0;
            if (!ReadFile(source, imageChunk + offset, bytes - offset, &actual, NULL) || actual == 0 || actual > bytes - offset) {
                return pipeline_failure(stats, "Source image read failed or ended early.", callback, userData);
            }
            offset += actual;
        }
        if (!mmc_read_blocks(drive, lba, count, driveChunk)) {
            stats->retriesCount++;
            /* 多扇区失败时逐扇区重试；任何不可读扇区都会让校验失败。 */
            for (uint16_t sector = 0; sector < count; sector++) {
                if (!mmc_read_blocks(drive, lba + sector, 1, driveChunk + (size_t)sector * 2048)) {
                    stats->miscomparesCount++;
                    allMatch = false;
                    continue;
                }
                if (memcmp(driveChunk + (size_t)sector * 2048, imageChunk + (size_t)sector * 2048, 2048) != 0) {
                    stats->miscomparesCount++;
                    allMatch = false;
                }
            }
        } else {
            for (uint16_t sector = 0; sector < count; sector++) {
                if (memcmp(driveChunk + (size_t)sector * 2048, imageChunk + (size_t)sector * 2048, 2048) != 0) {
                    stats->miscomparesCount++;
                    allMatch = false;
                }
            }
        }
        lba += count;
        update_progress(stats, lba, start, &previousTime, &previousLba);
        if (callback) callback(stats, userData);
    }
    if (!allMatch) return pipeline_failure(stats, "Verification failed: unreadable or mismatching sectors.", callback, userData);
    return true;
}

static bool burn_failure(PipelineStats* stats, DWORD error, const char* message, PipelineProgressCallback callback, void* userData) { SetLastError(error ? error : ERROR_GEN_FAILURE); return pipeline_failure(stats,message,callback,userData); }
static bool burn_cancelled(const BurnConfig* config, PipelineStats* stats, PipelineProgressCallback callback, void* userData) {
    if (!config->cancelEvent || WaitForSingleObject(config->cancelEvent,0) != WAIT_OBJECT_0) return false;
    stats->state = PIPELINE_STATE_ABORTED; snprintf(stats->statusMessage,sizeof(stats->statusMessage),"Burn cancelled; recorded sectors cannot be undone."); if (callback) callback(stats,userData); SetLastError(ERROR_CANCELLED); return true;
}
/* 空白单层 DVD-R/+R 的容量来自可写轨道，不使用空白盘的 READ CAPACITY。 */
static bool burn_check_media(OpticalDrive* drive, uint32_t sectors, MmcProfile* profile) {
    MmcDiscInfo disc; MmcTrackInfo track;
    if (!mmc_test_unit_ready(drive) || !mmc_read_disc_information(drive,&disc)) return false;
    if (disc.currentProfile != MMC_PROFILE_DVD_PLUS_R && disc.currentProfile != MMC_PROFILE_DVD_R_SEQ) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (*profile != MMC_PROFILE_NONE && *profile != disc.currentProfile) { SetLastError(ERROR_MEDIA_CHANGED); return false; } *profile = disc.currentProfile;
    if (disc.discStatus != DISC_STATUS_EMPTY || disc.lastSessionStatus != 0 || disc.numSessions != 1 || disc.firstTrack != 1 || disc.lastTrack != 1 || disc.isErasable) { SetLastError(ERROR_INVALID_DATA); return false; }
    if (!mmc_read_track_information(drive,1,false,&track)) return false;
    if (track.trackNumber != 1 || track.sessionNumber != 1 || !track.isBlank || track.isDamage || !track.nwaValid || track.trackStartLba || track.nextWritableAddress) { SetLastError(ERROR_INVALID_DATA); return false; }
    /* 两类介质的轨道按 ECC 块占用，容量检查计入最多 15 个尾部扇区。 */
    if (((uint64_t)sectors + 15) / 16 * 16 > track.freeBlocks) { SetLastError(ERROR_DISK_FULL); return false; } return true;
}
static bool burn_buffer_read(const BurnConfig* config, ProducerContext* context, uint8_t* chunk, size_t bytes, PipelineStats* stats, PipelineProgressCallback callback, void* userData) {
    size_t offset = 0;
    while (offset < bytes) {
        if (burn_cancelled(config,stats,callback,userData)) return false;
        offset += ringbuffer_read(context->rb,chunk + offset,bytes - offset,250); DWORD error = (DWORD)InterlockedCompareExchange(&context->readError,0,0);
        if (error) return burn_failure(stats,error,"Source image read failed.",callback,userData);
        if (offset < bytes && ringbuffer_is_finished(context->rb)) {
            /* 生产者可在读取超时后刚好结束；先耗尽已排队的数据，再判断是否提前结束。 */
            offset += ringbuffer_read(context->rb,chunk + offset,bytes - offset,0);
            if (offset < bytes) return burn_failure(stats,ERROR_HANDLE_EOF,"Source image ended early.",callback,userData);
        }
    } return !burn_cancelled(config,stats,callback,userData);
}
static bool burn_wait_ready(const BurnConfig* config, PipelineStats* stats, PipelineProgressCallback callback, void* userData) {
    ULONGLONG start = GetTickCount64();
    do { if (burn_cancelled(config,stats,callback,userData)) return false; if (mmc_test_unit_ready(config->drive)) return true;
        ScsiSenseResult* sense = &config->drive->lastSense;
        if (sense->senseKey != 6 && !(sense->senseKey == 2 && (sense->asc == 4 || sense->asc == 0x3A))) return burn_failure(stats,GetLastError(),"Device did not become ready after reload.",callback,userData);
        if (config->cancelEvent) WaitForSingleObject(config->cancelEvent,250); else Sleep(250);
    } while (GetTickCount64() - start < 90000); return burn_failure(stats,ERROR_TIMEOUT,"Timed out waiting for the reloaded disc.",callback,userData);
}
typedef struct { PipelineStats* stats; PipelineProgressCallback callback; void* userData; } BurnVerifyProgress;
static void burn_verify_progress(const PipelineStats* data, void* context) {
    BurnVerifyProgress* progress = context; *progress->stats = *data;
    if (data->state == PIPELINE_STATE_COMPLETED) { progress->stats->state = PIPELINE_STATE_VERIFYING; progress->stats->progressPercent = 99; }
    if (progress->callback) progress->callback(progress->stats,progress->userData);
}
static bool burn_restore_parameters(OpticalDrive* drive, const MmcWriteParameters* original) {
    MmcWriteParameters restore = *original, actual; restore.page[2] &= (uint8_t)~0x10;
    if (!mmc_set_write_parameters(drive,&restore) || !mmc_get_write_parameters(drive,&actual)) return false;
    if (actual.size != restore.size || (actual.page[0] & 0x7F) != (restore.page[0] & 0x7F) || memcmp(actual.page + 1,restore.page + 1,restore.size - 1)) { SetLastError(ERROR_INVALID_DATA); return false; } return true;
}
bool pipeline_burn_image(const BurnConfig* config, PipelineProgressCallback callback, void* userData) {
    PipelineStats stats = {0};
    if (!config || !config->drive || !config->imagePath) return burn_failure(&stats,ERROR_INVALID_PARAMETER,"Invalid burn configuration.",callback,userData);
    if (burn_cancelled(config,&stats,callback,userData)) return false;
    const wchar_t* extension = wcsrchr(config->imagePath,L'.');
    if (!extension || (_wcsicmp(extension,L".iso") && _wcsicmp(extension,L".bin"))) return burn_failure(&stats,ERROR_NOT_SUPPORTED,"Write currently accepts 2048-byte ISO/BIN images.",callback,userData);
    HANDLE source = open_image(config->imagePath,&stats.totalSectors);
    if (source == INVALID_HANDLE_VALUE) return pipeline_failure(&stats,"Cannot open a nonempty, aligned 2048-byte source image.",callback,userData);
    bool success = false, ownedLock = false, prevented = false, writeAttempted = false, syncAttempted = false, parametersChanged = false; DWORD error = ERROR_SUCCESS;
    MmcProfile profile = MMC_PROFILE_NONE; MmcDvdWriteFeatures features = {0}; MmcWriteParameters original = {0};
    RingBuffer rb = {0}; HANDLE producer = NULL; uint8_t* buffers = NULL; ProducerContext context = {0}; context.hSourceFile = source; context.rb = &rb; context.expectedBytes = (uint64_t)stats.totalSectors * 2048;
    stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Checking blank DVD-R/+R and source image..."); if (callback) callback(&stats,userData);
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    if (!burn_check_media(config->drive,stats.totalSectors,&profile)) { pipeline_failure(&stats,"Require a blank single-layer DVD-R/+R with enough writable sectors.",callback,userData); goto cleanup; }
    if (config->testMode && profile != MMC_PROFILE_DVD_R_SEQ) { burn_failure(&stats,ERROR_NOT_SUPPORTED,"Device test writing requires supported DVD-R media.",callback,userData); goto cleanup; }
    if (profile == MMC_PROFILE_DVD_R_SEQ) {
        if (!mmc_get_dvd_write_features(config->drive,&features)) { pipeline_failure(&stats,"Cannot query DVD-R DAO write capabilities.",callback,userData); goto cleanup; }
        if (config->testMode && !features.testWrite) { burn_failure(&stats,ERROR_NOT_SUPPORTED,"The drive does not support DVD-R test writing.",callback,userData); goto cleanup; }
        if (!mmc_get_write_parameters(config->drive,&original)) { pipeline_failure(&stats,"Cannot read the current write parameters.",callback,userData); goto cleanup; }
    }
    buffers = malloc(CHUNK_BYTES * 2); if (!buffers) { burn_failure(&stats,ERROR_NOT_ENOUGH_MEMORY,"Cannot allocate source buffers.",callback,userData); goto cleanup; } context.buffer = buffers + CHUNK_BYTES;
    if (!ringbuffer_init(&rb,config->ringBufferBytes ? config->ringBufferBytes : 32 * 1024 * 1024,true)) { pipeline_failure(&stats,"Cannot initialize source buffer.",callback,userData); goto cleanup; }
    producer = CreateThread(NULL,0,producer_thread,&context,0,NULL); if (!producer) { pipeline_failure(&stats,"Cannot create source reader thread.",callback,userData); goto cleanup; }
    uint16_t count = (uint16_t)(stats.totalSectors < CHUNK_SECTORS ? stats.totalSectors : CHUNK_SECTORS);
    if (!burn_buffer_read(config,&context,buffers,(size_t)count * 2048,&stats,callback,userData)) goto cleanup;
    if (!config->drive->isLocked) { if (!spti_lock_drive(config->drive,true)) { pipeline_failure(&stats,"Cannot lock the target volume.",callback,userData); goto cleanup; } ownedLock = true; }
    if (!mmc_prevent_removal(config->drive,true)) { pipeline_failure(&stats,"Cannot prevent medium removal.",callback,userData); goto cleanup; } prevented = true;
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    if (!burn_check_media(config->drive,stats.totalSectors,&profile)) { pipeline_failure(&stats,"Target media changed before writing.",callback,userData); goto cleanup; }
    if (profile == MMC_PROFILE_DVD_R_SEQ) {
        MmcWriteParameters selected = original, actual; selected.page[2] = (uint8_t)(2 | (features.bufferUnderrunFree ? 0x40 : 0) | (config->testMode ? 0x10 : 0)); selected.page[3] = 0x25; selected.page[4] = 8; parametersChanged = true;
        if (!mmc_set_write_parameters(config->drive,&selected) || !mmc_get_write_parameters(config->drive,&actual)) { pipeline_failure(&stats,"Cannot set DVD-R DAO write parameters.",callback,userData); goto cleanup; }
        if ((actual.page[2] & 0x5F) != selected.page[2] || actual.page[3] != selected.page[3] || actual.page[4] != 8) { burn_failure(&stats,ERROR_INVALID_DATA,"The drive did not accept the requested write mode.",callback,userData); goto cleanup; }
    }
    if (!mmc_set_speed(config->drive,0xFFFF,config->writeSpeedKbps ? config->writeSpeedKbps : 0xFFFF)) { pipeline_failure(&stats,"Cannot set write speed.",callback,userData); goto cleanup; }
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    if (config->performOpc) { snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Performing optimum power calibration..."); if (callback) callback(&stats,userData); if (burn_cancelled(config,&stats,callback,userData)) goto cleanup; if (!mmc_perform_opc(config->drive)) { pipeline_failure(&stats,"Optimum power calibration failed.",callback,userData); goto cleanup; } }
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    if ((profile == MMC_PROFILE_DVD_R_SEQ || config->reserveTrack) && !mmc_reserve_track(config->drive,stats.totalSectors)) { pipeline_failure(&stats,"Cannot reserve the DVD track.",callback,userData); goto cleanup; }
    stats.state = PIPELINE_STATE_WRITING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),config->testMode ? "Device test writing DVD-R DAO..." : profile == MMC_PROFILE_DVD_R_SEQ ? "Writing DVD-R DAO..." : "Writing DVD+R track..."); DWORD start = GetTickCount(), previousTime = start; uint32_t previousLba = 0;
    for (uint32_t lba = 0; lba < stats.totalSectors;) {
        if (burn_cancelled(config,&stats,callback,userData)) goto cleanup; writeAttempted = true;
        if (!mmc_write_blocks(config->drive,lba,count,buffers)) { pipeline_failure(&stats,"Device write failed or returned a short transfer.",callback,userData); goto cleanup; }
        lba += count; update_progress(&stats,lba,start,&previousTime,&previousLba); stats.hostBufferPercent = (uint32_t)ringbuffer_get_level_percent(&rb); stats.speedMultiplier = calculate_multiplier(stats.currentSpeedKBps,profile); if (callback) callback(&stats,userData);
        if (lba == stats.totalSectors) break; count = (uint16_t)(stats.totalSectors - lba < CHUNK_SECTORS ? stats.totalSectors - lba : CHUNK_SECTORS);
        if (!burn_buffer_read(config,&context,buffers,(size_t)count * 2048,&stats,callback,userData)) goto cleanup;
    }
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    if (WaitForSingleObject(producer,INFINITE) != WAIT_OBJECT_0 || InterlockedCompareExchange(&context.readError,0,0)) { burn_failure(&stats,(DWORD)context.readError,"Source reader did not finish successfully.",callback,userData); goto cleanup; }
    stats.state = PIPELINE_STATE_FLUSHING_CACHE; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Synchronizing drive cache..."); if (callback) callback(&stats,userData);
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup; syncAttempted = true;
    if (!mmc_synchronize_cache(config->drive)) { pipeline_failure(&stats,"Drive cache synchronization failed.",callback,userData); goto cleanup; }
    /* DVD-R DAO 在同步缓存时结束光盘，不发送 DVD+R 专用的关闭功能。 */
    if (profile == MMC_PROFILE_DVD_PLUS_R) {
        stats.state = PIPELINE_STATE_CLOSING_SESSION; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Closing DVD+R track..."); if (callback) callback(&stats,userData);
        if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
        if (!mmc_close_track_session(config->drive,1,1)) { pipeline_failure(&stats,"Closing DVD+R track failed.",callback,userData); goto cleanup; }
        snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Finalizing DVD+R disc..."); if (callback) callback(&stats,userData);
        if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
        if (!mmc_close_track_session(config->drive,6,0)) { pipeline_failure(&stats,"Finalizing DVD+R disc failed.",callback,userData); goto cleanup; }
    }
    if (config->testMode) { if (!burn_restore_parameters(config->drive,&original)) { pipeline_failure(&stats,"Cannot leave the device test-write mode.",callback,userData); goto cleanup; } parametersChanged = false; }
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    MmcDiscInfo finalDisc;
    if (!mmc_read_disc_information(config->drive,&finalDisc)) { pipeline_failure(&stats,"Cannot check finalized disc status.",callback,userData); goto cleanup; }
    if (finalDisc.currentProfile != profile || finalDisc.discStatus != (config->testMode ? DISC_STATUS_EMPTY : DISC_STATUS_COMPLETE) || finalDisc.numSessions != 1 || finalDisc.firstTrack != 1 || finalDisc.lastTrack != 1) { burn_failure(&stats,ERROR_INVALID_DATA,"Disc did not report the expected final layout.",callback,userData); goto cleanup; }
    if (config->verifyAfterWrite && !config->testMode) {
        if (config->cycleTrayBeforeVerify) {
            if (!mmc_prevent_removal(config->drive,false)) { pipeline_failure(&stats,"Cannot release the tray for verification.",callback,userData); goto cleanup; } prevented = false;
            if (!mmc_load_eject(config->drive,true) || !mmc_load_eject(config->drive,false)) { pipeline_failure(&stats,"Cannot reload the disc for verification.",callback,userData); goto cleanup; }
            if (!burn_wait_ready(config,&stats,callback,userData)) goto cleanup;
            if (!mmc_prevent_removal(config->drive,true)) { pipeline_failure(&stats,"Cannot prevent medium removal during verification.",callback,userData); goto cleanup; } prevented = true;
            MmcDiscInfo loaded;
            if (!mmc_read_disc_information(config->drive,&loaded)) { pipeline_failure(&stats,"Cannot inspect reloaded media.",callback,userData); goto cleanup; }
            if (loaded.currentProfile != finalDisc.currentProfile || loaded.discStatus != finalDisc.discStatus || loaded.numSessions != finalDisc.numSessions || loaded.firstTrack != finalDisc.firstTrack || loaded.lastTrack != finalDisc.lastTrack) { burn_failure(&stats,ERROR_MEDIA_CHANGED,"Reloaded disc layout does not match the recorded disc.",callback,userData); goto cleanup; }
        }
        VerifyConfig verify = {0}; verify.drive = config->drive; verify.imagePath = config->imagePath; verify.cancelEvent = config->cancelEvent;
        BurnVerifyProgress progress = {&stats,callback,userData}; if (!pipeline_verify_disc_ex(&verify,burn_verify_progress,&progress)) goto cleanup;
    }
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup; success = true;
cleanup:
    error = GetLastError();
    if (producer) { InterlockedExchange(&context.abortRequested,1); ringbuffer_set_finished(&rb); if (!success) CancelSynchronousIo(producer); WaitForSingleObject(producer,INFINITE); CloseHandle(producer); }
    /* 已发出的写入不可回滚；DVD-R DAO 的错误冲刷也可能使设备结束已写入区域。 */
    if (writeAttempted && !syncAttempted) mmc_synchronize_cache(config->drive);
    if (parametersChanged && (config->testMode || !writeAttempted) && !burn_restore_parameters(config->drive,&original) && success) { error = GetLastError(); success = burn_failure(&stats,error,"Cannot restore device write parameters.",callback,userData); }
    if (prevented && !mmc_prevent_removal(config->drive,false) && success) { error = GetLastError(); success = burn_failure(&stats,error,"Cannot restore medium removal.",callback,userData); }
    if (ownedLock && !spti_lock_drive(config->drive,false) && success) { error = GetLastError(); success = burn_failure(&stats,error,"Cannot unlock target volume.",callback,userData); }
    ringbuffer_destroy(&rb); free(buffers); CloseHandle(source);
    if (success && config->ejectWhenDone && !mmc_load_eject(config->drive,true)) { error = GetLastError(); success = burn_failure(&stats,error,"Eject failed.",callback,userData); }
    if (success) { stats.state = PIPELINE_STATE_COMPLETED; stats.currentLba = stats.totalSectors; stats.progressPercent = 100; stats.remainingTimeSec = 0; snprintf(stats.statusMessage,sizeof(stats.statusMessage),config->testMode ? "Device test write completed; no recorded-data verification performed." : "Burn process completed successfully."); if (callback) callback(&stats,userData); }
    SetLastError(success ? ERROR_SUCCESS : error); return success;
}

static bool erase_supported(MmcProfile profile) { return profile == MMC_PROFILE_CD_RW || profile == MMC_PROFILE_DVD_RW_SEQ || profile == MMC_PROFILE_DVD_RW_RO; }
static bool erase_requested(const EraseConfig* config) { return config->cancelEvent && WaitForSingleObject(config->cancelEvent,0) == WAIT_OBJECT_0; }
static bool erase_aborted(PipelineStats* stats, bool started, PipelineProgressCallback callback, void* userData) {
    stats->state = PIPELINE_STATE_ABORTED; snprintf(stats->statusMessage,sizeof(stats->statusMessage),started ? "Cancelled after device erase finished; disc contents were erased." : "Erase cancelled before the device command."); if (callback) callback(stats,userData); SetLastError(ERROR_CANCELLED); return false;
}
bool pipeline_erase_disc(const EraseConfig* config, PipelineProgressCallback callback, void* userData) {
    PipelineStats stats = {0};
    if (!config || !config->drive) return burn_failure(&stats,ERROR_INVALID_PARAMETER,"Invalid erase configuration.",callback,userData);
    if (erase_requested(config)) return erase_aborted(&stats,false,callback,userData);
    bool success = false, ownedLock = false, prevented = false, cancelled = false; DWORD error; MmcDiscInfo initial, checked;
    stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Checking rewritable CD/DVD media..."); if (callback) callback(&stats,userData);
    if (!mmc_test_unit_ready(config->drive) || !mmc_read_disc_information(config->drive,&initial)) return pipeline_failure(&stats,"Cannot inspect media before erase.",callback,userData);
    if (!erase_supported(initial.currentProfile) || !initial.isErasable) return burn_failure(&stats,ERROR_NOT_SUPPORTED,"Erase currently supports CD-RW and single-layer DVD-RW.",callback,userData);
    if (initial.discStatus == DISC_STATUS_OTHER) return burn_failure(&stats,ERROR_INVALID_DATA,"Invalid disc state before erase.",callback,userData);
    if (erase_requested(config)) return erase_aborted(&stats,false,callback,userData);
    if (!config->drive->isLocked) { if (!spti_lock_drive(config->drive,true)) { pipeline_failure(&stats,"Cannot lock the volume for erase.",callback,userData); goto cleanup; } ownedLock = true; }
    if (!mmc_prevent_removal(config->drive,true)) { pipeline_failure(&stats,"Cannot prevent medium removal during erase.",callback,userData); goto cleanup; } prevented = true;
    if (!mmc_test_unit_ready(config->drive) || !mmc_read_disc_information(config->drive,&checked)) { pipeline_failure(&stats,"Cannot recheck media before erase.",callback,userData); goto cleanup; }
    if (checked.currentProfile != initial.currentProfile || !checked.isErasable || checked.discStatus != initial.discStatus || checked.numSessions != initial.numSessions || checked.lastTrack != initial.lastTrack) { burn_failure(&stats,ERROR_MEDIA_CHANGED,"Media layout changed before erase.",callback,userData); goto cleanup; }
    if (erase_requested(config)) { erase_aborted(&stats,false,callback,userData); goto cleanup; }
    stats.state = PIPELINE_STATE_ERASING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),config->full ? "Starting full disc erase..." : "Starting quick disc erase..."); if (callback) callback(&stats,userData);
    if (erase_requested(config)) { erase_aborted(&stats,false,callback,userData); goto cleanup; }
    if (!mmc_start_blank_disc(config->drive,config->full)) { pipeline_failure(&stats,"Device rejected the erase command.",callback,userData); goto cleanup; }
    ULONGLONG start = GetTickCount64();
    for (;;) {
        cancelled = cancelled || erase_requested(config);
        /* 传输失败可能不更新 sense，不能沿用上一次忙碌响应继续等待。 */
        memset(&config->drive->lastSense,0,sizeof(config->drive->lastSense)); bool ready = mmc_test_unit_ready(config->drive);
        ScsiSenseResult* sense = &config->drive->lastSense;
        if (!ready && !(sense->senseKey == 2 && sense->asc == 4 && sense->ascq == 7)) { pipeline_failure(&stats,"Device failed while erasing.",callback,userData); goto cleanup; }
        MmcEraseProgress progress; if (!mmc_read_erase_progress(config->drive,&progress)) { pipeline_failure(&stats,"Cannot read device erase status.",callback,userData); goto cleanup; }
        stats.elapsedTimeSec = (uint32_t)((GetTickCount64() - start) / 1000);
        if (ready && !progress.active) {
            if (mmc_read_disc_information(config->drive,&checked)) break;
            sense = &config->drive->lastSense;
            if (!(sense->senseKey == 2 && sense->asc == 4 && sense->ascq == 7)) { pipeline_failure(&stats,"Cannot inspect erased media.",callback,userData); goto cleanup; }
        }
        if (progress.valid) { int percent = (int)((uint32_t)progress.value * 100 / 65536); if (percent > stats.progressPercent) stats.progressPercent = percent; }
        snprintf(stats.statusMessage,sizeof(stats.statusMessage),cancelled ? "Cancellation requested; waiting for device erase to finish..." : "Device erase in progress..."); if (callback) callback(&stats,userData);
        if (stats.elapsedTimeSec >= 7200) { burn_failure(&stats,ERROR_TIMEOUT,"Device erase timed out; its physical operation may continue.",callback,userData); goto cleanup; }
        /* 取消事件已触发时仍须等待物理操作结束，避免忙循环或提前解除托盘保护。 */
        Sleep(1000);
    }
    MmcProfile expected = initial.currentProfile == MMC_PROFILE_CD_RW ? MMC_PROFILE_CD_RW : MMC_PROFILE_DVD_RW_SEQ;
    if (checked.currentProfile != expected || !checked.isErasable || checked.discStatus != DISC_STATUS_EMPTY || checked.lastSessionStatus) { burn_failure(&stats,ERROR_INVALID_DATA,"Device did not report blank media after erase.",callback,userData); goto cleanup; }
    if (cancelled || erase_requested(config)) { erase_aborted(&stats,true,callback,userData); goto cleanup; } success = true;
cleanup:
    error = GetLastError();
    if (prevented && !mmc_prevent_removal(config->drive,false) && success) { error = GetLastError(); success = burn_failure(&stats,error,"Cannot restore medium removal after erase.",callback,userData); }
    if (ownedLock && !spti_lock_drive(config->drive,false) && success) { error = GetLastError(); success = burn_failure(&stats,error,"Cannot unlock the erased volume.",callback,userData); }
    if (success) { stats.state = PIPELINE_STATE_COMPLETED; stats.progressPercent = 100; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Disc erase completed."); if (callback) callback(&stats,userData); }
    SetLastError(success ? ERROR_SUCCESS : error); return success;
}

bool pipeline_verify_disc(OpticalDrive* drive, const wchar_t* sourceImagePath, PipelineProgressCallback callback, void* userData) {
    PipelineStats stats = {0};
    if (!drive || !sourceImagePath) return pipeline_failure(&stats, "Invalid verify configuration.", callback, userData);
    HANDLE source = open_image(sourceImagePath, &stats.totalSectors);
    if (source == INVALID_HANDLE_VALUE) return pipeline_failure(&stats, "Cannot open image: require nonempty, aligned 2048-byte sectors within the supported size.", callback, userData);
    bool success = verify_image_handle(drive, source, &stats, callback, userData);
    CloseHandle(source);
    if (success) {
        stats.state = PIPELINE_STATE_COMPLETED;
        stats.progressPercent = 100;
        stats.remainingTimeSec = 0;
        snprintf(stats.statusMessage, sizeof(stats.statusMessage), "Verification successful. 0 miscompares.");
        if (callback) callback(&stats, userData);
    }
    return success;
}
