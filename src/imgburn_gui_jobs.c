#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_gui_internal.h"
#include "imgburn_audio.h"
#include "imgburn_pipeline.h"
#include "imgburn_preview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

const wchar_t* imgburn_gui_basename(const wchar_t* path) {
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; p++) if ((*p == L'\\' || *p == L'/') && p[1]) name = p + 1;
    return name;
}

static void job_text(GuiJob* job, const wchar_t* format, ...) {
    size_t used = wcslen(job->result); if (used >= 8191) return;
    va_list args; va_start(args, format); _vsnwprintf_s(job->result + used, 8192 - used, _TRUNCATE, format, args); va_end(args);
}
static bool job_cancelled(const GuiJob* job) { return job->cancel && WaitForSingleObject(job->cancel, 0) == WAIT_OBJECT_0; }
static void job_progress(int percent, uint32_t current, uint32_t total, void* context) {
    (void)current; (void)total; GuiJob* job = (GuiJob*)context;
    if (job->owner) PostMessageW(job->owner, GUI_JOB_PROGRESS, (WPARAM)percent, (LPARAM)job);
}
static void job_pipeline_progress(const PipelineStats* stats, void* context) { job_progress(stats->progressPercent, stats->currentLba, stats->totalSectors, context); }

static bool job_prepare(GuiJob* job) {
    char label[512]; if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, job->label, -1, label, sizeof(label), NULL, NULL) || !image_builder_init(&job->builder, label)) return false;
    job->builderReady = true; job->builder.cancelEvent = job->cancel;
    job->builder.config.fsTypes = job->fsTypes; job->builder.config.udfRev = job->revision; job->builder.config.optimizeDuplicateFiles = job->optimize;
    job->builder.config.dvdVideoCompliance = job->dvdVideo; job->builder.config.blurayCompliance = job->bluray;
    if (job->dvdVideo) { job->builder.config.fsTypes = FS_TYPE_ISO_UDF; job->builder.config.udfRev = UDF_REV_102; }
    if (job->bluray) { job->builder.config.fsTypes = FS_TYPE_UDF; job->builder.config.udfRev = UDF_REV_250; }
    for (size_t i = 0; i < job->sourceCount; i++) {
        if (job_cancelled(job)) return false;
        const wchar_t* path = job->sources[i].path; DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES) return false;
        bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const wchar_t* name = directory && job->sourceCount == 1 ? L"" : imgburn_gui_basename(path);
        if (!(directory ? image_builder_add_folder(&job->builder, path, name) : image_builder_add_file(&job->builder, path, name))) return false;
    }
    if (!image_builder_calculate_layout(&job->builder)) return false;
    ImageBuildConfig* config = &job->builder.config;
    job_text(job, L"文件数量：%u\r\n文件夹数量：%u\r\n总计文件大小：%llu 字节\r\n镜像大小：%llu 字节\r\n镜像扇区：%u\r\n去重节省：%llu 字节\r\n", config->totalFiles, config->totalDirectories, config->totalSourceBytes, (uint64_t)config->totalImageSectors * 2048, config->totalImageSectors, config->deduplicatedBytesSaved);
    return true;
}

static bool job_build(GuiJob* job) {
    if (!job_prepare(job)) return false;
    if (job->dvdVideo && job->builder.config.totalImageSectors > 2295104) { job_text(job, L"此镜像需要双层单元选择；请先使用 dvd-layers 与 build --dvd-break 完成布局。\r\n"); SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (!image_builder_create_iso(&job->builder, job->destination, job_progress, job)) return false;
    if (job->dvdVideo) {
        wchar_t mds[MAX_PATH]; wcscpy(mds, job->destination); wchar_t* dot = wcsrchr(mds, L'.');
        if (!dot || dot < imgburn_gui_basename(mds)) dot = mds + wcslen(mds);
        if ((size_t)(dot - mds) + 4 >= MAX_PATH) return false;
        wcscpy(dot, L".mds");
        if (!imgburn_mds_create(mds, job->destination, job->builder.config.totalImageSectors, 0)) { job_text(job, L"ISO 已生成，但 MDS 生成失败。\r\n"); return false; }
    }
    job_text(job, L"镜像已生成：%ls\r\n", job->destination); return true;
}

static bool job_inspect(GuiJob* job) {
    const wchar_t* path = job->sources[0].path; DiscImageType type = imgburn_image_detect_type(path);
    job_text(job, L"来源：%ls\r\n", path);
    if (type == IMAGE_TYPE_CUE) {
        CueSheet* cue = (CueSheet*)calloc(1, sizeof(*cue)); if (!cue) return false;
        bool ok = imgburn_cue_parse(path, cue);
        if (ok) { job_text(job, L"CUE：%u 会话，%d 轨道，%u 扇区\r\n", cue->sessionCount, cue->trackCount, cue->totalSectors); for (int i = 0; i < cue->trackCount; i++) job_text(job, L"会话 %u / 轨道 %d：LBA %u，长度 %u，扇区 %u 字节\r\n", cue->tracks[i].sessionNumber, cue->tracks[i].trackNumber, cue->tracks[i].startLba, cue->tracks[i].sectorCount, cue->tracks[i].sectorSize); }
        free(cue); return ok;
    }
    if (type == IMAGE_TYPE_MDS) {
        MdsSheet* mds = (MdsSheet*)calloc(1, sizeof(*mds)); if (!mds) return false;
        bool ok = imgburn_mds_parse(path, mds);
        if (ok) { job_text(job, L"MDS：%u 会话，%d 轨道，%u 扇区\r\n分卷：%u\r\n层切换：%u\r\n路径：%ls\r\n", mds->sessionCount, mds->trackCount, mds->totalSectors, mds->dataFileCount, mds->layerBreakLba, mds->oppositeTrackPath ? L"OTP" : L"PTP"); for (unsigned i = 0; i < mds->dataFileCount; i++) job_text(job, L"%ls\r\n", mds->files[i].path); }
        free(mds); return ok;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size; bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart % 2048 == 0;
    if (ok) {
        job_text(job, L"大小：%llu 字节\r\n扇区：%llu (MODE1/2048)\r\n", (uint64_t)size.QuadPart, (uint64_t)size.QuadPart / 2048);
        LARGE_INTEGER position; position.QuadPart = 16 * 2048; uint8_t descriptor[2048]; DWORD got = 0;
        if (SetFilePointerEx(file, position, NULL, FILE_BEGIN) && ReadFile(file, descriptor, sizeof(descriptor), &got, NULL) && got == sizeof(descriptor) && descriptor[0] == 1 && !memcmp(descriptor + 1, "CD001", 5)) {
            wchar_t label[33]; for (unsigned i = 0; i < 32; i++) label[i] = descriptor[40 + i]; label[32] = 0;
            for (int i = 31; i >= 0 && label[i] == L' '; i--) label[i] = 0; job_text(job, L"ISO9660 卷标：%ls\r\n", label);
        }
    }
    CloseHandle(file); return ok;
}

static bool job_cue(GuiJob* job) {
    if (job->sourceCount > MAX_AUDIO_TRACKS) return false;
    AudioCompilation* comp = (AudioCompilation*)calloc(1, sizeof(*comp)); if (!comp) return false;
    bool ok = true; comp->sessionCount = 1;
    for (size_t i = 0; i < job->sourceCount && ok; i++) {
        uint64_t pcmBytes = 0; ok = !job_cancelled(job) && audio_measure_decoded(job->sources[i].path, &pcmBytes);
        uint64_t sectors = (pcmBytes + 2351) / 2352; if (!ok || !sectors || sectors >= 100 * 60 * 75 || comp->totalSectors > UINT32_MAX - sectors) { ok = false; break; }
        AudioTrack* track = &comp->tracks[i]; wcscpy(track->filePath, job->sources[i].path); track->trackNumber = (int)i + 1; track->sourceType = CUE_FILE_WAVE;
        const wchar_t* ext = wcsrchr(track->filePath, L'.');
        if (!ext || _wcsicmp(ext, L".wav")) track->sourceType = ext && (!_wcsicmp(ext, L".aif") || !_wcsicmp(ext, L".aiff")) ? CUE_FILE_AIFF : ext && !_wcsicmp(ext, L".mp3") ? CUE_FILE_MP3 : CUE_FILE_AUDIO;
        track->sourceIsWave = track->sourceType == CUE_FILE_WAVE; track->sourceFileGroup = (unsigned)i + 1; track->sessionNumber = 1; track->decodedSourceSectors = (uint32_t)sectors; track->sectorCount = (uint32_t)sectors; track->startLba = comp->totalSectors;
        track->duration = lba_to_msf((uint32_t)sectors); comp->totalSectors += (uint32_t)sectors; comp->trackCount++; job_progress((int)((i + 1) * 100 / job->sourceCount), 0, 0, job);
    }
    if (ok) ok = !job_cancelled(job) && cue_sheet_generate(comp, job->destination);
    if (ok) job_text(job, L"CUE 已生成：%ls\r\n轨道：%d，扇区：%u\r\n", job->destination, comp->trackCount, comp->totalSectors);
    free(comp); return ok;
}

static bool job_mds(GuiJob* job) {
    if (job->sourceCount > MAX_MDS_FILES) return false;
    const wchar_t** paths = (const wchar_t**)calloc(job->sourceCount, sizeof(*paths)); if (!paths) return false; uint64_t bytes = 0; bool ok = true;
    for (size_t i = 0; i < job->sourceCount; i++) {
        paths[i] = job->sources[i].path; WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(paths[i], GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { ok = false; break; }
        uint64_t part = (uint64_t)data.nFileSizeHigh << 32 | data.nFileSizeLow;
        if (!part || part % 2048 || bytes > UINT64_MAX - part) { ok = false; break; } bytes += part;
    }
    if (ok) ok = !job_cancelled(job) && bytes / 2048 <= UINT32_MAX && imgburn_mds_create_files_ex(job->destination, paths, job->sourceCount, (uint32_t)(bytes / 2048), job->layerBreak, job->oppositeTrackPath);
    free(paths); if (ok) job_text(job, L"MDS 已生成：%ls\r\n扇区：%llu，分卷：%zu\r\n", job->destination, bytes / 2048, job->sourceCount); return ok;
}

static bool job_drive(GuiJob* job) {
    OpticalDrive drive; if (!spti_open_drive(&drive, job->drive, job->operation == GUI_WRITE_DISC)) return false;
    bool ok = false; uint32_t last = 0, bytes = 0; bool ready = mmc_test_unit_ready(&drive);
    job->mediaReady = ready;
    if (job->operation == GUI_DRIVE_INFO) {
        uint8_t inquiry[36] = {0}; ok = mmc_inquiry(&drive, inquiry, sizeof(inquiry));
        if (ok) { char text[29]; memcpy(text, inquiry + 8, 28); text[28] = 0; wchar_t wide[29]; MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 29); job_text(job, L"%ls\r\n%ls\r\n", wide, ready ? L"介质已就绪" : L"介质未就绪或未插入光盘"); }
        if (ready && mmc_read_capacity(&drive, &last, &bytes)) job_text(job, L"容量：%llu 扇区，每扇区 %u 字节\r\n", (uint64_t)last + 1, bytes);
    } else if (!ready) SetLastError(ERROR_NOT_READY);
    else if (job->operation == GUI_READ_DISC) {
        ReadConfig config = {0}; config.drive = &drive; config.imagePath = job->destination; config.cancelEvent = job->cancel; config.retryCount = 20; config.replaceExisting = job->replaceExisting;
        const wchar_t* extension = wcsrchr(job->destination,L'.'); ok = extension && !_wcsicmp(extension,L".cue") ? pipeline_read_cd(&config,job_pipeline_progress,job) : pipeline_read_disc(&config,job_pipeline_progress,job);
        if (ok) job_text(job,L"光盘镜像已读取：%ls\r\n",job->destination);
        else { job->error = GetLastError(); if (job->error == ERROR_NOT_SUPPORTED) job_text(job,L"当前光盘布局或光驱能力不支持所选镜像格式。\r\n"); else if (job->error == ERROR_DISK_FULL) job_text(job,L"目标位置的可用空间不足。\r\n"); }
    }
    else if (job->operation == GUI_VERIFY_DISC) {
        VerifyConfig config = {0}; config.drive = &drive; config.imagePath = job->sources[0].path; config.cancelEvent = job->cancel;
        ok = pipeline_verify_disc_ex(&config,job_pipeline_progress,job); if (!ok) job->error = GetLastError();
        if (ok) job_text(job,job->sources[0].path[0] ? L"光盘数据与所选镜像一致。\r\n" : L"光盘数据扇区可读性检查通过。\r\n");
    }
    else if (mmc_read_capacity(&drive, &last, &bytes) && bytes == 2048) {
        WIN32_FILE_ATTRIBUTE_DATA data; const wchar_t* image = job->sources[0].path; DiscImageType type = imgburn_image_detect_type(image);
        if ((type == IMAGE_TYPE_ISO || type == IMAGE_TYPE_BIN) && GetFileAttributesExW(image, GetFileExInfoStandard, &data)) {
            uint64_t length = (uint64_t)data.nFileSizeHigh << 32 | data.nFileSizeLow;
            if (length && length % 2048 == 0 && length / 2048 <= (uint64_t)last + 1) { BurnConfig config = {0}; config.drive = &drive; config.imagePath = image; config.writeSpeedKbps = job->speed; config.verifyAfterWrite = job->verify; config.ringBufferBytes = 32 * 1024 * 1024; ok = pipeline_burn_image(&config, job_pipeline_progress, job); }
        }
    }
    if (!ok && !job->error) job->error = drive.lastError ? drive.lastError : GetLastError();
    if (!ok && job->error == ERROR_ACCESS_DENIED && drive.lastError == ERROR_ACCESS_DENIED) job_text(job,drive.accessKnown && (drive.deviceAccess & (GENERIC_READ | GENERIC_WRITE)) != (GENERIC_READ | GENERIC_WRITE) ? L"未取得 SPTI 所需的设备读写访问权，请尝试以管理员身份运行。\r\n" : L"光驱或驱动程序拒绝了设备命令。\r\n");
    spti_close_drive(&drive); return ok;
}

DWORD WINAPI imgburn_gui_job_run(void* context) {
    GuiJob* job = (GuiJob*)context; SetLastError(ERROR_SUCCESS);
    if (!job) return 1;
    if (!job->sourceCount || !job->sources) { job->error = ERROR_INVALID_PARAMETER; if (job->owner) PostMessageW(job->owner, GUI_JOB_DONE, 0, (LPARAM)job); return 1; }
    if (!job_cancelled(job)) switch (job->operation) {
        case GUI_CALCULATE: job->succeeded = job_prepare(job); break;
        case GUI_BUILD_IMAGE: job->succeeded = job_build(job); break;
        case GUI_INSPECT: job->succeeded = job_inspect(job); break;
        case GUI_CREATE_CUE: job->succeeded = job_cue(job); break;
        case GUI_CREATE_MDS: job->succeeded = job_mds(job); break;
        case GUI_DRIVE_INFO: case GUI_VERIFY_DISC: case GUI_WRITE_DISC: case GUI_READ_DISC: job->succeeded = job_drive(job); break;
        case GUI_DVD_INFO: job->dvdVideo = true; job->succeeded = job_prepare(job) && imgburn_dvd_read_navigation(&job->builder, &job->navigation); break;
        case GUI_DVD_PREVIEW: job->succeeded = imgburn_dvd_preview_show(&job->builder, &job->preview, NULL) == 0; break;
    }
    if (!job->succeeded && job_cancelled(job)) job->error = ERROR_CANCELLED;
    if (!job->succeeded) { if (!job->error) job->error = GetLastError(); if (!job->error) job->error = ERROR_INVALID_DATA; job_text(job, job->error == ERROR_CANCELLED ? L"操作已取消。\r\n" : L"操作失败（错误 %lu）。\r\n", job->error); }
    if (job->owner) PostMessageW(job->owner, GUI_JOB_DONE, 0, (LPARAM)job); return job->succeeded ? 0 : 1;
}

void imgburn_gui_job_free(GuiJob* job) {
    if (!job) return;
    if (job->thread) CloseHandle(job->thread); if (job->cancel) CloseHandle(job->cancel);
    imgburn_dvd_free_navigation(&job->navigation); if (job->builderReady) image_builder_destroy(&job->builder); free(job->sources); free(job);
}
