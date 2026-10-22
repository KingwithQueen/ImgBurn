/* 刻录与校验流水线：完整传递输入、设备和收尾错误。 */
#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_pipeline.h"
#include <stdio.h>
#include <string.h>

#define CHUNK_SECTORS 32
#define CHUNK_BYTES (CHUNK_SECTORS * 2048)

typedef struct {
    HANDLE hSourceFile;
    RingBuffer* rb;
    uint64_t expectedBytes;
    volatile LONG abortRequested;
    volatile LONG readError;
} ProducerContext;

static bool pipeline_failure(PipelineStats* stats, const char* message, PipelineProgressCallback callback, void* userData) {
    stats->state = PIPELINE_STATE_FAILED;
    snprintf(stats->statusMessage, sizeof(stats->statusMessage), "%s", message);
    if (callback) callback(stats, userData);
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
    uint8_t buffer[CHUNK_BYTES];
    uint64_t remaining = ctx->expectedBytes;
    while (remaining > 0 && InterlockedCompareExchange(&ctx->abortRequested, 0, 0) == 0) {
        DWORD requested = remaining < sizeof(buffer) ? (DWORD)remaining : (DWORD)sizeof(buffer);
        DWORD actual = 0;
        if (!ReadFile(ctx->hSourceFile, buffer, requested, &actual, NULL) || actual == 0 || actual > requested) {
            DWORD error = GetLastError();
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

bool pipeline_burn_image(const BurnConfig* config, PipelineProgressCallback callback, void* userData) {
    PipelineStats stats = {0};
    if (!config || !config->drive || !config->imagePath) return pipeline_failure(&stats, "Invalid burn configuration.", callback, userData);
    /* 尚未实现设备测试写入协议，拒绝执行以免误发改变介质状态的命令。 */
    if (config->testMode) return pipeline_failure(&stats, "Device test-write mode is not implemented yet.", callback, userData);
    stats.state = PIPELINE_STATE_INITIALIZING;
    snprintf(stats.statusMessage, sizeof(stats.statusMessage), "Initializing drive and media...");
    if (callback) callback(&stats, userData);
    HANDLE source = open_image(config->imagePath, &stats.totalSectors);
    if (source == INVALID_HANDLE_VALUE) return pipeline_failure(&stats, "Cannot open image: require nonempty, aligned 2048-byte sectors within the supported size.", callback, userData);
    bool success = false;
    RingBuffer rb = {0};
    HANDLE producer = NULL;
    ProducerContext context = {0};
    context.hSourceFile = source;
    context.rb = &rb;
    context.expectedBytes = (uint64_t)stats.totalSectors * 2048;
    if (!mmc_test_unit_ready(config->drive)) { pipeline_failure(&stats, "Optical drive is not ready.", callback, userData); goto cleanup; }
    MmcProfile profile = mmc_get_current_profile(config->drive);
    MmcDiscInfo discInfo;
    if (!mmc_read_disc_information(config->drive, &discInfo)) { pipeline_failure(&stats, "Cannot query disc information.", callback, userData); goto cleanup; }
    if (!mmc_set_speed(config->drive, 0xFFFF, config->writeSpeedKbps)) { pipeline_failure(&stats, "Cannot set write speed.", callback, userData); goto cleanup; }
    if (!mmc_reserve_track(config->drive, stats.totalSectors)) { pipeline_failure(&stats, "Cannot reserve track.", callback, userData); goto cleanup; }
    if (!ringbuffer_init(&rb, config->ringBufferBytes ? config->ringBufferBytes : 32 * 1024 * 1024, true)) {
        pipeline_failure(&stats, "Cannot initialize source buffer.", callback, userData); goto cleanup;
    }
    producer = CreateThread(NULL, 0, producer_thread, &context, 0, NULL);
    if (!producer) { pipeline_failure(&stats, "Cannot create source reader thread.", callback, userData); goto cleanup; }
    stats.state = PIPELINE_STATE_WRITING;
    snprintf(stats.statusMessage, sizeof(stats.statusMessage), "Writing to disc...");
    uint8_t chunk[CHUNK_BYTES];
    DWORD start = GetTickCount(), previousTime = start;
    uint32_t previousLba = 0;
    for (uint32_t lba = 0; lba < stats.totalSectors;) {
        uint16_t count = (uint16_t)((stats.totalSectors - lba < CHUNK_SECTORS) ? stats.totalSectors - lba : CHUNK_SECTORS);
        size_t needed = (size_t)count * 2048;
        if (ringbuffer_read(&rb, chunk, needed, 5000) != needed || InterlockedCompareExchange(&context.readError, 0, 0) != 0) {
            pipeline_failure(&stats, "Source read failed, ended early, or timed out.", callback, userData); goto cleanup;
        }
        if (!mmc_write_blocks(config->drive, lba, count, chunk)) { pipeline_failure(&stats, "Device write failed.", callback, userData); goto cleanup; }
        lba += count;
        update_progress(&stats, lba, start, &previousTime, &previousLba);
        stats.hostBufferPercent = (uint32_t)ringbuffer_get_level_percent(&rb);
        stats.speedMultiplier = calculate_multiplier(stats.currentSpeedKBps, profile);
        if (callback) callback(&stats, userData);
    }
    if (WaitForSingleObject(producer, INFINITE) != WAIT_OBJECT_0 || InterlockedCompareExchange(&context.readError, 0, 0) != 0) {
        pipeline_failure(&stats, "Source reader did not finish successfully.", callback, userData); goto cleanup;
    }
    stats.state = PIPELINE_STATE_FLUSHING_CACHE;
    snprintf(stats.statusMessage, sizeof(stats.statusMessage), "Synchronizing drive cache...");
    if (callback) callback(&stats, userData);
    if (!mmc_synchronize_cache(config->drive)) { pipeline_failure(&stats, "Drive cache synchronization failed.", callback, userData); goto cleanup; }
    stats.state = PIPELINE_STATE_CLOSING_SESSION;
    snprintf(stats.statusMessage, sizeof(stats.statusMessage), "Closing session...");
    if (callback) callback(&stats, userData);
    if (!mmc_close_track_session(config->drive, 0x02, 0)) { pipeline_failure(&stats, "Closing session failed.", callback, userData); goto cleanup; }
    /* 源句柄保持只读共享，写后校验继续使用同一文件。 */
    if (config->verifyAfterWrite && !verify_image_handle(config->drive, source, &stats, callback, userData)) goto cleanup;
    if (config->ejectWhenDone && !mmc_load_eject(config->drive, true)) { pipeline_failure(&stats, "Eject failed.", callback, userData); goto cleanup; }
    success = true;
cleanup:
    if (producer) {
        InterlockedExchange(&context.abortRequested, 1);
        ringbuffer_set_finished(&rb);
        if (!success) CancelSynchronousIo(producer);
        WaitForSingleObject(producer, INFINITE);
        CloseHandle(producer);
    }
    ringbuffer_destroy(&rb);
    CloseHandle(source);
    if (success) {
        stats.state = PIPELINE_STATE_COMPLETED;
        stats.currentLba = stats.totalSectors;
        stats.progressPercent = 100;
        stats.remainingTimeSec = 0;
        snprintf(stats.statusMessage, sizeof(stats.statusMessage), "Burn process completed successfully.");
        if (callback) callback(&stats, userData);
    }
    return success;
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
