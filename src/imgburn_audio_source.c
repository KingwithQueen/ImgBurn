#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#include "imgburn_audio_source.h"
#include "imgburn_audio.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define AUDIO_PCM_MAX_BYTES (UINT64_C(359849) * 2352)
#define AUDIO_PCM_STALL_MS UINT64_C(30000)
#define AUDIO_PCM_TOTAL_MS UINT64_C(1800000)

static bool pcm_cancelled(HANDLE event) {
    if (!event) return false;
    DWORD state = WaitForSingleObject(event,0);
    if (state == WAIT_OBJECT_0) { SetLastError(ERROR_CANCELLED); return true; }
    return state == WAIT_FAILED;
}

void audio_pcm_source_close(AudioPcmSource* source) {
    if (!source) return;
    DWORD error = GetLastError();
    if (source->pcm && source->pcm != INVALID_HANDLE_VALUE) CloseHandle(source->pcm);
    if (source->source && source->source != INVALID_HANDLE_VALUE) CloseHandle(source->source);
    memset(source,0,sizeof(*source)); SetLastError(error);
}

static bool pcm_path(const wchar_t* path, wchar_t full[MAX_PATH]) {
    if (!path || !*path || wcslen(path) >= MAX_PATH) { SetLastError(ERROR_INVALID_NAME); return false; }
    if (!_wcsnicmp(path,L"\\\\.\\",4) || !_wcsnicmp(path,L"\\\\?\\",4) || !_wcsnicmp(path,L"\\??\\",4)) { SetLastError(ERROR_INVALID_NAME); return false; }
    for (size_t i = 0; path[i]; i++) if (path[i] < 32 || wcschr(L"\"<>|?*",path[i]) || (path[i] == L':' && (i != 1 || !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))))) { SetLastError(ERROR_INVALID_NAME); return false; }
    DWORD chars = GetFullPathNameW(path,MAX_PATH,full,NULL);
    if (!chars || chars >= MAX_PATH) { if (chars >= MAX_PATH) SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
    return true;
}

static HANDLE pcm_temporary(void) {
    wchar_t directory[MAX_PATH], path[MAX_PATH];
    DWORD chars = GetTempPathW(MAX_PATH,directory);
    if (!chars || chars >= MAX_PATH) { if (chars >= MAX_PATH) SetLastError(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        unsigned random[4];
        for (unsigned i = 0; i < 4; i++) if (rand_s(&random[i])) { SetLastError(ERROR_GEN_FAILURE); return INVALID_HANDLE_VALUE; }
        if (swprintf(path,MAX_PATH,L"%lsImgBurnPCM-%08x%08x%08x%08x.tmp",directory,random[0],random[1],random[2],random[3]) < 0) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return INVALID_HANDLE_VALUE; }
        HANDLE file = CreateFileW(path,GENERIC_READ | GENERIC_WRITE | DELETE,FILE_SHARE_READ | FILE_SHARE_DELETE,NULL,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,NULL);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) return file;
    }
    SetLastError(ERROR_FILE_EXISTS); return INVALID_HANDLE_VALUE;
}

static bool pcm_start_decoder(const wchar_t* path, HANDLE* process, HANDLE* pipe, HANDLE* job) {
    wchar_t executable[MAX_PATH], command[MAX_PATH * 2 + 256];
    DWORD chars = SearchPathW(NULL,L"ffmpeg.exe",NULL,MAX_PATH,executable,NULL);
    if (!chars || chars >= MAX_PATH) { if (chars >= MAX_PATH) SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
    /* 输入仅开放本地文件协议；只输出一条44.1kHz、16位双声道PCM流，不启动外部设备或交互输入。 */
    if (swprintf(command,sizeof(command) / sizeof(*command),L"\"%ls\" -nostdin -hide_banner -v error -xerror -protocol_whitelist file -i \"%ls\" -f s16le -acodec pcm_s16le -ac 2 -ar 44100 -vn -sn -dn pipe:1",executable,path) < 0) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
    SECURITY_ATTRIBUTES security = {sizeof(security),NULL,TRUE};
    HANDLE readPipe = NULL, writePipe = NULL, nullStream = INVALID_HANDLE_VALUE, decoderJob = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL; bool initialized = false, success = false;
    PROCESS_INFORMATION child = {0}; DWORD error = ERROR_SUCCESS; SIZE_T bytes = 0;
    if (!CreatePipe(&readPipe,&writePipe,&security,0) || !SetHandleInformation(readPipe,HANDLE_FLAG_INHERIT,0)) goto cleanup;
    nullStream = CreateFileW(L"NUL",GENERIC_READ | GENERIC_WRITE,FILE_SHARE_READ | FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (nullStream == INVALID_HANDLE_VALUE) goto cleanup;
    InitializeProcThreadAttributeList(NULL,1,0,&bytes);
    if (!bytes) goto cleanup;
    attributes = malloc(bytes);
    if (!attributes) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto cleanup; }
    if (!InitializeProcThreadAttributeList(attributes,1,0,&bytes)) goto cleanup;
    initialized = true;
    /* 即使其他线程有可继承句柄，子进程也只继承PCM输出及有效的空输入、错误流句柄。 */
    HANDLE allowed[2] = {writePipe,nullStream};
    if (!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,allowed,sizeof(allowed),NULL,NULL)) goto cleanup;
    decoderJob = CreateJobObjectW(NULL,NULL);
    if (!decoderJob) goto cleanup;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(decoderJob,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) goto cleanup;
    STARTUPINFOEXW startup = {0}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullStream; startup.StartupInfo.hStdOutput = writePipe; startup.StartupInfo.hStdError = nullStream; startup.lpAttributeList = attributes;
    if (!CreateProcessW(executable,command,NULL,NULL,TRUE,CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&startup.StartupInfo,&child)) goto cleanup;
    if (!AssignProcessToJobObject(decoderJob,child.hProcess) || ResumeThread(child.hThread) == (DWORD)-1) goto cleanup;
    *process = child.hProcess; child.hProcess = NULL; *pipe = readPipe; readPipe = NULL; *job = decoderJob; decoderJob = NULL; success = true;
cleanup:
    error = GetLastError();
    if (child.hProcess) { TerminateProcess(child.hProcess,error ? error : ERROR_GEN_FAILURE); WaitForSingleObject(child.hProcess,5000); CloseHandle(child.hProcess); }
    if (child.hThread) CloseHandle(child.hThread);
    if (decoderJob) CloseHandle(decoderJob);
    if (readPipe) CloseHandle(readPipe);
    if (writePipe) CloseHandle(writePipe);
    if (nullStream != INVALID_HANDLE_VALUE) CloseHandle(nullStream);
    if (initialized) DeleteProcThreadAttributeList(attributes);
    free(attributes); SetLastError(error); return success;
}

static bool pcm_decode(const wchar_t* path, HANDLE cancelEvent, AudioPcmSource* source) {
    HANDLE process = NULL, pipe = NULL, job = NULL; uint8_t* buffer = NULL;
    bool success = false, broken = false; DWORD error = ERROR_SUCCESS;
    source->pcm = pcm_temporary();
    if (source->pcm == INVALID_HANDLE_VALUE) return false;
    if (pcm_cancelled(cancelEvent) || !pcm_start_decoder(path,&process,&pipe,&job)) goto cleanup;
    buffer = malloc(65536);
    if (!buffer) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto cleanup; }
    ULONGLONG started = GetTickCount64(), lastOutput = started;
    for (;;) {
        if (pcm_cancelled(cancelEvent)) goto cleanup;
        ULONGLONG now = GetTickCount64();
        if (now - started >= AUDIO_PCM_TOTAL_MS || now - lastOutput >= AUDIO_PCM_STALL_MS) { SetLastError(ERROR_TIMEOUT); goto cleanup; }
        DWORD available = 0;
        if (!broken && !PeekNamedPipe(pipe,NULL,0,NULL,&available,NULL)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) goto cleanup;
            broken = true;
        }
        if (available) {
            DWORD actual = 0, wanted = available < 65536 ? available : 65536;
            if (!ReadFile(pipe,buffer,wanted,&actual,NULL)) goto cleanup;
            if (!actual || actual > wanted) { SetLastError(ERROR_READ_FAULT); goto cleanup; }
            if (source->dataBytes > AUDIO_PCM_MAX_BYTES - actual) { SetLastError(ERROR_FILE_TOO_LARGE); goto cleanup; }
            DWORD written = 0;
            while (written < actual) {
                if (pcm_cancelled(cancelEvent)) goto cleanup;
                DWORD count = 0;
                if (!WriteFile(source->pcm,buffer + written,actual - written,&count,NULL)) goto cleanup;
                if (!count || count > actual - written) { SetLastError(ERROR_WRITE_FAULT); goto cleanup; }
                written += count;
            }
            source->dataBytes += actual; lastOutput = now; continue;
        }
        DWORD state = WaitForSingleObject(process,0);
        if (state == WAIT_FAILED) goto cleanup;
        if (broken && state == WAIT_OBJECT_0) break;
        if (cancelEvent) { state = WaitForSingleObject(cancelEvent,10); if (state == WAIT_FAILED) goto cleanup; }
        else Sleep(10);
    }
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process,&exitCode)) goto cleanup;
    if (exitCode || !source->dataBytes || source->dataBytes % CDDA_BYTES_PER_SAMPLE) { SetLastError(ERROR_INVALID_DATA); goto cleanup; }
    LARGE_INTEGER actualLength, start = {0};
    if (!FlushFileBuffers(source->pcm) || !GetFileSizeEx(source->pcm,&actualLength) || !SetFilePointerEx(source->pcm,start,NULL,FILE_BEGIN)) goto cleanup;
    if (actualLength.QuadPart < 0 || (uint64_t)actualLength.QuadPart != source->dataBytes) { SetLastError(ERROR_INVALID_DATA); goto cleanup; }
    source->sectorCount = (uint32_t)((source->dataBytes + 2351) / 2352); success = true;
cleanup:
    error = GetLastError();
    if (process && !success) { TerminateProcess(process,error ? error : ERROR_GEN_FAILURE); WaitForSingleObject(process,5000); }
    if (job) CloseHandle(job);
    if (process) CloseHandle(process);
    if (pipe) CloseHandle(pipe);
    free(buffer); SetLastError(error); return success;
}

bool audio_pcm_source_open(const wchar_t* path, HANDLE cancelEvent, AudioPcmSource* source) {
    if (!source) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    memset(source,0,sizeof(*source));
    if (pcm_cancelled(cancelEvent)) return false;
    wchar_t full[MAX_PATH];
    if (!pcm_path(path,full)) return false;
    source->source = CreateFileW(full,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if (source->source == INVALID_HANDLE_VALUE) { source->source = NULL; return false; }
    bool success = false; DWORD error = ERROR_SUCCESS; BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(source->source) != FILE_TYPE_DISK) { SetLastError(ERROR_INVALID_NAME); goto cleanup; }
    if (!GetFileInformationByHandle(source->source,&info)) goto cleanup;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { SetLastError(ERROR_INVALID_NAME); goto cleanup; }
    AudioWaveLayout layout;
    if (audio_wave_get_layout(source->source,&layout)) {
        if (!layout.dataBytes || layout.dataBytes > AUDIO_PCM_MAX_BYTES) { SetLastError(layout.dataBytes ? ERROR_FILE_TOO_LARGE : ERROR_INVALID_DATA); goto cleanup; }
        if (!DuplicateHandle(GetCurrentProcess(),source->source,GetCurrentProcess(),&source->pcm,GENERIC_READ,FALSE,0)) goto cleanup;
        source->dataOffset = layout.dataOffset; source->dataBytes = layout.dataBytes; source->sectorCount = layout.sectorCount;
        success = !pcm_cancelled(cancelEvent);
    } else if (GetLastError() == ERROR_BAD_FORMAT) success = pcm_decode(full,cancelEvent,source);
cleanup:
    error = GetLastError();
    if (!success) audio_pcm_source_close(source);
    SetLastError(success ? ERROR_SUCCESS : error); return success;
}
