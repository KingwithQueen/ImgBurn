#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_preview.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "comctl32.lib")

static bool preview_ffmpeg(const wchar_t* requested, wchar_t* path) {
    if (requested) { DWORD n = GetFullPathNameW(requested, MAX_PATH, path, NULL); return n && n < MAX_PATH && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
    DWORD n = SearchPathW(NULL, L"ffmpeg.exe", NULL, MAX_PATH, path, NULL); return n && n < MAX_PATH;
}

bool imgburn_frame_decoder_open(DvdFrameDecoder* d, const wchar_t* video, const wchar_t* ffmpeg, unsigned width, unsigned height, uint64_t firstFrame) {
    if (!d) return false; memset(d, 0, sizeof(*d)); d->exitCode = STILL_ACTIVE;
    if (!video || !width || width > 720 || width % 4 || !height || height > 576 || firstFrame > UINT32_MAX || wcschr(video, L'"')) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    wchar_t executable[MAX_PATH], command[2048]; if (!preview_ffmpeg(ffmpeg, executable)) { SetLastError(ERROR_FILE_NOT_FOUND); return false; }
    if (swprintf(command, 2048, L"\"%ls\" -nostdin -v error -i \"%ls\" -map 0:v:0 -vf \"select=gte(n\\,%llu),scale=%u:%u\" -vsync 0 -an -sn -dn -pix_fmt bgr24 -f rawvideo pipe:1", executable, video, firstFrame, width, height) < 0) return false;
    SECURITY_ATTRIBUTES sa = {sizeof(sa),NULL,TRUE}; HANDLE writer = NULL, nullFile = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&d->output, &writer, &sa, 0) || !SetHandleInformation(d->output, HANDLE_FLAG_INHERIT, 0)) goto failed;
    nullFile = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL); if (nullFile == INVALID_HANDLE_VALUE) goto failed;
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(bytes); if (!attributes) goto failed;
    bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE;
    HANDLE inherited[] = {writer,nullFile};
    bool ready = initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), NULL, NULL);
    STARTUPINFOEXW si = {0}; si.StartupInfo.cb = sizeof(si); si.StartupInfo.dwFlags = STARTF_USESTDHANDLES; si.StartupInfo.hStdInput = nullFile; si.StartupInfo.hStdOutput = writer; si.StartupInfo.hStdError = nullFile; si.lpAttributeList = attributes;
    PROCESS_INFORMATION pi = {0}; BOOL created = ready && CreateProcessW(executable, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi);
    if (initialized) DeleteProcThreadAttributeList(attributes); free(attributes); CloseHandle(nullFile); nullFile = INVALID_HANDLE_VALUE; CloseHandle(writer); writer = NULL;
    if (!created) goto failed;
    CloseHandle(pi.hThread); d->process = pi.hProcess; d->width = width; d->height = height; return true;
failed:
    if (writer) CloseHandle(writer); if (nullFile != INVALID_HANDLE_VALUE) CloseHandle(nullFile); imgburn_frame_decoder_close(d); return false;
}

bool imgburn_frame_decoder_read(DvdFrameDecoder* d, void* bgr, size_t capacity) {
    if (!d || !d->process || !bgr || d->ended) return false;
    size_t expected = (size_t)d->width * d->height * 3, used = 0;
    if (capacity < expected) { d->failed = true; SetLastError(ERROR_INSUFFICIENT_BUFFER); return false; }
    while (used < expected) {
        DWORD got = 0; if (!ReadFile(d->output, (uint8_t*)bgr + used, (DWORD)(expected - used), &got, NULL) || !got) break; used += got;
    }
    if (used == expected) return true;
    d->ended = true;
    DWORD wait = WaitForSingleObject(d->process, 5000); bool exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(d->process, &d->exitCode);
    d->failed = used != 0 || !exited || d->exitCode != 0; if (d->failed) SetLastError(ERROR_INVALID_DATA); return false;
}

void imgburn_frame_decoder_close(DvdFrameDecoder* d) {
    if (!d) return;
    if (d->process) { if (WaitForSingleObject(d->process, 0) == WAIT_TIMEOUT) { TerminateProcess(d->process, ERROR_CANCELLED); WaitForSingleObject(d->process, 5000); } CloseHandle(d->process); }
    if (d->output) CloseHandle(d->output); memset(d, 0, sizeof(*d));
}

#define PREVIEW_FRAME (WM_APP + 1)
#define PREVIEW_DONE (WM_APP + 2)
enum { PREVIEW_PLAY = 100, PREVIEW_BACK, PREVIEW_NEXT, PREVIEW_FIRST, PREVIEW_LAST, PREVIEW_CELL_BACK, PREVIEW_CELL_NEXT, PREVIEW_SIZE, PREVIEW_SLIDER, PREVIEW_CLOSE };
typedef struct {
    const ImageBuilder* builder;
    DvdPreviewPlan plan;
    const wchar_t* ffmpeg;
    HWND window, slider, label, playButton, cellBack, cellNext;
    DvdFrameDecoder decoder;
    HANDLE worker, cancel, consumed;
    uint8_t* pixels;
    uint8_t* incoming;
    size_t pixelBytes;
    wchar_t clips[2][MAX_PATH];
    unsigned cellIndex;
    uint64_t frame, nextFrame, clockFrame, clockStart;
    bool playing, pending, realSize, failed, painted, ended;
} PreviewWindow;

static bool preview_write(const void* data, size_t bytes, void* context) {
    HANDLE file = (HANDLE)context; DWORD written; return bytes <= MAXDWORD && WriteFile(file, data, (DWORD)bytes, &written, NULL) && written == bytes;
}

static bool preview_clip(PreviewWindow* p, unsigned index) {
    if (p->clips[index][0]) return true;
    wchar_t folder[MAX_PATH], name[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, folder);
    if (!n || n >= MAX_PATH || !GetTempFileNameW(folder, L"IBP", 0, name)) return false;
    HANDLE file = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL); bool ok = file != INVALID_HANDLE_VALUE;
    uint64_t bytes = 0; if (ok) { ok = imgburn_dvd_read_cell(p->builder, &p->plan.cells[index], preview_write, file, &bytes); CloseHandle(file); }
    if (!ok || !bytes) { DeleteFileW(name); return false; } wcscpy(p->clips[index], name); return true;
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
    MSG msg; while (PeekMessageW(&msg, p->window, PREVIEW_FRAME, PREVIEW_DONE, PM_REMOVE)) {}
}

static uint64_t preview_frames(const PreviewWindow* p) {
    return ((uint64_t)p->plan.cells[p->cellIndex].playbackMilliseconds * p->plan.frameRateNumerator + 500 * p->plan.frameRateDenominator) / (1000 * p->plan.frameRateDenominator);
}

static void preview_status(PreviewWindow* p) {
    wchar_t title[256], text[256]; const DvdNavigationCell* c = &p->plan.cells[p->cellIndex];
    swprintf(title, 256, L"ImgBurn Preview - VTS %u %ls LU %u PGC %u Cell %u", c->titleSet, c->domain == DVD_DOMAIN_TITLE ? L"Title" : L"Menu", c->languageUnit, c->pgc, c->cell); SetWindowTextW(p->window, title);
    uint64_t ms = p->frame * 1000 * p->plan.frameRateDenominator / p->plan.frameRateNumerator;
    swprintf(text, 256, L"Cell %u/%u   Frame %llu   %02llu:%02llu.%03llu   %ls", p->cellIndex + 1, p->plan.cellCount, p->frame, ms / 60000, ms / 1000 % 60, ms % 1000, p->failed ? L"Decode failed" : L"Silent"); SetWindowTextW(p->label, text);
    SetWindowTextW(p->playButton, p->playing ? L"Pause" : L"Play"); EnableWindow(p->cellBack, p->cellIndex > 0); EnableWindow(p->cellNext, p->cellIndex + 1 < p->plan.cellCount);
    uint64_t frames = preview_frames(p); SendMessageW(p->slider, TBM_SETPOS, TRUE, frames > 1 ? (LPARAM)(p->frame * 1000 / (frames - 1)) : 0);
}

static bool preview_start(PreviewWindow* p, uint64_t frame, bool play) {
    preview_stop(p); p->playing = play; p->failed = false; p->painted = false; p->ended = false; p->frame = frame; p->nextFrame = frame; p->clockFrame = frame; p->clockStart = GetTickCount64(); ResetEvent(p->cancel); ResetEvent(p->consumed);
    if (!preview_clip(p, p->cellIndex) || !imgburn_frame_decoder_open(&p->decoder, p->clips[p->cellIndex], p->ffmpeg, p->plan.width, p->plan.height, frame)) goto failed;
    p->worker = CreateThread(NULL, 0, preview_worker, p, 0, NULL); if (!p->worker) goto failed;
    preview_status(p); return true;
failed:
    p->failed = true; p->playing = false; preview_status(p); return false;
}

static void preview_layout(PreviewWindow* p) {
    RECT r; GetClientRect(p->window, &r); int w = r.right;
    MoveWindow(p->slider, 8, 40, w - 16, 28, TRUE); MoveWindow(p->label, 8, r.bottom - 28, w - 105, 24, TRUE); MoveWindow(GetDlgItem(p->window, PREVIEW_CLOSE), w - 96, r.bottom - 30, 88, 25, TRUE);
}

static void preview_paint(PreviewWindow* p, HDC dc) {
    RECT r; GetClientRect(p->window, &r); r.top = 72; r.bottom -= 36; FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); if (!p->painted) return;
    int width = r.right, height = r.bottom - r.top; if (height <= 0) return;
    unsigned an = p->plan.aspectNumerator, ad = p->plan.aspectDenominator;
    int dw = p->realSize ? (int)p->plan.width : width, dh = p->realSize ? (int)p->plan.height : (int)((uint64_t)width * ad / an);
    if (!p->realSize && dh > height) { dh = height; dw = (int)((uint64_t)height * an / ad); }
    BITMAPINFO bitmap = {0}; bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bitmap.bmiHeader.biWidth = p->plan.width; bitmap.bmiHeader.biHeight = -(LONG)p->plan.height; bitmap.bmiHeader.biPlanes = 1; bitmap.bmiHeader.biBitCount = 24; bitmap.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE); StretchDIBits(dc, (width - dw) / 2, r.top + (height - dh) / 2, dw, dh, 0, 0, p->plan.width, p->plan.height, p->pixels, &bitmap, DIB_RGB_COLORS, SRCCOPY);
}

static LRESULT CALLBACK preview_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    PreviewWindow* p = (PreviewWindow*)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) { p = (PreviewWindow*)((CREATESTRUCTW*)lp)->lpCreateParams; p->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)p); }
    if (!p) return DefWindowProcW(window, message, wp, lp);
    if (message == WM_CREATE) {
        HINSTANCE instance = GetModuleHandleW(NULL); const wchar_t* labels[] = {L"Play",L"<",L">",L"|<",L">|",L"Cell-",L"Cell+",L"1:1"};
        for (unsigned i = 0; i < 8; i++) CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP, 8 + i * 65, 6, 60, 28, window, (HMENU)(UINT_PTR)(PREVIEW_PLAY + i), instance, NULL);
        p->slider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS, 8, 40, 600, 28, window, (HMENU)PREVIEW_SLIDER, instance, NULL); SendMessageW(p->slider, TBM_SETRANGE, TRUE, MAKELONG(0,1000));
        p->label = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 8, 510, 540, 24, window, NULL, instance, NULL); CreateWindowW(L"BUTTON", L"Close Preview", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 545, 510, 100, 25, window, (HMENU)PREVIEW_CLOSE, instance, NULL);
        p->playButton = GetDlgItem(window, PREVIEW_PLAY); p->cellBack = GetDlgItem(window, PREVIEW_CELL_BACK); p->cellNext = GetDlgItem(window, PREVIEW_CELL_NEXT); SetTimer(window, 1, 10, NULL); return 0;
    }
    if (message == PREVIEW_FRAME) { memcpy(p->pixels, p->incoming, p->pixelBytes); p->frame = p->nextFrame++; p->pending = true; p->painted = true; if (p->frame == p->clockFrame) p->clockStart = GetTickCount64(); preview_status(p); InvalidateRect(window, NULL, FALSE); return 0; }
    if (message == WM_TIMER) {
        if (p->playing && p->pending && GetTickCount64() >= p->clockStart + (p->frame - p->clockFrame + 1) * 1000 * p->plan.frameRateDenominator / p->plan.frameRateNumerator) { p->pending = false; SetEvent(p->consumed); } return 0;
    }
    if (message == PREVIEW_DONE) {
        p->failed = wp != 0; p->ended = true;
        if (!p->failed && p->playing && p->cellIndex + 1 < p->plan.cellCount) { p->cellIndex++; preview_start(p, 0, true); }
        else { p->playing = false; preview_status(p); } return 0;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wp)) {
            case PREVIEW_PLAY:
                if (p->ended) preview_start(p, 0, true); else { p->playing = !p->playing; p->clockFrame = p->frame; p->clockStart = GetTickCount64(); preview_status(p); } break;
            case PREVIEW_NEXT: if (p->pending) { p->playing = false; p->pending = false; SetEvent(p->consumed); preview_status(p); } break;
            case PREVIEW_BACK: preview_start(p, p->frame ? p->frame - 1 : 0, false); break;
            case PREVIEW_FIRST: preview_start(p, 0, false); break;
            case PREVIEW_LAST: { uint64_t frames = preview_frames(p); preview_start(p, frames ? frames - 1 : 0, false); break; }
            case PREVIEW_CELL_BACK: if (p->cellIndex) { p->cellIndex--; preview_start(p, 0, false); } break;
            case PREVIEW_CELL_NEXT: if (p->cellIndex + 1 < p->plan.cellCount) { p->cellIndex++; preview_start(p, 0, false); } break;
            case PREVIEW_SIZE: {
                p->realSize = !p->realSize;
                if (p->realSize) { RECT size = {0,0,(LONG)p->plan.width + 16,(LONG)p->plan.height + 108}; AdjustWindowRect(&size, (DWORD)GetWindowLongPtrW(window, GWL_STYLE), FALSE); SetWindowPos(window, NULL, 0, 0, size.right - size.left, size.bottom - size.top, SWP_NOMOVE | SWP_NOZORDER); }
                InvalidateRect(window, NULL, FALSE); break;
            }
            case PREVIEW_CLOSE: SendMessageW(window, WM_CLOSE, 0, 0); break;
        }
        return 0;
    }
    if (message == WM_HSCROLL && (HWND)lp == p->slider) {
        unsigned event = LOWORD(wp); if (event != TB_THUMBTRACK && event != TB_ENDTRACK) { uint64_t frames = preview_frames(p); unsigned pos = (unsigned)SendMessageW(p->slider, TBM_GETPOS, 0, 0); preview_start(p, frames > 1 ? (frames - 1) * pos / 1000 : 0, false); } return 0;
    }
    if (message == WM_KEYDOWN) { if (wp == VK_SPACE) SendMessageW(window, WM_COMMAND, PREVIEW_PLAY, 0); else if (wp == VK_RIGHT) SendMessageW(window, WM_COMMAND, PREVIEW_NEXT, 0); else if (wp == VK_LEFT) SendMessageW(window, WM_COMMAND, PREVIEW_BACK, 0); else if (wp == VK_ESCAPE) SendMessageW(window, WM_CLOSE, 0, 0); return 0; }
    if (message == WM_GETMINMAXINFO) { MINMAXINFO* limits = (MINMAXINFO*)lp; limits->ptMinTrackSize.x = 560; limits->ptMinTrackSize.y = 260; return 0; }
    if (message == WM_SIZE) { preview_layout(p); InvalidateRect(window, NULL, FALSE); return 0; }
    if (message == WM_PAINT) { PAINTSTRUCT ps; HDC dc = BeginPaint(window, &ps); preview_paint(p, dc); EndPaint(window, &ps); return 0; }
    if (message == WM_CLOSE) { preview_stop(p); DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { KillTimer(window, 1); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}

int imgburn_dvd_preview_show(const ImageBuilder* builder, const DvdPreviewPlan* plan, const wchar_t* ffmpeg) {
    if (!builder || !plan || !plan->cellCount || plan->cellCount > 2 || plan->selectedIndex >= plan->cellCount || !plan->frameRateNumerator || !plan->frameRateDenominator || !plan->width || plan->width > 720 || plan->width % 4 || !plan->height || plan->height > 576 || !plan->aspectNumerator || !plan->aspectDenominator) return 1;
    PreviewWindow* p = (PreviewWindow*)calloc(1, sizeof(*p)); if (!p) return 1;
    p->builder = builder; p->plan = *plan; p->ffmpeg = ffmpeg; p->cellIndex = plan->selectedIndex; p->pixelBytes = (size_t)plan->width * plan->height * 3;
    p->pixels = (uint8_t*)malloc(p->pixelBytes); p->incoming = (uint8_t*)malloc(p->pixelBytes); p->cancel = CreateEventW(NULL, TRUE, FALSE, NULL); p->consumed = CreateEventW(NULL, FALSE, FALSE, NULL);
    INITCOMMONCONTROLSEX controls = {sizeof(controls),ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW wc = {0}; wc.lpfnWndProc = preview_proc; wc.hInstance = GetModuleHandleW(NULL); wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"ImgBurnCellPreview";
    bool registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    HWND window = p->pixels && p->incoming && p->cancel && p->consumed && registered ? CreateWindowW(wc.lpszClassName, L"ImgBurn Preview", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 680, NULL, NULL, wc.hInstance, p) : NULL;
    if (window) {
        ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window); preview_start(p, 0, true); MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) { if (msg.message == WM_KEYDOWN && (msg.wParam == VK_SPACE || msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT || msg.wParam == VK_ESCAPE)) SendMessageW(window, msg.message, msg.wParam, msg.lParam); else { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    } else p->failed = true;
    preview_stop(p); for (unsigned i = 0; i < 2; i++) if (p->clips[i][0]) DeleteFileW(p->clips[i]);
    if (p->cancel) CloseHandle(p->cancel); if (p->consumed) CloseHandle(p->consumed); free(p->incoming); free(p->pixels); bool failed = p->failed; free(p); return failed ? 1 : 0;
}
