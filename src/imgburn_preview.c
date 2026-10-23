#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_preview.h"
#include <commctrl.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winmm.lib")

static bool preview_ffmpeg(const wchar_t* requested, wchar_t* path) {
    if (requested) { DWORD n = GetFullPathNameW(requested, MAX_PATH, path, NULL); return n && n < MAX_PATH && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
    DWORD n = SearchPathW(NULL, L"ffmpeg.exe", NULL, MAX_PATH, path, NULL); return n && n < MAX_PATH;
}

static bool preview_process(const wchar_t* executable, wchar_t* command, HANDLE* process, HANDLE* output, HANDLE* errors) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa),NULL,TRUE}; HANDLE writer = NULL, errorWriter = NULL, nullFile = INVALID_HANDLE_VALUE; bool created = false;
    if (!CreatePipe(output, &writer, &sa, 65536) || !SetHandleInformation(*output, HANDLE_FLAG_INHERIT, 0)) goto done;
    if (errors && (!CreatePipe(errors, &errorWriter, &sa, 65536) || !SetHandleInformation(*errors, HANDLE_FLAG_INHERIT, 0))) goto done;
    nullFile = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL); if (nullFile == INVALID_HANDLE_VALUE) goto done;
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(bytes); if (!attributes) goto done;
    bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE;
    HANDLE inherited[] = {writer,nullFile,errorWriter};
    bool ready = initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, (errorWriter ? 3 : 2) * sizeof(HANDLE), NULL, NULL);
    STARTUPINFOEXW si = {0}; si.StartupInfo.cb = sizeof(si); si.StartupInfo.dwFlags = STARTF_USESTDHANDLES; si.StartupInfo.hStdInput = nullFile; si.StartupInfo.hStdOutput = writer; si.StartupInfo.hStdError = errorWriter ? errorWriter : nullFile; si.lpAttributeList = attributes;
    PROCESS_INFORMATION pi = {0}; created = ready && CreateProcessW(executable, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi);
    if (initialized) DeleteProcThreadAttributeList(attributes); free(attributes);
    if (created) { CloseHandle(pi.hThread); *process = pi.hProcess; }
done:
    if (writer) CloseHandle(writer); if (errorWriter) CloseHandle(errorWriter); if (nullFile != INVALID_HANDLE_VALUE) CloseHandle(nullFile);
    if (!created) { if (*output) { CloseHandle(*output); *output = NULL; } if (errors && *errors) { CloseHandle(*errors); *errors = NULL; } } return created;
}

typedef struct { HANDLE pipe, thread, changed; CRITICAL_SECTION lock; DvdPreviewFrameInfo frames[64]; uint8_t* yuv; bool ended; } PreviewMetadata;

static unsigned preview_chroma_row(const uint8_t* plane, unsigned stride, unsigned rows, int row, unsigned x) {
    if (row < 0) row = 0; if ((unsigned)row >= rows) row = (int)rows - 1; return plane[(size_t)row * stride + x];
}

static unsigned preview_chroma_sample(const uint8_t* plane, unsigned width, unsigned height, unsigned x, unsigned y, bool interlaced) {
    unsigned stride = width / 2, rows = height / 2; int row = (int)(y / 2);
    unsigned current = preview_chroma_row(plane, stride, rows, row, x);
    if (!interlaced) {
        if (!y || y + 1 == height) return current;
        int other = y == 1 ? 1 : row - 1;
        return (3 * current + preview_chroma_row(plane, stride, rows, other, x) + 2) / 4;
    }
    /* 按原版分别重建两场的色度，首尾保留其边缘采样规则。 */
    int base = (int)(y / 4) * 2; unsigned a, c;
    switch (y % 4) {
        case 0: return y ? (7 * current + preview_chroma_row(plane, stride, rows, row - 2, x) + 4) / 8 : current;
        case 1:
            a = preview_chroma_row(plane, stride, rows, base ? base - 1 : 0, x); c = preview_chroma_row(plane, stride, rows, base + 1, x); return (3 * a + 5 * c + 4) / 8;
        case 2:
            a = preview_chroma_row(plane, stride, rows, base, x); return y + 2 == height ? a : (5 * a + 3 * preview_chroma_row(plane, stride, rows, base + 2, x) + 4) / 8;
        default:
            c = preview_chroma_row(plane, stride, rows, y + 1 == height ? row - 1 : row + 2, x); return (7 * current + c + 4) / 8;
    }
}

static uint8_t preview_rgb_byte(int value) { return value <= 0 ? 0 : value >= 255 * 8192 ? 255 : (uint8_t)(value / 8192); }

static void preview_convert_frame(const uint8_t* yuv, uint8_t* bgr, unsigned width, unsigned height, bool interlaced) {
    size_t pixels = (size_t)width * height; const uint8_t* planes[] = {yuv + pixels,yuv + pixels + pixels / 4};
    uint8_t chroma[2][720];
    for (unsigned y = 0; y < height; y++) {
        for (unsigned c = 0; c < 2; c++) for (unsigned x = 0; x < width; x += 2) {
            unsigned a = preview_chroma_sample(planes[c], width, height, x / 2, y, interlaced); chroma[c][x] = (uint8_t)a;
            unsigned next = x + 16 < width ? preview_chroma_sample(planes[c], width, height, x / 2 + 1, y, interlaced) : a;
            chroma[c][x + 1] = (uint8_t)((a + next + 1) / 2);
        }
        for (unsigned x = 0; x < width; x += 4) {
            uint8_t values[4][3], carry[4][3];
            for (unsigned i = 0; i < 4; i++) {
                int u = chroma[0][x + i] - 128, v = chroma[1][x + i] - 128, luma = (yuv[(size_t)y * width + x + i] - 16) * 9539 + 4096;
                int rgb[] = {luma + 16525 * u,luma - 3209 * u - 6660 * v,luma + 13075 * v};
                for (unsigned c = 0; c < 3; c++) { values[i][c] = preview_rgb_byte(rgb[c]); carry[i][c] = rgb[c] < 0 ? 7 : 0; }
            }
            /* 保留原版四像素打包时的低位合并结果，不能用常规钳位改变同帧BMP。 */
            values[0][0] |= carry[0][0]; values[0][2] |= carry[0][2];
            values[1][0] |= carry[0][0] | carry[0][1]; values[1][2] |= carry[0][2];
            values[2][0] |= carry[2][0] | carry[1][1]; values[2][2] |= carry[2][2] | carry[2][1];
            values[3][0] |= carry[2][0] | carry[2][1]; values[3][2] |= carry[2][2];
            memcpy(bgr + ((size_t)y * width + x) * 3, values, sizeof(values));
        }
    }
}

static void preview_log_line(PreviewMetadata* m, char* line) {
    char* n = strstr(line, " n:"); char* pts = strstr(line, " pts:"); char* pos = strstr(line, " pos:"); char* type = strstr(line, " type:");
    if (!n || !pts || !pos || !type || !strstr(line, "showinfo")) return;
    int64_t ticks = _strtoi64(pts + 5, NULL, 10); if (ticks > INT64_MAX / 1000000 || ticks < INT64_MIN / 1000000) return;
    DvdPreviewFrameInfo info = {0}; info.number = _strtoui64(n + 3, NULL, 10); info.timestampUs = ticks * 1000000 / 90000; info.byteOffset = _strtoi64(pos + 5, NULL, 10);
    info.pictureType = type[6]; info.interlaced = strstr(line, " i:P") == NULL; info.valid = true;
    EnterCriticalSection(&m->lock); m->frames[info.number % 64] = info; SetEvent(m->changed); LeaveCriticalSection(&m->lock);
}

static DWORD WINAPI preview_log_worker(void* context) {
    PreviewMetadata* m = (PreviewMetadata*)context; char data[4096], line[2048]; size_t used = 0; bool discard = false; DWORD got = 0;
    while (ReadFile(m->pipe, data, sizeof(data), &got, NULL) && got) for (DWORD i = 0; i < got; i++) {
        if (data[i] == '\n' || data[i] == '\r') { if (!discard) { line[used] = 0; preview_log_line(m, line); } used = 0; discard = false; }
        else if (used + 1 < sizeof(line)) line[used++] = data[i]; else discard = true;
    }
    EnterCriticalSection(&m->lock); m->ended = true; SetEvent(m->changed); LeaveCriticalSection(&m->lock); return 0;
}

bool imgburn_frame_decoder_open(DvdFrameDecoder* d, const wchar_t* video, const wchar_t* ffmpeg, unsigned width, unsigned height, uint64_t firstFrame) {
    if (!d) return false; memset(d, 0, sizeof(*d)); d->exitCode = STILL_ACTIVE;
    if (!video || !width || width > 720 || width % 4 || !height || height > 576 || height % 2 || firstFrame > UINT32_MAX || wcschr(video, L'"')) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    wchar_t executable[MAX_PATH], command[2048]; if (!preview_ffmpeg(ffmpeg, executable)) { SetLastError(ERROR_FILE_NOT_FOUND); return false; }
    if (swprintf(command, 2048, L"\"%ls\" -nostdin -v info -threads 1 -filter_threads 1 -copyts -i \"%ls\" -map 0:v:0 -threads:v 1 -vf \"select=gte(n\\,%llu),scale=%u:%u,format=yuv420p,settb=1/90000,showinfo\" -vsync 0 -an -sn -dn -pix_fmt yuv420p -flush_packets 1 -f rawvideo pipe:1", executable, video, firstFrame, width, height) < 0) return false;
    PreviewMetadata* m = (PreviewMetadata*)calloc(1, sizeof(*m)); if (!m) return false; InitializeCriticalSection(&m->lock); d->metadata = m; m->changed = CreateEventW(NULL, TRUE, FALSE, NULL);
    m->yuv = (uint8_t*)malloc((size_t)width * height * 3 / 2);
    if (!m->yuv || !m->changed || !preview_process(executable, command, &d->process, &d->output, &m->pipe)) goto failed;
    m->thread = CreateThread(NULL, 0, preview_log_worker, m, 0, NULL); if (!m->thread) goto failed;
    d->width = width; d->height = height; return true;
failed:
    imgburn_frame_decoder_close(d); return false;
}

bool imgburn_frame_decoder_read(DvdFrameDecoder* d, void* bgr, size_t capacity) {
    if (!d || !d->process || !bgr || d->ended) return false;
    PreviewMetadata* m = (PreviewMetadata*)d->metadata; size_t expected = (size_t)d->width * d->height * 3 / 2, used = 0;
    if (capacity < (size_t)d->width * d->height * 3) { d->failed = true; SetLastError(ERROR_INSUFFICIENT_BUFFER); return false; }
    while (used < expected) {
        DWORD got = 0; if (!ReadFile(d->output, m->yuv + used, (DWORD)(expected - used), &got, NULL) || !got) break; used += got;
    }
    if (used == expected) {
        memset(&d->info, 0, sizeof(d->info));
        for (;;) {
            EnterCriticalSection(&m->lock); DvdPreviewFrameInfo info = m->frames[d->frameIndex % 64]; bool ended = m->ended;
            ResetEvent(m->changed); LeaveCriticalSection(&m->lock);
            if (info.valid && info.number == d->frameIndex) { d->info = info; break; }
            if (ended || WaitForSingleObject(m->changed, 5000) != WAIT_OBJECT_0) break;
        }
        preview_convert_frame(m->yuv, (uint8_t*)bgr, d->width, d->height, d->info.interlaced); d->frameIndex++; return true;
    }
    d->ended = true;
    DWORD wait = WaitForSingleObject(d->process, 5000); bool exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(d->process, &d->exitCode);
    d->failed = used != 0 || !exited || d->exitCode != 0; if (d->failed) SetLastError(ERROR_INVALID_DATA); return false;
}

void imgburn_frame_decoder_close(DvdFrameDecoder* d) {
    if (!d) return;
    if (d->process) { if (WaitForSingleObject(d->process, 0) == WAIT_TIMEOUT) { TerminateProcess(d->process, ERROR_CANCELLED); WaitForSingleObject(d->process, 5000); } CloseHandle(d->process); }
    PreviewMetadata* m = (PreviewMetadata*)d->metadata;
    if (m) { if (m->thread) { WaitForSingleObject(m->thread, INFINITE); CloseHandle(m->thread); } if (m->pipe) CloseHandle(m->pipe); if (m->changed) CloseHandle(m->changed); DeleteCriticalSection(&m->lock); free(m->yuv); free(m); }
    if (d->output) CloseHandle(d->output); memset(d, 0, sizeof(*d));
}

static bool preview_video_pts(const wchar_t* video, uint64_t* pts) {
    FILE* file = _wfopen(video, L"rb"); if (!file) return false; uint8_t data[2048]; bool found = false;
    for (unsigned sector = 0; sector < 16384 && fread(data, 1, sizeof(data), file) == sizeof(data); sector++) {
        if (memcmp(data, "\0\0\1\xBA", 4)) break;
        size_t at = 14 + (data[13] & 7);
        while (at + 6 <= sizeof(data) && !memcmp(data + at, "\0\0\1", 3)) {
            unsigned id = data[at + 3]; size_t bytes = ((size_t)data[at + 4] << 8 | data[at + 5]) + 6;
            if (bytes < 6 || bytes > sizeof(data) - at) break;
            if (id >= 0xE0 && id <= 0xEF && bytes >= 14 && (data[at + 6] & 0xC0) == 0x80 && (data[at + 7] & 0x80) && data[at + 8] >= 5) {
                const uint8_t* p = data + at + 9;
                if ((p[0] & 1) && (p[2] & 1) && (p[4] & 1)) { *pts = (uint64_t)((p[0] >> 1) & 7) << 30 | (uint64_t)p[1] << 22 | (uint64_t)(p[2] >> 1) << 15 | (uint64_t)p[3] << 7 | p[4] >> 1; found = true; break; }
            }
            at += bytes;
        }
        if (found) break;
    }
    fclose(file); return found;
}

bool imgburn_pcm_decoder_open(DvdPcmDecoder* d, const wchar_t* video, const wchar_t* ffmpeg, unsigned streamId, uint64_t firstSample) {
    if (!d) return false; memset(d, 0, sizeof(*d)); d->exitCode = STILL_ACTIVE;
    if (!video || wcschr(video, L'"') || firstSample > INT64_MAX || !((streamId >= 0x80 && streamId <= 0x8F) || (streamId >= 0xA0 && streamId <= 0xA7) || (streamId >= 0x1C0 && streamId <= 0x1C7))) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    wchar_t executable[MAX_PATH], command[2048]; uint64_t start = 0;
    if (!preview_ffmpeg(ffmpeg, executable) || !preview_video_pts(video, &start)) { SetLastError(ERROR_INVALID_DATA); return false; }
    if (swprintf(command, 2048, L"\"%ls\" -nostdin -v error -copyts -i \"%ls\" -map 0:i:0x%X -vn -sn -dn -af \"asetpts=PTS-%llu/90000/TB,aresample=48000:async=1:first_pts=0,atrim=start_sample=%llu\" -ac 2 -ar 48000 -c:a pcm_s16le -f s16le pipe:1", executable, video, streamId, start, firstSample) < 0) return false;
    return preview_process(executable, command, &d->process, &d->output, NULL);
}

size_t imgburn_pcm_decoder_read(DvdPcmDecoder* d, void* pcm, size_t capacity) {
    if (!d || !d->process || !pcm || d->ended) return 0;
    if (!capacity || capacity > MAXDWORD || capacity % 4) { d->failed = true; SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    size_t used = 0;
    while (used < capacity) { DWORD got = 0; if (!ReadFile(d->output, (uint8_t*)pcm + used, (DWORD)(capacity - used), &got, NULL) || !got) break; used += got; }
    if (used < capacity) { d->ended = true; bool exited = WaitForSingleObject(d->process, 5000) == WAIT_OBJECT_0 && GetExitCodeProcess(d->process, &d->exitCode); d->failed = used % 4 != 0 || !exited || d->exitCode != 0; }
    if (d->failed) { SetLastError(ERROR_INVALID_DATA); return 0; } return used;
}

void imgburn_pcm_decoder_close(DvdPcmDecoder* d) {
    if (!d) return;
    if (d->process) { if (WaitForSingleObject(d->process, 0) == WAIT_TIMEOUT) { TerminateProcess(d->process, ERROR_CANCELLED); WaitForSingleObject(d->process, 5000); } CloseHandle(d->process); }
    if (d->output) CloseHandle(d->output); memset(d, 0, sizeof(*d));
}

#define PREVIEW_AUDIO_BUFFERS 8
#define PREVIEW_AUDIO_BYTES 19200
struct DvdAudioPlayer {
    HWAVEOUT wave;
    DvdPcmDecoder decoder;
    HANDLE worker, cancel, changed;
    CRITICAL_SECTION lock;
    WAVEHDR headers[PREVIEW_AUDIO_BUFFERS];
    uint8_t data[PREVIEW_AUDIO_BUFFERS][PREVIEW_AUDIO_BYTES];
    uint64_t queued;
    bool ready, ended, failed;
};

static DWORD WINAPI preview_audio_worker(void* context) {
    DvdAudioPlayer* p = (DvdAudioPlayer*)context; unsigned index = 0;
    while (WaitForSingleObject(p->cancel, 0) != WAIT_OBJECT_0) {
        WAVEHDR* h = &p->headers[index];
        if (h->dwFlags & WHDR_INQUEUE) { HANDLE waits[] = {p->cancel,p->changed}; if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0; continue; }
        size_t bytes = imgburn_pcm_decoder_read(&p->decoder, h->lpData, PREVIEW_AUDIO_BYTES);
        if (!bytes) break;
        EnterCriticalSection(&p->lock); h->dwBufferLength = (DWORD)bytes;
        if (waveOutWrite(p->wave, h, sizeof(*h)) != MMSYSERR_NOERROR) p->failed = true;
        else { p->queued += bytes / 4; p->ready = true; }
        bool failed = p->failed; LeaveCriticalSection(&p->lock); if (failed) break; index = (index + 1) % PREVIEW_AUDIO_BUFFERS;
    }
    EnterCriticalSection(&p->lock); p->failed = p->failed || p->decoder.failed; p->ended = true; LeaveCriticalSection(&p->lock); return 0;
}

DvdAudioPlayer* imgburn_preview_audio_open(const wchar_t* video, const wchar_t* ffmpeg, unsigned streamId, uint64_t firstSample) {
    DvdAudioPlayer* p = (DvdAudioPlayer*)calloc(1, sizeof(*p)); if (!p) return NULL; InitializeCriticalSection(&p->lock);
    p->cancel = CreateEventW(NULL, TRUE, FALSE, NULL); p->changed = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!p->cancel || !p->changed || !imgburn_pcm_decoder_open(&p->decoder, video, ffmpeg, streamId, firstSample)) goto failed;
    WAVEFORMATEX format = {WAVE_FORMAT_PCM,2,48000,192000,4,16,0};
    if (waveOutOpen(&p->wave, WAVE_MAPPER, &format, (DWORD_PTR)p->changed, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR || waveOutPause(p->wave) != MMSYSERR_NOERROR) goto failed;
    for (unsigned i = 0; i < PREVIEW_AUDIO_BUFFERS; i++) { p->headers[i].lpData = (LPSTR)p->data[i]; p->headers[i].dwBufferLength = PREVIEW_AUDIO_BYTES; if (waveOutPrepareHeader(p->wave, &p->headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) goto failed; }
    p->worker = CreateThread(NULL, 0, preview_audio_worker, p, 0, NULL); if (!p->worker) goto failed; return p;
failed:
    imgburn_preview_audio_close(p); SetLastError(ERROR_OPEN_FAILED); return NULL;
}

void imgburn_preview_audio_pause(DvdAudioPlayer* p, bool pause) {
    if (!p) return; EnterCriticalSection(&p->lock); if ((pause ? waveOutPause(p->wave) : waveOutRestart(p->wave)) != MMSYSERR_NOERROR) p->failed = true; LeaveCriticalSection(&p->lock);
}

bool imgburn_preview_audio_position(DvdAudioPlayer* p, uint64_t* played, bool* drained, bool* failed) {
    if (!p || !played || !drained || !failed) return false;
    EnterCriticalSection(&p->lock); MMTIME time = {0}; time.wType = TIME_SAMPLES; MMRESULT result = waveOutGetPosition(p->wave, &time, sizeof(time));
    *played = time.wType == TIME_SAMPLES ? time.u.sample : time.wType == TIME_BYTES ? time.u.cb / 4 : time.wType == TIME_MS ? (uint64_t)time.u.ms * 48 : 0;
    *drained = p->ended && *played >= p->queued; *failed = p->failed || result != MMSYSERR_NOERROR; bool ready = p->ready; LeaveCriticalSection(&p->lock); return ready;
}

void imgburn_preview_audio_close(DvdAudioPlayer* p) {
    if (!p) return;
    if (p->worker) { SetEvent(p->cancel); if (p->decoder.process) TerminateProcess(p->decoder.process, ERROR_CANCELLED); CancelSynchronousIo(p->worker); WaitForSingleObject(p->worker, INFINITE); CloseHandle(p->worker); }
    if (p->wave) { waveOutReset(p->wave); for (unsigned i = 0; i < PREVIEW_AUDIO_BUFFERS; i++) if (p->headers[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(p->wave, &p->headers[i], sizeof(WAVEHDR)); waveOutClose(p->wave); }
    imgburn_pcm_decoder_close(&p->decoder); if (p->cancel) CloseHandle(p->cancel); if (p->changed) CloseHandle(p->changed); DeleteCriticalSection(&p->lock); free(p);
}

bool imgburn_preview_save_bmp(const wchar_t* path, const void* bgr, unsigned width, unsigned height, bool replace) {
    if (!path || !bgr || !width || width > 720 || width % 4 || !height || height > 576) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    BITMAPFILEHEADER fileHeader = {0}; BITMAPINFOHEADER header = {0}; header.biSize = sizeof(header); header.biWidth = width; header.biHeight = height; header.biPlanes = 1; header.biBitCount = 24; header.biSizeImage = width * height * 3;
    fileHeader.bfType = 0x4D42; fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(header); fileHeader.bfSize = fileHeader.bfOffBits + header.biSizeImage;
    HANDLE output = CreateFileW(path, GENERIC_WRITE, 0, NULL, replace ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL); if (output == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0; bool ok = WriteFile(output, &fileHeader, sizeof(fileHeader), &got, NULL) && got == sizeof(fileHeader) && WriteFile(output, &header, sizeof(header), &got, NULL) && got == sizeof(header);
    for (unsigned row = height; ok && row; row--) ok = WriteFile(output, (const uint8_t*)bgr + (row - 1) * width * 3, width * 3, &got, NULL) && got == width * 3;
    ok = ok && FlushFileBuffers(output); CloseHandle(output); return ok;
}

#define PREVIEW_FRAME (WM_APP + 1)
#define PREVIEW_DONE (WM_APP + 2)
enum { PREVIEW_PLAY = 100, PREVIEW_BACK, PREVIEW_NEXT, PREVIEW_FIRST, PREVIEW_LAST, PREVIEW_CELL_BACK, PREVIEW_CELL_NEXT, PREVIEW_SIZE, PREVIEW_SLIDER, PREVIEW_CLOSE, PREVIEW_BMP, PREVIEW_INFO, PREVIEW_AUDIO };
typedef struct {
    const ImageBuilder* builder;
    DvdPreviewPlan plan;
    const wchar_t* ffmpeg;
    HWND window, slider, label, playButton, cellBack, cellNext;
    HWND infoWindow, infoValues[13], audioCombo;
    DvdFrameDecoder decoder;
    DvdAudioPlayer* audio;
    DvdPreviewFrameInfo frameInfo;
    uint64_t audioFirstFrame;
    unsigned audioSelection;
    HANDLE worker, cancel, consumed;
    uint8_t* pixels;
    uint8_t* incoming;
    size_t pixelBytes;
    wchar_t clips[2][MAX_PATH];
    uint64_t videoBytes[2];
    unsigned cellIndex;
    uint64_t frame, nextFrame, clockFrame, clockStart;
    bool playing, pending, realSize, failed, painted, ended;
    bool audioStarted, audioNeedsSeek, audioFailed, hadFailure;
} PreviewWindow;

static uint32_t preview_be32(const uint8_t* d) { return (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]; }

bool imgburn_preview_packet_info(const wchar_t* video, int64_t byteOffset, DvdPreviewPacketInfo* info) {
    if (!info) return false; memset(info, 0, sizeof(*info)); if (!video || byteOffset < 0) return false;
    FILE* file = _wfopen(video, L"rb"); if (!file) return false; uint8_t d[2048]; bool ok = false;
    uint64_t sector = (uint64_t)byteOffset / 2048;
    for (unsigned back = 0; back < 1024 && back <= sector; back++) {
        if (_fseeki64(file, (int64_t)(sector - back) * 2048, SEEK_SET) || fread(d, 1, sizeof(d), file) != sizeof(d)) break;
        if (memcmp(d, "\0\0\1\xBA", 4) || memcmp(d + 38, "\0\0\1\xBF\x03\xD4\0", 7) || memcmp(d + 1024, "\0\0\1\xBF\x03\xFA\x01", 7)) continue;
        if (preview_be32(d + 45) != preview_be32(d + 1035)) break;
        info->sourceSector = preview_be32(d + 45); info->startTime = preview_be32(d + 57); info->endTime = preview_be32(d + 61);
        info->vobId = (unsigned)d[1055] << 8 | d[1056]; info->cellId = d[1058]; info->highlightStatus = d[142] & 3;
        info->buttonCount = d[158] & 63; info->forcedSelect = d[161] & 63; info->forcedActivate = d[162] & 63;
        if (info->buttonCount > 36 || info->forcedSelect > info->buttonCount || info->forcedActivate > info->buttonCount) break;
        bool valid = true;
        for (unsigned i = 0; i < info->buttonCount; i++) {
            const uint8_t* b = d + 187 + i * 18; DvdPreviewButton* button = &info->buttons[i];
            button->left = (unsigned)(b[0] & 63) << 4 | b[1] >> 4; button->right = (unsigned)(b[1] & 3) << 8 | b[2];
            button->top = (unsigned)(b[3] & 63) << 4 | b[4] >> 4; button->bottom = (unsigned)(b[4] & 3) << 8 | b[5];
            button->autoAction = (b[3] & 0x40) != 0; button->up = b[6] & 63; button->down = b[7] & 63; button->previous = b[8] & 63; button->next = b[9] & 63; memcpy(button->command, b + 10, 8);
            if (button->left > button->right || button->right >= 720 || button->top > button->bottom || button->bottom >= 576 || button->up > info->buttonCount || button->down > info->buttonCount || button->previous > info->buttonCount || button->next > info->buttonCount) valid = false;
        }
        ok = valid; break;
    }
    fclose(file); if (!ok) memset(info, 0, sizeof(*info)); return ok;
}

static const wchar_t* preview_audio_format(unsigned format) { return format == 0 ? L"AC3" : format == 4 ? L"LPCM" : format == 6 ? L"DTS" : L"MPEG"; }

static void preview_info_update(PreviewWindow* p) {
    if (!p->infoWindow || !IsWindowVisible(p->infoWindow)) return; wchar_t text[256]; const DvdPreviewPlan* plan = &p->plan;
    swprintf(text, 256, L"%u:%u", plan->aspectNumerator, plan->aspectDenominator); SetWindowTextW(p->infoValues[0], text);
    swprintf(text, 256, L"%.3f fps", (double)plan->frameRateNumerator / plan->frameRateDenominator); SetWindowTextW(p->infoValues[1], text);
    SetWindowTextW(p->infoValues[2], plan->frameRateNumerator == 25 ? L"PAL" : L"NTSC");
    SetWindowTextW(p->infoValues[3], !p->frameInfo.valid ? L"" : p->frameInfo.interlaced ? L"Interlaced" : L"Progressive");
    int64_t us = p->frameInfo.timestampUs; if (us < 0) us = 0; uint64_t seconds = (uint64_t)us / 1000000;
    swprintf(text, 256, L"%llu:%02llu:%02llu.%02llu", seconds / 3600, seconds / 60 % 60, seconds % 60, (uint64_t)us % 1000000 * plan->frameRateNumerator / (1000000ull * plan->frameRateDenominator)); SetWindowTextW(p->infoValues[4], p->frameInfo.valid ? text : L"");
    DvdPreviewPacketInfo packet = {0}; bool got = p->frameInfo.valid && imgburn_preview_packet_info(p->clips[p->cellIndex], p->frameInfo.byteOffset, &packet);
    unsigned part = 1; uint64_t rel = got ? packet.sourceSector : 0; const DvdNavigationCell* c = &plan->cells[p->cellIndex];
    if (got && c->domain == DVD_DOMAIN_TITLE) {
        FsDirectoryNode* dir = p->builder->rootDir->firstChildDir; while (dir && _wcsicmp(dir->nameW, L"VIDEO_TS")) dir = dir->nextSibling;
        for (; dir && part <= 9; part++) { wchar_t name[20]; swprintf(name, 20, L"VTS_%02u_%u.VOB", c->titleSet, part); FsFileNode* f = dir->firstFile; while (f && _wcsicmp(f->nameW, name)) f = f->next; if (!f || rel < f->sectorCount) break; rel -= f->sectorCount; }
    } else part = 0;
    swprintf(text, 256, L"%u / %u", got ? packet.sourceSector : c->firstSector, part); SetWindowTextW(p->infoValues[5], text);
    swprintf(text, 256, L"%u / %u", got ? packet.vobId : c->vobId, got ? packet.cellId : c->cellId); SetWindowTextW(p->infoValues[6], text);
    swprintf(text, 256, L"%u", got ? packet.buttonCount : 0); SetWindowTextW(p->infoValues[7], got ? text : L"");
    SendMessageW(p->infoValues[8], PBM_SETPOS, c->playbackMilliseconds ? (WPARAM)(p->videoBytes[p->cellIndex] * 8 / c->playbackMilliseconds) : 0, 0);
    if (p->audioSelection) { const DvdPreviewAudio* a = &plan->audio[p->audioSelection - 1]; swprintf(text, 256, L"%ls %u ch, %u Hz", preview_audio_format(a->format), a->channels, a->sampleRate); SetWindowTextW(p->infoValues[9], text); swprintf(text, 256, L"0x%02X", a->streamId & 255); SetWindowTextW(p->infoValues[10], text); }
    else { SetWindowTextW(p->infoValues[9], L"Silent"); SetWindowTextW(p->infoValues[10], L""); }
    swprintf(text, 256, L"%c", p->frameInfo.valid ? p->frameInfo.pictureType : L' '); SetWindowTextW(p->infoValues[11], text);
    uint64_t elapsed = GetTickCount64() - p->clockStart;
    swprintf(text, 256, L"%.3f", p->playing && elapsed ? (double)(p->frame - p->clockFrame) * 1000 / elapsed : 0.0); SetWindowTextW(p->infoValues[12], text);
}

static LRESULT CALLBACK preview_info_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    PreviewWindow* p = (PreviewWindow*)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) { p = (PreviewWindow*)((CREATESTRUCTW*)lp)->lpCreateParams; p->infoWindow = window; SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)p); }
    if (message == WM_CREATE) {
        const wchar_t* labels[] = {L"Aspect Ratio",L"Frame Rate",L"Video Type",L"Frame Type",L"Timestamp",L"Sector / VOB",L"Vob Cell ID",L"Num. buttons",L"Avg. Bitrate",L"Format",L"Channel",L"Info",L"FPS"};
        CreateWindowW(L"BUTTON", L"Video", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 8, 6, 344, 294, window, NULL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Audio", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 8, 306, 344, 84, window, NULL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Status", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 8, 396, 344, 84, window, NULL, NULL, NULL);
        for (unsigned i = 0; i < 13; i++) { int y = i < 9 ? 28 + i * 29 : i < 11 ? 328 + (i - 9) * 29 : 418 + (i - 11) * 29; CreateWindowW(L"STATIC", labels[i], WS_CHILD | WS_VISIBLE, 20, y, 125, 22, window, NULL, NULL, NULL); p->infoValues[i] = CreateWindowW(i == 8 ? PROGRESS_CLASSW : L"STATIC", L"", WS_CHILD | WS_VISIBLE | (i == 8 ? 0 : SS_CENTER), 148, y, 194, 22, window, NULL, NULL, NULL); }
        SendMessageW(p->infoValues[8], PBM_SETRANGE32, 0, 10080); return 0;
    }
    if (message == WM_CLOSE) { ShowWindow(window, SW_HIDE); if (p) { SendDlgItemMessageW(p->window, PREVIEW_INFO, BM_SETCHECK, BST_UNCHECKED, 0); InvalidateRect(p->window, NULL, FALSE); } return 0; }
    return DefWindowProcW(window, message, wp, lp);
}

typedef struct { HANDLE file; uint64_t videoBytes; } PreviewClipOutput;
static bool preview_write(const void* data, size_t bytes, void* context) {
    PreviewClipOutput* output = (PreviewClipOutput*)context; DWORD written;
    if (bytes > MAXDWORD || !WriteFile(output->file, data, (DWORD)bytes, &written, NULL) || written != bytes) return false;
    for (size_t sector = 0; sector + 2048 <= bytes; sector += 2048) {
        const uint8_t* d = (const uint8_t*)data + sector; size_t at = 14 + (d[13] & 7);
        while (at + 6 <= 2048 && !memcmp(d + at, "\0\0\1", 3)) {
            size_t count = ((size_t)d[at + 4] << 8 | d[at + 5]) + 6; if (count > 2048 - at) break;
            if (d[at + 3] >= 0xE0 && d[at + 3] <= 0xEF && count >= 9 && (d[at + 6] & 0xC0) == 0x80 && d[at + 8] <= count - 9) output->videoBytes += count - 9 - d[at + 8];
            at += count;
        }
    }
    return true;
}

static bool preview_clip(PreviewWindow* p, unsigned index) {
    if (p->clips[index][0]) return true;
    wchar_t folder[MAX_PATH], name[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, folder);
    if (!n || n >= MAX_PATH || !GetTempFileNameW(folder, L"IBP", 0, name)) return false;
    HANDLE file = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL); bool ok = file != INVALID_HANDLE_VALUE;
    PreviewClipOutput output = {file,0}; uint64_t bytes = 0; if (ok) { ok = imgburn_dvd_read_cell(p->builder, &p->plan.cells[index], preview_write, &output, &bytes); CloseHandle(file); }
    if (!ok || !bytes) { DeleteFileW(name); return false; } p->videoBytes[index] = output.videoBytes; wcscpy(p->clips[index], name); return true;
}

static DWORD WINAPI preview_worker(void* context) {
    PreviewWindow* p = (PreviewWindow*)context;
    while (WaitForSingleObject(p->cancel, 0) != WAIT_OBJECT_0) {
        if (!imgburn_frame_decoder_read(&p->decoder, p->incoming, p->pixelBytes)) break;
        if (!PostMessageW(p->window, PREVIEW_FRAME, 0, 0)) return 0;
        HANDLE events[] = {p->cancel,p->consumed}; if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0;
    }
    if (WaitForSingleObject(p->cancel, 0) != WAIT_OBJECT_0) PostMessageW(p->window, PREVIEW_DONE, p->decoder.failed, 0); return 0;
}

static void preview_stop(PreviewWindow* p) {
    if (p->worker) {
        SetEvent(p->cancel); SetEvent(p->consumed);
        /* 只终止由当前预览创建的解码进程，解除管道读取后再等待工作线程退出。 */
        if (p->decoder.process) TerminateProcess(p->decoder.process, ERROR_CANCELLED); CancelSynchronousIo(p->worker); WaitForSingleObject(p->worker, INFINITE); CloseHandle(p->worker); p->worker = NULL;
    }
    imgburn_frame_decoder_close(&p->decoder); p->pending = false;
    imgburn_preview_audio_close(p->audio); p->audio = NULL; p->audioStarted = false;
    MSG msg; while (PeekMessageW(&msg, p->window, PREVIEW_FRAME, PREVIEW_DONE, PM_REMOVE)) {}
}

static uint64_t preview_frames(const PreviewWindow* p) {
    return ((uint64_t)p->plan.cells[p->cellIndex].playbackMilliseconds * p->plan.frameRateNumerator + 500 * p->plan.frameRateDenominator) / (1000 * p->plan.frameRateDenominator);
}

static void preview_status(PreviewWindow* p) {
    wchar_t title[256], text[256], audio[64] = L"Silent"; const DvdNavigationCell* c = &p->plan.cells[p->cellIndex];
    swprintf(title, 256, L"ImgBurn Preview - VTS %u %ls LU %u PGC %u Cell %u", c->titleSet, c->domain == DVD_DOMAIN_TITLE ? L"Title" : L"Menu", c->languageUnit, c->pgc, c->cell); SetWindowTextW(p->window, title);
    uint64_t ms = p->frame * 1000 * p->plan.frameRateDenominator / p->plan.frameRateNumerator;
    if (p->audioSelection) { const DvdPreviewAudio* a = &p->plan.audio[p->audioSelection - 1]; swprintf(audio, 64, L"0x%02X (%ls)", a->streamId & 255, preview_audio_format(a->format)); }
    swprintf(text, 256, L"Cell %u/%u   Frame %llu   %02llu:%02llu.%03llu   %ls", p->cellIndex + 1, p->plan.cellCount, p->frame, ms / 60000, ms / 1000 % 60, ms % 1000, p->failed ? L"Decode failed" : p->audioFailed ? L"Audio failed" : audio); SetWindowTextW(p->label, text);
    SetWindowTextW(p->playButton, p->playing ? L"Pause" : L"Play"); EnableWindow(p->cellBack, p->cellIndex > 0); EnableWindow(p->cellNext, p->cellIndex + 1 < p->plan.cellCount);
    uint64_t frames = preview_frames(p); SendMessageW(p->slider, TBM_SETPOS, TRUE, frames > 1 ? (LPARAM)(p->frame * 1000 / (frames - 1)) : 0);
    preview_info_update(p);
}

static void preview_audio_restart(PreviewWindow* p) {
    imgburn_preview_audio_close(p->audio); p->audio = NULL; p->audioStarted = false; p->audioFailed = false; p->audioNeedsSeek = false; p->audioFirstFrame = p->frame;
    if (p->audioSelection) { uint64_t sample = p->frame * 48000 * p->plan.frameRateDenominator / p->plan.frameRateNumerator; p->audio = imgburn_preview_audio_open(p->clips[p->cellIndex], p->ffmpeg, p->plan.audio[p->audioSelection - 1].streamId, sample); p->audioFailed = p->audio == NULL; }
    p->hadFailure = p->hadFailure || p->audioFailed;
}

static bool preview_start(PreviewWindow* p, uint64_t frame, bool play) {
    preview_stop(p); p->playing = play; p->failed = false; p->painted = false; p->ended = false; p->frame = frame; p->nextFrame = frame; p->clockFrame = frame; p->clockStart = GetTickCount64(); ResetEvent(p->cancel); ResetEvent(p->consumed);
    memset(&p->frameInfo, 0, sizeof(p->frameInfo)); InvalidateRect(p->window, NULL, FALSE);
    if (!preview_clip(p, p->cellIndex) || !imgburn_frame_decoder_open(&p->decoder, p->clips[p->cellIndex], p->ffmpeg, p->plan.width, p->plan.height, frame)) goto failed;
    preview_audio_restart(p);
    p->worker = CreateThread(NULL, 0, preview_worker, p, 0, NULL); if (!p->worker) goto failed;
    preview_status(p); return true;
failed:
    p->failed = true; p->hadFailure = true; p->playing = false; preview_status(p); return false;
}

static void preview_layout(PreviewWindow* p) {
    RECT r; GetClientRect(p->window, &r); int w = r.right;
    MoveWindow(p->slider, 8, 40, w - 16, 28, TRUE); MoveWindow(p->label, 8, r.bottom - 28, w - 105, 24, TRUE); MoveWindow(GetDlgItem(p->window, PREVIEW_CLOSE), w - 96, r.bottom - 30, 88, 25, TRUE);
    MoveWindow(GetDlgItem(p->window, PREVIEW_BMP), w - 325, 6, 60, 28, TRUE); MoveWindow(GetDlgItem(p->window, PREVIEW_INFO), w - 260, 6, 62, 28, TRUE); MoveWindow(p->audioCombo, w - 194, 8, 186, 240, TRUE);
}

static void preview_paint(PreviewWindow* p, HDC dc) {
    RECT r; GetClientRect(p->window, &r); r.top = 72; r.bottom -= 36; FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); if (!p->painted) return;
    int width = r.right, height = r.bottom - r.top; if (height <= 0) return;
    unsigned an = p->plan.aspectNumerator, ad = p->plan.aspectDenominator;
    int dw = p->realSize ? (int)p->plan.width : width, dh = p->realSize ? (int)p->plan.height : (int)((uint64_t)width * ad / an);
    if (!p->realSize && dh > height) { dh = height; dw = (int)((uint64_t)height * an / ad); }
    BITMAPINFO bitmap = {0}; bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bitmap.bmiHeader.biWidth = p->plan.width; bitmap.bmiHeader.biHeight = -(LONG)p->plan.height; bitmap.bmiHeader.biPlanes = 1; bitmap.bmiHeader.biBitCount = 24; bitmap.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE); StretchDIBits(dc, (width - dw) / 2, r.top + (height - dh) / 2, dw, dh, 0, 0, p->plan.width, p->plan.height, p->pixels, &bitmap, DIB_RGB_COLORS, SRCCOPY);
    /* 原版只在Info开启时标出菜单按钮边界，边框不写入BMP，也不执行按钮命令。 */
    DvdPreviewPacketInfo packet;
    if (SendDlgItemMessageW(p->window, PREVIEW_INFO, BM_GETCHECK, 0, 0) == BST_CHECKED && p->frameInfo.valid && imgburn_preview_packet_info(p->clips[p->cellIndex], p->frameInfo.byteOffset, &packet) && packet.highlightStatus) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(255,0,0));
        if (pen) {
            HGDIOBJ previousPen = SelectObject(dc, pen), previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH)); int left = (width - dw) / 2, top = r.top + (height - dh) / 2;
            for (unsigned i = 0; i < packet.buttonCount; i++) {
                const DvdPreviewButton* button = &packet.buttons[i];
                Rectangle(dc, left + (int)((uint64_t)button->left * dw / p->plan.width), top + (int)((uint64_t)button->top * dh / p->plan.height), left + (int)((uint64_t)(button->right + 1) * dw / p->plan.width), top + (int)((uint64_t)(button->bottom + 1) * dh / p->plan.height));
            }
            SelectObject(dc, previousBrush); SelectObject(dc, previousPen); DeleteObject(pen);
        }
    }
}

static LRESULT CALLBACK preview_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    PreviewWindow* p = (PreviewWindow*)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) { p = (PreviewWindow*)((CREATESTRUCTW*)lp)->lpCreateParams; p->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)p); }
    if (!p) return DefWindowProcW(window, message, wp, lp);
    if (message == WM_CREATE) {
        HINSTANCE instance = GetModuleHandleW(NULL); const wchar_t* labels[] = {L"Play",L"<",L">",L"|<",L">|",L"Cell-",L"Cell+",L"1:1"};
        for (unsigned i = 0; i < 8; i++) CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP, 8 + i * 65, 6, 60, 28, window, (HMENU)(UINT_PTR)(PREVIEW_PLAY + i), instance, NULL);
        CreateWindowW(L"BUTTON", L"BMP", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 528, 6, 60, 28, window, (HMENU)PREVIEW_BMP, instance, NULL);
        CreateWindowW(L"BUTTON", L"Info", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 594, 6, 62, 28, window, (HMENU)PREVIEW_INFO, instance, NULL);
        p->audioCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 660, 8, 180, 240, window, (HMENU)PREVIEW_AUDIO, instance, NULL);
        SendMessageW(p->audioCombo, CB_ADDSTRING, 0, (LPARAM)L"Silent");
        SendMessageW(p->audioCombo, CB_SETITEMDATA, 0, 0);
        for (unsigned i = 0; i < p->plan.audioCount; i++) {
            /* 原版预览只列AC3与LPCM，元数据解析能力与可选播放格式分开。 */
            if (p->plan.audio[i].format != 0 && p->plan.audio[i].format != 4) continue;
            wchar_t name[64]; swprintf(name, 64, L"0x%02X (%ls)", p->plan.audio[i].streamId & 255, preview_audio_format(p->plan.audio[i].format)); LRESULT index = SendMessageW(p->audioCombo, CB_ADDSTRING, 0, (LPARAM)name); SendMessageW(p->audioCombo, CB_SETITEMDATA, index, i + 1);
            if (p->audioSelection == i + 1) SendMessageW(p->audioCombo, CB_SETCURSEL, index, 0);
        }
        if (!p->audioSelection) SendMessageW(p->audioCombo, CB_SETCURSEL, 0, 0);
        p->slider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS, 8, 40, 600, 28, window, (HMENU)PREVIEW_SLIDER, instance, NULL); SendMessageW(p->slider, TBM_SETRANGE, TRUE, MAKELONG(0,1000));
        p->label = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 8, 510, 540, 24, window, NULL, instance, NULL); CreateWindowW(L"BUTTON", L"Close Preview", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 545, 510, 100, 25, window, (HMENU)PREVIEW_CLOSE, instance, NULL);
        p->playButton = GetDlgItem(window, PREVIEW_PLAY); p->cellBack = GetDlgItem(window, PREVIEW_CELL_BACK); p->cellNext = GetDlgItem(window, PREVIEW_CELL_NEXT); SetTimer(window, 1, 10, NULL); return 0;
    }
    if (message == PREVIEW_FRAME) { memcpy(p->pixels, p->incoming, p->pixelBytes); p->frameInfo = p->decoder.info; p->frame = p->nextFrame++; p->pending = true; p->painted = true; if (p->frame == p->clockFrame) p->clockStart = GetTickCount64(); preview_status(p); InvalidateRect(window, NULL, FALSE); return 0; }
    if (message == WM_TIMER) {
        if (p->playing && p->pending) {
            uint64_t played = 0; bool drained = false, failed = false, ready = p->audio && imgburn_preview_audio_position(p->audio, &played, &drained, &failed);
            if (failed) { p->audioFailed = true; p->hadFailure = true; preview_status(p); }
            if (p->audio && !p->audioStarted) { if (!ready && !drained && !failed) return 0; imgburn_preview_audio_pause(p->audio, false); p->audioStarted = true; p->clockFrame = p->frame; p->clockStart = GetTickCount64(); }
            bool due = ready && !drained && !failed ? played >= (p->frame - p->audioFirstFrame + 1) * 48000 * p->plan.frameRateDenominator / p->plan.frameRateNumerator : GetTickCount64() >= p->clockStart + (p->frame - p->clockFrame + 1) * 1000 * p->plan.frameRateDenominator / p->plan.frameRateNumerator;
            if (due) { p->pending = false; SetEvent(p->consumed); }
        } return 0;
    }
    if (message == PREVIEW_DONE) {
        p->failed = wp != 0; p->hadFailure = p->hadFailure || p->failed; p->ended = true;
        if (!p->failed && p->playing && p->cellIndex + 1 < p->plan.cellCount) { p->cellIndex++; preview_start(p, 0, true); }
        else { p->playing = false; imgburn_preview_audio_pause(p->audio, true); preview_status(p); } return 0;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wp)) {
            case PREVIEW_PLAY:
                if (p->ended) preview_start(p, 0, true); else { p->playing = !p->playing; if (p->playing && p->audioNeedsSeek) preview_audio_restart(p); if (!p->playing) p->audioNeedsSeek = true; imgburn_preview_audio_pause(p->audio, !p->playing || !p->audioStarted); p->clockFrame = p->frame; p->clockStart = GetTickCount64(); preview_status(p); } break;
            case PREVIEW_NEXT: if (p->pending) { p->playing = false; imgburn_preview_audio_pause(p->audio, true); p->audioNeedsSeek = true; p->pending = false; SetEvent(p->consumed); preview_status(p); } break;
            case PREVIEW_BACK: preview_start(p, p->frame ? p->frame - 1 : 0, false); break;
            case PREVIEW_FIRST: preview_start(p, 0, false); break;
            case PREVIEW_LAST: { uint64_t frames = preview_frames(p); preview_start(p, frames ? frames - 1 : 0, false); break; }
            case PREVIEW_CELL_BACK: if (p->cellIndex) { p->cellIndex--; preview_start(p, 0, false); } break;
            case PREVIEW_CELL_NEXT: if (p->cellIndex + 1 < p->plan.cellCount) { p->cellIndex++; preview_start(p, 0, false); } break;
            case PREVIEW_SIZE: {
                p->realSize = !p->realSize;
                if (p->realSize) { RECT size = {0,0,(LONG)p->plan.width + 16 > 864 ? (LONG)p->plan.width + 16 : 864,(LONG)p->plan.height + 108}; AdjustWindowRect(&size, (DWORD)GetWindowLongPtrW(window, GWL_STYLE), FALSE); SetWindowPos(window, NULL, 0, 0, size.right - size.left, size.bottom - size.top, SWP_NOMOVE | SWP_NOZORDER); }
                InvalidateRect(window, NULL, FALSE); break;
            }
            case PREVIEW_AUDIO: if (HIWORD(wp) == CBN_SELCHANGE) { LRESULT index = SendMessageW(p->audioCombo, CB_GETCURSEL, 0, 0); LRESULT selected = index < 0 ? -1 : SendMessageW(p->audioCombo, CB_GETITEMDATA, index, 0); if (selected >= 0 && (unsigned)selected <= p->plan.audioCount) { p->audioSelection = (unsigned)selected; preview_start(p, p->frame, p->playing); } } break;
            case PREVIEW_INFO:
                if (!p->infoWindow) {
                    RECT r; GetWindowRect(window, &r); MONITORINFO monitor = {sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
                    int x = r.right, y = r.top; if (x + 378 > monitor.rcWork.right) x = monitor.rcWork.right - 378; if (x < monitor.rcWork.left) x = monitor.rcWork.left;
                    if (y + 526 > monitor.rcWork.bottom) y = monitor.rcWork.bottom - 526; if (y < monitor.rcWork.top) y = monitor.rcWork.top;
                    CreateWindowExW(WS_EX_TOOLWINDOW, L"ImgBurnPreviewInfo", L"Information", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, 378, 526, window, NULL, GetModuleHandleW(NULL), p);
                }
                if (p->infoWindow) { ShowWindow(p->infoWindow, SendDlgItemMessageW(window, PREVIEW_INFO, BM_GETCHECK, 0, 0) == BST_CHECKED ? SW_SHOWNORMAL : SW_HIDE); preview_info_update(p); }
                else SendDlgItemMessageW(window, PREVIEW_INFO, BM_SETCHECK, BST_UNCHECKED, 0); InvalidateRect(window, NULL, FALSE); break;
            case PREVIEW_BMP: {
                if (!p->painted) break; bool resume = p->playing; p->playing = false; imgburn_preview_audio_pause(p->audio, true);
                wchar_t path[MAX_PATH] = L"ImgBurn_Preview.bmp"; OPENFILENAMEW save = {0}; save.lStructSize = sizeof(save); save.hwndOwner = window; save.lpstrFilter = L"BMP File (*.bmp)\0*.bmp\0\0"; save.lpstrFile = path; save.nMaxFile = MAX_PATH; save.lpstrDefExt = L"bmp"; save.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
                if (GetSaveFileNameW(&save) && !imgburn_preview_save_bmp(path, p->pixels, p->plan.width, p->plan.height, true)) MessageBoxW(window, L"Unable to save the current frame.", L"ImgBurn Preview", MB_OK | MB_ICONERROR);
                p->playing = resume; imgburn_preview_audio_pause(p->audio, !resume); p->clockFrame = p->frame; p->clockStart = GetTickCount64(); preview_status(p); break;
            }
            case PREVIEW_CLOSE: SendMessageW(window, WM_CLOSE, 0, 0); break;
        }
        return 0;
    }
    if (message == WM_HSCROLL && (HWND)lp == p->slider) {
        unsigned event = LOWORD(wp); if (event != TB_THUMBTRACK && event != TB_ENDTRACK) { uint64_t frames = preview_frames(p); unsigned pos = (unsigned)SendMessageW(p->slider, TBM_GETPOS, 0, 0); preview_start(p, frames > 1 ? (frames - 1) * pos / 1000 : 0, false); } return 0;
    }
    if (message == WM_KEYDOWN) { if (wp == VK_SPACE) SendMessageW(window, WM_COMMAND, PREVIEW_PLAY, 0); else if (wp == VK_RIGHT) SendMessageW(window, WM_COMMAND, PREVIEW_NEXT, 0); else if (wp == VK_LEFT) SendMessageW(window, WM_COMMAND, PREVIEW_BACK, 0); else if (wp == VK_ESCAPE) SendMessageW(window, WM_CLOSE, 0, 0); return 0; }
    if (message == WM_GETMINMAXINFO) { MINMAXINFO* limits = (MINMAXINFO*)lp; limits->ptMinTrackSize.x = 880; limits->ptMinTrackSize.y = 260; return 0; }
    if (message == WM_SIZE) { preview_layout(p); InvalidateRect(window, NULL, FALSE); return 0; }
    if (message == WM_PAINT) { PAINTSTRUCT ps; HDC dc = BeginPaint(window, &ps); preview_paint(p, dc); EndPaint(window, &ps); return 0; }
    if (message == WM_CLOSE) { preview_stop(p); DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { KillTimer(window, 1); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}

int imgburn_dvd_preview_show(const ImageBuilder* builder, const DvdPreviewPlan* plan, const wchar_t* ffmpeg) {
    if (!builder || !plan || !plan->cellCount || plan->cellCount > 2 || plan->audioCount > 8 || plan->selectedIndex >= plan->cellCount || !plan->frameRateNumerator || !plan->frameRateDenominator || !plan->width || plan->width > 720 || plan->width % 4 || !plan->height || plan->height > 576 || !plan->aspectNumerator || !plan->aspectDenominator) return 1;
    PreviewWindow* p = (PreviewWindow*)calloc(1, sizeof(*p)); if (!p) return 1;
    p->builder = builder; p->plan = *plan; p->ffmpeg = ffmpeg; p->cellIndex = plan->selectedIndex; p->pixelBytes = (size_t)plan->width * plan->height * 3;
    for (unsigned i = 0; i < plan->audioCount; i++) if (plan->audio[i].format == 0 || plan->audio[i].format == 4) { p->audioSelection = i + 1; break; }
    p->pixels = (uint8_t*)malloc(p->pixelBytes); p->incoming = (uint8_t*)malloc(p->pixelBytes); p->cancel = CreateEventW(NULL, TRUE, FALSE, NULL); p->consumed = CreateEventW(NULL, FALSE, FALSE, NULL);
    INITCOMMONCONTROLSEX controls = {sizeof(controls),ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW wc = {0}; wc.lpfnWndProc = preview_proc; wc.hInstance = GetModuleHandleW(NULL); wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"ImgBurnCellPreview";
    bool registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    WNDCLASSW infoClass = wc; infoClass.lpfnWndProc = preview_info_proc; infoClass.lpszClassName = L"ImgBurnPreviewInfo"; registered = registered && (RegisterClassW(&infoClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    HWND window = p->pixels && p->incoming && p->cancel && p->consumed && registered ? CreateWindowW(wc.lpszClassName, L"ImgBurn Preview", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 680, NULL, NULL, wc.hInstance, p) : NULL;
    if (window) {
        ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window); preview_start(p, 0, true); MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) { if (msg.message == WM_KEYDOWN && msg.hwnd != p->audioCombo && (msg.wParam == VK_SPACE || msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT || msg.wParam == VK_ESCAPE)) SendMessageW(window, msg.message, msg.wParam, msg.lParam); else { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    } else p->failed = true;
    preview_stop(p); for (unsigned i = 0; i < 2; i++) if (p->clips[i][0]) DeleteFileW(p->clips[i]);
    if (p->cancel) CloseHandle(p->cancel); if (p->consumed) CloseHandle(p->consumed); free(p->incoming); free(p->pixels); bool failed = p->failed || p->audioFailed || p->hadFailure; free(p); return failed ? 1 : 0;
}
