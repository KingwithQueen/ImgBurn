/* 扇区查看器命令窗口：后台只读，翻页和刷新共用同一读取接口。 */
#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_gui_sector.h"
#include "imgburn_sector.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

enum { SV_DEVICE = 2001, SV_LBA, SV_MODE, SV_READ, SV_PREVIOUS, SV_NEXT, SV_REFRESH, SV_LAST, SV_CLOSE, SV_CANCEL, SV_DONE = WM_APP + 71 };
typedef struct { HWND window; wchar_t device[MAX_PATH]; SectorRequest request; SectorData data; DWORD error; bool succeeded; } SectorTask;
typedef struct { HWND window, device, lba, mode, text, status; HINSTANCE instance; HFONT font, mono; const OpticalDriveInfo* devices; int count, selected; const GuiAssets* assets; HANDLE worker, cancel; SectorTask* task; uint32_t last; bool known, done, closing; } SectorWindow;
static HWND sector_control(SectorWindow* view, const wchar_t* cls, const wchar_t* label, DWORD style, int id, int x, int y, int width, int height) {
    HWND control = CreateWindowExW(!wcscmp(cls,L"EDIT") ? WS_EX_CLIENTEDGE : 0,cls,label,WS_CHILD | WS_VISIBLE | style,x,y,width,height,view->window,(HMENU)(INT_PTR)id,view->instance,NULL);
    SendMessageW(control,WM_SETFONT,(WPARAM)view->font,TRUE); return control;
}
static GuiAssetId sector_asset(UINT id) { return id == SV_PREVIOUS ? ASSET_SECTOR_PREVIOUS : id == SV_NEXT ? ASSET_SECTOR_NEXT : id == SV_REFRESH ? ASSET_SECTOR_REFRESH : id == SV_LAST ? ASSET_SECTOR_LAST : ASSET_COUNT; }
static void sector_buttons(SectorWindow* view) {
    const int ids[] = {SV_DEVICE,SV_LBA,SV_MODE,SV_READ,SV_PREVIOUS,SV_NEXT,SV_REFRESH,SV_LAST};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) EnableWindow(GetDlgItem(view->window,ids[i]),!view->worker && view->count > 0);
    EnableWindow(GetDlgItem(view->window,SV_CANCEL),view->worker && !view->closing);
    if (!view->worker) { wchar_t number[16]; GetWindowTextW(view->lba,number,16); unsigned long long value = _wcstoui64(number,NULL,10); EnableWindow(GetDlgItem(view->window,SV_PREVIOUS),view->count > 0 && value > 0); EnableWindow(GetDlgItem(view->window,SV_NEXT),view->count > 0 && value < UINT32_MAX && (!view->known || value < view->last)); }
}
static DWORD WINAPI sector_worker(void* context) {
    SectorTask* task = context; OpticalDrive drive;
    if (!spti_open_drive(&drive,task->device,false)) task->error = drive.lastError ? drive.lastError : GetLastError();
    else { task->succeeded = imgburn_sector_read(&drive,&task->request,&task->data); if (!task->succeeded) task->error = GetLastError(); spti_close_drive(&drive); }
    if (!task->succeeded && !task->error) task->error = ERROR_READ_FAULT; PostMessageW(task->window,SV_DONE,0,(LPARAM)task); return 0;
}
static void sector_start(SectorWindow* view, bool useLast) {
    if (view->worker) return; LRESULT selected = SendMessageW(view->device,CB_GETCURSEL,0,0), mode = SendMessageW(view->mode,CB_GETCURSEL,0,0);
    if (selected < 0 || selected >= view->count || mode < 0 || mode > 2) return;
    wchar_t number[16], *end; GetWindowTextW(view->lba,number,16); unsigned long long value = _wcstoui64(number,&end,10);
    if (!useLast && (!number[0] || *end || wcsspn(number,L"0123456789") != wcslen(number) || value > UINT32_MAX)) { SetWindowTextW(view->status,L"请输入有效的十进制 LBA。"); return; }
    SectorTask* task = calloc(1,sizeof(*task)); if (!task) return; task->window = view->window; wcscpy(task->device,view->devices[selected].devicePath); task->request.lba = useLast ? 0 : (uint32_t)value; task->request.mode = (SectorMode)mode; task->request.useLast = useLast; task->request.cancelEvent = view->cancel; ResetEvent(view->cancel);
    view->task = task; SetWindowTextW(view->text,L""); SetWindowTextW(view->status,L"正在读取扇区...");
    view->worker = CreateThread(NULL,0,sector_worker,task,0,NULL); if (!view->worker) { free(task); view->task = NULL; SetWindowTextW(view->status,L"无法启动读取。"); } sector_buttons(view);
}
static LRESULT CALLBACK sector_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    SectorWindow* view = (SectorWindow*)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message == WM_NCCREATE) { view = ((CREATESTRUCTW*)lp)->lpCreateParams; view->window = window; SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)view); }
    if (!view) return DefWindowProcW(window,message,wp,lp);
    if (message == WM_CREATE) {
        sector_control(view,L"STATIC",L"设备：",0,0,12,13,45,20); view->device = sector_control(view,L"COMBOBOX",L"",WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,SV_DEVICE,58,9,608,180);
        for (int i = 0; i < view->count; i++) { wchar_t text[128]; swprintf(text,128,L"%c:  %hs %hs",view->devices[i].driveLetter ? view->devices[i].driveLetter : L'?',view->devices[i].vendorId,view->devices[i].productId); SendMessageW(view->device,CB_ADDSTRING,0,(LPARAM)text); } SendMessageW(view->device,CB_SETCURSEL,view->selected,0);
        sector_control(view,L"STATIC",L"LBA：",0,0,12,48,42,20); view->lba = sector_control(view,L"EDIT",L"0",WS_TABSTOP | ES_AUTOHSCROLL,SV_LBA,58,43,116,24); SendMessageW(view->lba,EM_SETLIMITTEXT,10,0);
        const wchar_t* labels[] = {L"<",L">",L"↻",L">|"}; const UINT ids[] = {SV_PREVIOUS,SV_NEXT,SV_REFRESH,SV_LAST};
        for (unsigned i = 0; i < 4; i++) { GuiAssetId asset = sector_asset(ids[i]); sector_control(view,L"BUTTON",labels[i],WS_TABSTOP | (view->assets && view->assets->images[asset] ? BS_OWNERDRAW : 0),ids[i],182 + (int)i * 33,43,30,24); }
        view->mode = sector_control(view,L"COMBOBOX",L"",WS_TABSTOP | CBS_DROPDOWNLIST,SV_MODE,322,43,246,120); const wchar_t* modes[] = {L"用户数据（2048 字节）",L"RAW（2352 字节）",L"RAW + 子通道（2448 字节）"}; for (unsigned i = 0; i < 3; i++) SendMessageW(view->mode,CB_ADDSTRING,0,(LPARAM)modes[i]); SendMessageW(view->mode,CB_SETCURSEL,0,0);
        sector_control(view,L"BUTTON",L"读取",WS_TABSTOP | BS_DEFPUSHBUTTON,SV_READ,584,42,82,26);
        view->text = sector_control(view,L"EDIT",L"",WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,0,12,78,654,442); SendMessageW(view->text,WM_SETFONT,(WPARAM)view->mono,TRUE); SendMessageW(view->text,EM_SETLIMITTEXT,16384,0);
        view->status = sector_control(view,L"STATIC",view->count ? L"选择 LBA 和读取模式，然后读取扇区。" : L"未找到光驱。",0,0,12,529,654,20); sector_control(view,L"BUTTON",L"取消读取",WS_TABSTOP,SV_CANCEL,484,557,86,28); sector_control(view,L"BUTTON",L"关闭",WS_TABSTOP,SV_CLOSE,580,557,86,28); sector_buttons(view); return 0;
    }
    if (message == WM_DRAWITEM && sector_asset((UINT)wp) != ASSET_COUNT && imgburn_gui_asset_button(view->assets,sector_asset((UINT)wp),(DRAWITEMSTRUCT*)lp)) return TRUE;
    if (message == SV_DONE && view->task == (SectorTask*)lp) {
        WaitForSingleObject(view->worker,INFINITE); CloseHandle(view->worker); view->worker = NULL; SectorTask* task = view->task; view->task = NULL;
        if (task->succeeded) {
            wchar_t* text = calloc(16384,sizeof(wchar_t)); bool formatted = text && imgburn_sector_format(&task->data,text,16384); if (formatted) SetWindowTextW(view->text,text); free(text);
            wchar_t label[180], number[16]; swprintf(number,16,L"%u",task->data.lba); SetWindowTextW(view->lba,number); view->last = task->data.lastLba; view->known = true;
            if (task->data.profile == MMC_PROFILE_CD_ROM || task->data.profile == MMC_PROFILE_CD_R || task->data.profile == MMC_PROFILE_CD_RW) { uint64_t frames = (uint64_t)task->data.lba + 150; swprintf(label,180,L"LBA %u / %u，MSF %llu:%02u:%02u，%u 字节",task->data.lba,task->data.lastLba,frames / 4500,(unsigned)(frames / 75 % 60),(unsigned)(frames % 75),task->data.bytes); }
            else swprintf(label,180,L"LBA %u / %u，%u 字节",task->data.lba,task->data.lastLba,task->data.bytes); SetWindowTextW(view->status,formatted ? label : L"已读取扇区，但无法生成显示内容。");
        } else { wchar_t label[180]; view->known = false; swprintf(label,180,task->error == ERROR_ACCESS_DENIED ? L"设备访问被拒绝（%lu）；请尝试以管理员身份运行。" : task->error == ERROR_CANCELLED ? L"读取已取消（%lu）。" : L"读取失败（错误 %lu）；请检查介质、LBA 和读取模式。",task->error); SetWindowTextW(view->status,label); }
        free(task); if (view->closing) DestroyWindow(window); else sector_buttons(view); return 0;
    }
    if (message == WM_COMMAND) {
        UINT id = LOWORD(wp); if (id == SV_CLOSE || id == IDCANCEL) SendMessageW(window,WM_CLOSE,0,0);
        else if (id == SV_CANCEL) { SetEvent(view->cancel); EnableWindow(GetDlgItem(window,SV_CANCEL),FALSE); SetWindowTextW(view->status,L"正在取消读取..."); }
        else if (!view->worker) {
            if (id == SV_READ || id == IDOK || id == SV_REFRESH || id == SV_LAST) sector_start(view,id == SV_LAST);
            else if (id == SV_PREVIOUS || id == SV_NEXT) { wchar_t number[16]; GetWindowTextW(view->lba,number,16); wchar_t* end; unsigned long long value = _wcstoui64(number,&end,10); if (number[0] && !*end && value <= UINT32_MAX && (id == SV_PREVIOUS ? value > 0 : value < UINT32_MAX)) { value = id == SV_PREVIOUS ? value - 1 : value + 1; swprintf(number,16,L"%u",(unsigned)value); SetWindowTextW(view->lba,number); sector_start(view,false); } }
            else if ((id == SV_DEVICE || id == SV_MODE) && HIWORD(wp) == CBN_SELCHANGE) { view->known = false; SetWindowTextW(view->text,L""); SetWindowTextW(view->status,L"已更改设备或模式，请读取扇区。"); sector_buttons(view); }
            else if (id == SV_LBA && HIWORD(wp) == EN_CHANGE && view->lba) sector_buttons(view);
        } return 0;
    }
    if (message == WM_CLOSE) { if (view->worker) { view->closing = true; SetEvent(view->cancel); SetWindowTextW(view->status,L"正在取消，读取结束后关闭..."); sector_buttons(view); } else DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { view->done = true; return 0; } return DefWindowProcW(window,message,wp,lp);
}
int imgburn_gui_sector_show(HWND owner, HINSTANCE instance, const OpticalDriveInfo* devices, int count, int selected, const GuiAssets* assets) {
    if (count < 0 || count > MAX_OPTICAL_DRIVES || (count && !devices)) return 1; SectorWindow* view = calloc(1,sizeof(*view)); if (!view) return 1;
    view->instance = instance; view->devices = devices; view->count = count; view->selected = selected >= 0 && selected < count ? selected : count ? 0 : -1; view->assets = assets;
    view->font = CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI"); view->mono = CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,ANSI_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas"); view->cancel = CreateEventW(NULL,TRUE,FALSE,NULL);
    WNDCLASSW cls = {0}; cls.lpfnWndProc = sector_proc; cls.hInstance = instance; cls.hCursor = LoadCursorW(NULL,MAKEINTRESOURCEW(32512)); cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE); cls.lpszClassName = L"ImgBurnSectorViewer";
    bool registered = RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS; RECT rect = {0,0,678,597}; DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU; AdjustWindowRectEx(&rect,style,FALSE,WS_EX_DLGMODALFRAME);
    HWND window = registered && view->cancel ? CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,L"扇区查看器",style,CW_USEDEFAULT,CW_USEDEFAULT,rect.right - rect.left,rect.bottom - rect.top,owner,NULL,instance,view) : NULL;
    if (window) { if (assets && assets->smallIcon) SendMessageW(window,WM_SETICON,ICON_SMALL,(LPARAM)assets->smallIcon); EnableWindow(owner,FALSE); ShowWindow(window,SW_SHOW); MSG msg = {0}; int result = 1;
        while (!view->done && (result = (int)GetMessageW(&msg,NULL,0,0)) > 0) { if (!IsDialogMessageW(window,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
        if (!view->done) { if (view->worker) { SetEvent(view->cancel); WaitForSingleObject(view->worker,INFINITE); CloseHandle(view->worker); view->worker = NULL; free(view->task); view->task = NULL; } DestroyWindow(window); if (result == 0) PostQuitMessage((int)msg.wParam); }
        EnableWindow(owner,TRUE); SetForegroundWindow(owner);
    }
    if (view->cancel) CloseHandle(view->cancel); if (view->font) DeleteObject(view->font); if (view->mono) DeleteObject(view->mono); free(view); return window ? 0 : 1;
}
