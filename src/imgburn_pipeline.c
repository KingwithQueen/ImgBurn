/* 刻录与校验流水线：完整传递输入、设备和收尾错误。 */
#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_pipeline.h"
#include "imgburn_image.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CHUNK_SECTORS 32
#define CHUNK_BYTES (CHUNK_SECTORS * 2048)

typedef struct { HANDLE file; uint32_t start, end; uint64_t offset; bool swap; } BurnCdSpan;
typedef struct {
    CueSheet sheet;
    BurnCdSpan spans[MAX_CUE_TRACKS * 3];
    unsigned spanCount, readSpan;
    uint32_t readLba, cueBytes;
    uint8_t cue[(MAX_CUE_TRACKS * (MAX_CUE_INDICES + 4) + 5) * 8];
} BurnCdImage;

static void burn_cd_free(BurnCdImage* image) {
    if (!image) return;
    for (unsigned i = 0; i < image->spanCount && i < MAX_CUE_TRACKS * 3; i++) {
        HANDLE sourceFile = image->spans[i].file; image->spans[i].file = NULL;
        if (sourceFile && sourceFile != INVALID_HANDLE_VALUE) CloseHandle(sourceFile);
    }
    free(image);
}
static uint8_t burn_cd_control(const CueTrack* track) { return (uint8_t)(((track->flags & CUE_FLAG_DCP) ? 2 : 0) | ((track->flags & CUE_FLAG_4CH) ? 8 : 0) | ((track->flags & CUE_FLAG_PRE) ? 1 : 0)); }
static void burn_cd_entry(BurnCdImage* image, uint8_t control, unsigned track, unsigned index, unsigned form, uint8_t scms, int32_t lba) {
    uint8_t* entry = image->cue + image->cueBytes; uint32_t absolute = (uint32_t)(lba + 150);
    entry[0] = (uint8_t)((control << 4) | 1); entry[1] = (uint8_t)track; entry[2] = (uint8_t)index; entry[3] = (uint8_t)form; entry[4] = scms;
    entry[5] = (uint8_t)(absolute / 4500); entry[6] = (uint8_t)(absolute / 75 % 60); entry[7] = (uint8_t)(absolute % 75); image->cueBytes += 8;
}
static bool burn_cd_span(BurnCdImage* image, uint32_t start, uint32_t end, const CueTrack* source, uint64_t offset) {
    if (start == end) return true;
    if (start > end || image->spanCount >= MAX_CUE_TRACKS * 3) { SetLastError(ERROR_INVALID_DATA); return false; }
    BurnCdSpan* span = &image->spans[image->spanCount++]; memset(span,0,sizeof(*span)); span->start = start; span->end = end; span->offset = offset;
    if (!source) return true;
    span->swap = source->sourceType == CUE_FILE_MOTOROLA;
    span->file = CreateFileW(source->sourceFilePath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if (span->file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length; BY_HANDLE_FILE_INFORMATION info; uint64_t bytes = (uint64_t)(end - start) * 2352;
    if (GetFileType(span->file) != FILE_TYPE_DISK || !GetFileInformationByHandle(span->file,&info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !GetFileSizeEx(span->file,&length) || length.QuadPart < 0 || length.QuadPart % 2352 || offset > (uint64_t)length.QuadPart || bytes > (uint64_t)length.QuadPart - offset) { SetLastError(ERROR_INVALID_DATA); return false; } return true;
}
static bool burn_cd_plan(BurnCdImage* image) {
    CueSheet* sheet = &image->sheet;
    if (sheet->trackCount < 1 || sheet->trackCount > MAX_CUE_TRACKS || !sheet->totalSectors || sheet->totalSectors > 359849) { SetLastError(ERROR_INVALID_DATA); return false; }
    if (sheet->sessionCount != 1 || sheet->discTitle[0] || sheet->discPerformer[0] || sheet->songwriter[0] || sheet->cdTextFile[0]) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (sheet->catalogNumber[0]) {
        if (strlen(sheet->catalogNumber) != 13) { SetLastError(ERROR_INVALID_DATA); return false; }
        for (unsigned i = 0; i < 13; i++) if (!isdigit((unsigned char)sheet->catalogNumber[i])) { SetLastError(ERROR_INVALID_DATA); return false; }
        image->cue[0] = image->cue[8] = 2; memcpy(image->cue + 1,sheet->catalogNumber,7); memcpy(image->cue + 9,sheet->catalogNumber + 7,6); image->cueBytes = 16;
    }
    burn_cd_entry(image,burn_cd_control(&sheet->tracks[0]),0,0,1,0,-150);
    uint32_t cursor = 0;
    for (int i = 0; i < sheet->trackCount; i++) {
        const CueTrack* track = &sheet->tracks[i]; uint8_t control = burn_cd_control(track), scms = (track->flags & CUE_FLAG_SCMS) ? 0x80 : 0;
        if (track->mode != TRACK_MODE_AUDIO || track->sectorSize != 2352 || track->sourceType > CUE_FILE_MOTOROLA || track->title[0] || track->performer[0] || track->songwriter[0]) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        if (track->trackNumber != i + 1 || track->sessionNumber != 1 || track->indexCount < 1 || track->indexCount > MAX_CUE_INDICES || track->sectorCount < 300 || (scms && (control & 2))) { SetLastError(ERROR_INVALID_DATA); return false; }
        uint32_t indexOne = 0; bool found = false;
        for (int j = 0; j < track->indexCount; j++) if (track->indices[j].indexNumber == 1) { indexOne = track->indices[j].lba; found = true; }
        bool firstFileTrack = !i || track->sourceFileGroup != sheet->tracks[i - 1].sourceFileGroup;
        uint32_t fileStart = firstFileTrack ? 0 : track->indices[0].lba;
        if (!found || fileStart > indexOne || indexOne - fileStart > track->startLba) { SetLastError(ERROR_INVALID_DATA); return false; }
        /* 首轨非零偏移及 FILE 前未明确标为 INDEX00 的内容，待原版寻址对照后接通。 */
        if ((!i && track->startLba) || (firstFileTrack && indexOne && (track->indices[0].indexNumber != 0 || track->indices[0].lba))) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        uint32_t payloadStart = track->startLba - (indexOne - fileStart); uint64_t end = (uint64_t)track->startLba + track->sectorCount, prefix = (uint64_t)(indexOne - fileStart) * 2352;
        if (track->pregapSectors > payloadStart || payloadStart - track->pregapSectors != cursor || end + track->postgapSectors > sheet->totalSectors || track->sourceByteOffset < prefix || track->sourceByteLength != (uint64_t)track->sectorCount * 2352) { SetLastError(ERROR_INVALID_DATA); return false; }
        if (i && ((control ^ burn_cd_control(&sheet->tracks[i - 1])) & 9) && track->startLba - cursor < 150) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        if (track->isrc[0]) {
            if (strlen(track->isrc) != 12) { SetLastError(ERROR_INVALID_DATA); return false; }
            for (unsigned j = 0; j < 12; j++) if (j < 2 ? !isalpha((unsigned char)track->isrc[j]) : j < 5 ? !isalnum((unsigned char)track->isrc[j]) : !isdigit((unsigned char)track->isrc[j])) { SetLastError(ERROR_INVALID_DATA); return false; }
            uint8_t* pair = image->cue + image->cueBytes; pair[0] = pair[8] = (uint8_t)((control << 4) | 3); pair[1] = pair[9] = (uint8_t)track->trackNumber;
            for (unsigned j = 0; j < 12; j++) pair[(j < 6 ? 2 : 10) + j % 6] = (uint8_t)toupper((unsigned char)track->isrc[j]); image->cueBytes += 16;
        }
        /* 首轨的标准两秒预间隙位于负 LBA；文件中的 INDEX00 内容仍从 LBA0 起保留。 */
        if (!i) burn_cd_entry(image,control,1,0,1,scms,-150);
        if (track->startLba > cursor) burn_cd_entry(image,control,(unsigned)i + 1,0,0,scms,(int32_t)cursor);
        for (int j = 0; j < track->indexCount; j++) if (track->indices[j].indexNumber) burn_cd_entry(image,control,(unsigned)i + 1,(unsigned)track->indices[j].indexNumber,0,scms,(int32_t)(track->startLba + track->indices[j].lba - indexOne));
        if (!burn_cd_span(image,cursor,payloadStart,NULL,0) || !burn_cd_span(image,payloadStart,(uint32_t)end,track,track->sourceByteOffset - prefix) || !burn_cd_span(image,(uint32_t)end,(uint32_t)end + track->postgapSectors,NULL,0)) return false;
        cursor = (uint32_t)end + track->postgapSectors;
    }
    if (cursor != sheet->totalSectors) { SetLastError(ERROR_INVALID_DATA); return false; }
    burn_cd_entry(image,burn_cd_control(&sheet->tracks[sheet->trackCount - 1]),0xAA,1,1,0,(int32_t)cursor); return true;
}
static BOOL burn_cd_read(BurnCdImage* image, uint8_t* data, DWORD wanted, DWORD* actual) {
    *actual = 0;
    while (*actual < wanted) {
        while (image->readSpan < image->spanCount && image->readLba == image->spans[image->readSpan].end) image->readSpan++;
        if (image->readSpan >= image->spanCount || (wanted - *actual) % 2352) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }
        const BurnCdSpan* span = &image->spans[image->readSpan]; DWORD frames = (wanted - *actual) / 2352;
        if (frames > span->end - image->readLba) frames = span->end - image->readLba; DWORD bytes = frames * 2352;
        if (!span->file) memset(data + *actual,0,bytes);
        else {
            LARGE_INTEGER position; position.QuadPart = (LONGLONG)(span->offset + (uint64_t)(image->readLba - span->start) * 2352);
            if (!SetFilePointerEx(span->file,position,NULL,FILE_BEGIN)) return FALSE;
            DWORD offset = 0;
            while (offset < bytes) { DWORD got = 0; if (!ReadFile(span->file,data + *actual + offset,bytes - offset,&got,NULL)) return FALSE; if (!got || got > bytes - offset) { SetLastError(ERROR_HANDLE_EOF); return FALSE; } offset += got; }
            if (span->swap) for (DWORD j = 0; j < bytes; j += 2) { uint8_t value = data[*actual + j]; data[*actual + j] = data[*actual + j + 1]; data[*actual + j + 1] = value; }
        }
        image->readLba += frames; *actual += bytes;
    } return TRUE;
}

typedef struct {
    HANDLE hSourceFile;
    RingBuffer* rb;
    uint64_t expectedBytes;
    volatile LONG abortRequested;
    volatile LONG readError;
    uint8_t* buffer;
    BurnCdImage* cd;
    DWORD chunkBytes;
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
        DWORD requested = remaining < ctx->chunkBytes ? (DWORD)remaining : ctx->chunkBytes;
        DWORD actual = 0;
        BOOL ok = ctx->cd ? burn_cd_read(ctx->cd,buffer,requested,&actual) : ReadFile(ctx->hSourceFile,buffer,requested,&actual,NULL);
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

static void update_progress(PipelineStats* stats, uint32_t lba, DWORD start, DWORD* previousTime, uint32_t* previousLba, uint32_t sectorBytes) {
    DWORD now = GetTickCount(), duration = now - *previousTime;
    if (duration > 0) stats->currentSpeedKBps = (double)(lba - *previousLba) * sectorBytes * 1000.0 / 1024 / duration;
    stats->currentLba = lba;
    stats->progressPercent = (int)((uint64_t)lba * 100 / stats->totalSectors);
    if (stats->progressPercent == 100) stats->progressPercent = 99;
    stats->elapsedTimeSec = (now - start) / 1000;
    if (stats->currentSpeedKBps > 0) stats->remainingTimeSec = (uint32_t)((double)(stats->totalSectors - lba) * sectorBytes / 1024 / stats->currentSpeedKBps);
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
        update_progress(stats,lba,start,&previousTime,&previousLba,2048);
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
/* 空白 CD-R 使用 ATIP，DVD-R/+R、BD-R SRM 使用可写轨道，不查询 READ CAPACITY。 */
static bool burn_check_media(OpticalDrive* drive, uint32_t sectors, MmcProfile* profile) {
    MmcDiscInfo disc; MmcTrackInfo track;
    if (!mmc_test_unit_ready(drive) || !mmc_read_disc_information(drive,&disc)) return false;
    if (disc.currentProfile != MMC_PROFILE_DVD_PLUS_R && disc.currentProfile != MMC_PROFILE_DVD_R_SEQ && disc.currentProfile != MMC_PROFILE_BD_R_SRM && disc.currentProfile != MMC_PROFILE_CD_R) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (*profile != MMC_PROFILE_NONE && *profile != disc.currentProfile) { SetLastError(ERROR_MEDIA_CHANGED); return false; } *profile = disc.currentProfile;
    if (disc.discStatus != DISC_STATUS_EMPTY || disc.lastSessionStatus != 0 || disc.numSessions != 1 || disc.firstTrack != 1 || disc.lastTrack != 1 || disc.isErasable) { SetLastError(ERROR_INVALID_DATA); return false; }
    if (disc.currentProfile == MMC_PROFILE_CD_R) {
        uint32_t capacity; if (sectors < 300) { SetLastError(ERROR_INVALID_DATA); return false; }
        if (!mmc_read_cd_write_capacity(drive,&capacity)) return false;
        if (sectors > capacity) { SetLastError(ERROR_DISK_FULL); return false; } return true;
    }
    if (!mmc_read_track_information(drive,1,false,&track)) return false;
    if (track.trackNumber != 1 || track.sessionNumber != 1 || !track.isBlank || track.isDamage || !track.nwaValid || track.trackStartLba || track.nextWritableAddress) { SetLastError(ERROR_INVALID_DATA); return false; }
    /* DVD 按 16 扇区 ECC 块、BD 按 32 扇区簇占用；BD 预留不能耗尽全部可用空间。 */
    uint32_t unit = disc.currentProfile == MMC_PROFILE_BD_R_SRM ? 32 : 16; uint64_t occupied = ((uint64_t)sectors + unit - 1) / unit * unit;
    if (occupied > track.freeBlocks || (disc.currentProfile == MMC_PROFILE_BD_R_SRM && occupied == track.freeBlocks)) { SetLastError(ERROR_DISK_FULL); return false; } return true;
}
static bool burn_check_cd_layout(OpticalDrive* drive, uint32_t sectors, const BurnCdImage* image) {
    MmcCdToc toc; if (!mmc_read_cd_toc(drive,&toc)) return false;
    unsigned trackCount = image ? (unsigned)image->sheet.trackCount : 1;
    if (toc.trackCount != trackCount || toc.leadOutLba != sectors) { SetLastError(ERROR_INVALID_DATA); return false; }
    for (unsigned i = 0; i < trackCount; i++) if (toc.tracks[i].startLba != (image ? image->sheet.tracks[i].startLba : 0) || toc.tracks[i].control != (image ? burn_cd_control(&image->sheet.tracks[i]) : 4)) { SetLastError(ERROR_INVALID_DATA); return false; } return true;
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
    bool cueInput = extension && !_wcsicmp(extension,L".cue"); BurnCdImage* cdImage = NULL;
    if (!extension || (!cueInput && _wcsicmp(extension,L".iso") && _wcsicmp(extension,L".bin"))) return burn_failure(&stats,ERROR_NOT_SUPPORTED,"Write accepts 2048-byte ISO/BIN or supported audio CUE images.",callback,userData);
    HANDLE source = cueInput ? CreateFileW(config->imagePath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL) : open_image(config->imagePath,&stats.totalSectors);
    if (source == INVALID_HANDLE_VALUE) return pipeline_failure(&stats,"Cannot open the source image or CUE descriptor.",callback,userData);
    if (cueInput) {
        cdImage = calloc(1,sizeof(*cdImage));
        if (!cdImage) { CloseHandle(source); return burn_failure(&stats,ERROR_NOT_ENOUGH_MEMORY,"Cannot allocate the CD source plan.",callback,userData); }
        SetLastError(ERROR_SUCCESS); bool parsed = imgburn_cue_parse(config->imagePath,&cdImage->sheet);
        if (!parsed || !burn_cd_plan(cdImage)) { DWORD prepareError = GetLastError(); burn_cd_free(cdImage); CloseHandle(source); return burn_failure(&stats,prepareError ? prepareError : ERROR_INVALID_DATA,"Cannot prepare this audio CUE and its locked source files.",callback,userData); }
        stats.totalSectors = cdImage->sheet.totalSectors;
    }
    uint16_t chunkSectors = cdImage ? 27 : CHUNK_SECTORS; DWORD sectorBytes = cdImage ? 2352 : 2048, chunkBytes = chunkSectors * sectorBytes;
    bool success = false, ownedLock = false, prevented = false, writeAttempted = false, syncAttempted = false, parametersChanged = false; DWORD error = ERROR_SUCCESS;
    MmcProfile profile = MMC_PROFILE_NONE; MmcDvdWriteFeatures features = {0}; MmcWriteParameters original = {0}; bool bdVerifyNotRequired = false;
    RingBuffer rb = {0}; HANDLE producer = NULL; uint8_t* buffers = NULL; ProducerContext context = {0}; context.hSourceFile = source; context.rb = &rb; context.expectedBytes = (uint64_t)stats.totalSectors * sectorBytes; context.cd = cdImage; context.chunkBytes = chunkBytes;
    stats.state = PIPELINE_STATE_INITIALIZING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Checking blank CD-R, DVD-R/+R or BD-R SRM and source image..."); if (callback) callback(&stats,userData);
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    if (!burn_check_media(config->drive,stats.totalSectors,&profile)) { pipeline_failure(&stats,"Require blank CD-R, DVD-R/+R or BD-R SRM with enough writable sectors.",callback,userData); goto cleanup; }
    if (cdImage && profile != MMC_PROFILE_CD_R) { burn_failure(&stats,ERROR_NOT_SUPPORTED,"Audio CUE writing requires blank CD-R media.",callback,userData); goto cleanup; }
    if (profile != MMC_PROFILE_BD_R_SRM && config->writeSpeedKbps > 0xFFFF && config->writeSpeedKbps != UINT32_MAX) { burn_failure(&stats,ERROR_INVALID_PARAMETER,"CD/DVD write speed exceeds the command range.",callback,userData); goto cleanup; }
    if (config->testMode && profile != MMC_PROFILE_DVD_R_SEQ && profile != MMC_PROFILE_CD_R) { burn_failure(&stats,ERROR_NOT_SUPPORTED,"Device test writing requires supported CD-R or DVD-R media.",callback,userData); goto cleanup; }
    if (profile == MMC_PROFILE_CD_R) {
        MmcCdWriteFeatures cd; if (!mmc_get_cd_write_features(config->drive,&cd)) { pipeline_failure(&stats,"Cannot query CD-R SAO write capabilities.",callback,userData); goto cleanup; }
        if (cdImage && cdImage->cueBytes > cd.maxCueSheetBytes) { burn_failure(&stats,ERROR_NOT_SUPPORTED,"The complete CD cue sheet exceeds the device buffer.",callback,userData); goto cleanup; }
        features.testWrite = cd.testWrite; features.bufferUnderrunFree = cd.bufferUnderrunFree;
    }
    if (profile == MMC_PROFILE_DVD_R_SEQ) {
        if (!mmc_get_dvd_write_features(config->drive,&features)) { pipeline_failure(&stats,"Cannot query DVD-R DAO write capabilities.",callback,userData); goto cleanup; }
    }
    if (profile == MMC_PROFILE_DVD_R_SEQ || profile == MMC_PROFILE_CD_R) {
        if (config->testMode && !features.testWrite) { burn_failure(&stats,ERROR_NOT_SUPPORTED,"The drive does not support the requested test writing.",callback,userData); goto cleanup; }
        if (!mmc_get_write_parameters(config->drive,&original)) { pipeline_failure(&stats,"Cannot read the current write parameters.",callback,userData); goto cleanup; }
    }
    if (profile == MMC_PROFILE_BD_R_SRM && !mmc_get_bd_write_features(config->drive,&bdVerifyNotRequired)) { pipeline_failure(&stats,"Cannot confirm BD-R write capability.",callback,userData); goto cleanup; }
    buffers = malloc((size_t)chunkBytes * 2); if (!buffers) { burn_failure(&stats,ERROR_NOT_ENOUGH_MEMORY,"Cannot allocate source buffers.",callback,userData); goto cleanup; } context.buffer = buffers + chunkBytes;
    if (!ringbuffer_init(&rb,config->ringBufferBytes ? config->ringBufferBytes : 32 * 1024 * 1024,true)) { pipeline_failure(&stats,"Cannot initialize source buffer.",callback,userData); goto cleanup; }
    producer = CreateThread(NULL,0,producer_thread,&context,0,NULL); if (!producer) { pipeline_failure(&stats,"Cannot create source reader thread.",callback,userData); goto cleanup; }
    uint16_t count = (uint16_t)(stats.totalSectors < chunkSectors ? stats.totalSectors : chunkSectors);
    if (!burn_buffer_read(config,&context,buffers,(size_t)count * sectorBytes,&stats,callback,userData)) goto cleanup;
    if (!config->drive->isLocked) { if (!spti_lock_drive(config->drive,true)) { pipeline_failure(&stats,"Cannot lock the target volume.",callback,userData); goto cleanup; } ownedLock = true; }
    if (!mmc_prevent_removal(config->drive,true)) { pipeline_failure(&stats,"Cannot prevent medium removal.",callback,userData); goto cleanup; } prevented = true;
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    if (!burn_check_media(config->drive,stats.totalSectors,&profile)) { pipeline_failure(&stats,"Target media changed before writing.",callback,userData); goto cleanup; }
    if (profile == MMC_PROFILE_DVD_R_SEQ || profile == MMC_PROFILE_CD_R) {
        MmcWriteParameters selected = original, actual; selected.page[2] = (uint8_t)(2 | (features.bufferUnderrunFree ? 0x40 : 0) | (config->testMode ? 0x10 : 0)); selected.page[3] = cdImage ? burn_cd_control(&cdImage->sheet.tracks[0]) : profile == MMC_PROFILE_CD_R ? 4 : 0x25; selected.page[4] = cdImage ? 0 : 8; parametersChanged = true;
        /* 普通 MODE1 CD 不继承上一张音频盘的 MCN、ISRC 或 XA 会话格式，保留厂商尾部。 */
        if (profile == MMC_PROFILE_CD_R) { memset(selected.page + 7,0,45); selected.page[15] = 150; }
        if (!mmc_set_write_parameters(config->drive,&selected) || !mmc_get_write_parameters(config->drive,&actual)) { pipeline_failure(&stats,"Cannot set CD-R SAO or DVD-R DAO write parameters.",callback,userData); goto cleanup; }
        if ((actual.page[2] & 0x5F) != selected.page[2] || actual.page[3] != selected.page[3] || actual.page[4] != selected.page[4] || (profile == MMC_PROFILE_CD_R && ((actual.page[7] & 0x3F) || actual.page[8] || (actual.page[16] & 0x80) || (actual.page[32] & 0x80)))) { burn_failure(&stats,ERROR_INVALID_DATA,"The drive did not accept the requested write mode.",callback,userData); goto cleanup; }
    }
    if (!mmc_set_burn_speed(config->drive,profile,config->writeSpeedKbps)) { pipeline_failure(&stats,"Cannot set write speed.",callback,userData); goto cleanup; }
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    if (config->performOpc) { snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Performing optimum power calibration..."); if (callback) callback(&stats,userData); if (burn_cancelled(config,&stats,callback,userData)) goto cleanup; if (!mmc_perform_opc(config->drive)) { pipeline_failure(&stats,"Optimum power calibration failed.",callback,userData); goto cleanup; } }
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    if (profile == MMC_PROFILE_CD_R) { if (!(cdImage ? mmc_send_cd_cue(config->drive,cdImage->cue,cdImage->cueBytes) : mmc_send_cd_mode1_cue(config->drive,stats.totalSectors))) { pipeline_failure(&stats,"Cannot send the CD-R cue sheet.",callback,userData); goto cleanup; } }
    else if ((profile == MMC_PROFILE_DVD_R_SEQ || profile == MMC_PROFILE_BD_R_SRM || config->reserveTrack) && !mmc_reserve_track(config->drive,stats.totalSectors)) { pipeline_failure(&stats,"Cannot reserve the recording track.",callback,userData); goto cleanup; }
    stats.state = PIPELINE_STATE_WRITING; snprintf(stats.statusMessage,sizeof(stats.statusMessage),config->testMode ? "Device test writing CD-R SAO / DVD-R DAO..." : cdImage ? "Writing CD-R audio SAO..." : profile == MMC_PROFILE_CD_R ? "Writing CD-R MODE1 SAO..." : profile == MMC_PROFILE_DVD_R_SEQ ? "Writing DVD-R DAO..." : profile == MMC_PROFILE_BD_R_SRM ? "Writing BD-R SRM track..." : "Writing DVD+R track..."); DWORD start = GetTickCount(), previousTime = start; uint32_t previousLba = 0;
    if (profile == MMC_PROFILE_CD_R) {
        if (callback) callback(&stats,userData); if (burn_cancelled(config,&stats,callback,userData)) goto cleanup; writeAttempted = true;
        if (!mmc_write_cd_leadin_gap(config->drive)) { pipeline_failure(&stats,"Cannot write the generated CD lead-in pregap.",callback,userData); goto cleanup; }
    }
    for (uint32_t lba = 0; lba < stats.totalSectors;) {
        if (burn_cancelled(config,&stats,callback,userData)) goto cleanup; writeAttempted = true;
        bool wrote = profile == MMC_PROFILE_BD_R_SRM ? mmc_write_bd_blocks(config->drive,lba,count,buffers,bdVerifyNotRequired) : cdImage ? mmc_write_cd_audio_blocks(config->drive,lba,count,buffers) : profile == MMC_PROFILE_CD_R ? mmc_write_cd_mode1_blocks(config->drive,lba,count,buffers) : mmc_write_blocks(config->drive,lba,count,buffers);
        if (!wrote) { pipeline_failure(&stats,"Device write failed or returned a short transfer.",callback,userData); goto cleanup; }
        lba += count; update_progress(&stats,lba,start,&previousTime,&previousLba,sectorBytes); stats.hostBufferPercent = (uint32_t)ringbuffer_get_level_percent(&rb); stats.speedMultiplier = cdImage ? stats.currentSpeedKBps / 172.265625 : calculate_multiplier(stats.currentSpeedKBps,profile); if (callback) callback(&stats,userData);
        if (lba == stats.totalSectors) break; count = (uint16_t)(stats.totalSectors - lba < chunkSectors ? stats.totalSectors - lba : chunkSectors);
        if (!burn_buffer_read(config,&context,buffers,(size_t)count * sectorBytes,&stats,callback,userData)) goto cleanup;
    }
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    if (WaitForSingleObject(producer,INFINITE) != WAIT_OBJECT_0 || InterlockedCompareExchange(&context.readError,0,0)) { burn_failure(&stats,(DWORD)context.readError,"Source reader did not finish successfully.",callback,userData); goto cleanup; }
    stats.state = PIPELINE_STATE_FLUSHING_CACHE; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Synchronizing drive cache..."); if (callback) callback(&stats,userData);
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup; syncAttempted = true;
    if (!(profile == MMC_PROFILE_CD_R ? mmc_finish_cd_sao(config->drive) : mmc_synchronize_cache(config->drive))) { pipeline_failure(&stats,"Drive cache synchronization failed.",callback,userData); goto cleanup; }
    /* CD SAO 按 CUE 结束会话，DVD-R DAO 也在同步时结束；不发送 DVD+R 的关闭功能。 */
    if (profile == MMC_PROFILE_DVD_PLUS_R || profile == MMC_PROFILE_BD_R_SRM) {
        stats.state = PIPELINE_STATE_CLOSING_SESSION; snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Closing recorded track..."); if (callback) callback(&stats,userData);
        if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
        if (!mmc_close_track_session(config->drive,1,1)) { pipeline_failure(&stats,"Closing recorded track failed.",callback,userData); goto cleanup; }
        snprintf(stats.statusMessage,sizeof(stats.statusMessage),"Finalizing recorded disc..."); if (callback) callback(&stats,userData);
        if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
        if (!mmc_close_track_session(config->drive,6,0)) { pipeline_failure(&stats,"Finalizing recorded disc failed.",callback,userData); goto cleanup; }
    }
    if (config->testMode) { if (!burn_restore_parameters(config->drive,&original)) { pipeline_failure(&stats,"Cannot leave the device test-write mode.",callback,userData); goto cleanup; } parametersChanged = false; }
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup;
    MmcDiscInfo finalDisc;
    if (!mmc_read_disc_information(config->drive,&finalDisc)) { pipeline_failure(&stats,"Cannot check finalized disc status.",callback,userData); goto cleanup; }
    if (finalDisc.currentProfile != profile || finalDisc.discStatus != (config->testMode ? DISC_STATUS_EMPTY : DISC_STATUS_COMPLETE) || finalDisc.lastSessionStatus != (config->testMode ? 0 : 3) || finalDisc.numSessions != 1 || finalDisc.firstTrack != 1 || finalDisc.lastTrack != (config->testMode || !cdImage ? 1 : cdImage->sheet.trackCount)) { burn_failure(&stats,ERROR_INVALID_DATA,"Disc did not report the expected final layout.",callback,userData); goto cleanup; }
    if (profile == MMC_PROFILE_CD_R && !config->testMode && !burn_check_cd_layout(config->drive,stats.totalSectors,cdImage)) { pipeline_failure(&stats,"Recorded CD layout does not match the source.",callback,userData); goto cleanup; }
    if (config->verifyAfterWrite && !config->testMode) {
        if (config->cycleTrayBeforeVerify) {
            if (!mmc_prevent_removal(config->drive,false)) { pipeline_failure(&stats,"Cannot release the tray for verification.",callback,userData); goto cleanup; } prevented = false;
            if (!mmc_load_eject(config->drive,true) || !mmc_load_eject(config->drive,false)) { pipeline_failure(&stats,"Cannot reload the disc for verification.",callback,userData); goto cleanup; }
            if (!burn_wait_ready(config,&stats,callback,userData)) goto cleanup;
            if (!mmc_prevent_removal(config->drive,true)) { pipeline_failure(&stats,"Cannot prevent medium removal during verification.",callback,userData); goto cleanup; } prevented = true;
            MmcDiscInfo loaded;
            if (!mmc_read_disc_information(config->drive,&loaded)) { pipeline_failure(&stats,"Cannot inspect reloaded media.",callback,userData); goto cleanup; }
            if (loaded.currentProfile != finalDisc.currentProfile || loaded.discStatus != finalDisc.discStatus || loaded.lastSessionStatus != finalDisc.lastSessionStatus || loaded.numSessions != finalDisc.numSessions || loaded.firstTrack != finalDisc.firstTrack || loaded.lastTrack != finalDisc.lastTrack) { burn_failure(&stats,ERROR_MEDIA_CHANGED,"Reloaded disc layout does not match the recorded disc.",callback,userData); goto cleanup; }
        }
        VerifyConfig verify = {0}; verify.drive = config->drive; verify.imagePath = config->imagePath; verify.cancelEvent = config->cancelEvent;
        BurnVerifyProgress progress = {&stats,callback,userData}; if (!pipeline_verify_disc_ex(&verify,burn_verify_progress,&progress)) goto cleanup;
    }
    if (burn_cancelled(config,&stats,callback,userData)) goto cleanup; success = true;
cleanup:
    error = GetLastError();
    if (producer) { InterlockedExchange(&context.abortRequested,1); ringbuffer_set_finished(&rb); if (!success) CancelSynchronousIo(producer); WaitForSingleObject(producer,INFINITE); CloseHandle(producer); }
    /* 已发出的写入不可回滚；SAO/DAO 的错误冲刷可能留下无法继续写入的部分光盘。 */
    if (writeAttempted && !syncAttempted) { if (profile == MMC_PROFILE_CD_R) mmc_finish_cd_sao(config->drive); else mmc_synchronize_cache(config->drive); }
    if (parametersChanged && (config->testMode || !writeAttempted) && !burn_restore_parameters(config->drive,&original) && success) { error = GetLastError(); success = burn_failure(&stats,error,"Cannot restore device write parameters.",callback,userData); }
    if (prevented && !mmc_prevent_removal(config->drive,false) && success) { error = GetLastError(); success = burn_failure(&stats,error,"Cannot restore medium removal.",callback,userData); }
    if (ownedLock && !spti_lock_drive(config->drive,false) && success) { error = GetLastError(); success = burn_failure(&stats,error,"Cannot unlock target volume.",callback,userData); }
    ringbuffer_destroy(&rb); free(buffers); burn_cd_free(cdImage); CloseHandle(source);
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
