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
static bool job_read_destination_info(GuiJob* job) {
    job->readDestinationReady = false; job->readDestinationFreeBytes = job->readDestinationTotalBytes = 0; job->readDestinationFileSystem[0] = 0;
    wchar_t parent[MAX_PATH], volume[MAX_PATH], system[40]; DWORD length = GetFullPathNameW(job->destination,MAX_PATH,parent,NULL);
    if (!length || length >= MAX_PATH) { SetLastError(ERROR_INVALID_NAME); return false; }
    wchar_t* slash = wcsrchr(parent,L'\\'); if (!slash || !slash[1]) { SetLastError(ERROR_INVALID_NAME); return false; } slash[1] = 0;
    ULARGE_INTEGER available, total; if (!GetDiskFreeSpaceExW(parent,&available,&total,NULL)) return false;
    if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); return false; }
    if (!GetVolumePathNameW(parent,volume,MAX_PATH) || !GetVolumeInformationW(volume,NULL,0,NULL,NULL,NULL,system,40)) return false;
    if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); return false; }
    job->readDestinationFreeBytes = available.QuadPart; job->readDestinationTotalBytes = total.QuadPart; wcscpy(job->readDestinationFileSystem,system); job->readDestinationReady = true;
    job_text(job,L"目标位置：%ls\r\n容量：%llu 字节\r\n可用空间：%llu 字节\r\n文件系统：%ls\r\n",parent,job->readDestinationTotalBytes,job->readDestinationFreeBytes,system); return true;
}
static void job_progress(int percent, uint32_t current, uint32_t total, void* context) {
    (void)current; (void)total; GuiJob* job = (GuiJob*)context;
    if (job->owner) PostMessageW(job->owner, GUI_JOB_PROGRESS, (WPARAM)percent, (LPARAM)job);
}
static void job_pipeline_progress(const PipelineStats* stats, void* context) {
    GuiJob* job = context; if (job->operation == GUI_VERIFY_DISC || job->operation == GUI_DISCOVERY_DISC) { job->comparedSectors = stats->comparedSectors; job->readableOnlySectors = stats->readableOnlySectors; job->miscompares = stats->miscomparesCount; }
    if (job->owner && stats->statusMessage[0] && strcmp(job->lastStatus,stats->statusMessage)) {
        strcpy(job->lastStatus,stats->statusMessage); wchar_t* text = (wchar_t*)calloc(256,sizeof(wchar_t));
        if (text) { MultiByteToWideChar(CP_UTF8,0,stats->statusMessage,-1,text,256); if (!PostMessageW(job->owner,GUI_JOB_STATUS,(WPARAM)job,(LPARAM)text)) free(text); }
    }
    job_progress(stats->progressPercent, stats->currentLba, stats->totalSectors, context);
}

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

static void job_source_ascii(wchar_t* output, size_t capacity, const uint8_t* input, size_t bytes) {
    size_t length = bytes; while (length && (input[length - 1] == ' ' || !input[length - 1])) length--;
    for (size_t i = 0; i < length; i++) if (input[i] < 32 || input[i] > 126) return;
    if (length >= capacity) length = capacity - 1; for (size_t i = 0; i < length; i++) output[i] = input[i]; output[length] = 0;
}

/* 只识别已读到的描述符标志，不以摘要代替写入格式检查；不解析新的原始轨道地址框架。 */
static bool job_source_descriptors(GuiJob* job, HANDLE file, DataImageSource* source, uint64_t sectors, bool logLabel) {
    bool iso = false, joliet = false, udf = false;
    for (uint32_t lba = 16; lba < 32 && lba < sectors; lba++) {
        if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); return false; }
        uint8_t descriptor[2048]; bool read;
        if (source) read = imgburn_data_source_read(source,lba,1,descriptor);
        else { LARGE_INTEGER position; position.QuadPart = (LONGLONG)lba * 2048; DWORD got = 0; read = SetFilePointerEx(file,position,NULL,FILE_BEGIN) && ReadFile(file,descriptor,sizeof(descriptor),&got,NULL) && got == sizeof(descriptor); }
        /* 原镜像信息流程允许附加卷标读取失败；基本文件/描述解析结果继续保留。 */
        if (!read) break;
        if (descriptor[6] == 1 && !memcmp(descriptor + 1,"CD001",5)) {
            if (descriptor[0] == 1 && !iso) {
                iso = true; job_source_ascii(job->sourceVolume,128,descriptor + 40,32);
                /* ECMA-119 8.4.22：Application Identifier为BP575–702，不冒充UDF实现标识。 */
                if (descriptor[574] != '_') job_source_ascii(job->sourceImplementation,128,descriptor + 574,128);
                if (logLabel && lba == 16) { wchar_t label[33]; for (unsigned i = 0; i < 32; i++) label[i] = descriptor[40 + i]; label[32] = 0; for (int i = 31; i >= 0 && label[i] == L' '; i--) label[i] = 0; job_text(job,L"ISO9660 卷标：%ls\r\n",label); }
            } else if (descriptor[0] == 2 && descriptor[88] == '%' && descriptor[89] == '/' && (descriptor[90] == '@' || descriptor[90] == 'C' || descriptor[90] == 'E')) joliet = true;
        }
        if (!descriptor[0] && descriptor[6] == 1 && (!memcmp(descriptor + 1,"NSR02",5) || !memcmp(descriptor + 1,"NSR03",5))) udf = true;
    }
    if (iso) wcscpy(job->sourceFileSystems,L"ISO9660");
    if (iso && joliet) wcscat_s(job->sourceFileSystems,64,L", Joliet");
    if (udf) wcscat_s(job->sourceFileSystems,64,iso ? L", UDF" : L"UDF");
    if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); return false; } return true;
}

static bool job_inspect(GuiJob* job) {
    const wchar_t* path = job->sources[0].path; DiscImageType type = imgburn_image_detect_type(path);
    job->sourceInfoReady = false; job->sourceBytes = 0; job->sourceSectors = job->sourceSectorBytes = 0; job->sourceVolume[0] = job->sourceImplementation[0] = job->sourceFileSystems[0] = 0;
    job_text(job, L"来源：%ls\r\n", path);
    if (type == IMAGE_TYPE_CUE) {
        CueSheet* cue = (CueSheet*)calloc(1, sizeof(*cue)); if (!cue) return false;
        bool ok = imgburn_cue_parse(path, cue);
        if (ok) { job_text(job, L"CUE：%u 会话，%d 轨道，%u 扇区\r\n", cue->sessionCount, cue->trackCount, cue->totalSectors); for (int i = 0; i < cue->trackCount; i++) job_text(job, L"会话 %u / 轨道 %d：LBA %u，长度 %u，扇区 %u 字节\r\n", cue->tracks[i].sessionNumber, cue->tracks[i].trackNumber, cue->tracks[i].startLba, cue->tracks[i].sectorCount, cue->tracks[i].sectorSize); }
        if (ok) { job->sourceSectors = cue->totalSectors; uint32_t bytes = cue->trackCount ? cue->tracks[0].sectorSize : 0; for (int i = 1; i < cue->trackCount; i++) if (cue->tracks[i].sectorSize != bytes) bytes = 0; job->sourceSectorBytes = bytes; job->sourceBytes = (uint64_t)cue->totalSectors * bytes; }
        if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); ok = false; } job->sourceInfoReady = ok; free(cue); return ok;
    }
    if (type == IMAGE_TYPE_MDS) {
        MdsSheet* mds = (MdsSheet*)calloc(1, sizeof(*mds)); if (!mds) return false;
        bool ok = imgburn_mds_parse_source(path, mds);
        if (ok) { job_text(job, L"MDS来源：%u 会话，%d 轨道，%u 扇区\r\n分卷：%u\r\n描述层数：%u\r\n来源层提示（仅描述）：%u\r\n来源路径：%ls\r\n", mds->sessionCount, mds->trackCount, mds->totalSectors, mds->dataFileCount, mds->layerCount, mds->layerBreakLba, mds->oppositeTrackPath ? L"OTP" : L"PTP"); for (unsigned i = 0; i < mds->dataFileCount; i++) job_text(job, L"%ls\r\n", mds->files[i].path); }
        if (ok) {
            job->sourceSectors = mds->totalSectors; uint32_t bytes = mds->trackCount ? mds->tracks[0].sectorSize : 0; for (int i = 1; i < mds->trackCount; i++) if (mds->tracks[i].sectorSize != bytes) bytes = 0; job->sourceSectorBytes = bytes; job->sourceBytes = (uint64_t)mds->totalSectors * bytes;
            DataImageSource source = {0}; if (imgburn_data_source_open(&source,path)) { job->sourceSectors = source.sectors; job->sourceSectorBytes = 2048; job->sourceBytes = (uint64_t)source.sectors * 2048; ok = job_source_descriptors(job,NULL,&source,source.sectors,false); imgburn_data_source_close(&source); }
        }
        if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); ok = false; } job->sourceInfoReady = ok; free(mds); return ok;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size; bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart % 2048 == 0;
    if (ok) {
        job_text(job, L"大小：%llu 字节\r\n扇区：%llu (MODE1/2048)\r\n", (uint64_t)size.QuadPart, (uint64_t)size.QuadPart / 2048);
        uint64_t sectors = (uint64_t)size.QuadPart / 2048; job->sourceBytes = (uint64_t)size.QuadPart; job->sourceSectorBytes = 2048; if (sectors <= UINT32_MAX) job->sourceSectors = (uint32_t)sectors;
        ok = job_source_descriptors(job,file,NULL,sectors,true);
    }
    CloseHandle(file); job->sourceInfoReady = ok; return ok;
}

static bool job_dvd_candidates(GuiJob* job) {
    const DvdMediaProfile profile = {0,2086912,4173824}; size_t count = 0;
    job_progress(0,0,0,job);
    /* 保留全部来源的只读句柄，候选在窗口及明确选择存续期间始终对应同一份来源。 */
    if (!imgburn_data_source_open(&job->dvdCandidateSource,job->sources[0].path)) goto failed;
    bool ok = imgburn_dvd_image_layer_break_candidates_ex(job->sources[0].path,&profile,NULL,0,&count,&job->dvdImageSectors,job->cancel);
    if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); goto failed; }
    if (!ok && GetLastError() != ERROR_INSUFFICIENT_BUFFER) goto failed;
    if (count) {
        job->dvdCandidates = calloc(count,sizeof(*job->dvdCandidates)); if (!job->dvdCandidates) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto failed; }
        if (!imgburn_dvd_image_layer_break_candidates_ex(job->sources[0].path,&profile,job->dvdCandidates,count,&job->dvdCandidateCount,&job->dvdImageSectors,job->cancel)) goto failed;
    }
    if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); goto failed; }
    if (!job->dvdCandidateCount) {
        imgburn_data_source_close(&job->dvdCandidateSource);
        job_text(job,L"分析完成：没有符合标准双层容量且已对齐的视频单元候选。\r\n未填写 L0；手工值、MDS 层位置或数据自动规则仍按原流程处理，数据自动规则不代表视频单元选择。\r\n");
    } else job_text(job,L"找到 %zu 个 DVD-Video 单元候选（镜像 %u 扇区）。\r\n请明确选择单元后填写 L0；查询不会自动选择最佳单元或修改镜像。\r\n",job->dvdCandidateCount,job->dvdImageSectors);
    job_progress(100,0,0,job); return true;
failed:
    job->error = GetLastError(); if (!job->error) job->error = ERROR_INVALID_DATA;
    free(job->dvdCandidates); job->dvdCandidates = NULL; job->dvdCandidateCount = 0; job->dvdImageSectors = 0; imgburn_data_source_close(&job->dvdCandidateSource);
    if (job->error == ERROR_NOT_SUPPORTED) job_text(job,L"此来源不支持成品 DVD-Video 单元分析。仅支持现有 ISO/BIN/数据 MDS 范围；UDF-only、复杂角度和交错尚未支持。\r\n");
    else if (job->error != ERROR_CANCELLED) job_text(job,L"读取或分析来源失败（错误 %lu），没有保留候选。\r\n",job->error);
    return false;
}

static void job_cd_iso_layout(GuiJob* job, const MmcCdAppendPlan* data) {
    job_text(job,L"会话：%u\r\n轨号：%u\r\n原末会话 lead-out：%u\r\n下一可写位置 NWA：%u\r\n",data->sessionNumber,data->trackNumber,data->previousLeadOutLba,data->nextWritableAddress);
    job_text(job,L"数据起始 LBA：%u\r\n数据结束 LBA（不含）：%u\r\n来源扇区：%u\r\n可用数据扇区：%u\r\n剩余数据扇区：%u\r\n",data->dataStartLba,data->dataEndLba,data->sourceSectors,data->availableDataSectors,data->remainingDataSectors);
    job_text(job,L"扇区字节：%u\r\n写类型：%u（TAO）\r\n轨道模式：%u（MODE1）\r\n数据块类型：%u（2048）\r\n",data->sectorBytes,data->writeType,data->trackMode,data->dataBlockType);
}

/* 独立ISO扩展不经过原Write打开流程；默认规划只查询，写入只消费明确选择的计划。 */
static bool job_cd_iso(GuiJob* job) {
    bool writing = job->operation == GUI_CD_ISO_WRITE, opened = false, ok = false; DWORD error = ERROR_SUCCESS; OpticalDrive drive = {0};
    if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); goto done; }
    const wchar_t* extension = wcsrchr(job->sources[0].path,L'.'); if (!extension || _wcsicmp(extension,L".iso")) { SetLastError(ERROR_NOT_SUPPORTED); goto done; }
    if (writing && (!job->cdPlanSelected || !job->cdPlan.sourceSectors)) { SetLastError(ERROR_INVALID_PARAMETER); goto done; }
    if (!job->cdIsoSource.file || job->cdIsoSource.file == INVALID_HANDLE_VALUE) {
        if (!imgburn_iso_session_open(&job->cdIsoSource,job->sources[0].path,job->cancel)) goto done;
    }
    job->cdIsoSource.cancelEvent = job->cancel;
    /* 先固定来源再查询光驱；Joliet、UDF及坏来源均不进入设备路径。 */
    if (job->cdIsoSource.joliet) { SetLastError(ERROR_NOT_SUPPORTED); goto done; }
    if (job->cdIsoSource.sectors < 300) { SetLastError(ERROR_INVALID_DATA); goto done; }
    if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); goto done; }
    job_progress(0,0,job->cdIsoSource.sectors,job);
    if (!spti_open_drive_passive(&drive,job->drive)) goto done; opened = true;
    if (drive.accessKnown && (drive.deviceAccess & (GENERIC_READ | GENERIC_WRITE)) != (GENERIC_READ | GENERIC_WRITE)) { SetLastError(ERROR_ACCESS_DENIED); goto done; }
    CdIsoSessionConfig config = {&drive,job->sources[0].path,job->cancel,job->cdAllowBlank,job->cdFinalize};
    if (writing) ok = pipeline_write_cd_iso_session_planned(&config,&job->cdPlan,&job->cdResult,job_pipeline_progress,job);
    else {
        ok = pipeline_plan_cd_iso_session(&config,&job->cdPlan);
        if (ok) ok = imgburn_iso_session_set_start(&job->cdIsoSource,job->cdPlan.dataStartLba);
        if (ok && job_cancelled(job)) { SetLastError(ERROR_CANCELLED); ok = false; }
    }
done:
    error = ok ? ERROR_SUCCESS : GetLastError(); if (!ok && !error) error = ERROR_INVALID_DATA;
    if (opened) spti_close_drive(&drive);
    if (writing) {
        if (job->cdResult.data.sourceSectors) { job_text(job,L"本次执行布局：\r\n"); job_cd_iso_layout(job,&job->cdResult.data); }
        job_text(job,L"writeStarted：%u\r\nsessionClosed：%u\r\nverified：%u\r\n",job->cdResult.writeStarted ? 1 : 0,job->cdResult.sessionClosed ? 1 : 0,job->cdResult.verified ? 1 : 0);
        if (ok) job_text(job,L"独立 ISO 会话写入完成并通过全部回读校验。\r\n");
        else if (error == ERROR_MEDIA_CHANGED) job_text(job,job->cdResult.writeStarted ? L"介质或布局发生变化；本次已开始写入，请核对以上实际状态，不自动重试。\r\n" : L"当前介质或布局与所选计划不一致，未开始写入；请重新查询并选择。\r\n");
        job->cdPlanSelected = false;
    } else if (ok) {
        job_text(job,L"独立纯 ISO9660 会话规划（只读）：\r\n"); job_cd_iso_layout(job,&job->cdPlan);
        job_text(job,L"空白首会话：%ls\r\n结束选择：%ls\r\n此查询没有写入；需要明确选择此计划后再执行写入并回读。不合并旧文件树。\r\n",job->cdAllowBlank ? L"显式允许" : L"不允许",job->cdFinalize ? L"封盘" : L"留盘开放");
        job_progress(100,0,job->cdIsoSource.sectors,job);
    }
    if (!ok) {
        job->error = error; job->cdPlanSelected = false; memset(&job->cdPlan,0,sizeof(job->cdPlan));
        if (error == ERROR_NOT_SUPPORTED) job_text(job,L"仅支持已确认 TAO/MODE1 的可追加 CD-R 与独立纯 ISO9660；Joliet、UDF、引导等来源和其他介质不在此入口范围。\r\n");
        else if (error == ERROR_ACCESS_DENIED) job_text(job,L"未取得光驱命令访问权，或设备拒绝了命令；没有更改权限。\r\n");
        else if (error == ERROR_DISK_FULL) job_text(job,L"当前可用数据扇区不足。\r\n");
    }
    if (writing || !ok) imgburn_iso_session_close(&job->cdIsoSource);
    SetLastError(error); return ok;
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
    if (!job->sources || !job->sourceCount || job->sourceCount > MAX_MDS_FILES) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    const wchar_t* firstDot = wcsrchr(job->sources[0].path,L'.'); bool list = firstDot && !_wcsicmp(firstDot,L".lst"), gi = firstDot && !_wcsicmp(firstDot,L".gi"), description = false;
    for (size_t i = 0; i < job->sourceCount; i++) {
        const wchar_t* dot = wcsrchr(job->sources[i].path,L'.');
        if (dot && !_wcsicmp(dot,L".dvd")) description = true;
    }
    if (description || list || gi) {
        if (!list && !gi && job->sourceCount != 1) { SetLastError(ERROR_NOT_SUPPORTED); job_text(job,L"请单独选择一个 .dvd 描述文件。\r\n"); return false; }
        if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); return false; }
        MdsToolsInput* input = calloc(1,sizeof(*input)); if (!input) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
        bool ok = list ? imgburn_mds_load_tools_lst(job->sources[0].path,input) : gi ? imgburn_mds_load_tools_gi(job->sources[0].path,input) : imgburn_mds_load_tools_dvd(job->sources[0].path,input);
        DWORD loadError = GetLastError();
        if (!ok && (list || gi) && loadError == ERROR_BAD_FORMAT) { free(input); goto raw_files; }
        if (ok && job_cancelled(job)) { SetLastError(ERROR_CANCELLED); ok = false; }
        if (ok) ok = imgburn_mds_create_tools_input_ex(job->destination,input,job->mdsLayerBreakOverride,job->layerBreak,job->oppositeTrackPath,job->mdsMedium,job->mdsPreservePaths,job->mdsAnsiFilenames);
        DWORD error = GetLastError();
        if (ok) job_text(job,L"MDS 已生成：%ls\r\n扇区：%u，分卷：%u\r\n",job->destination,input->totalSectors,input->fileCount);
        free(input); SetLastError(error); return ok;
    }
raw_files:;
    const wchar_t** paths = (const wchar_t**)calloc(job->sourceCount, sizeof(*paths)); if (!paths) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; } uint64_t bytes = 0; bool ok = true;
    for (size_t i = 0; i < job->sourceCount; i++) {
        if (job_cancelled(job)) { SetLastError(ERROR_CANCELLED); ok = false; break; }
        paths[i] = job->sources[i].path; WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW(paths[i], GetFileExInfoStandard, &data)) { ok = false; break; }
        uint64_t part = (uint64_t)data.nFileSizeHigh << 32 | data.nFileSizeLow;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !part || part % 2048 || bytes > UINT64_MAX - part) { SetLastError(ERROR_INVALID_DATA); ok = false; break; } bytes += part;
    }
    if (ok && job_cancelled(job)) { SetLastError(ERROR_CANCELLED); ok = false; }
    if (ok && bytes / 2048 > INT32_MAX) { SetLastError(ERROR_NOT_SUPPORTED); ok = false; }
    if (ok) ok = imgburn_mds_create_tools_files_ex(job->destination,paths,job->sourceCount,(uint32_t)(bytes / 2048),job->layerBreak,job->oppositeTrackPath,job->mdsMedium,job->mdsPreservePaths,job->mdsAnsiFilenames);
    free(paths); if (ok) job_text(job, L"MDS 已生成：%ls\r\n扇区：%llu，分卷：%zu\r\n", job->destination, bytes / 2048, job->sourceCount); return ok;
}

/* 元数据查询失败只显示未知，不改变逻辑读取资格，也不猜介质层数。 */
static bool job_bd_information(GuiJob* job, OpticalDrive* drive) {
    MmcBdDiscStructure structure;
    if (mmc_read_bd_disc_structure(drive,&structure)) {
        wchar_t physical[16] = L"未知", manufacturer[7] = L"", media[4] = L"";
        MultiByteToWideChar(CP_UTF8,0,mmc_get_bd_physical_type_name(structure.physicalType),-1,physical,16);
        job_text(job,L"当前BD Profile：0x%04X\r\nBD物理介质：%ls\r\nBD物理层数：%u\r\n",structure.currentProfile,physical,structure.layerCount);
        job_text(job,L"BD类别/版本：%u/%u\r\n",structure.discClass,structure.discVersion);
        if (structure.hasMediaId) { MultiByteToWideChar(CP_UTF8,0,structure.manufacturerId,-1,manufacturer,7); MultiByteToWideChar(CP_UTF8,0,structure.mediaTypeId,-1,media,4); job_text(job,L"BD介质ID：%ls / %ls（修订 %u）\r\n",manufacturer,media,structure.productRevision); }
        else job_text(job,L"BD介质ID：未知\r\n");
        return structure.physicalType == MMC_BD_PHYSICAL_RE;
    } else job_text(job,L"BD物理介质：未知（错误 %lu）\r\nBD物理层数：未知\r\nBD类别/版本：未知\r\nBD介质ID：未知\r\n",GetLastError());
    return false;
}
static void job_bd_capacity(GuiJob* job, OpticalDrive* drive, const MmcDiscInfo* disc) {
    if (disc && disc->discStatus == DISC_STATUS_EMPTY) {
        MmcTrackInfo track;
        if (mmc_read_track_information(drive,1,false,&track) && track.trackNumber == 1 && track.isBlank && !track.isDamage && track.nwaValid) job_text(job,L"空白轨1空余：%u 扇区（按2048字节数据计 %llu 字节）\r\n",track.freeBlocks,(uint64_t)track.freeBlocks * 2048);
        else job_text(job,L"空白轨1空余：未知\r\n");
    } else {
        uint32_t last = 0, bytes = 0;
        if (mmc_read_capacity(drive,&last,&bytes) && bytes) { uint64_t sectors = (uint64_t)last + 1; job_text(job,L"逻辑可读容量：%llu 扇区，每扇区 %u 字节（%llu 字节）\r\n",sectors,bytes,sectors * bytes); }
        else job_text(job,L"逻辑可读容量：未知\r\n");
    }
}

/* 格式描述符仍原序显示；单独的BDRE候选不代替核心写入资格复核。 */
static void job_bd_format_capacities(GuiJob* job, OpticalDrive* drive) {
    MmcFormatCapacities capacity;
    if (!mmc_read_format_capacities(drive,&capacity)) { job_text(job,L"BD-RE格式状态：未知（错误 %lu）\r\n",GetLastError()); return; }
    const wchar_t* state = capacity.descriptorCode == 1 ? L"未格式化或空白" : capacity.descriptorCode == 2 ? L"已格式化" : L"无介质或容量未知";
    job_text(job,L"BD-RE格式状态：%ls\r\n格式容量描述符：\r\n当前/最大：DT=%u，块数=%u，原始TDP=0x%06X\r\n",state,capacity.descriptorCode,capacity.blockCount,capacity.typeDependentParameter);
    if (capacity.descriptorCode == 1) job_text(job,L"最大备用区簇数：%u\r\n",capacity.typeDependentParameter);
    else if (capacity.descriptorCode == 2) job_text(job,L"已分配备用区簇数：%u\r\n",capacity.typeDependentParameter);
    else job_text(job,L"描述符数值不代表已确认的当前介质容量。\r\n");
    if (job->bdReMedia && capacity.descriptorCode == 2) { uint32_t last, bytes; if (mmc_read_capacity(drive,&last,&bytes) && last < UINT32_MAX && bytes == 2048 && capacity.blockCount == last + 1) job->bdReWritable = true; }
    if (!capacity.formatCount) job_text(job,L"可格式化描述符：设备未返回。\r\n");
    for (unsigned i = 0; i < capacity.formatCount; i++) { const MmcFormattableCapacity* format = &capacity.formats[i]; job_text(job,L"可格式化 [%u]：FT=0x%02X，块数=%u，原始TDP=0x%06X\r\n",i,format->formatType,format->blockCount,format->typeDependentParameter); }
}

static bool job_drive(GuiJob* job) {
    OpticalDrive drive; bool opened = job->operation == GUI_DRIVE_INFO || job->bdReOverwrite || job->bdReErase ? spti_open_drive_passive(&drive,job->drive) : spti_open_drive(&drive,job->drive,job->operation == GUI_WRITE_DISC || job->operation == GUI_DISCOVERY_DISC); if (!opened) return false;
    bool ok = false; uint32_t last = 0, bytes = 0; bool ready = mmc_test_unit_ready(&drive);
    job->mediaReady = ready;
    if (job->operation == GUI_DRIVE_INFO) {
        uint8_t inquiry[36] = {0}; ok = mmc_inquiry(&drive, inquiry, sizeof(inquiry));
        if (ok) { char text[29]; memcpy(text, inquiry + 8, 28); text[28] = 0; wchar_t wide[29]; MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 29); job_text(job, L"%ls\r\n%ls\r\n", wide, ready ? L"介质已就绪" : L"介质未就绪或未插入光盘"); }
        MmcProfile infoProfile = ready ? mmc_get_current_profile(&drive) : MMC_PROFILE_NONE; bool bd = infoProfile >= MMC_PROFILE_BD_ROM && infoProfile <= MMC_PROFILE_BD_RE;
        bool physicalRE = false; if (ready && bd) physicalRE = job_bd_information(job,&drive);
        if (ready && !bd && mmc_read_capacity(&drive, &last, &bytes)) job_text(job, L"容量：%llu 扇区，每扇区 %u 字节\r\n", (uint64_t)last + 1, bytes);
        if (ready && (mmc_get_current_profile(&drive) == MMC_PROFILE_DVD_R_SEQ || mmc_get_current_profile(&drive) == MMC_PROFILE_DVD_R_DL_SEQ)) { MmcDvdWriteFeatures features; if (mmc_get_dvd_write_features(&drive,&features)) job->testWriteSupported = features.testWrite; }
        if (ready && mmc_get_current_profile(&drive) == MMC_PROFILE_CD_R) { MmcCdWriteFeatures features; if (mmc_get_cd_write_features(&drive,&features)) job->testWriteSupported = features.testWrite; }
        bool bdCapacityShown = false, bdReErasable = false;
        if (ready) { MmcDiscInfo disc; if (mmc_read_disc_information(&drive,&disc)) {
            bdReErasable = disc.currentProfile == MMC_PROFILE_BD_RE && disc.isErasable;
            if (bd) { job_bd_capacity(job,&drive,&disc); bdCapacityShown = true; }
            /* 只供原Write按钮显示空白介质候选；完整能力/布局仍由既有写入worker检查。 */
            job->writeMediaReady = disc.discStatus == DISC_STATUS_EMPTY && !disc.isErasable && !disc.lastSessionStatus && disc.numSessions == 1 && disc.firstTrack == 1 && disc.lastTrack == 1 && (disc.currentProfile == MMC_PROFILE_CD_R || disc.currentProfile == MMC_PROFILE_DVD_R_SEQ || disc.currentProfile == MMC_PROFILE_DVD_R_DL_SEQ || disc.currentProfile == MMC_PROFILE_DVD_PLUS_R || disc.currentProfile == MMC_PROFILE_DVD_PLUS_R_DL || disc.currentProfile == MMC_PROFILE_BD_R_SRM);
            job->eraseSupported = disc.isErasable && (disc.currentProfile == MMC_PROFILE_CD_RW || disc.currentProfile == MMC_PROFILE_DVD_RW_SEQ || disc.currentProfile == MMC_PROFILE_DVD_RW_RO);
            if (disc.currentProfile >= MMC_PROFILE_CD_ROM && disc.currentProfile <= MMC_PROFILE_CD_RW && disc.numSessions > 1) {
                MmcCdFullToc* toc = calloc(1,sizeof(*toc));
                if (toc && mmc_read_cd_full_toc(&drive,toc)) for (unsigned i = 0; i < toc->sessionCount; i++) { const MmcCdSession* session = &toc->sessions[i]; job_text(job,L"会话 %u：轨道 %u–%u，首轨 LBA %u，lead-out LBA %u。\r\n",session->number,session->firstTrack,session->lastTrack,toc->tracks[session->firstTrackIndex].startLba,session->leadOutLba); }
                else job_text(job,L"无法读取完整 CD 会话结构。\r\n"); free(toc);
            }
        } }
        if (ready && bd && !bdCapacityShown) job_bd_capacity(job,&drive,NULL);
        job->bdReMedia = ready && bdReErasable && infoProfile == MMC_PROFILE_BD_RE;
        if (ready && bd && (infoProfile == MMC_PROFILE_BD_RE || physicalRE)) job_bd_format_capacities(job,&drive);
        if (job->bdReWritable) job->eraseSupported = true;
        if (ready) pipeline_discovery_capacity(&drive,&job->discoveryCapacity);
    } else if (!ready) SetLastError(ERROR_NOT_READY);
    else if (job->operation == GUI_READ_DISC) {
        ReadConfig config = {0}; config.drive = &drive; config.imagePath = job->destination; config.cancelEvent = job->cancel; config.retryCount = job->readRetries; config.autoSplit = job->readAutoSplit; config.ejectWhenDone = job->readEject; config.replaceExisting = job->replaceExisting; config.readCdText = job->readCdText; config.splitSectors = job->readSplitSectors; config.rawCdMds = job->rawCdMds; config.includeSessionPregaps = job->includeSessionPregaps;
        if (job->setReadSpeeds) {
            MmcProfile profile = mmc_get_current_profile(&drive); double unit = profile <= MMC_PROFILE_CD_RW ? 176.4 : profile >= MMC_PROFILE_BD_ROM ? 4495.5 : 1385.0;
            config.readSpeedKbps = job->readSpeedMultiplier ? (uint32_t)(job->readSpeedMultiplier * unit + 0.5) : UINT32_MAX;
            const wchar_t* type = wcsrchr(job->destination,L'.'); if (job->rawCdMds || (type && !_wcsicmp(type,L".cue"))) config.audioReadSpeedKbps = job->audioReadSpeedMultiplier ? (uint32_t)(job->audioReadSpeedMultiplier * 176.4 + 0.5) : UINT32_MAX;
        }
        const wchar_t* extension = wcsrchr(job->destination,L'.'); ok = extension && !_wcsicmp(extension,L".cue") ? pipeline_read_cd(&config,job_pipeline_progress,job) : pipeline_read_disc(&config,job_pipeline_progress,job);
        bool mds = extension && !_wcsicmp(extension,L".mds");
        if (ok) job_text(job,job->readAutoSplit ? L"光盘镜像已读取：%ls\r\n如受文件系统上限而分卷，请使用同名 MDS 载入整组数据。\r\n" : mds ? L"光盘镜像和 MDS 描述已读取：%ls\r\n配套数据文件按顺序记录在 MDS 中。\r\n" : L"光盘镜像已读取：%ls\r\n",job->destination);
        else { job->error = GetLastError(); if (job->error == ERROR_NOT_SUPPORTED) job_text(job,mds ? L"MDS 支持单轨 MODE1 数据 CD，显式原始读取支持 MODE1/MODE2 的2352字节主通道，以及单层、已结束的单会话双层 DVD、受支持的单层/双层 BD-R/BD-RE 和已识别的 BD-R XL 三层/四层来源；未知 BD 格式、BD-ROM及层位置超出支持范围的常规双层来源暂不支持 MDS，音频及混合轨 CD 请选择 CUE。\r\n" : L"当前光盘布局或光驱能力不支持所选镜像格式。\r\n"); else if (job->error == ERROR_DISK_FULL) job_text(job,L"目标位置的可用空间不足。\r\n"); }
    }
    else if (job->operation == GUI_VERIFY_DISC) {
        VerifyConfig config = {0}; config.drive = &drive; config.imagePath = job->sources[0].path; config.cancelEvent = job->cancel;
        config.includeSessionPregaps = job->includeSessionPregaps;
        ok = pipeline_verify_disc_ex(&config,job_pipeline_progress,job); if (!ok) job->error = GetLastError();
        if (ok) job_text(job,L"校验通过：已比较 %u 扇区，仅检查可读性 %u 扇区。\r\n",job->comparedSectors,job->readableOnlySectors);
        else if (job->miscompares) job_text(job,L"发现 %u 个内容不一致的扇区。\r\n",job->miscompares);
    }
    else if (job->operation == GUI_ERASE_QUICK || job->operation == GUI_ERASE_FULL) {
        EraseConfig config = {0}; config.drive = &drive; config.cancelEvent = job->cancel; config.full = job->operation == GUI_ERASE_FULL; config.bdReErase = job->bdReErase; ok = pipeline_erase_disc(&config,job_pipeline_progress,job); if (!ok) job->error = GetLastError();
        if (ok && job->bdReErase) job_text(job,config.full ? L"BD-RE全部逻辑用户区已写零；这不是安全擦除。\r\n" : L"BD-RE前800个逻辑扇区已写零，其余数据可能保留。\r\n");
        else if (ok) job_text(job,config.full ? L"完整擦除完成，设备已报告空白介质。\r\n" : L"快速擦除完成，设备已报告空白介质。\r\n");
        else if (job->error == ERROR_CANCELLED) job_text(job,job->bdReErase ? L"已停止清零，已发出写入无法撤销。\r\n" : L"取消不会中断已经开始的物理擦除；若已开始，已等待设备结束。\r\n");
        else if (job->error == ERROR_NOT_SUPPORTED) job_text(job,job->bdReErase ? L"BD-RE清零仅支持已格式化、具备2048字节逻辑用户区的受支持BD-RE；不会自动格式化。\r\n" : L"当前擦除流程支持 CD-RW 和单层 DVD-RW。\r\n");
    }
    else if (job->operation == GUI_DISCOVERY_DISC) {
        DiscoveryConfig config = {0}; config.drive = &drive; config.sectors = job->discoverySectors; config.writeSpeedKbps = job->speed; config.testMode = job->testMode; config.verifyAfterWrite = job->verify; config.ringBufferBytes = 32 * 1024 * 1024; config.cancelEvent = job->cancel; config.cycleTrayBeforeVerify = true;
        if (job->discoverySpeedMultiplier > 0) { MmcProfile profile = mmc_get_current_profile(&drive); config.writeSpeedKbps = (uint32_t)(job->discoverySpeedMultiplier * (profile == MMC_PROFILE_BD_R_SRM ? 4495.5 : profile == MMC_PROFILE_CD_R ? 176.4 : 1385.0) + 0.5); }
        ok = pipeline_discover_disc(&config,job_pipeline_progress,job); if (!ok) job->error = GetLastError();
        if (ok && job->testMode) job_text(job,L"品质测试的设备测试写入完成；未执行记录数据的回读校验。\r\n");
        else if (ok) job_text(job,L"品质测试写入完成%ls。\r\n已比较 %u 扇区。\r\n",job->verify ? L"并通过回读校验" : L"",job->comparedSectors);
        else if (job->miscompares) job_text(job,L"品质测试发现 %u 个内容不一致的扇区。\r\n",job->miscompares);
        else if (job->error == ERROR_NOT_SUPPORTED) job_text(job,L"当前品质测试支持空白 CD-R、单层 DVD-R/+R 与 BD-R SRM；测试模式需要光驱和介质支持。\r\n");
        else if (job->error == ERROR_DISK_FULL) job_text(job,L"所选扇区数超过光盘可写容量。\r\n");
    }
    else if (job->operation == GUI_WRITE_DISC) {
        BurnConfig config = {0}; config.drive = &drive; config.imagePath = job->sources[0].path; config.writeSpeedKbps = job->speed; config.verifyAfterWrite = job->verify; config.ringBufferBytes = 32 * 1024 * 1024; config.cancelEvent = job->cancel; config.cycleTrayBeforeVerify = true; config.testMode = job->testMode; config.layerBreakLba = job->writeLayerBreak; config.bdReOverwrite = job->bdReOverwrite; config.bdWriteType = job->bdWriteType; config.bdDisableVnr = job->bdDisableVnr; if (job->bdReOverwrite) { config.keepCurrentWriteSpeed = true; config.cycleTrayBeforeVerify = false; }
        if (job->bdWriteType != BD_WRITE_TYPE_CURRENT || job->bdDisableVnr) job_text(job,L"本次BD-R选项：写类型%ls，VNR关闭%u；MODE页不改变BD物理写法，也不提供追加或多会话。备用区资格仍须单独复核。\r\n",job->bdWriteType == BD_WRITE_TYPE_INCREMENTAL ? L"Incremental" : job->bdWriteType == BD_WRITE_TYPE_SAO ? L"SAO" : L"保持当前",job->bdDisableVnr);
        ok = pipeline_burn_image(&config,job_pipeline_progress,job); if (!ok) job->error = GetLastError();
        if (ok && job->testMode) job_text(job,L"设备测试写入完成；未执行记录数据的回读校验。\r\n");
        else if (ok && job->bdReOverwrite) job_text(job,L"BD-RE镜像覆写完成%ls；来源范围外旧数据未作清除承诺。\r\n",job->verify ? L"并通过回读校验" : L"");
        else if (ok) job_text(job,L"光盘镜像写入完成%ls。\r\n",job->verify ? L"并通过回读校验" : L"");
        else if (job->error == ERROR_NOT_SUPPORTED) job_text(job,job->bdReOverwrite ? L"BD-RE覆写需要受支持且已格式化的2048字节数据介质；不支持测试模式、预留或L0设置。\r\n" : L"当前支持空白 CD-R SAO、DVD-R（含固定层 DL）DAO、DVD+R（含自动或指定层 DL）与 BD-R SRM；CD-TEXT 还要求光驱支持 R-W 写入。\r\n");
        else if (job->error == ERROR_DISK_FULL) job_text(job,L"光盘的可写扇区不足。\r\n");
    }
    if (!ok && !job->error) job->error = drive.lastError ? drive.lastError : GetLastError();
    if (!ok && job->error == ERROR_ACCESS_DENIED && drive.lastError == ERROR_ACCESS_DENIED) job_text(job,drive.accessKnown && (drive.deviceAccess & (GENERIC_READ | GENERIC_WRITE)) != (GENERIC_READ | GENERIC_WRITE) ? L"未取得 SPTI 所需的设备读写访问权，请尝试以管理员身份运行。\r\n" : L"光驱或驱动程序拒绝了设备命令。\r\n");
    spti_close_drive(&drive); return ok;
}

/* 格式化列表查询与执行均在worker；窗口只携带明确选中的原索引及描述符。 */
static bool job_bd_re_format(GuiJob* job) {
    OpticalDrive drive; if (!spti_open_drive_passive(&drive,job->drive)) return false; bool ok;
    bool bdr = job->operation == GUI_BD_R_FORMAT_LIST || job->operation == GUI_BD_R_FORMAT;
    if (job->operation == GUI_BD_RE_FORMAT_LIST || job->operation == GUI_BD_R_FORMAT_LIST) {
        ok = mmc_get_current_profile(&drive) == (bdr ? MMC_PROFILE_BD_R_SRM : MMC_PROFILE_BD_RE);
        if (!ok) SetLastError(ERROR_NOT_SUPPORTED);
        if (ok) ok = mmc_read_format_capacities(&drive,&job->bdReCapacities);
        if (ok) job_text(job,bdr ? L"只读格式列表已取得。请选择FT32带备用区原描述符；只允许新的空白、从未格式化一次写BD-R，提交前会再次核对。\r\n" : L"只读格式列表已取得。请选择FT30/31原描述符与认证方式，提交前会再次核对。\r\n");
    } else if (bdr) {
        BdRFormatConfig config = job->bdRFormat; config.drive = &drive; config.cancelEvent = job->cancel;
        ok = pipeline_format_bd_r(&config,&job->bdRFormatResult,job_pipeline_progress,job);
        job_text(job,L"BD-R格式命令：尝试%u，接受%u，完成%u，取消请求%u；已格式化扇区%u，已分配备用块%u，可用备用块%u。\r\n",job->bdRFormatResult.commandAttempted,job->bdRFormatResult.commandAccepted,job->bdRFormatResult.completed,job->bdRFormatResult.cancelRequested,job->bdRFormatResult.formattedSectors,job->bdRFormatResult.allocatedSpareBlocks,job->bdRFormatResult.freeSpareBlocks);
        if (job->bdRFormatResult.commandAttempted) job_text(job,L"一次写BD格式化尝试不可回滚；请重新查询介质，本任务不会自动写入、回退或重试。\r\n");
    } else {
        BdReFormatConfig config = job->bdReFormat; config.drive = &drive; config.cancelEvent = job->cancel;
        ok = pipeline_format_bd_re(&config,&job->bdReFormatResult,job_pipeline_progress,job);
        job_text(job,L"格式命令：尝试%u，接受%u，完成%u，取消请求%u；已格式化扇区%u，备用簇%u。\r\n",job->bdReFormatResult.commandAttempted,job->bdReFormatResult.commandAccepted,job->bdReFormatResult.completed,job->bdReFormatResult.cancelRequested,job->bdReFormatResult.formattedSectors,job->bdReFormatResult.spareClusters);
        if (job->bdReFormatResult.commandAttempted) job_text(job,L"格式化后请重新查询介质；本任务不会自动覆写或重试。\r\n");
    }
    DWORD error = GetLastError(); spti_close_drive(&drive); if (!ok) job->error = error; SetLastError(error); return ok;
}

DWORD WINAPI imgburn_gui_job_run(void* context) {
    GuiJob* job = (GuiJob*)context; SetLastError(ERROR_SUCCESS);
    if (!job) return 1;
    if (!job->sourceCount || !job->sources) { job->error = ERROR_INVALID_PARAMETER; if (job->owner) PostMessageW(job->owner, GUI_JOB_DONE, 0, (LPARAM)job); return 1; }
    if (!job_cancelled(job) || job->operation == GUI_CD_ISO_PLAN || job->operation == GUI_CD_ISO_WRITE) switch (job->operation) {
        case GUI_CALCULATE: job->succeeded = job_prepare(job); break;
        case GUI_BUILD_IMAGE: job->succeeded = job_build(job); break;
        case GUI_INSPECT: job->succeeded = job_inspect(job); break;
        case GUI_BD_RE_FORMAT_LIST: case GUI_BD_RE_FORMAT: case GUI_BD_R_FORMAT_LIST: case GUI_BD_R_FORMAT: job->succeeded = job_bd_re_format(job); break;
        case GUI_READ_DEST_INFO: job->succeeded = job_read_destination_info(job); break;
        case GUI_CREATE_CUE: job->succeeded = job_cue(job); break;
        case GUI_CREATE_MDS: job->succeeded = job_mds(job); break;
        case GUI_DRIVE_INFO: case GUI_VERIFY_DISC: case GUI_WRITE_DISC: case GUI_READ_DISC: case GUI_ERASE_QUICK: case GUI_ERASE_FULL: case GUI_DISCOVERY_DISC: job->succeeded = job_drive(job); break;
        case GUI_DVD_INFO: job->dvdVideo = true; job->succeeded = job_prepare(job) && imgburn_dvd_read_navigation(&job->builder, &job->navigation); break;
        case GUI_DVD_PREVIEW: job->succeeded = imgburn_dvd_preview_show(&job->builder, &job->preview, NULL) == 0; break;
        case GUI_DVD_CANDIDATES: job->succeeded = job_dvd_candidates(job); break;
        case GUI_CD_ISO_PLAN: case GUI_CD_ISO_WRITE: job->succeeded = job_cd_iso(job); break;
    }
    if (!job->succeeded && job_cancelled(job) && (!job->error || (job->operation != GUI_ERASE_QUICK && job->operation != GUI_ERASE_FULL && job->operation != GUI_BD_RE_FORMAT && job->operation != GUI_BD_R_FORMAT))) job->error = ERROR_CANCELLED;
    if (!job->succeeded) { if (!job->error) job->error = GetLastError(); if (!job->error) job->error = ERROR_INVALID_DATA; job_text(job, job->error == ERROR_CANCELLED ? L"操作已取消。\r\n" : L"操作失败（错误 %lu）。\r\n", job->error); }
    if (job->owner) PostMessageW(job->owner, GUI_JOB_DONE, 0, (LPARAM)job); return job->succeeded ? 0 : 1;
}

void imgburn_gui_job_free(GuiJob* job) {
    if (!job) return;
    if (job->thread) CloseHandle(job->thread); if (job->cancel) CloseHandle(job->cancel);
    imgburn_dvd_free_navigation(&job->navigation); free(job->dvdCandidates); imgburn_data_source_close(&job->dvdCandidateSource); imgburn_iso_session_close(&job->cdIsoSource); if (job->builderReady) image_builder_destroy(&job->builder); free(job->sources); free(job);
}
