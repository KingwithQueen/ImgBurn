#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_gui.h"
#include "imgburn_gui_internal.h"
#include "imgburn_gui_assets.h"
#include "imgburn_gui_sector.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

/* 命令编号及菜单层级对应原版；尚未接通的入口保持禁用。 */
enum {
    UI_SOURCE_FILE = 2, UI_SOURCE_FOLDER = 3, UI_CLEAR = 4, UI_DESTINATION = 8, UI_CALCULATE = 10, UI_BUILD = 12,
    UI_READ = 11, UI_WRITE = 13, UI_VERIFY = 14, UI_NEW = 16, UI_LOAD_RECENT = 17, UI_LOAD = 18, UI_SAVE = 19,
    UI_GRAPH_EXPORT = 25, UI_GRAPH_SHOW = 26, UI_EXIT = 49, UI_MODE_EZ = 58, UI_MODE_READ = 60, UI_MODE_BUILD = 61,
    UI_MODE_WRITE = 62, UI_MODE_VERIFY = 63, UI_MODE_DISCOVERY = 64,
    UI_LOG = 90, UI_REFRESH, UI_QUEUE, UI_DROP, UI_LAYOUT, UI_INPUT_STANDARD, UI_INPUT_ADVANCED, UI_OUTPUT_IMAGE, UI_OUTPUT_DEVICE,
    UI_TOOL_CUE = 144, UI_TOOL_DVD, UI_TOOL_MDS, UI_SEARCH = 149, UI_SETTINGS = 155,
    UI_ISO_LABEL = 160, UI_ISO_IFO, UI_DEVICE_REFRESH, UI_DEVICE_LOAD, UI_DEVICE_EJECT, UI_DEVICE_LOCK, UI_DEVICE_UNLOCK,
    UI_DEVICE_ERASE_QUICK, UI_DEVICE_ERASE_FULL, UI_DEVICE_SYNC, UI_DEVICE_CLOSE_TRACK, UI_DEVICE_CLOSE_SESSION, UI_DEVICE_CLOSE_DISC,
    UI_DEVICE_ADVANCED, UI_DEVICE_BOOKTYPE, UI_DEVICE_SECTORS, UI_DEVICE_REGION, UI_DEVICE_CAPABILITIES, UI_DEVICE_FAMILY, UI_DEVICE_FIRMWARE,
    UI_HELP_GUIDES = 200, UI_HELP_SITE, UI_HELP_FORUM, UI_ABOUT, UI_LOG_SAVE, UI_LOG_COPY,
    UI_SOURCE_EDIT = 1000, UI_SOURCE_LIST, UI_DEST_EDIT, UI_DRIVE, UI_GO, UI_CANCEL, UI_FS, UI_UDF, UI_LABEL,
    UI_DVD, UI_BD, UI_OPTIMIZE, UI_TAB, UI_TEST, UI_VERIFY_CHECK, UI_TOOL_ADD, UI_TOOL_REMOVE, UI_TOOL_UP, UI_TOOL_DOWN,
    UI_TOOL_LAYER, UI_TOOL_LBA, UI_TOOL_OTP, UI_TOOL_PTP, UI_TOOL_OK, UI_TOOL_CLOSE, UI_IMAGE_INFO, UI_REMOVE_SOURCE, UI_VERIFY_COMPARE, UI_READ_CDTEXT,
    UI_DISCOVERY_ALL, UI_DISCOVERY_CUSTOM, UI_DISCOVERY_SECTORS, UI_DISCOVERY_CAPACITY, UI_DISCOVERY_SPEED, UI_DISCOVERY_SIZE, UI_READ_SPLIT, UI_READ_RAW_MDS, UI_SESSION_PREGAPS, UI_READ_SPEED, UI_READ_AUDIO_SPEED, UI_READ_RETRIES, UI_READ_EJECT, UI_WRITE_LAYER, UI_DVD_CANDIDATES, UI_DVD_CANDIDATE_LIST,
    UI_CD_ISO_OPEN, UI_CD_ALLOW_BLANK, UI_CD_FINALIZE, UI_CD_PLAN, UI_CD_SELECT, UI_CD_WRITE, UI_CD_PLAN_LIST, UI_CD_SOURCE, UI_CD_DRIVE,
    UI_EZ_FIRST = 1100,
    UI_WRITE_SOURCE_GROUP = 1200, UI_WRITE_DEST_GROUP, UI_WRITE_SETTINGS_GROUP, UI_WRITE_INFO, UI_WRITE_OPEN,
    UI_WRITE_SPEED, UI_WRITE_COPIES, UI_WRITE_SPEED_EFFECTIVE, UI_WRITE_LABEL, UI_WRITE_IMPLEMENTATION, UI_WRITE_FILESYSTEM,
    UI_WRITE_SECTORS, UI_WRITE_SIZE, UI_WRITE_TIME, UI_WRITE_OPTIONS,
    UI_READ_SOURCE_GROUP = 1300, UI_READ_DEST_GROUP, UI_READ_SETTINGS_GROUP, UI_READ_INFO, UI_READ_SECTOR_VIEWER,
    UI_READ_IMAGE_INFO, UI_READ_OPEN, UI_READ_LABEL, UI_READ_IMPLEMENTATION, UI_READ_FILESYSTEM, UI_READ_CAPACITY,
    UI_READ_FREE_SPACE, UI_READ_ADD_QUEUE, UI_READ_BATCH, UI_READ_OPTIONS, UI_READ_DEST_LABEL, UI_READ_EDIT_OK, UI_READ_EDIT_CANCEL
};
enum { GUI_MAIN_CONTENT_WIDTH = 665, GUI_MAIN_CONTENT_HEIGHT = 385, GUI_LOG_CLIENT_WIDTH = 633, GUI_LOG_CLIENT_HEIGHT = 180 };

typedef struct GuiWindow GuiWindow;
typedef struct {
    GuiWindow* parent;
    HWND window, list, layer, lba, otp, ptp, ok, candidateStatus;
    GuiOperation operation;
    GuiPath* sources;
    size_t count;
    bool candidateAccepted, candidateClosing;
    HWND cdAllowBlank, cdFinalize, cdPlanButton, cdSelect, cdWrite;
    bool cdClosing;
} GuiTool;

struct GuiWindow {
    HINSTANCE instance;
    HWND window, status, logWindow, logList, sources, sourceEdit, destination, drives, info, progress, go, cancel, tabs;
    HWND fs, udf, label, dvd, bd, optimize, verify, compare, readText, readSplit, readRawMds, sessionPregaps, discoverySectors, discoverySpeed, readSpeed, readAudioSpeed, readRetries, readEject, writeLayer;
    HFONT font, bold;
    GuiMode mode;
    unsigned dpi;
    bool outputDevice, logVisible, mediaReady, writeMediaReady, writeNeedsRefresh, eraseSupported;
    GuiPath* paths;
    size_t pathCount;
    wchar_t sourceImage[MAX_PATH], destinationImage[MAX_PATH], readImage[MAX_PATH], volumeLabel[127];
    FsTypeFlags fsTypes;
    UdfRevision revision;
    bool dvdVideo, bluray, deduplicate, verifyAfter, verifyAgainstImage, testMode, testWriteSupported, readCdText, rawCdMds, includeSessionPregaps;
    bool discoveryAll;
    bool readEjectWhenDone;
    wchar_t readSplitText[40], readRate[24], readAudioRate[24], readRetryText[8];
    uint32_t discoveryCapacity;
    wchar_t discoveryCount[32], discoveryRate[24], writeLayerValue[16];
    DvdLayerBreakCandidate* dvdCandidates;
    size_t dvdCandidateCount, dvdSelected;
    wchar_t dvdCandidateSource[MAX_PATH];
    DataImageSource dvdCandidateLock;
    bool dvdLayerDerived, updatingWriteLayer;
    MmcCdAppendPlan cdPlan;
    IsoSessionSource cdIsoLock;
    wchar_t cdPlanSource[MAX_PATH], cdPlanDrive[MAX_PATH];
    bool cdAllowBlank, cdFinalize, cdPlanValid, cdPlanSelected;
    bool sourceInfoReady, sourceInfoAttempted, sourceInspectPopup;
    uint64_t sourceBytes;
    uint32_t sourceSectors, sourceSectorBytes;
    wchar_t sourceVolume[128], sourceImplementation[128], sourceFileSystems[64];
    HWND hotControl, tips;
    bool readDestinationReady, readDestinationAttempted;
    uint64_t readDestinationTotalBytes, readDestinationFreeBytes;
    wchar_t readDestinationQuery[MAX_PATH], readDiscText[8192];
    OpticalDriveInfo devices[MAX_OPTICAL_DRIVES];
    int driveCount, selectedDrive;
    GuiJob* job;
    GuiTool* tool;
    GuiAssets assets;
};

static LRESULT CALLBACK gui_proc(HWND window, UINT message, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK gui_tool_proc(HWND window, UINT message, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK gui_log_proc(HWND window, UINT message, WPARAM wp, LPARAM lp);
static void gui_mode(GuiWindow* ui, GuiMode mode);
static void gui_start(GuiWindow* ui, GuiOperation operation);
static void gui_menu(GuiWindow* ui);
static void gui_refresh(GuiWindow* ui);
static void gui_tool_open(GuiWindow* ui, GuiOperation operation);
static void gui_dvd_candidates_open(GuiWindow* ui);
static void gui_dvd_candidates_fill(GuiTool* tool);
static void gui_cd_plan_fill(GuiTool* tool);
static void gui_cd_iso_open(GuiWindow* ui);
static bool gui_cd_iso_start(GuiWindow* ui, bool writing);
static void gui_write_source_info(GuiWindow* ui);
static void gui_read_destination_info(GuiWindow* ui);
static void gui_read_destination_query(GuiWindow* ui);

static int px(const GuiWindow* ui, int value) { return MulDiv(value, (int)ui->dpi, 96); }
static void gui_fonts(GuiWindow* ui) {
    ui->font = CreateFontW(-px(ui,11),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Tahoma");
    ui->bold = CreateFontW(-px(ui,11),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Tahoma");
}
static void gui_main_resize(GuiWindow* ui) {
    RECT status = {0}, rect = {0,0,px(ui,GUI_MAIN_CONTENT_WIDTH),px(ui,GUI_MAIN_CONTENT_HEIGHT)};
    if (ui->status) { SendMessageW(ui->status,WM_SIZE,0,0); GetWindowRect(ui->status,&status); rect.bottom += status.bottom - status.top; }
    DWORD style = (DWORD)GetWindowLongPtrW(ui->window,GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(ui->window,GWL_EXSTYLE);
    AdjustWindowRectEx(&rect,style,GetMenu(ui->window) != NULL,ex); SetWindowPos(ui->window,NULL,0,0,rect.right - rect.left,rect.bottom - rect.top,SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static GuiAssetId gui_control_asset(UINT id) {
    switch (id) {
        case UI_SOURCE_FILE: case UI_DESTINATION: return ASSET_BROWSE;
        case UI_SOURCE_FOLDER: return ASSET_FOLDER; case UI_REMOVE_SOURCE: return ASSET_REMOVE; case UI_IMAGE_INFO: return ASSET_INFO;
        case UI_QUEUE: return ASSET_WRITE_QUEUE; case UI_WRITE_OPEN: return ASSET_OPEN_FOLDER; case UI_TOOL_CUE: return ASSET_CREATE_CUE;
        case UI_DEVICE_LOAD: return ASSET_DEVICE_LOAD; case UI_DEVICE_EJECT: return ASSET_DEVICE_EJECT; case UI_DEVICE_ERASE_QUICK: return ASSET_DEVICE_ERASE; case UI_DEVICE_FAMILY: return ASSET_DEVICE_CHANGER;
        case UI_DEVICE_BOOKTYPE: return ASSET_BOOKTYPE; case UI_DEVICE_ADVANCED: return ASSET_ADVANCED; case UI_SETTINGS: return ASSET_AWS;
        case UI_GRAPH_SHOW: return ASSET_GRAPH_DISPLAY; case UI_GRAPH_EXPORT: return ASSET_GRAPH_EXPORT;
        case UI_READ_SECTOR_VIEWER: return ASSET_READ_SECTOR_VIEWER; case UI_READ_IMAGE_INFO: return ASSET_INFO; case UI_READ_OPEN: return ASSET_OPEN_FOLDER;
        default: return ASSET_COUNT;
    }
}
static LRESULT CALLBACK gui_hover_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    GuiWindow* ui = (GuiWindow*)data;
    int control = GetDlgCtrlID(window); if (message == WM_SETCURSOR && control >= UI_EZ_FIRST && control < UI_EZ_FIRST + 6 && LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(NULL,MAKEINTRESOURCEW(32649))); return TRUE; }
    if (message == WM_MOUSEMOVE && ui->hotControl != window) { HWND old = ui->hotControl; ui->hotControl = window; if (old) InvalidateRect(old,NULL,TRUE); InvalidateRect(window,NULL,TRUE); TRACKMOUSEEVENT tracking = {sizeof(tracking),TME_LEAVE,window,0}; TrackMouseEvent(&tracking); }
    if (message == WM_MOUSELEAVE || message == WM_NCDESTROY) { if (ui->hotControl == window) { ui->hotControl = NULL; if (message != WM_NCDESTROY) InvalidateRect(window,NULL,TRUE); } if (message == WM_NCDESTROY) RemoveWindowSubclass(window,gui_hover_proc,id); }
    return DefSubclassProc(window,message,wp,lp);
}
static HWND ctl(GuiWindow* ui, HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
    GuiAssetId asset = id == UI_GO && (ui->mode == GUI_WRITE || ui->mode == GUI_READ) ? ui->mode == GUI_READ ? ASSET_READ : ASSET_WRITE : gui_control_asset((UINT)id); if (!wcscmp(cls,L"BUTTON") && asset != ASSET_COUNT && ui->assets.images[asset]) style |= BS_OWNERDRAW;
    HWND child = CreateWindowExW(!wcscmp(cls, L"EDIT") || !wcscmp(cls, L"LISTBOX") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, px(ui,x), px(ui,y), px(ui,w), px(ui,h), parent, (HMENU)(INT_PTR)id, ui->instance, NULL);
    if (child) { SendMessageW(child, WM_SETFONT, (WPARAM)ui->font, TRUE); SetWindowPos(child,HWND_TOP,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); if (!wcscmp(cls,L"BUTTON") && (style & BS_TYPEMASK) == BS_OWNERDRAW) SetWindowSubclass(child,gui_hover_proc,1,(DWORD_PTR)ui); } return child;
}
static void gui_tip(GuiWindow* ui, HWND control, const wchar_t* text) {
    if (!ui->tips || !control) return; TOOLINFOW tip = {0}; tip.cbSize = sizeof(tip); tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tip.hwnd = ui->window; tip.uId = (UINT_PTR)control; tip.lpszText = (wchar_t*)text; SendMessageW(ui->tips,TTM_ADDTOOLW,0,(LPARAM)&tip);
}
static void gui_combo_height(GuiWindow* ui, HWND combo, int height) {
    RECT rect; LRESULT itemHeight = SendMessageW(combo,CB_GETITEMHEIGHT,(WPARAM)-1,0);
    if (itemHeight != CB_ERR && GetWindowRect(combo,&rect)) { int border = rect.bottom - rect.top - (int)itemHeight, wanted = px(ui,height) - border; if (wanted > 0) SendMessageW(combo,CB_SETITEMHEIGHT,(WPARAM)-1,wanted); }
}
static void item(HMENU menu, UINT id, const wchar_t* text, bool enabled) { AppendMenuW(menu, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED), id, text); }
static void separator(HMENU menu) { AppendMenuW(menu, MF_SEPARATOR, 0, NULL); }
static void popup(HMENU menu, HMENU sub, const wchar_t* text) { AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, text); }
static void gui_status(GuiWindow* ui, const wchar_t* text) { SendMessageW(ui->status, SB_SETTEXTW, 0, (LPARAM)text); }
static void gui_log(GuiWindow* ui, const wchar_t* text) {
    wchar_t* line = (wchar_t*)calloc(8500,sizeof(wchar_t)); if (!line) return;
    SYSTEMTIME now; GetLocalTime(&now); _snwprintf_s(line,8500,_TRUNCATE,L"%02u:%02u:%02u  %ls",now.wHour,now.wMinute,now.wSecond,text);
    LRESULT at = SendMessageW(ui->logList, LB_ADDSTRING, 0, (LPARAM)line); SendMessageW(ui->logList, LB_SETTOPINDEX, at, 0);
    free(line);
}
static void gui_number(wchar_t* output, size_t capacity, uint64_t value) {
    wchar_t digits[32]; swprintf(digits,32,L"%llu",value); size_t length = wcslen(digits), at = 0;
    for (size_t i = 0; i < length && at + 1 < capacity; i++) { if (i && (length - i) % 3 == 0 && at + 1 < capacity) output[at++] = L','; if (at + 1 < capacity) output[at++] = digits[i]; } output[at] = 0;
}
static void gui_read_destination_info(GuiWindow* ui) {
    if (ui->mode != GUI_READ) return;
    SetWindowTextW(GetDlgItem(ui->window,UI_READ_DEST_LABEL),ui->readImage[0] ? ui->readImage : L"请选择文件...");
    wchar_t total[80] = L"未知", available[80] = L"未知", number[40];
    if (ui->readDestinationReady) { gui_number(number,40,ui->readDestinationTotalBytes); swprintf(total,80,L"%ls 字节",number); gui_number(number,40,ui->readDestinationFreeBytes); swprintf(available,80,L"%ls 字节",number); }
    SetWindowTextW(GetDlgItem(ui->window,UI_READ_CAPACITY),total); SetWindowTextW(GetDlgItem(ui->window,UI_READ_FREE_SPACE),available);
}
static void gui_write_source_info(GuiWindow* ui) {
    if (ui->mode != GUI_WRITE) return;
    if (ui->sourceEdit) SetWindowTextW(ui->sourceEdit,ui->sourceImage[0] ? ui->sourceImage : L"请选择文件...");
    SetWindowTextW(GetDlgItem(ui->window,UI_WRITE_LABEL),ui->sourceInfoReady && ui->sourceVolume[0] ? ui->sourceVolume : L"未知");
    SetWindowTextW(GetDlgItem(ui->window,UI_WRITE_IMPLEMENTATION),ui->sourceInfoReady && ui->sourceImplementation[0] ? ui->sourceImplementation : L"未知");
    SetWindowTextW(GetDlgItem(ui->window,UI_WRITE_FILESYSTEM),ui->sourceInfoReady && ui->sourceFileSystems[0] ? ui->sourceFileSystems : L"未知");
    wchar_t sectors[96] = L"未知", bytes[80] = L"未知", time[80] = L"未知", number[40];
    if (ui->sourceInfoReady && ui->sourceSectors) {
        gui_number(number,40,ui->sourceSectors); if (ui->sourceSectorBytes) swprintf(sectors,96,L"%ls（%u 字节）",number,ui->sourceSectorBytes); else wcscpy(sectors,number);
        uint64_t frames = (uint64_t)ui->sourceSectors + 150; swprintf(time,80,L"%02llu:%02llu:%02llu (MM:SS:FF)",frames / 4500,frames / 75 % 60,frames % 75);
    }
    if (ui->sourceInfoReady && ui->sourceBytes) { gui_number(number,40,ui->sourceBytes); swprintf(bytes,80,L"%ls 字节",number); }
    SetWindowTextW(GetDlgItem(ui->window,UI_WRITE_SECTORS),sectors); SetWindowTextW(GetDlgItem(ui->window,UI_WRITE_SIZE),bytes); SetWindowTextW(GetDlgItem(ui->window,UI_WRITE_TIME),time);
}
static void gui_source_summary_clear(GuiWindow* ui) {
    ui->sourceInfoReady = ui->sourceInfoAttempted = false; ui->sourceBytes = 0; ui->sourceSectors = ui->sourceSectorBytes = 0;
    ui->sourceVolume[0] = ui->sourceImplementation[0] = ui->sourceFileSystems[0] = 0; gui_write_source_info(ui);
}
static void gui_source_summary_apply(GuiWindow* ui, const GuiJob* job) {
    if (job->sourceCount != 1 || _wcsicmp(ui->sourceImage,job->sources[0].path)) return;
    gui_source_summary_clear(ui); ui->sourceInfoAttempted = true;
    if (job->succeeded && job->sourceInfoReady && (!job->cancel || WaitForSingleObject(job->cancel,0) != WAIT_OBJECT_0)) {
        ui->sourceInfoReady = true; ui->sourceBytes = job->sourceBytes; ui->sourceSectors = job->sourceSectors; ui->sourceSectorBytes = job->sourceSectorBytes;
        wcscpy(ui->sourceVolume,job->sourceVolume); wcscpy(ui->sourceImplementation,job->sourceImplementation); wcscpy(ui->sourceFileSystems,job->sourceFileSystems);
    }
    gui_write_source_info(ui);
}

/* 派生选择与已锁定来源一起保存；手工输入的L0不随候选取消而清除。 */
static void gui_dvd_candidates_clear(GuiWindow* ui, bool clearDerived) {
    if (clearDerived && ui->dvdLayerDerived) { ui->writeLayerValue[0] = 0; ui->updatingWriteLayer = true; if (ui->writeLayer) SetWindowTextW(ui->writeLayer,L""); ui->updatingWriteLayer = false; }
    free(ui->dvdCandidates); ui->dvdCandidates = NULL; ui->dvdCandidateCount = 0; ui->dvdSelected = SIZE_MAX; ui->dvdCandidateSource[0] = 0; ui->dvdLayerDerived = false; imgburn_data_source_close(&ui->dvdCandidateLock);
    if (ui->tool && ui->tool->operation == GUI_DVD_CANDIDATES && ui->tool->list) { ListView_DeleteAllItems(ui->tool->list); EnableWindow(ui->tool->ok,FALSE); }
}

/* 计划与只读来源一起持有，取消或上下文变化即收回写入选择。 */
static void gui_cd_plan_clear(GuiWindow* ui) {
    imgburn_iso_session_close(&ui->cdIsoLock); memset(&ui->cdPlan,0,sizeof(ui->cdPlan));
    ui->cdPlanSource[0] = ui->cdPlanDrive[0] = 0; ui->cdPlanValid = ui->cdPlanSelected = false;
    if (ui->tool && ui->tool->operation == GUI_CD_ISO_PLAN) { ListView_DeleteAllItems(ui->tool->list); EnableWindow(ui->tool->cdSelect,FALSE); EnableWindow(ui->tool->cdWrite,FALSE); }
}
static bool gui_cd_plan_current(const GuiWindow* ui) {
    return ui->mode == GUI_WRITE && ui->cdPlanValid && ui->cdIsoLock.file && ui->cdIsoLock.file != INVALID_HANDLE_VALUE && ui->selectedDrive >= 0 && ui->selectedDrive < ui->driveCount && !_wcsicmp(ui->sourceImage,ui->cdPlanSource) && !_wcsicmp(ui->devices[ui->selectedDrive].devicePath,ui->cdPlanDrive);
}
static bool gui_cd_plan_take(GuiWindow* ui, GuiJob* job) {
    gui_cd_plan_clear(ui);
    if (!job->succeeded || !ui->tool || ui->tool->operation != GUI_CD_ISO_PLAN || ui->tool->cdClosing || (job->cancel && WaitForSingleObject(job->cancel,0) == WAIT_OBJECT_0) || job->sourceCount != 1 || ui->mode != GUI_WRITE || _wcsicmp(ui->sourceImage,job->sources[0].path) || ui->selectedDrive < 0 || ui->selectedDrive >= ui->driveCount || _wcsicmp(ui->devices[ui->selectedDrive].devicePath,job->drive) || ui->cdAllowBlank != job->cdAllowBlank || ui->cdFinalize != job->cdFinalize) return false;
    if (!job->cdPlan.sourceSectors || !job->cdIsoSource.file || job->cdIsoSource.file == INVALID_HANDLE_VALUE) return false;
    ui->cdPlan = job->cdPlan; ui->cdIsoLock = job->cdIsoSource; ui->cdIsoLock.cancelEvent = NULL; memset(&job->cdIsoSource,0,sizeof(job->cdIsoSource));
    wcscpy(ui->cdPlanSource,job->sources[0].path); wcscpy(ui->cdPlanDrive,job->drive); ui->cdPlanValid = true; return true;
}
static bool gui_cd_plan_select(GuiWindow* ui) {
    if (ui->job || !gui_cd_plan_current(ui)) { gui_cd_plan_clear(ui); return false; }
    ui->cdPlanSelected = true; if (ui->tool && ui->tool->operation == GUI_CD_ISO_PLAN) gui_cd_plan_fill(ui->tool); gui_status(ui,L"已明确选择追加计划，可在扩展窗口执行写入并回读"); return true;
}

static void gui_dvd_source_set(GuiWindow* ui, const wchar_t* path) {
    if (_wcsicmp(ui->sourceImage,path)) { gui_dvd_candidates_clear(ui,true); gui_cd_plan_clear(ui); gui_source_summary_clear(ui); }
    wcscpy(ui->sourceImage,path);
    gui_write_source_info(ui);
    if (ui->tool && ui->tool->operation == GUI_CD_ISO_PLAN && !ui->job) gui_cd_plan_fill(ui->tool);
}

static bool gui_dvd_candidates_take(GuiWindow* ui, GuiJob* job) {
    gui_dvd_candidates_clear(ui,true);
    if (!job->succeeded || (job->cancel && WaitForSingleObject(job->cancel,0) == WAIT_OBJECT_0) || job->sourceCount != 1 || _wcsicmp(ui->sourceImage,job->sources[0].path)) return false;
    if (!job->dvdCandidateCount) return true;
    if (!job->dvdCandidates || !job->dvdCandidateSource.partCount) return false;
    ui->dvdCandidates = job->dvdCandidates; ui->dvdCandidateCount = job->dvdCandidateCount; wcscpy(ui->dvdCandidateSource,job->sources[0].path); ui->dvdCandidateLock = job->dvdCandidateSource;
    job->dvdCandidates = NULL; job->dvdCandidateCount = 0; memset(&job->dvdCandidateSource,0,sizeof(job->dvdCandidateSource)); return true;
}

static bool gui_dvd_candidate_select(GuiWindow* ui, size_t index) {
    if (ui->mode != GUI_WRITE || !ui->dvdCandidates || index >= ui->dvdCandidateCount || !ui->dvdCandidateLock.partCount || _wcsicmp(ui->sourceImage,ui->dvdCandidateSource)) { gui_dvd_candidates_clear(ui,true); return false; }
    ui->dvdSelected = index; ui->dvdLayerDerived = true; swprintf(ui->writeLayerValue,16,L"%u",ui->dvdCandidates[index].lba);
    ui->updatingWriteLayer = true; if (ui->writeLayer) SetWindowTextW(ui->writeLayer,ui->writeLayerValue); ui->updatingWriteLayer = false; return true;
}
static bool gui_paths_add(GuiPath** paths, size_t* count, const wchar_t* path) {
    if (!path || !path[0] || *count >= 4096) return false;
    wchar_t full[MAX_PATH]; DWORD chars = GetFullPathNameW(path, MAX_PATH, full, NULL);
    if (!chars || chars >= MAX_PATH || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return false;
    for (size_t i = 0; i < *count; i++) if (!_wcsicmp((*paths)[i].path, full)) return true;
    GuiPath* grown = (GuiPath*)realloc(*paths, (*count + 1) * sizeof(*grown)); if (!grown) return false;
    *paths = grown; wcscpy(grown[(*count)++].path, full); return true;
}
static bool gui_folder(HWND owner, wchar_t* result) {
    BROWSEINFOW browse = {0}; browse.hwndOwner = owner; browse.lpszTitle = L"选择来源文件夹"; browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&browse); if (!id) return false;
    bool ok = SHGetPathFromIDListW(id, result) != FALSE; CoTaskMemFree(id); return ok;
}
static const wchar_t readFilter[] = L"数据镜像 (*.iso)\0*.iso\0原始 CD 镜像 (*.cue)\0*.cue\0数据镜像及 MDS 描述 (*.mds)\0*.mds\0\0";
typedef struct { const wchar_t* label; uint64_t bytes; } GuiReadSplit;
static const GuiReadSplit readSplits[] = {
    {L"Auto",0},{L"不分卷",0},{L"650 MiB",650ULL * 1048576},{L"700 MiB",700ULL * 1048576},{L"780 MiB",780ULL * 1048576},{L"810 MiB",810ULL * 1048576},{L"870 MiB",870ULL * 1048576},{L"890 MiB",890ULL * 1048576},
    {L"1 GiB",1073741824ULL},{L"2 GiB",2ULL * 1073741824},{L"4 GiB",4ULL * 1073741824},{L"4.37 GiB",437ULL * 1073741824 / 100},{L"4.4 GiB",44ULL * 1073741824 / 10},{L"4.45 GiB",445ULL * 1073741824 / 100},
    {L"4.5 GiB",45ULL * 1073741824 / 10},{L"4.55 GiB",455ULL * 1073741824 / 100},{L"4.6 GiB",46ULL * 1073741824 / 10},{L"4.65 GiB",465ULL * 1073741824 / 100},{L"4.7 GiB",47ULL * 1073741824 / 10},{L"7.95 GiB",795ULL * 1073741824 / 100},{L"23.3 GiB",233ULL * 1073741824 / 10},{L"46.6 GiB",466ULL * 1073741824 / 10}
};
static bool gui_read_split(const wchar_t* text, bool* automatic, uint64_t* bytes) {
    *automatic = false; *bytes = 0;
    for (unsigned i = 0; i < sizeof(readSplits) / sizeof(*readSplits); i++) if (!_wcsicmp(text,readSplits[i].label)) { *automatic = i == 0; *bytes = readSplits[i].bytes; return true; }
    wchar_t* end; uint64_t value = _wcstoui64(text,&end,10); while (*end == L' ') end++;
    if (!text[0] || text[0] == L'-' || (*end && _wcsicmp(end,L"MiB")) || value > UINT32_MAX / 512) return false;
    *bytes = value * 1048576; return true;
}
static bool gui_read_rate(const wchar_t* text, double* value) {
    *value = 0; if (!_wcsicmp(text,L"MAX")) return true;
    wchar_t* end; double rate = wcstod(text,&end); if ((*end == L'x' || *end == L'X') && !end[1]) end++;
    if (!text[0] || *end || !isfinite(rate) || rate < 1 || rate * 4495.5 + 0.5 >= UINT32_MAX) return false; *value = rate; return true;
}
static UINT_PTR CALLBACK gui_read_file_hook(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    (void)wp;
    if (message == WM_NOTIFY) { const OFNOTIFYW* notice = (const OFNOTIFYW*)lp; if (notice->hdr.code == CDN_TYPECHANGE || notice->hdr.code == CDN_INITDONE) SendMessageW(GetParent(window),CDM_SETDEFEXT,0,(LPARAM)(notice->lpOFN->nFilterIndex == 3 ? L"mds" : notice->lpOFN->nFilterIndex == 2 ? L"cue" : L"iso")); }
    return 0;
}
static bool gui_file(HWND owner, wchar_t* result, bool save, const wchar_t* filter, const wchar_t* extension) {
    OPENFILENAMEW file = {0}; file.lStructSize = sizeof(file); file.hwndOwner = owner; file.lpstrFilter = filter; file.lpstrFile = result; file.nMaxFile = MAX_PATH; file.lpstrDefExt = extension;
    file.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save && filter == readFilter) { file.Flags |= OFN_ENABLEHOOK; file.lpfnHook = gui_read_file_hook; }
    return (save ? GetSaveFileNameW(&file) : GetOpenFileNameW(&file)) != FALSE;
}
static const wchar_t imageFilter[] = L"镜像文件 (*.iso;*.bin;*.cue;*.mds)\0*.iso;*.bin;*.cue;*.mds\0所有文件\0*.*\0\0";
static void gui_read_controls(GuiWindow* ui) {
    if (ui->destination) { if (ui->mode == GUI_VERIFY) { wchar_t path[MAX_PATH]; GetWindowTextW(ui->destination,path,MAX_PATH); gui_dvd_source_set(ui,path); } else if (ui->mode == GUI_READ) { wchar_t path[MAX_PATH]; GetWindowTextW(ui->destination,path,MAX_PATH); if (wcscmp(path,ui->readImage)) { wcscpy(ui->readImage,path); ui->readDestinationReady = ui->readDestinationAttempted = false; ui->readDestinationTotalBytes = ui->readDestinationFreeBytes = 0; } gui_read_destination_info(ui); } else GetWindowTextW(ui->destination,ui->destinationImage,MAX_PATH); }
    if (ui->label) GetWindowTextW(ui->label, ui->volumeLabel, 127);
    if (ui->fs) { static const FsTypeFlags flags[] = {FS_TYPE_ISO9660,FS_TYPE_ISO_JOLIET,FS_TYPE_ISO_UDF,FS_TYPE_ALL,FS_TYPE_UDF}; LRESULT i = SendMessageW(ui->fs, CB_GETCURSEL, 0, 0); if (i >= 0 && i < 5) ui->fsTypes = flags[i]; }
    if (ui->udf) ui->revision = SendMessageW(ui->udf, CB_GETCURSEL, 0, 0) == 1 ? UDF_REV_250 : UDF_REV_102;
    if (ui->dvd) ui->dvdVideo = SendMessageW(ui->dvd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ui->bd) ui->bluray = SendMessageW(ui->bd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ui->optimize) ui->deduplicate = SendMessageW(ui->optimize, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if ((ui->mode == GUI_WRITE || ui->mode == GUI_DISCOVERY) && GetDlgItem(ui->window,UI_TEST)) ui->testMode = ui->testWriteSupported && SendMessageW(GetDlgItem(ui->window,UI_TEST),BM_GETCHECK,0,0) == BST_CHECKED;
    if (ui->verify && !ui->testMode) ui->verifyAfter = SendMessageW(ui->verify, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ui->compare) ui->verifyAgainstImage = SendMessageW(ui->compare, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ui->readText) ui->readCdText = SendMessageW(ui->readText, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ui->readRawMds) ui->rawCdMds = SendMessageW(ui->readRawMds,BM_GETCHECK,0,0) == BST_CHECKED;
    if (ui->sessionPregaps) ui->includeSessionPregaps = SendMessageW(ui->sessionPregaps,BM_GETCHECK,0,0) == BST_CHECKED;
    if (ui->readSplit) GetWindowTextW(ui->readSplit,ui->readSplitText,40);
    if (ui->readSpeed) GetWindowTextW(ui->readSpeed,ui->readRate,24);
    if (ui->readAudioSpeed) GetWindowTextW(ui->readAudioSpeed,ui->readAudioRate,24);
    if (ui->readRetries) GetWindowTextW(ui->readRetries,ui->readRetryText,8);
    if (ui->readEject) ui->readEjectWhenDone = SendMessageW(ui->readEject,BM_GETCHECK,0,0) == BST_CHECKED;
    if (ui->mode == GUI_DISCOVERY && ui->discoverySectors) { ui->discoveryAll = SendMessageW(GetDlgItem(ui->window,UI_DISCOVERY_ALL),BM_GETCHECK,0,0) == BST_CHECKED; GetWindowTextW(ui->discoverySectors,ui->discoveryCount,32); GetWindowTextW(ui->discoverySpeed,ui->discoveryRate,24); }
    if (ui->writeLayer) { wchar_t layer[16]; GetWindowTextW(ui->writeLayer,layer,16); if (ui->dvdLayerDerived && wcscmp(layer,ui->writeLayerValue)) gui_dvd_candidates_clear(ui,false); wcscpy(ui->writeLayerValue,layer); }
    if (ui->drives) ui->selectedDrive = (int)SendMessageW(ui->drives, CB_GETCURSEL, 0, 0);
}

static void gui_write_checks(GuiWindow* ui) {
    if (ui->mode != GUI_WRITE && ui->mode != GUI_DISCOVERY) return; HWND test = GetDlgItem(ui->window,UI_TEST); if (!test) return;
    if (!ui->testWriteSupported) ui->testMode = false;
    SendMessageW(test,BM_SETCHECK,ui->testMode ? BST_CHECKED : BST_UNCHECKED,0); EnableWindow(test,!ui->job && ui->testWriteSupported);
    if (ui->verify) { SendMessageW(ui->verify,BM_SETCHECK,!ui->testMode && ui->verifyAfter ? BST_CHECKED : BST_UNCHECKED,0); EnableWindow(ui->verify,!ui->job && !ui->testMode); }
}

static void gui_menu(GuiWindow* ui) {
    bool build = ui->mode == GUI_BUILD, write = ui->mode == GUI_WRITE, read = ui->mode == GUI_READ, verify = ui->mode == GUI_VERIFY, discovery = ui->mode == GUI_DISCOVERY;
    bool busy = ui->job != NULL, idle = !busy, source = ui->pathCount != 0, drive = ui->selectedDrive >= 0 && ui->selectedDrive < ui->driveCount;
    bool discAction = idle && drive && ui->mediaReady && ((verify && !ui->verifyAgainstImage) || ui->sourceImage[0]);
    bool writeAction = discAction && ui->writeMediaReady;
    bool readAction = idle && drive && ui->mediaReady;
    bool discoveryAction = idle && drive && ui->mediaReady && ui->discoveryCapacity != 0;
    const wchar_t* readExtension = wcsrchr(ui->readImage,L'.');
    if (read && ui->readText) EnableWindow(ui->readText,idle && readExtension && !_wcsicmp(readExtension,L".cue"));
    if (read && ui->readRawMds) EnableWindow(ui->readRawMds,idle && readExtension && !_wcsicmp(readExtension,L".mds"));
    if (read && ui->readSplit) EnableWindow(ui->readSplit,idle && (!readExtension || _wcsicmp(readExtension,L".cue")));
    if (read) { EnableWindow(ui->readSpeed,idle); EnableWindow(ui->readAudioSpeed,idle && readExtension && (!_wcsicmp(readExtension,L".cue") || (!_wcsicmp(readExtension,L".mds") && ui->rawCdMds))); EnableWindow(ui->readRetries,idle); EnableWindow(ui->readEject,idle); }
    if (ui->sessionPregaps) { const wchar_t* extension = wcsrchr(ui->sourceImage,L'.'); EnableWindow(ui->sessionPregaps,idle && (read ? readExtension && !_wcsicmp(readExtension,L".cue") : !ui->verifyAgainstImage || (extension && !_wcsicmp(extension,L".cue")))); }
    if (verify) { EnableWindow(ui->compare,idle); EnableWindow(ui->destination,idle && ui->verifyAgainstImage); EnableWindow(GetDlgItem(ui->window,UI_SOURCE_FILE),idle && ui->verifyAgainstImage); }
    if (discovery) {
        EnableWindow(GetDlgItem(ui->window,UI_DISCOVERY_ALL),idle); EnableWindow(GetDlgItem(ui->window,UI_DISCOVERY_CUSTOM),idle); EnableWindow(ui->discoverySectors,idle && !ui->discoveryAll); EnableWindow(ui->discoverySpeed,idle); EnableWindow(GetDlgItem(ui->window,UI_DISCOVERY_CAPACITY),idle && ui->discoveryCapacity != 0);
        wchar_t size[80]; uint64_t sectors = ui->discoveryAll ? ui->discoveryCapacity : _wcstoui64(ui->discoveryCount,NULL,10); swprintf(size,80,L"数据大小：%.2f MiB",(double)sectors / 512.0); SetWindowTextW(GetDlgItem(ui->window,UI_DISCOVERY_SIZE),size);
    }
    if (ui->writeLayer) EnableWindow(ui->writeLayer,idle);
    if (write) {
        EnableWindow(GetDlgItem(ui->window,UI_SOURCE_FILE),idle); EnableWindow(GetDlgItem(ui->window,UI_WRITE_OPEN),idle && ui->sourceImage[0]); EnableWindow(GetDlgItem(ui->window,UI_IMAGE_INFO),idle && ui->sourceImage[0]); EnableWindow(GetDlgItem(ui->window,UI_TOOL_CUE),idle);
        EnableWindow(ui->drives,idle && drive); EnableWindow(ui->info,drive && ui->mediaReady); EnableWindow(GetDlgItem(ui->window,UI_DEVICE_ERASE_QUICK),idle && drive && ui->mediaReady && ui->eraseSupported);
        bool transfer = busy && ui->job->operation != GUI_DRIVE_INFO && ui->job->operation != GUI_INSPECT && !ui->tool; ShowWindow(ui->progress,transfer ? SW_SHOW : SW_HIDE); ShowWindow(ui->cancel,transfer ? SW_SHOW : SW_HIDE);
        InvalidateRect(ui->go,NULL,TRUE);
    }
    if (read) {
        EnableWindow(ui->drives,idle && drive); EnableWindow(ui->info,drive && ui->mediaReady); EnableWindow(GetDlgItem(ui->window,UI_DESTINATION),idle); EnableWindow(GetDlgItem(ui->window,UI_READ_OPEN),idle && ui->readImage[0]);
        EnableWindow(GetDlgItem(ui->window,UI_READ_SECTOR_VIEWER),idle && drive && ui->mediaReady); EnableWindow(GetDlgItem(ui->window,UI_READ_IMAGE_INFO),idle && drive && ui->mediaReady && ui->readDiscText[0]);
        bool transfer = busy && ui->job->operation != GUI_DRIVE_INFO; ShowWindow(ui->progress,transfer ? SW_SHOW : SW_HIDE); ShowWindow(ui->cancel,transfer ? SW_SHOW : SW_HIDE); InvalidateRect(ui->go,NULL,TRUE); gui_read_destination_info(ui);
    }
    if (ui->go) EnableWindow(ui->go, build ? idle && source && !ui->outputDevice : read ? readAction : discovery ? discoveryAction : write ? writeAction : verify && discAction);
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu(), mode = CreatePopupMenu(), tools = CreatePopupMenu(), help = CreatePopupMenu();
    if (build || write || verify) item(file, UI_SOURCE_FILE, build ? L"浏览文件(&M)..." : L"浏览来源文件(&M)...\tCtrl+O", idle && (!verify || ui->verifyAgainstImage));
    if (build) { item(file, UI_SOURCE_FOLDER, L"浏览文件夹(&N)...", idle); item(file, UI_CLEAR, L"移除所有的项目(&O)", idle && source); separator(file); }
    if (build || read) { item(file, UI_DESTINATION, L"打开目标文件(&P)...", idle && (read || !ui->outputDevice)); separator(file); }
    if (build) { item(file, UI_CALCULATE, L"计算(&Q)", idle && source); item(file, UI_BUILD, L"构建(&R)", idle && source && !ui->outputDevice); separator(file); item(file, UI_NEW, L"新方案(&S)", idle); item(file, UI_LOAD_RECENT, L"载入最近的方案(&T)", false); item(file, UI_LOAD, L"载入方案(&U)...", false); item(file, UI_SAVE, L"储存方案(&V)...", false); separator(file); }
    if (read) { item(file, UI_READ, L"读取(&R)", readAction); separator(file); }
    if (write || discovery) { item(file, UI_WRITE, L"刻录(&R)", discovery ? discoveryAction : writeAction); separator(file); }
    if (verify) { item(file, UI_VERIFY, L"验证(&R)", discAction); separator(file); }
    item(file, UI_GRAPH_EXPORT, L"导出记录图表(&Y)...", false); item(file, UI_GRAPH_SHOW, L"显示记录图表(&Z)...", false); separator(file); item(file, UI_EXIT, L"退出(&X)", idle);
    item(view, UI_LAYOUT, L"光盘布局编辑器(&L)...", false); item(view, UI_DROP, L"拖放区域(&D)", false); separator(view); item(view, UI_LOG, L"日志(&L)", true); CheckMenuItem(view, UI_LOG, MF_BYCOMMAND | (ui->logVisible ? MF_CHECKED : MF_UNCHECKED));
    if (write) item(view, UI_QUEUE, L"刻录队列(&Q)...", false); item(view, UI_REFRESH, L"刷新(&R)\tF5", idle);
    const wchar_t* modes[] = {L"快速向导(&U)...\tCtrl+Shift+P",L"读取(&V)\tCtrl+Shift+R",L"构建(&W)\tCtrl+Shift+B",L"刻录(&X)\tCtrl+Shift+W",L"验证(&Y)\tCtrl+Shift+V",L"品质测试(&Z)\tCtrl+Shift+D"};
    const UINT ids[] = {UI_MODE_EZ,UI_MODE_READ,UI_MODE_BUILD,UI_MODE_WRITE,UI_MODE_VERIFY,UI_MODE_DISCOVERY};
    for (unsigned i = 0; i < 6; i++) { item(mode, ids[i], modes[i], idle); if (!i) separator(mode); if (ui->mode == (GuiMode)i) CheckMenuItem(mode, ids[i], MF_CHECKED); }
    popup(bar, file, L"文件(&F)"); popup(bar, view, L"查看(&V)"); popup(bar, mode, L"模式(&M)");
    if (build) { HMENU input = CreatePopupMenu(), output = CreatePopupMenu(); item(input, UI_INPUT_STANDARD, L"标准(&S)", idle); CheckMenuItem(input, UI_INPUT_STANDARD, MF_CHECKED); item(input, UI_INPUT_ADVANCED, L"高级(&A)", false); item(output, UI_OUTPUT_DEVICE, L"驱动器(&D)", idle); item(output, UI_OUTPUT_IMAGE, L"镜像文件(&I)", idle); CheckMenuItem(output, ui->outputDevice ? UI_OUTPUT_DEVICE : UI_OUTPUT_IMAGE, MF_CHECKED); popup(bar, input, L"输入(&I)"); popup(bar, output, L"输出(&O)"); }
    HMENU iso = CreatePopupMenu(), device = CreatePopupMenu(), erase = CreatePopupMenu(), close = CreatePopupMenu();
    item(iso, UI_ISO_LABEL, L"更改卷标(&V)...", false); item(iso, UI_ISO_IFO, L"显示 IFO 换层信息(&I)...", false); popup(tools, iso, L"ISO");
    item(device, UI_DEVICE_REFRESH, L"刷新(&R)", idle && drive); separator(device);
    item(device, UI_DEVICE_LOAD, L"关闭托盘(&L)", false); item(device, UI_DEVICE_EJECT, L"弹出托盘(&E)", false); item(device, UI_DEVICE_LOCK, L"锁定托盘(&K)", false); item(device, UI_DEVICE_UNLOCK, L"解锁托盘(&U)", false); separator(device);
    item(erase, UI_DEVICE_ERASE_QUICK, L"快速(&Q)", idle && drive && ui->mediaReady && ui->eraseSupported); item(erase, UI_DEVICE_ERASE_FULL, L"完整(&F)", idle && drive && ui->mediaReady && ui->eraseSupported); popup(device, erase, L"擦除光盘(&E)"); item(device, UI_DEVICE_SYNC, L"同步缓存(&S)", false);
    item(close, UI_DEVICE_CLOSE_TRACK, L"轨道(&T)", false); item(close, UI_DEVICE_CLOSE_SESSION, L"区段(&S)", false); item(close, UI_DEVICE_CLOSE_DISC, L"光盘(&D)", false); popup(device, close, L"关闭(&C)"); separator(device);
    item(device, UI_DEVICE_ADVANCED, L"更改高级设置(&A)...", false); item(device, UI_DEVICE_BOOKTYPE, L"更改 Book Type(&B)...", false); item(device, UI_DEVICE_SECTORS, L"扇区查看器(&V)...", idle && drive); item(device, UI_DEVICE_REGION, L"地区码(&R)...", false); item(device, UI_DEVICE_CAPABILITIES, L"功能(&C)...", idle && drive); item(device, UI_DEVICE_FAMILY, L"设备关系(&F)...", false); item(device, UI_DEVICE_FIRMWARE, L"检查固件更新(&U)...", false); popup(tools, device, L"驱动器(&V)");
    HMENU changer = CreatePopupMenu(); item(changer, 180, L"状态", false); item(changer, 181, L"装载", false); item(changer, 182, L"卸载", false); popup(tools, changer, L"换盘器(&W)"); separator(tools);
    item(tools, UI_TOOL_CUE, L"生成 CUE 文件(&C)...", idle); item(tools, UI_TOOL_DVD, L"创建 DVD 文件(&D)...", false); item(tools, UI_TOOL_MDS, L"生成 MDS 文件(&M)...", idle); separator(tools); item(tools, UI_SEARCH, L"寻找 SCSI / ATAPI 设备(&S)", idle); item(tools, 151, L"自动判断刻录速度(&X)...", false); item(tools, 152, L"驱动程序载入规则(&Y)...", false); item(tools, 153, L"重置 DMA(&A)", false); separator(tools); item(tools, UI_SETTINGS, L"选项(&Z)...", false);
    if (write) { HMENU extensions = CreatePopupMenu(); const wchar_t* extension = wcsrchr(ui->sourceImage,L'.'); bool image = extension && (!_wcsicmp(extension,L".iso") || !_wcsicmp(extension,L".bin") || !_wcsicmp(extension,L".mds")); item(extensions,UI_WRITE_OPTIONS,L"双层 L0 扇区...",idle); item(extensions,UI_DVD_CANDIDATES,L"查询视频换层单元...",idle && image); item(extensions,UI_CD_ISO_OPEN,L"CD-R 独立 ISO 追加（扩展）...",idle && drive && extension && !_wcsicmp(extension,L".iso")); separator(tools); popup(tools,extensions,L"Write 扩展"); }
    if (read) { separator(tools); item(tools,UI_READ_OPTIONS,L"读取选项...",idle); }
    item(help, UI_HELP_GUIDES, L"查看 ImgBurn 使用指南(&G)", true); item(help, UI_HELP_SITE, L"访问 ImgBurn 网站(&W)", true); item(help, UI_HELP_FORUM, L"访问 ImgBurn 支持论坛(&F)", true); separator(help); item(help, UI_ABOUT, L"关于 ImgBurn(&A)...", true);
    popup(bar, tools, L"工具(&T)"); popup(bar, help, L"帮助(&H)"); HMENU old = GetMenu(ui->window); SetMenu(ui->window, bar); if (old) DestroyMenu(old); DrawMenuBar(ui->window);
}

static void gui_write_page(GuiWindow* ui) {
    ui->tips = CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,NULL,WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,ui->window,NULL,ui->instance,NULL);
    ctl(ui,ui->window,L"BUTTON",L"来源",BS_GROUPBOX,UI_WRITE_SOURCE_GROUP,8,8,321,169);
    ui->sourceEdit = ctl(ui,ui->window,L"STATIC",L"请选择文件...",SS_PATHELLIPSIS | SS_CENTERIMAGE,UI_SOURCE_EDIT,44,24,229,17);
    gui_tip(ui,ctl(ui,ui->window,L"BUTTON",L"打开文件夹",WS_TABSTOP,UI_WRITE_OPEN,16,21,23,22),L"打开来源所在文件夹");
    gui_tip(ui,ctl(ui,ui->window,L"BUTTON",L"浏览",WS_TABSTOP,UI_SOURCE_FILE,275,21,23,22),L"浏览来源文件");
    HWND queue = ctl(ui,ui->window,L"BUTTON",L"队列",WS_TABSTOP,UI_QUEUE,298,21,23,22); EnableWindow(queue,FALSE); gui_tip(ui,queue,L"刻录队列尚未接通");
    ctl(ui,ui->window,L"STATIC",L"",SS_ETCHEDHORZ,0,16,48,305,3); ctl(ui,ui->window,L"STATIC",L"",SS_ETCHEDHORZ,0,16,113,305,3);
    const wchar_t* captions[] = {L"卷标：",L"实现 ID：",L"文件系统：",L"扇区：",L"大小：",L"时间："}; const int ys[] = {58,74,90,122,138,154};
    const int ids[] = {UI_WRITE_LABEL,UI_WRITE_IMPLEMENTATION,UI_WRITE_FILESYSTEM,UI_WRITE_SECTORS,UI_WRITE_SIZE,UI_WRITE_TIME};
    for (unsigned i = 0; i < 6; i++) { ctl(ui,ui->window,L"STATIC",captions[i],0,0,16,ys[i],54,13); ctl(ui,ui->window,L"STATIC",L"未知",SS_PATHELLIPSIS,ids[i],72,ys[i],i == 5 ? 199 : 249,i < 2 ? 15 : 13); }
    gui_tip(ui,ctl(ui,ui->window,L"BUTTON",L"CUE",WS_TABSTOP,UI_TOOL_CUE,275,148,23,22),L"生成 CUE 文件"); gui_tip(ui,ctl(ui,ui->window,L"BUTTON",L"镜像信息",WS_TABSTOP,UI_IMAGE_INFO,298,148,23,22),L"显示镜像信息");
    ctl(ui,ui->window,L"BUTTON",L"目标",BS_GROUPBOX,UI_WRITE_DEST_GROUP,8,184,321,65);
    ui->drives = ctl(ui,ui->window,L"COMBOBOX",L"",WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,UI_DRIVE,16,200,305,160); gui_combo_height(ui,ui->drives,23); SendMessageW(ui->drives,CB_SETITEMHEIGHT,0,px(ui,20));
    HWND erase = ctl(ui,ui->window,L"BUTTON",L"擦除",WS_TABSTOP,UI_DEVICE_ERASE_QUICK,16,224,23,22); gui_tip(ui,erase,L"快速擦除（按现有介质能力启用）");
    ctl(ui,ui->window,L"BUTTON",L"测试模式",WS_TABSTOP | BS_AUTOCHECKBOX,UI_TEST,44,227,101,17); ui->verify = ctl(ui,ui->window,L"BUTTON",L"验证",WS_TABSTOP | BS_AUTOCHECKBOX,UI_VERIFY_CHECK,148,227,69,17);
    const int deviceIds[] = {UI_DEVICE_LOAD,UI_DEVICE_EJECT,UI_DEVICE_FAMILY}; const int xs[] = {275,298,252}; const wchar_t* deviceNames[] = {L"关闭托盘",L"弹出托盘",L"换盘器"};
    for (unsigned i = 0; i < 3; i++) { HWND button = ctl(ui,ui->window,L"BUTTON",deviceNames[i],WS_TABSTOP,deviceIds[i],xs[i],224,23,22); EnableWindow(button,FALSE); gui_tip(ui,button,deviceNames[i]); if (i == 2) ShowWindow(button,SW_HIDE); }
    HWND graph = ctl(ui,ui->window,L"BUTTON",L"记录图表",WS_TABSTOP,UI_GRAPH_SHOW,306,249,23,22); EnableWindow(graph,FALSE); gui_tip(ui,graph,L"显示记录图表");
    HWND export = ctl(ui,ui->window,L"BUTTON",L"导出图表",WS_TABSTOP,UI_GRAPH_EXPORT,283,249,23,22); EnableWindow(export,FALSE); ShowWindow(export,SW_HIDE);
    ui->info = ctl(ui,ui->window,L"EDIT",L"",ES_MULTILINE | ES_READONLY | WS_VSCROLL,UI_WRITE_INFO,336,8,321,289);
    ctl(ui,ui->window,L"BUTTON",L"设置",BS_GROUPBOX,UI_WRITE_SETTINGS_GROUP,336,304,321,73); ctl(ui,ui->window,L"STATIC",L"写入速度：",0,0,344,324,78,13); ctl(ui,ui->window,L"STATIC",L"份数：",0,0,344,351,78,13);
    HWND speed = ctl(ui,ui->window,L"COMBOBOX",L"",CBS_DROPDOWNLIST,UI_WRITE_SPEED,424,320,65,140); gui_combo_height(ui,speed,21); SendMessageW(speed,CB_ADDSTRING,0,(LPARAM)L"MAX"); SendMessageW(speed,CB_SETCURSEL,0,0); EnableWindow(speed,FALSE);
    HWND copies = ctl(ui,ui->window,L"COMBOBOX",L"",CBS_DROPDOWNLIST,UI_WRITE_COPIES,424,347,65,140); gui_combo_height(ui,copies,21); SendMessageW(copies,CB_ADDSTRING,0,(LPARAM)L"1"); SendMessageW(copies,CB_SETCURSEL,0,0); EnableWindow(copies,FALSE);
    HWND effective = ctl(ui,ui->window,L"STATIC",L"(未知)",0,UI_WRITE_SPEED_EFFECTIVE,496,324,70,13); ShowWindow(effective,SW_HIDE);
    const int settingsIds[] = {UI_SETTINGS,UI_DEVICE_ADVANCED,UI_DEVICE_BOOKTYPE}; const int settingX[] = {624,601,624}, settingY[] = {320,345,345}; const wchar_t* settingsNames[] = {L"自动写入速度",L"高级设置",L"Book Type"};
    for (unsigned i = 0; i < 3; i++) { HWND button = ctl(ui,ui->window,L"BUTTON",settingsNames[i],WS_TABSTOP,settingsIds[i],settingX[i],settingY[i],23,22); EnableWindow(button,FALSE); gui_tip(ui,button,settingsNames[i]); }
    ui->go = ctl(ui,ui->window,L"BUTTON",L"刻录",WS_TABSTOP | BS_DEFPUSHBUTTON,UI_GO,92,312,153,63); gui_tip(ui,ui->go,L"刻录镜像至光盘");
    ui->cancel = ctl(ui,ui->window,L"BUTTON",L"取消",WS_TABSTOP,UI_CANCEL,16,334,64,30); EnableWindow(ui->cancel,FALSE); ShowWindow(ui->cancel,SW_HIDE);
    ui->progress = ctl(ui,ui->window,PROGRESS_CLASSW,L"",0,0,16,280,305,14); ShowWindow(ui->progress,SW_HIDE);
    for (int i = 0; i < ui->driveCount; i++) { wchar_t text[128]; swprintf(text,128,L"%c:  %hs %hs",ui->devices[i].driveLetter ? ui->devices[i].driveLetter : L'?',ui->devices[i].vendorId,ui->devices[i].productId); SendMessageW(ui->drives,CB_ADDSTRING,0,(LPARAM)text); } SendMessageW(ui->drives,CB_SETCURSEL,ui->selectedDrive,0);
    gui_write_source_info(ui); gui_write_checks(ui); gui_menu(ui); if (ui->selectedDrive >= 0) gui_start(ui,GUI_DRIVE_INFO); else if (ui->sourceImage[0] && !ui->sourceInfoAttempted) gui_start(ui,GUI_INSPECT);
}

static void gui_read_page(GuiWindow* ui) {
    ui->mediaReady = false; ui->readDiscText[0] = 0;
    ui->tips = CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,NULL,WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,ui->window,NULL,ui->instance,NULL);
    ctl(ui,ui->window,L"BUTTON",L"来源",BS_GROUPBOX,UI_READ_SOURCE_GROUP,8,8,321,129);
    ui->drives = ctl(ui,ui->window,L"COMBOBOX",L"",WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,UI_DRIVE,16,24,305,160); gui_combo_height(ui,ui->drives,23);
    gui_tip(ui,ctl(ui,ui->window,L"BUTTON",L"扇区查看器",WS_TABSTOP,UI_READ_SECTOR_VIEWER,16,48,23,22),L"查看所选光盘扇区");
    HWND load = ctl(ui,ui->window,L"BUTTON",L"装载",WS_TABSTOP,UI_DEVICE_LOAD,275,48,23,22), eject = ctl(ui,ui->window,L"BUTTON",L"弹出",WS_TABSTOP,UI_DEVICE_EJECT,298,48,23,22); EnableWindow(load,FALSE); EnableWindow(eject,FALSE); gui_tip(ui,load,L"装载托盘尚未接通"); gui_tip(ui,eject,L"弹出托盘尚未接通");
    HWND changer = ctl(ui,ui->window,L"BUTTON",L"换盘器",WS_TABSTOP,UI_DEVICE_FAMILY,252,48,23,22); EnableWindow(changer,FALSE); ShowWindow(changer,SW_HIDE);
    ctl(ui,ui->window,L"STATIC",L"",SS_ETCHEDHORZ,0,16,72,305,3);
    const wchar_t* labels[] = {L"卷标：",L"实现 ID：",L"文件系统："}; const int ids[] = {UI_READ_LABEL,UI_READ_IMPLEMENTATION,UI_READ_FILESYSTEM};
    for (unsigned i = 0; i < 3; i++) { ctl(ui,ui->window,L"STATIC",labels[i],0,0,16,80 + i * 16,54,13); ctl(ui,ui->window,L"STATIC",L"未知",SS_PATHELLIPSIS,ids[i],72,80 + i * 16,249,i < 2 ? 15 : 13); }
    gui_tip(ui,ctl(ui,ui->window,L"BUTTON",L"光盘信息",WS_TABSTOP,UI_READ_IMAGE_INFO,298,107,23,22),L"查看已载入的光盘信息");
    ctl(ui,ui->window,L"BUTTON",L"目标",BS_GROUPBOX,UI_READ_DEST_GROUP,8,144,321,89);
    gui_tip(ui,ctl(ui,ui->window,L"BUTTON",L"打开文件夹",WS_TABSTOP,UI_READ_OPEN,16,157,23,22),L"打开目标所在文件夹");
    ctl(ui,ui->window,L"STATIC",L"",SS_PATHELLIPSIS | SS_CENTERIMAGE | SS_NOPREFIX,UI_READ_DEST_LABEL,44,160,253,17);
    gui_tip(ui,ctl(ui,ui->window,L"BUTTON",L"浏览",WS_TABSTOP,UI_DESTINATION,298,157,23,22),L"选择目标镜像文件");
    /* 原主页以标签显示目标；隐藏编辑器只保存真实路径，不把提示文字当作文件名。 */
    ui->destination = ctl(ui,ui->window,L"COMBOBOX",ui->readImage,CBS_DROPDOWN | CBS_AUTOHSCROLL,UI_DEST_EDIT,16,160,305,140); gui_combo_height(ui,ui->destination,21); EnableWindow(ui->destination,FALSE); ShowWindow(ui->destination,SW_HIDE);
    HWND ok = ctl(ui,ui->window,L"BUTTON",L"确定",BS_DEFPUSHBUTTON,UI_READ_EDIT_OK,91,195,75,23), cancel = ctl(ui,ui->window,L"BUTTON",L"取消",0,UI_READ_EDIT_CANCEL,171,195,75,23); EnableWindow(ok,FALSE); EnableWindow(cancel,FALSE); ShowWindow(ok,SW_HIDE); ShowWindow(cancel,SW_HIDE);
    ctl(ui,ui->window,L"STATIC",L"",SS_ETCHEDHORZ,0,16,184,305,3); ctl(ui,ui->window,L"STATIC",L"容量：",0,0,16,194,78,13); ctl(ui,ui->window,L"STATIC",L"空余空间：",0,0,16,210,78,13);
    ctl(ui,ui->window,L"STATIC",L"未知",SS_PATHELLIPSIS,UI_READ_CAPACITY,96,194,225,13); ctl(ui,ui->window,L"STATIC",L"未知",SS_PATHELLIPSIS,UI_READ_FREE_SPACE,96,210,225,13);
    HWND graph = ctl(ui,ui->window,L"BUTTON",L"图表",WS_TABSTOP,UI_GRAPH_SHOW,306,233,23,22), exportGraph = ctl(ui,ui->window,L"BUTTON",L"导出图表",WS_TABSTOP,UI_GRAPH_EXPORT,283,233,23,22); EnableWindow(graph,FALSE); EnableWindow(exportGraph,FALSE); ShowWindow(exportGraph,SW_HIDE); gui_tip(ui,graph,L"图表尚未接通");
    ui->info = ctl(ui,ui->window,L"EDIT",L"",ES_MULTILINE | ES_READONLY | WS_VSCROLL,UI_READ_INFO,336,8,321,265); EnableWindow(ui->info,FALSE);
    ctl(ui,ui->window,L"BUTTON",L"设置",BS_GROUPBOX,UI_READ_SETTINGS_GROUP,336,280,321,97); ctl(ui,ui->window,L"STATIC",L"读取速度：",0,0,344,301,86,13); ctl(ui,ui->window,L"STATIC",L"/",0,0,496,301,20,13);
    ui->readSpeed = ctl(ui,ui->window,L"COMBOBOX",L"",WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,UI_READ_SPEED,432,297,65,180); ui->readAudioSpeed = ctl(ui,ui->window,L"COMBOBOX",L"",WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,UI_READ_AUDIO_SPEED,520,297,65,180);
    const wchar_t* rates[] = {L"MAX",L"1x",L"2x",L"4x",L"6x",L"8x",L"12x",L"16x",L"24x",L"32x",L"48x",L"52x"}; for (unsigned i = 0; i < sizeof(rates) / sizeof(*rates); i++) { SendMessageW(ui->readSpeed,CB_ADDSTRING,0,(LPARAM)rates[i]); SendMessageW(ui->readAudioSpeed,CB_ADDSTRING,0,(LPARAM)rates[i]); }
    SetWindowTextW(ui->readSpeed,ui->readRate); SetWindowTextW(ui->readAudioSpeed,ui->readAudioRate); SendMessageW(ui->readSpeed,CB_LIMITTEXT,23,0); SendMessageW(ui->readAudioSpeed,CB_LIMITTEXT,23,0); gui_combo_height(ui,ui->readSpeed,21); gui_combo_height(ui,ui->readAudioSpeed,21); gui_tip(ui,ui->readSpeed,L"数据读取速度"); gui_tip(ui,ui->readAudioSpeed,L"音频读取速度");
    ctl(ui,ui->window,L"STATIC",L"",SS_ETCHEDHORZ,0,344,327,305,3);
    HWND queue = ctl(ui,ui->window,L"BUTTON",L"完成后加入刻录队列",BS_AUTOCHECKBOX,UI_READ_ADD_QUEUE,344,336,305,17), batch = ctl(ui,ui->window,L"BUTTON",L"批量读取",BS_AUTOCHECKBOX,UI_READ_BATCH,344,352,305,17); EnableWindow(queue,FALSE); EnableWindow(batch,FALSE);
    ui->go = ctl(ui,ui->window,L"BUTTON",L"读取",WS_TABSTOP | BS_DEFPUSHBUTTON,UI_GO,92,312,153,63); EnableWindow(ui->go,FALSE);
    ui->cancel = ctl(ui,ui->window,L"BUTTON",L"取消",WS_TABSTOP,UI_CANCEL,16,334,64,30); EnableWindow(ui->cancel,FALSE); ShowWindow(ui->cancel,SW_HIDE); ui->progress = ctl(ui,ui->window,PROGRESS_CLASSW,L"",0,0,16,280,305,14); ShowWindow(ui->progress,SW_HIDE);
    for (int i = 0; i < ui->driveCount; i++) { wchar_t text[128]; swprintf(text,128,L"%c:  %hs %hs",ui->devices[i].driveLetter ? ui->devices[i].driveLetter : L'?',ui->devices[i].vendorId,ui->devices[i].productId); SendMessageW(ui->drives,CB_ADDSTRING,0,(LPARAM)text); } SendMessageW(ui->drives,CB_SETCURSEL,ui->selectedDrive,0);
    gui_read_destination_info(ui); gui_menu(ui); if (ui->selectedDrive >= 0) gui_start(ui,GUI_DRIVE_INFO); else gui_read_destination_query(ui);
}

static void gui_mode(GuiWindow* ui, GuiMode mode) {
    gui_read_controls(ui); if (mode != GUI_WRITE) gui_cd_plan_clear(ui); HWND child = GetWindow(ui->window, GW_CHILD);
    if (ui->tips) { DestroyWindow(ui->tips); ui->tips = NULL; }
    while (child) { HWND next = GetWindow(child, GW_HWNDNEXT); if (child != ui->status) DestroyWindow(child); child = next; }
    ui->sources = ui->sourceEdit = ui->destination = ui->drives = ui->info = ui->progress = ui->go = ui->cancel = ui->tabs = NULL;
    ui->fs = ui->udf = ui->label = ui->dvd = ui->bd = ui->optimize = ui->verify = ui->compare = ui->readText = ui->readSplit = ui->readRawMds = ui->sessionPregaps = ui->discoverySectors = ui->discoverySpeed = ui->readSpeed = ui->readAudioSpeed = ui->readRetries = ui->readEject = ui->writeLayer = NULL; ui->mode = mode;
    if (mode == GUI_EZ) {
        ctl(ui, ui->window, L"BUTTON", L"您打算做什么？", BS_GROUPBOX, 0, 8, 8, 649, 369);
        const wchar_t* texts[] = {L"刻录镜像至光盘",L"刻录文件/文件夹至光盘",L"将光盘抓取成镜像文件",L"从文件/文件夹制作镜像",L"验证光盘",L"品质测试"};
        for (int i = 0; i < 6; i++) ctl(ui, ui->window, L"BUTTON", texts[i], WS_TABSTOP | BS_OWNERDRAW, UI_EZ_FIRST + i, 16 + i % 2 * 320, 24 + i / 2 * 120, 313, 105);
        gui_status(ui, L"Ready"); gui_menu(ui); return;
    }
    if (mode == GUI_WRITE) { gui_write_page(ui); return; }
    if (mode == GUI_READ) { gui_read_page(ui); return; }
    bool build = mode == GUI_BUILD; bool imageDestination = build && !ui->outputDevice;
    ctl(ui, ui->window, L"BUTTON", mode == GUI_DISCOVERY ? L"品质测试" : build || mode == GUI_WRITE ? L"来源" : L"来源 / 光盘", BS_GROUPBOX, 0, 8, 8, 320, build ? 188 : mode == GUI_VERIFY ? 160 : 151);
    if (build) {
        ui->sourceEdit = ctl(ui, ui->window, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, UI_SOURCE_EDIT, 16, 25, 280, 23);
        ui->sources = ctl(ui, ui->window, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT, UI_SOURCE_LIST, 16, 50, 280, 132);
        for (size_t i = 0; i < ui->pathCount; i++) SendMessageW(ui->sources, LB_ADDSTRING, 0, (LPARAM)ui->paths[i].path);
        ctl(ui, ui->window, L"BUTTON", L"+", WS_TABSTOP, UI_SOURCE_FILE, 300, 25, 23, 23); ctl(ui, ui->window, L"BUTTON", L"…", WS_TABSTOP, UI_SOURCE_FOLDER, 300, 52, 23, 23); ctl(ui, ui->window, L"BUTTON", L"×", WS_TABSTOP, UI_REMOVE_SOURCE, 300, 80, 23, 23);
    } else if (mode == GUI_VERIFY) {
        ui->drives = ctl(ui,ui->window,L"COMBOBOX",L"",WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,UI_DRIVE,16,28,302,180);
        ctl(ui,ui->window,L"STATIC",L"检查光盘可读性，可选与镜像比较。\r\n音频轨默认只检查可读性。",0,0,18,64,290,55);
        ui->compare = ctl(ui,ui->window,L"BUTTON",L"与镜像文件比较",WS_TABSTOP | BS_AUTOCHECKBOX,UI_VERIFY_COMPARE,18,122,280,22); SendMessageW(ui->compare,BM_SETCHECK,ui->verifyAgainstImage ? BST_CHECKED : BST_UNCHECKED,0);
        ui->sessionPregaps = ctl(ui,ui->window,L"BUTTON",L"包含多会话首轨 INDEX 00",WS_TABSTOP | BS_AUTOCHECKBOX,UI_SESSION_PREGAPS,18,145,300,22); SendMessageW(ui->sessionPregaps,BM_SETCHECK,ui->includeSessionPregaps ? BST_CHECKED : BST_UNCHECKED,0);
    } else if (mode == GUI_DISCOVERY) {
        ctl(ui,ui->window,L"BUTTON",L"全部扇区",WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP,UI_DISCOVERY_ALL,18,29,126,23); ctl(ui,ui->window,L"BUTTON",L"自定义扇区：",WS_TABSTOP | BS_AUTORADIOBUTTON,UI_DISCOVERY_CUSTOM,18,62,126,23);
        SendMessageW(GetDlgItem(ui->window,ui->discoveryAll ? UI_DISCOVERY_ALL : UI_DISCOVERY_CUSTOM),BM_SETCHECK,BST_CHECKED,0);
        ui->discoverySectors = ctl(ui,ui->window,L"EDIT",ui->discoveryCount,WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,UI_DISCOVERY_SECTORS,147,61,130,24); SendMessageW(ui->discoverySectors,EM_SETLIMITTEXT,10,0);
        ctl(ui,ui->window,L"BUTTON",L"←",WS_TABSTOP,UI_DISCOVERY_CAPACITY,283,61,34,24); ctl(ui,ui->window,L"STATIC",L"",0,UI_DISCOVERY_SIZE,147,91,170,23);
    }
    int targetY = build ? 205 : 170; ctl(ui, ui->window, L"BUTTON", imageDestination ? L"目标" : mode == GUI_VERIFY ? L"镜像文件" : L"目标驱动器", BS_GROUPBOX, 0, 8, targetY, 320, 67);
    if (imageDestination || mode == GUI_VERIFY) {
        ui->destination = ctl(ui, ui->window, L"EDIT", mode == GUI_VERIFY ? ui->sourceImage : ui->destinationImage, WS_TABSTOP | ES_AUTOHSCROLL, UI_DEST_EDIT, 16, targetY + 24, 276, 24);
        ctl(ui, ui->window, L"BUTTON", L"…", WS_TABSTOP, mode == GUI_VERIFY ? UI_SOURCE_FILE : UI_DESTINATION, 297, targetY + 24, 23, 24);
    } else ui->drives = ctl(ui, ui->window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, UI_DRIVE, 16, targetY + 24, 302, 160);
    ui->info = ctl(ui, ui->window, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 344, 38, 298, 282);
    if (build) {
        ui->tabs = ctl(ui, ui->window, WC_TABCONTROLW, L"", WS_TABSTOP, UI_TAB, 336, 8, 316, 359);
        const wchar_t* pages[] = {L"信息",L"选项",L"标签",L"高级"}; for (int i = 0; i < 4; i++) { TCITEMW tab = {0}; tab.mask = TCIF_TEXT; tab.pszText = (wchar_t*)pages[i]; TabCtrl_InsertItem(ui->tabs, i, &tab); }
        SetWindowPos(ui->tabs, HWND_BOTTOM, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ctl(ui, ui->window, L"BUTTON", L"计算", WS_TABSTOP, UI_CALCULATE, 450, 326, 90, 30);
    }
    if (mode == GUI_DISCOVERY) { ui->verify = ctl(ui, ui->window, L"BUTTON", L"验证", WS_TABSTOP | BS_AUTOCHECKBOX, UI_VERIFY_CHECK, 110, 246, 65, 22); ctl(ui, ui->window, L"BUTTON", L"测试模式", WS_TABSTOP | BS_AUTOCHECKBOX, UI_TEST, 16, 246, 85, 22); gui_write_checks(ui); }
    if (mode == GUI_DISCOVERY) {
        ctl(ui,ui->window,L"STATIC",L"写入速度：",0,0,344,332,76,24); ui->discoverySpeed = ctl(ui,ui->window,L"COMBOBOX",L"",WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL | CBS_AUTOHSCROLL,UI_DISCOVERY_SPEED,424,328,130,220);
        const wchar_t* speeds[] = {L"MAX",L"1x",L"2x",L"4x",L"6x",L"8x",L"10x",L"12x",L"16x",L"20x",L"24x",L"32x",L"40x",L"48x",L"52x",L"56x",L"64x",L"72x"}; for (unsigned i = 0; i < sizeof(speeds) / sizeof(*speeds); i++) SendMessageW(ui->discoverySpeed,CB_ADDSTRING,0,(LPARAM)speeds[i]); SetWindowTextW(ui->discoverySpeed,ui->discoveryRate);
    }
    const wchar_t* start = build ? L"构建" : mode == GUI_READ ? L"读取" : mode == GUI_VERIFY ? L"验证" : mode == GUI_DISCOVERY ? L"品质测试" : L"刻录";
    ui->go = ctl(ui, ui->window, L"BUTTON", start, WS_TABSTOP | BS_DEFPUSHBUTTON, UI_GO, 98, 292, 140, 58);
    ui->cancel = ctl(ui, ui->window, L"BUTTON", L"取消", WS_TABSTOP, UI_CANCEL, 246, 328, 70, 24); EnableWindow(ui->cancel, FALSE);
    ui->progress = ctl(ui, ui->window, PROGRESS_CLASSW, L"", 0, 0, 16, 359, 308, 11);
    EnableWindow(ui->go, mode != GUI_READ && mode != GUI_DISCOVERY && !(build && ui->outputDevice));
    if (ui->drives) for (int i = 0; i < ui->driveCount; i++) { wchar_t text[128]; swprintf(text, 128, L"%c:  %hs %hs", ui->devices[i].driveLetter ? ui->devices[i].driveLetter : L'?', ui->devices[i].vendorId, ui->devices[i].productId); SendMessageW(ui->drives, CB_ADDSTRING, 0, (LPARAM)text); }
    if (ui->drives) SendMessageW(ui->drives, CB_SETCURSEL, ui->selectedDrive, 0);
    gui_status(ui, build && ui->outputDevice ? L"直接构建到光盘尚未接通；可选择输出镜像文件" : L"Ready"); gui_menu(ui);
    if (ui->drives && ui->selectedDrive >= 0) gui_start(ui, GUI_DRIVE_INFO);
}

static void gui_build_page(GuiWindow* ui) {
    gui_read_controls(ui); unsigned page = (unsigned)TabCtrl_GetCurSel(ui->tabs);
    for (HWND child = GetWindow(ui->window, GW_CHILD); child;) { HWND next = GetWindow(child, GW_HWNDNEXT); if (GetDlgCtrlID(child) >= UI_FS && GetDlgCtrlID(child) <= UI_OPTIMIZE) DestroyWindow(child); child = next; }
    ui->fs = ui->udf = ui->label = ui->dvd = ui->bd = ui->optimize = NULL;
    ShowWindow(ui->info, page == 0 ? SW_SHOW : SW_HIDE);
    if (page == 1) {
        ctl(ui, ui->window, L"STATIC", L"文件系统", 0, UI_FS, 350, 48, 270, 20);
        ui->fs = ctl(ui, ui->window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, UI_FS, 350, 72, 275, 180);
        const wchar_t* systems[] = {L"ISO9660",L"ISO9660 + Joliet",L"ISO9660 + UDF",L"ISO9660 + Joliet + UDF",L"UDF"}; const FsTypeFlags flags[] = {FS_TYPE_ISO9660,FS_TYPE_ISO_JOLIET,FS_TYPE_ISO_UDF,FS_TYPE_ALL,FS_TYPE_UDF};
        for (unsigned i = 0; i < 5; i++) { SendMessageW(ui->fs, CB_ADDSTRING, 0, (LPARAM)systems[i]); if (ui->fsTypes == flags[i]) SendMessageW(ui->fs, CB_SETCURSEL, i, 0); }
        ctl(ui, ui->window, L"STATIC", L"UDF 修订版本", 0, UI_UDF, 350, 114, 270, 20); ui->udf = ctl(ui, ui->window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, UI_UDF, 350, 138, 140, 100);
        SendMessageW(ui->udf, CB_ADDSTRING, 0, (LPARAM)L"1.02"); SendMessageW(ui->udf, CB_ADDSTRING, 0, (LPARAM)L"2.50"); SendMessageW(ui->udf, CB_SETCURSEL, ui->revision == UDF_REV_250, 0);
        ui->optimize = ctl(ui, ui->window, L"BUTTON", L"优化重复文件", WS_TABSTOP | BS_AUTOCHECKBOX, UI_OPTIMIZE, 350, 190, 220, 24); SendMessageW(ui->optimize, BM_SETCHECK, ui->deduplicate ? BST_CHECKED : BST_UNCHECKED, 0);
    } else if (page == 2) {
        ctl(ui, ui->window, L"STATIC", L"卷标", 0, UI_LABEL, 350, 48, 260, 22); ui->label = ctl(ui, ui->window, L"EDIT", ui->volumeLabel, WS_TABSTOP | ES_AUTOHSCROLL, UI_LABEL, 350, 78, 275, 25); SendMessageW(ui->label, EM_SETLIMITTEXT, 126, 0);
    } else if (page == 3) {
        ui->dvd = ctl(ui, ui->window, L"BUTTON", L"DVD-Video 布局", WS_TABSTOP | BS_AUTOCHECKBOX, UI_DVD, 350, 50, 270, 24); SendMessageW(ui->dvd, BM_SETCHECK, ui->dvdVideo ? BST_CHECKED : BST_UNCHECKED, 0);
        ui->bd = ctl(ui, ui->window, L"BUTTON", L"Blu-ray 布局", WS_TABSTOP | BS_AUTOCHECKBOX, UI_BD, 350, 84, 270, 24); SendMessageW(ui->bd, BM_SETCHECK, ui->bluray ? BST_CHECKED : BST_UNCHECKED, 0);
        ctl(ui, ui->window, L"STATIC", L"DVD-Video 使用 ISO9660 + UDF 1.02。\r\nBlu-ray 使用 UDF 2.50。", 0, UI_OPTIMIZE, 350, 126, 275, 60);
    }
}

static void gui_refresh(GuiWindow* ui) {
    gui_cd_plan_clear(ui);
    if (ui->mode == GUI_READ) { ui->readDiscText[0] = 0; ui->readDestinationReady = ui->readDestinationAttempted = false; if (ui->info) SetWindowTextW(ui->info,L""); gui_read_destination_info(ui); }
    ui->driveCount = spti_enumerate_drives(ui->devices, MAX_OPTICAL_DRIVES); ui->selectedDrive = ui->driveCount > 0 ? 0 : -1; ui->mediaReady = ui->writeMediaReady = ui->eraseSupported = ui->testWriteSupported = false; ui->discoveryCapacity = 0;
    if (ui->drives) { SendMessageW(ui->drives, CB_RESETCONTENT, 0, 0); for (int i = 0; i < ui->driveCount; i++) { wchar_t text[128]; swprintf(text, 128, L"%c:  %hs %hs", ui->devices[i].driveLetter ? ui->devices[i].driveLetter : L'?', ui->devices[i].vendorId, ui->devices[i].productId); SendMessageW(ui->drives, CB_ADDSTRING, 0, (LPARAM)text); } SendMessageW(ui->drives, CB_SETCURSEL, ui->selectedDrive, 0); }
    wchar_t text[80]; swprintf(text, 80, L"找到 %d 个光驱。", ui->driveCount); gui_log(ui, text); gui_menu(ui);
    if (ui->drives && ui->selectedDrive >= 0) gui_start(ui,GUI_DRIVE_INFO); else gui_read_destination_query(ui);
}

static void gui_source(GuiWindow* ui, bool folder) {
    if (ui->mode == GUI_DISCOVERY || (ui->mode == GUI_VERIFY && !ui->verifyAgainstImage)) return;
    wchar_t path[MAX_PATH] = L""; bool build = ui->mode == GUI_BUILD;
    if (!(folder ? gui_folder(ui->window, path) : gui_file(ui->window, path, false, build ? L"所有文件\0*.*\0\0" : imageFilter, NULL))) return;
    if (build) { if (!gui_paths_add(&ui->paths, &ui->pathCount, path)) MessageBoxW(ui->window, L"无法添加该路径。", L"ImgBurn", MB_OK | MB_ICONERROR); gui_mode(ui, ui->mode); }
    else { gui_dvd_source_set(ui,path); if (ui->sourceEdit) SetWindowTextW(ui->sourceEdit, path); if (ui->mode == GUI_VERIFY && ui->destination) SetWindowTextW(ui->destination, path); ui->sourceInspectPopup = false; gui_start(ui, GUI_INSPECT); }
}

static GuiJob* gui_job(GuiWindow* ui, GuiOperation operation, const GuiPath* paths, size_t count) {
    GuiJob* job = (GuiJob*)calloc(1, sizeof(*job)); if (!job) return NULL;
    const wchar_t* readExtension = wcsrchr(ui->readImage,L'.'); job->readCdText = operation == GUI_READ_DISC && ui->readCdText && readExtension && !_wcsicmp(readExtension,L".cue");
    if (operation == GUI_READ_DISC && readExtension && !_wcsicmp(readExtension,L".mds")) { job->rawCdMds = ui->rawCdMds; uint64_t bytes = 0; gui_read_split(ui->readSplitText,&job->readAutoSplit,&bytes); job->readSplitSectors = (uint32_t)(bytes / (job->rawCdMds ? 2352 : 2048)); }
    if (operation == GUI_READ_DISC) {
        if (!readExtension || !_wcsicmp(readExtension,L".iso")) job->readAutoSplit = !_wcsicmp(ui->readSplitText,L"Auto");
        job->readRetries = (unsigned)wcstoul(ui->readRetryText,NULL,10); job->readEject = ui->readEjectWhenDone; job->setReadSpeeds = true; gui_read_rate(ui->readRate,&job->readSpeedMultiplier); gui_read_rate(ui->readAudioRate,&job->audioReadSpeedMultiplier);
    }
    if (operation == GUI_READ_DISC) job->includeSessionPregaps = ui->includeSessionPregaps && readExtension && !_wcsicmp(readExtension,L".cue");
    if (operation == GUI_VERIFY_DISC) { const wchar_t* extension = wcsrchr(ui->sourceImage,L'.'); job->includeSessionPregaps = ui->includeSessionPregaps && (!ui->verifyAgainstImage || (extension && !_wcsicmp(extension,L".cue"))); }
    job->sources = (GuiPath*)calloc(count, sizeof(*paths)); job->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!job->sources || !job->cancel) { imgburn_gui_job_free(job); return NULL; }
    memcpy(job->sources, paths, count * sizeof(*paths)); job->sourceCount = count; job->owner = ui->window; job->operation = operation;
    wcscpy(job->destination, ui->mode == GUI_READ ? ui->readImage : ui->destinationImage); wcscpy(job->label, ui->volumeLabel); job->fsTypes = ui->fsTypes; job->revision = ui->revision; job->dvdVideo = ui->dvdVideo; job->bluray = ui->bluray; job->optimize = ui->deduplicate; job->testMode = (operation == GUI_WRITE_DISC || operation == GUI_DISCOVERY_DISC) && ui->testMode; job->verify = ui->verifyAfter && !job->testMode; job->speed = 0xFFFF; job->oppositeTrackPath = true;
    job->cdAllowBlank = ui->cdAllowBlank; job->cdFinalize = ui->cdFinalize;
    if (ui->selectedDrive >= 0 && ui->selectedDrive < ui->driveCount) wcscpy(job->drive, ui->devices[ui->selectedDrive].devicePath); return job;
}
static bool gui_launch(GuiWindow* ui, GuiJob* job) {
    if (!job || ui->job) { imgburn_gui_job_free(job); return false; }
    ui->job = job; job->thread = CreateThread(NULL, 0, imgburn_gui_job_run, job, 0, NULL);
    if (!job->thread) { ui->job = NULL; imgburn_gui_job_free(job); return false; }
    if (ui->go) EnableWindow(ui->go, FALSE); if (ui->cancel) EnableWindow(ui->cancel, job->operation == GUI_CALCULATE || job->operation == GUI_BUILD_IMAGE || job->operation == GUI_CREATE_CUE || job->operation == GUI_DVD_CANDIDATES || job->operation == GUI_CD_ISO_PLAN || job->operation == GUI_CD_ISO_WRITE || job->operation == GUI_READ_DEST_INFO || job->operation == GUI_READ_DISC || job->operation == GUI_VERIFY_DISC || job->operation == GUI_WRITE_DISC || job->operation == GUI_ERASE_QUICK || job->operation == GUI_ERASE_FULL || job->operation == GUI_DISCOVERY_DISC);
    if (job->operation == GUI_READ_DISC || job->operation == GUI_VERIFY_DISC || job->operation == GUI_WRITE_DISC || job->operation == GUI_ERASE_QUICK || job->operation == GUI_ERASE_FULL || job->operation == GUI_DISCOVERY_DISC) { EnableWindow(ui->destination,FALSE); EnableWindow(ui->drives,FALSE); }
    gui_write_checks(ui); gui_status(ui, L"正在执行..."); gui_menu(ui); return true;
}

static void gui_read_destination_query(GuiWindow* ui) {
    if (ui->mode != GUI_READ || ui->job || ui->tool || !ui->readImage[0] || ui->readDestinationAttempted) return;
    GuiPath path = {0}; wcscpy(path.path,ui->readImage); wcscpy(ui->readDestinationQuery,ui->readImage); ui->readDestinationAttempted = true;
    if (!gui_launch(ui,gui_job(ui,GUI_READ_DEST_INFO,&path,1))) { ui->readDestinationAttempted = false; gui_status(ui,L"无法查询目标空间"); }
}

static bool gui_cd_iso_start(GuiWindow* ui, bool writing) {
    if (ui->job || !ui->tool || ui->tool->operation != GUI_CD_ISO_PLAN || ui->mode != GUI_WRITE) return false;
    gui_read_controls(ui); GuiTool* tool = ui->tool;
    bool allow = SendMessageW(tool->cdAllowBlank,BM_GETCHECK,0,0) == BST_CHECKED, finalize = SendMessageW(tool->cdFinalize,BM_GETCHECK,0,0) == BST_CHECKED;
    if (allow != ui->cdAllowBlank || finalize != ui->cdFinalize) { ui->cdAllowBlank = allow; ui->cdFinalize = finalize; gui_cd_plan_clear(ui); }
    if (ui->selectedDrive < 0 || ui->selectedDrive >= ui->driveCount || !ui->sourceImage[0]) return false;
    if (writing && (!ui->cdPlanSelected || !gui_cd_plan_current(ui))) { gui_cd_plan_clear(ui); SetWindowTextW(tool->candidateStatus,L"请先只读规划并明确使用此计划。"); gui_cd_plan_fill(tool); return false; }
    if (!writing) gui_cd_plan_clear(ui);
    GuiPath path = {0}; wcscpy(path.path,ui->sourceImage); GuiJob* job = gui_job(ui,writing ? GUI_CD_ISO_WRITE : GUI_CD_ISO_PLAN,&path,1);
    if (!job) { gui_cd_plan_clear(ui); return false; }
    if (writing) {
        job->cdPlan = ui->cdPlan; job->cdPlanSelected = true; job->cdIsoSource = ui->cdIsoLock; job->cdIsoSource.cancelEvent = job->cancel; memset(&ui->cdIsoLock,0,sizeof(ui->cdIsoLock));
        ui->cdPlanValid = ui->cdPlanSelected = false;
    }
    if (!gui_launch(ui,job)) { gui_cd_plan_clear(ui); SetWindowTextW(tool->candidateStatus,L"无法启动后台操作，请重新规划。"); gui_cd_plan_fill(tool); return false; }
    SetWindowTextW(tool->candidateStatus,writing ? L"正在执行明确选择的计划并回读；实际状态会在完成后显示。" : L"正在后台只读查询来源和光盘布局..."); gui_cd_plan_fill(tool); return true;
}

static void gui_start(GuiWindow* ui, GuiOperation operation) {
    if (ui->job) return; gui_read_controls(ui);
    bool erase = operation == GUI_ERASE_QUICK || operation == GUI_ERASE_FULL, discovery = operation == GUI_DISCOVERY_DISC;
    uint32_t discoverySectors = 0, writeLayer = 0; double discoverySpeed = 0;
    if (operation == GUI_WRITE_DISC && ui->writeLayerValue[0]) { wchar_t* end; uint64_t amount = _wcstoui64(ui->writeLayerValue,&end,10); if (*end || ui->writeLayerValue[0] == L'-' || !amount || amount > UINT32_MAX || amount % 16) { MessageBoxW(ui->window,L"L0容量应为非零的16扇区倍数；留空自动选择。",L"ImgBurn",MB_OK | MB_ICONINFORMATION); return; } writeLayer = (uint32_t)amount; }
    if (discovery) {
        wchar_t* end;
        if (!ui->discoveryAll) { uint64_t count = _wcstoui64(ui->discoveryCount,&end,10); if (!ui->discoveryCount[0] || *end || !count || count > UINT32_MAX || ui->discoveryCount[0] == L'-') { MessageBoxW(ui->window,L"请输入有效的自定义扇区数。",L"ImgBurn",MB_OK | MB_ICONINFORMATION); return; } discoverySectors = (uint32_t)count; }
        if (_wcsicmp(ui->discoveryRate,L"MAX")) { discoverySpeed = wcstod(ui->discoveryRate,&end); if ((*end == L'x' || *end == L'X') && !end[1]) end++; if (!ui->discoveryRate[0] || *end || !isfinite(discoverySpeed) || discoverySpeed < 1 || discoverySpeed * 4495.5 + 0.5 >= UINT32_MAX) { MessageBoxW(ui->window,L"请输入有效的写入速度（例如 4x 或 MAX）。",L"ImgBurn",MB_OK | MB_ICONINFORMATION); return; } }
    }
    if (ui->mode == GUI_VERIFY && ui->destination) { wchar_t path[MAX_PATH]; GetWindowTextW(ui->destination,path,MAX_PATH); gui_dvd_source_set(ui,path); }
    GuiPath single = {0}; const GuiPath* sources = ui->paths; size_t count = ui->pathCount;
    if (erase || discovery || operation == GUI_DRIVE_INFO || operation == GUI_READ_DISC || operation == GUI_VERIFY_DISC || operation == GUI_WRITE_DISC) { if (ui->selectedDrive < 0 || ui->selectedDrive >= ui->driveCount) return; }
    if (erase || discovery || operation == GUI_INSPECT || operation == GUI_VERIFY_DISC || operation == GUI_WRITE_DISC || operation == GUI_DRIVE_INFO || operation == GUI_READ_DISC) { wcscpy(single.path, erase || discovery || operation == GUI_DRIVE_INFO || operation == GUI_READ_DISC ? ui->devices[ui->selectedDrive].devicePath : ui->sourceImage); sources = &single; count = 1; }
    if (operation == GUI_VERIFY_DISC && !ui->verifyAgainstImage) single.path[0] = 0;
    if (!count || (!single.path[0] && sources == &single && (operation != GUI_VERIFY_DISC || ui->verifyAgainstImage))) { MessageBoxW(ui->window, L"请先选择来源。", L"ImgBurn", MB_OK | MB_ICONINFORMATION); return; }
    if (operation == GUI_BUILD_IMAGE && !ui->destinationImage[0]) {
        if (!gui_file(ui->window, ui->destinationImage, true, L"ISO 镜像 (*.iso)\0*.iso\0\0", L"iso")) return;
        if (ui->destination) SetWindowTextW(ui->destination, ui->destinationImage);
    } else if (operation == GUI_BUILD_IMAGE && GetFileAttributesW(ui->destinationImage) != INVALID_FILE_ATTRIBUTES && MessageBoxW(ui->window, L"目标文件已存在，是否替换？", L"ImgBurn", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (operation == GUI_WRITE_DISC && MessageBoxW(ui->window,ui->testMode ? L"将以设备测试模式执行镜像写入流程，是否继续？" : L"将开始向所选光驱写入镜像，是否继续？", L"ImgBurn", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    if (discovery && MessageBoxW(ui->window,ui->testMode ? L"将以设备测试模式执行品质测试，是否继续？" : L"品质测试将向所选光盘写入零数据，是否继续？",L"ImgBurn",MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    if (erase && MessageBoxW(ui->window,operation == GUI_ERASE_FULL ? L"完整擦除将清除所选光盘的内容。开始后取消仍须等待设备结束。是否继续？" : L"快速擦除将使所选光盘重新可写，数据区可能仍保留原内容。开始后取消仍须等待设备结束。是否继续？",L"ImgBurn",MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    bool replace = false;
    if (operation == GUI_READ_DISC) {
        if (!ui->readImage[0]) { if (!gui_file(ui->window,ui->readImage,true,readFilter,L"iso")) return; if (ui->destination) SetWindowTextW(ui->destination,ui->readImage); replace = GetFileAttributesW(ui->readImage) != INVALID_FILE_ATTRIBUTES; }
        else if (GetFileAttributesW(ui->readImage) != INVALID_FILE_ATTRIBUTES) { if (MessageBoxW(ui->window,L"目标文件已存在，是否替换？",L"ImgBurn",MB_YESNO | MB_ICONQUESTION) != IDYES) return; replace = true; }
    }
    if (operation == GUI_READ_DISC) {
        bool automatic; uint64_t bytes; double rate; wchar_t* end; unsigned long retries = wcstoul(ui->readRetryText,&end,10); const wchar_t* extension = wcsrchr(ui->readImage,L'.'); bool cue = extension && !_wcsicmp(extension,L".cue");
        if (!cue && (!gui_read_split(ui->readSplitText,&automatic,&bytes) || (extension && !_wcsicmp(extension,L".iso") && bytes))) { MessageBoxW(ui->window,L"请选择 Auto、不分卷，或为 MDS 输入有效的自定义 MiB 容量。",L"ImgBurn",MB_OK | MB_ICONINFORMATION); return; }
        if (!ui->readRetryText[0] || *end || retries > 20 || ui->readRetryText[0] == L'-') { MessageBoxW(ui->window,L"软件重试次数应为 0 至 20。",L"ImgBurn",MB_OK | MB_ICONINFORMATION); return; }
        if (!gui_read_rate(ui->readRate,&rate) || !gui_read_rate(ui->readAudioRate,&rate)) { MessageBoxW(ui->window,L"请输入有效读取速度，例如 4x 或 MAX。",L"ImgBurn",MB_OK | MB_ICONINFORMATION); return; }
    }
    GuiJob* job = gui_job(ui,operation,sources,count); if (job) { job->writeLayerBreak = writeLayer; job->replaceExisting = replace; job->discoverySectors = discoverySectors; job->discoverySpeedMultiplier = discoverySpeed; }
    if (!gui_launch(ui,job)) MessageBoxW(ui->window, L"无法启动操作。", L"ImgBurn", MB_OK | MB_ICONERROR);
}

static void gui_tool_open(GuiWindow* ui, GuiOperation operation) {
    if (ui->tool || ui->job) return; gui_read_controls(ui);
    GuiTool* tool = (GuiTool*)calloc(1, sizeof(*tool)); if (!tool) return; tool->parent = ui; tool->operation = operation; ui->tool = tool;
    const wchar_t* title = operation == GUI_CREATE_CUE ? L"生成 CUE 文件" : operation == GUI_WRITE_OPTIONS ? L"写入扩展设置" : operation == GUI_READ_OPTIONS ? L"读取选项" : L"创建 MDS 文件";
    RECT rect = {0,0,px(ui,operation == GUI_WRITE_OPTIONS ? 380 : operation == GUI_READ_OPTIONS ? 464 : 520),px(ui,operation == GUI_WRITE_OPTIONS ? 144 : operation == GUI_READ_OPTIONS ? 248 : 372)}; AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ImgBurnCommand", title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, ui->window, NULL, ui->instance, tool);
    if (!window) { ui->tool = NULL; free(tool); return; } EnableWindow(ui->window, FALSE); ShowWindow(window, SW_SHOW);
}

static void gui_dvd_candidates_fill(GuiTool* tool) {
    GuiWindow* ui = tool->parent; ListView_DeleteAllItems(tool->list); EnableWindow(tool->ok,FALSE);
    for (size_t i = 0; i < ui->dvdCandidateCount; i++) {
        const DvdLayerBreakCandidate* candidate = &ui->dvdCandidates[i]; wchar_t values[8][96] = {{0}};
        swprintf(values[0],96,L"%u:%u:%u",candidate->titleSet,candidate->pgc,candidate->cell);
        if (candidate->domain == DVD_DOMAIN_TITLE) wcscpy(values[1],L"标题"); else swprintf(values[1],96,L"%ls / LU %u",candidate->domain == DVD_DOMAIN_VMG_MENU ? L"VMGM" : L"VTSM",candidate->languageUnit);
        swprintf(values[2],96,L"%u:%u",candidate->vobId,candidate->cellId); swprintf(values[3],96,L"%u",candidate->program);
        if (candidate->isChapterStart) swprintf(values[4],96,L"%u:%u",candidate->title,candidate->chapter); else wcscpy(values[4],L"—");
        swprintf(values[5],96,L"%u",candidate->lba); swprintf(values[6],96,L"%04X",candidate->languageCode); wcscpy(values[7],candidate->isSeamless ? L"是" : L"否");
        LVITEMW item = {0}; item.mask = LVIF_TEXT | LVIF_PARAM; item.iItem = (int)i; item.pszText = values[0]; item.lParam = (LPARAM)i; SendMessageW(tool->list,LVM_INSERTITEMW,0,(LPARAM)&item);
        for (int column = 1; column < 8; column++) { item.iSubItem = column; item.pszText = values[column]; SendMessageW(tool->list,LVM_SETITEMTEXTW,(WPARAM)i,(LPARAM)&item); }
    }
}

static void gui_dvd_candidates_open(GuiWindow* ui) {
    if (ui->mode != GUI_WRITE || ui->tool || ui->job || !ui->sourceImage[0]) return;
    gui_read_controls(ui); gui_dvd_candidates_clear(ui,true);
    GuiTool* tool = calloc(1,sizeof(*tool)); if (!tool) return; tool->parent = ui; tool->operation = GUI_DVD_CANDIDATES; ui->tool = tool;
    RECT rect = {0,0,px(ui,844),px(ui,414)}; AdjustWindowRectEx(&rect,WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME,L"ImgBurnCommand",L"DVD-Video 换层单元候选（只读查询）",WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,rect.right - rect.left,rect.bottom - rect.top,ui->window,NULL,ui->instance,tool);
    if (!window) { ui->tool = NULL; free(tool); return; }
    EnableWindow(ui->window,FALSE); ShowWindow(window,SW_SHOW);
    GuiPath source = {0}; wcscpy(source.path,ui->sourceImage); GuiJob* job = gui_job(ui,GUI_DVD_CANDIDATES,&source,1);
    if (!gui_launch(ui,job)) { SetWindowTextW(tool->candidateStatus,L"无法启动后台候选查询，请关闭后重试。"); SetWindowTextW(GetDlgItem(window,UI_TOOL_CLOSE),L"关闭"); }
}

static void gui_cd_plan_fill(GuiTool* tool) {
    GuiWindow* ui = tool->parent; bool busy = ui->job != NULL, current = gui_cd_plan_current(ui);
    SetWindowTextW(GetDlgItem(tool->window,UI_CD_SOURCE),ui->sourceImage);
    SetWindowTextW(GetDlgItem(tool->window,UI_CD_DRIVE),ui->selectedDrive >= 0 && ui->selectedDrive < ui->driveCount ? ui->devices[ui->selectedDrive].devicePath : L"未选择目标光驱");
    ListView_DeleteAllItems(tool->list);
    if (ui->cdPlan.sourceSectors) {
        const MmcCdAppendPlan* p = &ui->cdPlan;
        const wchar_t* names[] = {L"会话",L"轨号",L"原末会话 lead-out",L"下一可写位置 NWA",L"数据起始 LBA",L"数据结束 LBA（不含）",L"来源扇区",L"可用数据扇区",L"剩余数据扇区",L"扇区字节",L"写类型（TAO）",L"轨道模式（MODE1）",L"数据块类型（2048）"};
        const uint32_t values[] = {p->sessionNumber,p->trackNumber,p->previousLeadOutLba,p->nextWritableAddress,p->dataStartLba,p->dataEndLba,p->sourceSectors,p->availableDataSectors,p->remainingDataSectors,p->sectorBytes,p->writeType,p->trackMode,p->dataBlockType};
        for (unsigned i = 0; i < 13; i++) { wchar_t number[32]; swprintf(number,32,L"%u",values[i]); LVITEMW item = {0}; item.mask = LVIF_TEXT; item.iItem = (int)i; item.pszText = (wchar_t*)names[i]; SendMessageW(tool->list,LVM_INSERTITEMW,0,(LPARAM)&item); item.iSubItem = 1; item.pszText = number; SendMessageW(tool->list,LVM_SETITEMTEXTW,i,(LPARAM)&item); }
    }
    SendMessageW(tool->cdAllowBlank,BM_SETCHECK,ui->cdAllowBlank ? BST_CHECKED : BST_UNCHECKED,0); SendMessageW(tool->cdFinalize,BM_SETCHECK,ui->cdFinalize ? BST_CHECKED : BST_UNCHECKED,0);
    EnableWindow(tool->cdAllowBlank,!busy); EnableWindow(tool->cdFinalize,!busy);
    EnableWindow(tool->cdPlanButton,!busy && ui->mode == GUI_WRITE && ui->sourceImage[0] && ui->selectedDrive >= 0 && ui->selectedDrive < ui->driveCount);
    EnableWindow(tool->cdSelect,!busy && current && !ui->cdPlanSelected); EnableWindow(tool->cdWrite,!busy && current && ui->cdPlanSelected);
    SetWindowTextW(GetDlgItem(tool->window,UI_TOOL_CLOSE),busy ? L"取消" : L"关闭"); EnableWindow(GetDlgItem(tool->window,UI_TOOL_CLOSE),!tool->cdClosing);
}
static void gui_cd_iso_open(GuiWindow* ui) {
    if (ui->mode != GUI_WRITE || ui->tool || ui->job || !ui->sourceImage[0] || ui->selectedDrive < 0 || ui->selectedDrive >= ui->driveCount) return;
    GuiTool* tool = calloc(1,sizeof(*tool)); if (!tool) return; tool->parent = ui; tool->operation = GUI_CD_ISO_PLAN; ui->tool = tool;
    RECT rect = {0,0,px(ui,654),px(ui,460)}; AdjustWindowRectEx(&rect,WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME,L"ImgBurnCommand",L"CD-R 独立 ISO 追加（扩展）",WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,rect.right - rect.left,rect.bottom - rect.top,ui->window,NULL,ui->instance,tool);
    if (!window) { ui->tool = NULL; free(tool); return; } EnableWindow(ui->window,FALSE); ShowWindow(window,SW_SHOW);
}

static void gui_tool_fill(GuiTool* tool) { SendMessageW(tool->list, LB_RESETCONTENT, 0, 0); for (size_t i = 0; i < tool->count; i++) SendMessageW(tool->list, LB_ADDSTRING, 0, (LPARAM)tool->sources[i].path); EnableWindow(tool->ok, tool->count != 0); }
static void gui_read_options_state(GuiTool* tool) {
    const wchar_t* extension = wcsrchr(tool->parent->readImage,L'.'); bool cue = extension && !_wcsicmp(extension,L".cue"), mds = extension && !_wcsicmp(extension,L".mds");
    EnableWindow(GetDlgItem(tool->window,UI_READ_CDTEXT),cue); EnableWindow(GetDlgItem(tool->window,UI_SESSION_PREGAPS),cue); EnableWindow(GetDlgItem(tool->window,UI_READ_RAW_MDS),mds); EnableWindow(GetDlgItem(tool->window,UI_READ_SPLIT),!cue);
}

static LRESULT CALLBACK gui_tool_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    GuiTool* tool = (GuiTool*)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) { tool = (GuiTool*)((CREATESTRUCTW*)lp)->lpCreateParams; tool->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)tool); }
    if (!tool) return DefWindowProcW(window, message, wp, lp); GuiWindow* ui = tool->parent;
    if (message == WM_CREATE) {
        if (tool->operation == GUI_READ_OPTIONS) {
            ctl(ui,window,L"STATIC",L"分卷 / 自定义MiB：",0,0,12,18,126,20); HWND split = ctl(ui,window,L"COMBOBOX",L"",WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,UI_READ_SPLIT,142,14,306,240);
            for (unsigned i = 0; i < sizeof(readSplits) / sizeof(*readSplits); i++) SendMessageW(split,CB_ADDSTRING,0,(LPARAM)readSplits[i].label); SetWindowTextW(split,ui->readSplitText); SendMessageW(split,CB_LIMITTEXT,39,0); gui_combo_height(ui,split,23);
            ctl(ui,window,L"STATIC",L"软件重试（0–20）：",0,0,12,50,126,20); HWND retries = ctl(ui,window,L"EDIT",ui->readRetryText,WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,UI_READ_RETRIES,142,46,64,24); SendMessageW(retries,EM_SETLIMITTEXT,2,0);
            const wchar_t* labels[] = {L"读取 CD-TEXT（仅 CUE）",L"原始数据 CD（2352 字节 MDS）",L"包含多会话首轨 INDEX 00",L"读取成功后弹出光盘"}; const int ids[] = {UI_READ_CDTEXT,UI_READ_RAW_MDS,UI_SESSION_PREGAPS,UI_READ_EJECT}; const bool values[] = {ui->readCdText,ui->rawCdMds,ui->includeSessionPregaps,ui->readEjectWhenDone};
            for (unsigned i = 0; i < 4; i++) { HWND check = ctl(ui,window,L"BUTTON",labels[i],WS_TABSTOP | BS_AUTOCHECKBOX,ids[i],12,82 + i * 28,436,22); SendMessageW(check,BM_SETCHECK,values[i] ? BST_CHECKED : BST_UNCHECKED,0); }
            tool->ok = ctl(ui,window,L"BUTTON",L"应用",WS_TABSTOP | BS_DEFPUSHBUTTON,UI_TOOL_OK,286,208,76,28); ctl(ui,window,L"BUTTON",L"取消",WS_TABSTOP,UI_TOOL_CLOSE,372,208,76,28); gui_read_options_state(tool); return 0;
        }
        if (tool->operation == GUI_WRITE_OPTIONS) {
            ctl(ui,window,L"STATIC",L"双层 L0 扇区（留空自动）：",0,0,12,16,212,22); ui->writeLayer = ctl(ui,window,L"EDIT",ui->writeLayerValue,WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,UI_WRITE_LAYER,230,13,136,24); SendMessageW(ui->writeLayer,EM_SETLIMITTEXT,10,0);
            ctl(ui,window,L"STATIC",L"手工值覆盖 MDS 层位置；留空沿用原有自动规则。\r\n视频单元和独立 ISO 追加位于“工具 → Write 扩展”。",0,0,12,50,354,42);
            tool->ok = ctl(ui,window,L"BUTTON",L"应用",WS_TABSTOP | BS_DEFPUSHBUTTON,UI_TOOL_OK,202,104,76,28); ctl(ui,window,L"BUTTON",L"取消",WS_TABSTOP,UI_TOOL_CLOSE,290,104,76,28); return 0;
        }
        if (tool->operation == GUI_CD_ISO_PLAN) {
            ctl(ui,window,L"STATIC",L"独立纯 ISO9660，不合并旧文件树。默认只读规划；写入后完整回读。",0,0,10,10,634,22);
            ctl(ui,window,L"STATIC",ui->sourceImage,SS_PATHELLIPSIS,UI_CD_SOURCE,10,36,634,22); ctl(ui,window,L"STATIC",L"",0,UI_CD_DRIVE,10,60,634,22);
            tool->cdAllowBlank = ctl(ui,window,L"BUTTON",L"允许空白盘首会话",WS_TABSTOP | BS_AUTOCHECKBOX,UI_CD_ALLOW_BLANK,10,86,240,22);
            tool->cdFinalize = ctl(ui,window,L"BUTTON",L"本次封盘（不选则留盘开放）",WS_TABSTOP | BS_AUTOCHECKBOX,UI_CD_FINALIZE,270,86,374,22);
            tool->list = ctl(ui,window,WC_LISTVIEWW,L"",WS_TABSTOP | WS_VSCROLL | LVS_REPORT | LVS_SINGLESEL,UI_CD_PLAN_LIST,10,112,634,224); ListView_SetExtendedListViewStyle(tool->list,LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            const wchar_t* columns[] = {L"计划字段",L"值"}; for (unsigned i = 0; i < 2; i++) { LVCOLUMNW column = {0}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = (wchar_t*)columns[i]; column.cx = px(ui,i ? 280 : 320); SendMessageW(tool->list,LVM_INSERTCOLUMNW,i,(LPARAM)&column); }
            tool->candidateStatus = ctl(ui,window,L"EDIT",L"请选择首会话/结束方式，再点击只读规划。尚未选择计划，不能写入。",ES_MULTILINE | ES_READONLY | WS_VSCROLL,0,10,344,634,64);
            tool->cdPlanButton = ctl(ui,window,L"BUTTON",L"只读规划",WS_TABSTOP | BS_DEFPUSHBUTTON,UI_CD_PLAN,10,420,104,28);
            tool->cdSelect = ctl(ui,window,L"BUTTON",L"使用此计划",WS_TABSTOP,UI_CD_SELECT,124,420,124,28);
            tool->cdWrite = ctl(ui,window,L"BUTTON",L"写入并回读",WS_TABSTOP,UI_CD_WRITE,258,420,144,28);
            ctl(ui,window,L"BUTTON",L"关闭",WS_TABSTOP,UI_TOOL_CLOSE,556,420,88,28); gui_cd_plan_fill(tool); return 0;
        }
        if (tool->operation == GUI_DVD_CANDIDATES) {
            ctl(ui,window,L"STATIC",ui->sourceImage,SS_PATHELLIPSIS,0,10,10,824,24);
            tool->list = ctl(ui,window,WC_LISTVIEWW,L"",WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,UI_DVD_CANDIDATE_LIST,10,42,824,250); ListView_SetExtendedListViewStyle(tool->list,LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            const wchar_t* columns[] = {L"VTS:PGC:Cell",L"域 / 语言单元",L"VOB:Cell",L"程序",L"标题:章节",L"实际 LBA",L"语言",L"SPLIP"}; const int widths[] = {118,140,90,60,100,110,80,70};
            for (unsigned i = 0; i < 8; i++) { LVCOLUMNW column = {0}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = (wchar_t*)columns[i]; column.cx = px(ui,widths[i]); SendMessageW(tool->list,LVM_INSERTCOLUMNW,i,(LPARAM)&column); }
            tool->candidateStatus = ctl(ui,window,L"EDIT",L"正在后台读取镜像导航并查询候选，未选择任何单元...",ES_MULTILINE | ES_READONLY | WS_VSCROLL,0,10,300,824,64);
            tool->ok = ctl(ui,window,L"BUTTON",L"使用所选单元",WS_TABSTOP | BS_DEFPUSHBUTTON,UI_TOOL_OK,610,374,126,28); EnableWindow(tool->ok,FALSE); ctl(ui,window,L"BUTTON",L"取消",WS_TABSTOP,UI_TOOL_CLOSE,746,374,88,28); return 0;
        }
        tool->list = ctl(ui, window, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT, UI_SOURCE_LIST, 10, 12, 466, 220);
        const wchar_t* labels[] = {L"+",L"−",L"↑",L"↓"}; const int ids[] = {UI_TOOL_ADD,UI_TOOL_REMOVE,UI_TOOL_UP,UI_TOOL_DOWN}; for (int i = 0; i < 4; i++) ctl(ui, window, L"BUTTON", labels[i], WS_TABSTOP, ids[i], 482, 12 + i * 32, 28, 27);
        if (tool->operation == GUI_CREATE_MDS) {
            tool->layer = ctl(ui, window, L"BUTTON", L"换层点 LBA：", BS_AUTOCHECKBOX | WS_TABSTOP, UI_TOOL_LAYER, 10, 246, 110, 23);
            tool->lba = ctl(ui, window, L"EDIT", L"0", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, UI_TOOL_LBA, 126, 244, 130, 25); SendMessageW(tool->lba, EM_SETLIMITTEXT, 7, 0);
            tool->ptp = ctl(ui, window, L"BUTTON", L"PTP", WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP, UI_TOOL_PTP, 280, 246, 64, 23); tool->otp = ctl(ui, window, L"BUTTON", L"OTP", WS_TABSTOP | BS_AUTORADIOBUTTON, UI_TOOL_OTP, 352, 246, 64, 23); SendMessageW(tool->otp, BM_SETCHECK, BST_CHECKED, 0);
            EnableWindow(tool->lba, FALSE); EnableWindow(tool->ptp, FALSE); EnableWindow(tool->otp, FALSE); ctl(ui, window, L"STATIC", L"光盘类型：DVD-ROM", 0, 0, 10, 286, 360, 23);
        } else ctl(ui, window, L"STATIC", L"按解码后的音频长度生成轨道。\r\n标准音频为 44.1 kHz、16 位双声道。", 0, 0, 10, 246, 480, 52);
        tool->ok = ctl(ui, window, L"BUTTON", L"确定", WS_TABSTOP | BS_DEFPUSHBUTTON, UI_TOOL_OK, 356, 332, 72, 28); EnableWindow(tool->ok, FALSE); ctl(ui, window, L"BUTTON", L"取消", WS_TABSTOP, UI_TOOL_CLOSE, 436, 332, 72, 28); return 0;
    }
    if (message == WM_NOTIFY && tool->operation == GUI_DVD_CANDIDATES && ((NMHDR*)lp)->idFrom == UI_DVD_CANDIDATE_LIST) {
        NMHDR* notice = (NMHDR*)lp;
        if (notice->code == LVN_ITEMCHANGED) { int index = ListView_GetNextItem(tool->list,-1,LVNI_SELECTED); EnableWindow(tool->ok,!ui->job && index >= 0 && (size_t)index < ui->dvdCandidateCount); }
        if (notice->code == NM_DBLCLK && !ui->job && IsWindowEnabled(tool->ok)) SendMessageW(window,WM_COMMAND,UI_TOOL_OK,0);
        return 0;
    }
    if (message == WM_COMMAND) {
        if (ui->job && LOWORD(wp) != UI_TOOL_CLOSE && !((tool->operation == GUI_DVD_CANDIDATES || tool->operation == GUI_CD_ISO_PLAN) && LOWORD(wp) == IDCANCEL)) return 0;
        if (tool->operation == GUI_READ_OPTIONS) {
            if (LOWORD(wp) == UI_TOOL_OK) {
                wchar_t split[40], retries[8], *end; GetWindowTextW(GetDlgItem(window,UI_READ_SPLIT),split,40); GetWindowTextW(GetDlgItem(window,UI_READ_RETRIES),retries,8);
                bool automatic; uint64_t bytes; unsigned long count = wcstoul(retries,&end,10); const wchar_t* extension = wcsrchr(ui->readImage,L'.'); bool cue = extension && !_wcsicmp(extension,L".cue");
                if (!retries[0] || *end || count > 20 || retries[0] == L'-') { MessageBoxW(window,L"软件重试次数应为 0 至 20。",L"ImgBurn",MB_OK | MB_ICONINFORMATION); return 0; }
                if (!cue && (!gui_read_split(split,&automatic,&bytes) || (extension && !_wcsicmp(extension,L".iso") && bytes))) { MessageBoxW(window,L"请选择 Auto、不分卷，或为 MDS 输入有效的自定义 MiB 容量。",L"ImgBurn",MB_OK | MB_ICONINFORMATION); return 0; }
                wcscpy(ui->readSplitText,split); wcscpy(ui->readRetryText,retries); ui->readCdText = SendDlgItemMessageW(window,UI_READ_CDTEXT,BM_GETCHECK,0,0) == BST_CHECKED; ui->rawCdMds = SendDlgItemMessageW(window,UI_READ_RAW_MDS,BM_GETCHECK,0,0) == BST_CHECKED;
                ui->includeSessionPregaps = SendDlgItemMessageW(window,UI_SESSION_PREGAPS,BM_GETCHECK,0,0) == BST_CHECKED; ui->readEjectWhenDone = SendDlgItemMessageW(window,UI_READ_EJECT,BM_GETCHECK,0,0) == BST_CHECKED; DestroyWindow(window); gui_menu(ui);
            } else if (LOWORD(wp) == UI_TOOL_CLOSE || LOWORD(wp) == IDCANCEL) SendMessageW(window,WM_CLOSE,0,0); return 0;
        }
        if (tool->operation == GUI_WRITE_OPTIONS) {
            if (LOWORD(wp) == UI_TOOL_OK) { wchar_t value[16]; GetWindowTextW(ui->writeLayer,value,16); if (wcscmp(value,ui->writeLayerValue)) gui_dvd_candidates_clear(ui,false); wcscpy(ui->writeLayerValue,value); DestroyWindow(window); }
            else if (LOWORD(wp) == UI_TOOL_CLOSE || LOWORD(wp) == IDCANCEL) SendMessageW(window,WM_CLOSE,0,0); return 0;
        }
        if (tool->operation == GUI_CD_ISO_PLAN) {
            if (LOWORD(wp) == UI_CD_PLAN) gui_cd_iso_start(ui,false);
            else if (LOWORD(wp) == UI_CD_SELECT) { if (gui_cd_plan_select(ui)) SetWindowTextW(tool->candidateStatus,L"已明确使用此计划。可点击写入并回读；执行前会再次核对实际布局。"); else { gui_cd_plan_fill(tool); SetWindowTextW(tool->candidateStatus,L"计划已失效，请重新只读规划。"); } }
            else if (LOWORD(wp) == UI_CD_WRITE) gui_cd_iso_start(ui,true);
            else if (LOWORD(wp) == UI_CD_ALLOW_BLANK || LOWORD(wp) == UI_CD_FINALIZE) { ui->cdAllowBlank = SendMessageW(tool->cdAllowBlank,BM_GETCHECK,0,0) == BST_CHECKED; ui->cdFinalize = SendMessageW(tool->cdFinalize,BM_GETCHECK,0,0) == BST_CHECKED; gui_cd_plan_clear(ui); gui_cd_plan_fill(tool); SetWindowTextW(tool->candidateStatus,L"选项已变更，请重新只读规划并选择。"); }
            else if (LOWORD(wp) == UI_TOOL_CLOSE || LOWORD(wp) == IDCANCEL) SendMessageW(window,WM_CLOSE,0,0);
            return 0;
        }
        if (tool->operation == GUI_DVD_CANDIDATES) {
            if (LOWORD(wp) == UI_TOOL_OK) { int selected = ListView_GetNextItem(tool->list,-1,LVNI_SELECTED); if (selected >= 0 && gui_dvd_candidate_select(ui,(size_t)selected)) { tool->candidateAccepted = true; gui_status(ui,L"已明确选择视频单元，双层 L0 已填入。"); DestroyWindow(window); } else { gui_dvd_candidates_clear(ui,true); SetWindowTextW(tool->candidateStatus,L"候选来源已失效，请关闭后重新查询。"); } }
            else if (LOWORD(wp) == UI_TOOL_CLOSE || LOWORD(wp) == IDCANCEL) SendMessageW(window,WM_CLOSE,0,0);
            return 0;
        }
        switch (LOWORD(wp)) {
            case UI_TOOL_ADD: { wchar_t path[MAX_PATH] = L""; if (gui_file(window, path, false, tool->operation == GUI_CREATE_CUE ? L"音频文件\0*.wav;*.mp3;*.flac;*.aiff;*.aif;*.ogg;*.ape;*.wv\0所有文件\0*.*\0\0" : L"镜像及分卷\0*.iso;*.bin;*.mdf;*.i00;*.i01;*.i02\0所有文件\0*.*\0\0", NULL) && gui_paths_add(&tool->sources, &tool->count, path)) gui_tool_fill(tool); break; }
            case UI_TOOL_REMOVE: { LRESULT at = SendMessageW(tool->list, LB_GETCURSEL, 0, 0); if (at >= 0 && (size_t)at < tool->count) { memmove(tool->sources + at, tool->sources + at + 1, (tool->count - (size_t)at - 1) * sizeof(*tool->sources)); tool->count--; gui_tool_fill(tool); } break; }
            case UI_TOOL_UP: case UI_TOOL_DOWN: { LRESULT at = SendMessageW(tool->list, LB_GETCURSEL, 0, 0), next = at + (LOWORD(wp) == UI_TOOL_UP ? -1 : 1); if (at >= 0 && next >= 0 && (size_t)next < tool->count) { GuiPath tmp = tool->sources[at]; tool->sources[at] = tool->sources[next]; tool->sources[next] = tmp; gui_tool_fill(tool); SendMessageW(tool->list, LB_SETCURSEL, next, 0); } break; }
            case UI_TOOL_LAYER: { bool checked = SendMessageW(tool->layer, BM_GETCHECK, 0, 0) == BST_CHECKED; EnableWindow(tool->lba, checked); EnableWindow(tool->otp, checked); EnableWindow(tool->ptp, checked); break; }
            case UI_TOOL_OK: {
                wchar_t output[MAX_PATH] = L""; bool cue = tool->operation == GUI_CREATE_CUE; if (!gui_file(window, output, true, cue ? L"CUE 文件\0*.cue\0\0" : L"MDS 文件\0*.mds\0\0", cue ? L"cue" : L"mds")) break;
                GuiJob* job = gui_job(ui, tool->operation, tool->sources, tool->count); if (!job) break; wcscpy(job->destination, output);
                if (!cue && SendMessageW(tool->layer, BM_GETCHECK, 0, 0) == BST_CHECKED) { wchar_t number[16]; GetWindowTextW(tool->lba, number, 16); job->layerBreak = wcstoul(number, NULL, 10); if (!job->layerBreak) { MessageBoxW(window, L"请输入有效的换层扇区数。", L"ImgBurn", MB_OK | MB_ICONERROR); imgburn_gui_job_free(job); break; } job->oppositeTrackPath = SendMessageW(tool->otp, BM_GETCHECK, 0, 0) == BST_CHECKED; }
                if (gui_launch(ui, job)) for (HWND child = GetWindow(window,GW_CHILD); child; child = GetWindow(child,GW_HWNDNEXT)) if (GetDlgCtrlID(child) != UI_TOOL_CLOSE) EnableWindow(child,FALSE); break;
            }
            case UI_TOOL_CLOSE: SendMessageW(window, WM_CLOSE, 0, 0); break;
        } return 0;
    }
    if (message == WM_CLOSE) { if (tool->operation == GUI_CD_ISO_PLAN) { tool->cdClosing = true; gui_cd_plan_clear(ui); } if (tool->operation == GUI_DVD_CANDIDATES) { tool->candidateClosing = true; gui_dvd_candidates_clear(ui,true); if (!ui->job) gui_status(ui,L"已关闭候选窗口，未保留派生单元选择"); } if (!ui->job) DestroyWindow(window); else { SetEvent(ui->job->cancel); EnableWindow(GetDlgItem(window,UI_TOOL_CLOSE),FALSE); SetWindowTextW(window,L"正在取消..."); } return 0; }
    if (message == WM_DESTROY) { if (tool->operation == GUI_WRITE_OPTIONS) ui->writeLayer = NULL; if (tool->operation == GUI_CD_ISO_PLAN) gui_cd_plan_clear(ui); if (tool->operation == GUI_DVD_CANDIDATES && !tool->candidateAccepted) gui_dvd_candidates_clear(ui,true); EnableWindow(ui->window, TRUE); SetForegroundWindow(ui->window); ui->tool = NULL; SetWindowLongPtrW(window,GWLP_USERDATA,0); free(tool->sources); free(tool); if (ui->writeNeedsRefresh) { ui->writeNeedsRefresh = false; PostMessageW(ui->window,WM_COMMAND,UI_DEVICE_REFRESH,0); } return 0; }
    return DefWindowProcW(window, message, wp, lp);
}

static void gui_disc(HDC dc, int x, int y, int size, bool recording) {
    HGDIOBJ oldPen = SelectObject(dc,GetStockObject(NULL_PEN)), oldBrush = GetCurrentObject(dc,OBJ_BRUSH);
    for (int inset = 0; inset < size / 2; inset++) {
        int value = 170 + (size / 2 - inset) * 70 / (size / 2); HBRUSH shade = CreateSolidBrush(RGB(value,value,value)); SelectObject(dc,shade); Ellipse(dc,x + inset,y + inset,x + size - inset,y + size - inset); SelectObject(dc,oldBrush); DeleteObject(shade);
    }
    const COLORREF colors[] = {RGB(166,204,190),RGB(201,192,223),RGB(214,206,158),RGB(172,201,216)};
    POINT triangles[4][3] = {{{x + size / 2,y + size / 2},{x + 2,y + size / 4},{x + 3,y + size / 2}},{{x + size / 2,y + size / 2},{x + size - 3,y + size / 3},{x + size - 4,y + size / 2}},{{x + size / 2,y + size / 2},{x + size / 3,y + size - 3},{x + size / 2,y + size - 2}},{{x + size / 2,y + size / 2},{x + size / 3,y + 2},{x + size / 2,y + 2}}};
    for (unsigned i = 0; i < 4; i++) { HBRUSH brush = CreateSolidBrush(colors[i]); SelectObject(dc,brush); Polygon(dc,triangles[i],3); SelectObject(dc,oldBrush); DeleteObject(brush); }
    HPEN border = CreatePen(PS_SOLID,1,RGB(132,139,143)); SelectObject(dc,border); SelectObject(dc,GetStockObject(HOLLOW_BRUSH)); Ellipse(dc,x,y,x + size,y + size);
    SelectObject(dc,GetStockObject(WHITE_BRUSH)); Ellipse(dc,x + size * 3 / 8,y + size * 3 / 8,x + size * 5 / 8,y + size * 5 / 8);
    SelectObject(dc,GetStockObject(LTGRAY_BRUSH)); Ellipse(dc,x + size * 7 / 16,y + size * 7 / 16,x + size * 9 / 16,y + size * 9 / 16);
    if (recording) { HPEN red = CreatePen(PS_SOLID,size / 15 + 1,RGB(216,96,102)); SelectObject(dc,red); SelectObject(dc,GetStockObject(HOLLOW_BRUSH)); Arc(dc,x + size / 5,y + size / 7,x + size * 4 / 5,y + size * 6 / 7,x + size / 2,y + size,x + size / 2,y); SelectObject(dc,border); DeleteObject(red); }
    SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(border);
}
static void gui_ez_draw(GuiWindow* ui, DRAWITEMSTRUCT* draw) {
    RECT r = draw->rcItem; HDC dc = draw->hDC; FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE)); SetBkMode(dc, TRANSPARENT);
    bool hot = ui->hotControl == draw->hwndItem; if (draw->itemState & ODS_SELECTED) DrawEdge(dc,&r,BDR_SUNKENOUTER,BF_RECT); else if (hot) DrawEdge(dc,&r,BDR_RAISEDINNER,BF_RECT);
    unsigned asset = draw->CtlID - UI_EZ_FIRST; bool original = asset < 6 && imgburn_gui_asset_draw_state(&ui->assets,(GuiAssetId)asset,dc,(r.left + r.right - px(ui,128)) / 2,r.top + px(ui,20),px(ui,128),px(ui,48),!(draw->itemState & ODS_DISABLED),hot,true);
    if (!original) {
    int center = (r.right + r.left) / 2, y = r.top + px(ui,20), size = px(ui,46); HPEN pen = CreatePen(PS_SOLID, px(ui,1), RGB(138,145,151)); HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(WHITE_BRUSH));
    for (int i = 0; i < 2; i++) {
        int x = center + (i ? px(ui,26) : -px(ui,70)); bool folder = draw->CtlID == UI_EZ_FIRST + 1 || draw->CtlID == UI_EZ_FIRST + 3;
        if (!i && folder) {
            HBRUSH yellow = CreateSolidBrush(RGB(229,190,87)); SelectObject(dc,yellow); Rectangle(dc,x,y + px(ui,2),x + size / 2,y + px(ui,11)); Rectangle(dc,x,y + px(ui,8),x + size,y + size); SelectObject(dc,oldBrush); DeleteObject(yellow);
            HBRUSH front = CreateSolidBrush(RGB(251,219,124)); SelectObject(dc,front); POINT p[] = {{x + px(ui,4),y + size},{x + px(ui,9),y + px(ui,14)},{x + size + px(ui,4),y + px(ui,14)},{x + size - px(ui,1),y + size}}; Polygon(dc,p,4); SelectObject(dc,oldBrush); DeleteObject(front);
        } else {
            bool page = i ? draw->CtlID >= UI_EZ_FIRST + 2 && draw->CtlID <= UI_EZ_FIRST + 4 : draw->CtlID == UI_EZ_FIRST || draw->CtlID == UI_EZ_FIRST + 5;
            if (page) { SelectObject(dc,GetStockObject(WHITE_BRUSH)); Rectangle(dc,x,y,x + size,y + size); MoveToEx(dc,x + size * 3 / 4,y,NULL); LineTo(dc,x + size * 3 / 4,y + size / 4); LineTo(dc,x + size,y + size / 4); }
            if (!i && draw->CtlID == UI_EZ_FIRST + 5) { RECT binary = {x + px(ui,4),y + px(ui,6),x + size,y + size}; DrawTextW(dc,L"01001\n10110\n00101\n11010",-1,&binary,DT_LEFT); }
            else gui_disc(dc,x + (page ? size / 6 : 0),y + (page ? size / 4 : 0),page ? size * 2 / 3 : size,i && (draw->CtlID <= UI_EZ_FIRST + 1 || draw->CtlID == UI_EZ_FIRST + 5));
        }
    }
    POINT arrow[] = {{center - px(ui,15),y + px(ui,19)},{center + px(ui,3),y + px(ui,19)},{center + px(ui,3),y + px(ui,12)},{center + px(ui,18),y + px(ui,25)},{center + px(ui,3),y + px(ui,38)},{center + px(ui,3),y + px(ui,31)},{center - px(ui,15),y + px(ui,31)}};
    HBRUSH blue = CreateSolidBrush(RGB(73,157,203)); SelectObject(dc,blue); Polygon(dc,arrow,7); SelectObject(dc,oldBrush); DeleteObject(blue); SelectObject(dc,oldPen); DeleteObject(pen);
    }
    wchar_t text[128]; GetWindowTextW(draw->hwndItem,text,128); RECT caption = {r.left,r.top + px(ui,72),r.right,r.top + px(ui,85)}; HGDIOBJ oldFont = SelectObject(dc,ui->font); SetTextColor(dc,GetSysColor(COLOR_BTNTEXT)); DrawTextW(dc,text,-1,&caption,DT_CENTER | DT_VCENTER | DT_SINGLELINE); SelectObject(dc,oldFont); if (draw->itemState & ODS_FOCUS) { InflateRect(&r,-3,-3); DrawFocusRect(dc,&r); }
}
static void gui_drive_draw(GuiWindow* ui, const DRAWITEMSTRUCT* draw) {
    bool enabled = !(draw->itemState & ODS_DISABLED), selected = (draw->itemState & ODS_SELECTED) != 0; RECT rect = draw->rcItem;
    FillRect(draw->hDC,&rect,GetSysColorBrush(!enabled ? COLOR_BTNFACE : selected ? COLOR_HIGHLIGHT : COLOR_WINDOW)); if (draw->itemID == (UINT)-1) return;
    wchar_t text[128] = L""; LRESULT length = SendMessageW(draw->hwndItem,CB_GETLBTEXTLEN,draw->itemID,0); if (length >= 0 && length < 128) SendMessageW(draw->hwndItem,CB_GETLBTEXT,draw->itemID,(LPARAM)text);
    imgburn_gui_asset_draw_state(&ui->assets,ASSET_DRIVE,draw->hDC,rect.left + px(ui,2),(rect.top + rect.bottom - px(ui,16)) / 2,px(ui,16),px(ui,16),enabled,false,false);
    rect.left += px(ui,22); SetBkMode(draw->hDC,TRANSPARENT); SetTextColor(draw->hDC,GetSysColor(!enabled ? COLOR_GRAYTEXT : selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT)); HGDIOBJ font = SelectObject(draw->hDC,ui->font); DrawTextW(draw->hDC,text,-1,&rect,DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX); SelectObject(draw->hDC,font); if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC,&rect);
}

static void gui_complete(GuiWindow* ui, GuiJob* job) {
    if (ui->job != job) return; WaitForSingleObject(job->thread, INFINITE); ui->job = NULL;
    if (job->operation == GUI_READ_DEST_INFO) {
        bool current = ui->mode == GUI_READ && !_wcsicmp(ui->readImage,job->destination) && !_wcsicmp(ui->readDestinationQuery,job->destination);
        if (current) { ui->readDestinationReady = job->succeeded && job->readDestinationReady && WaitForSingleObject(job->cancel,0) != WAIT_OBJECT_0; ui->readDestinationTotalBytes = ui->readDestinationReady ? job->readDestinationTotalBytes : 0; ui->readDestinationFreeBytes = ui->readDestinationReady ? job->readDestinationFreeBytes : 0; gui_read_destination_info(ui); gui_status(ui,ui->readDestinationReady ? L"目标空间已载入" : job->error == ERROR_CANCELLED ? L"目标空间查询已取消" : L"无法确认目标空间，请查看日志"); gui_log(ui,job->result); }
        if (ui->cancel) EnableWindow(ui->cancel,FALSE); imgburn_gui_job_free(job); gui_menu(ui); if (!current) gui_read_destination_query(ui); return;
    }
    if (job->operation == GUI_CD_ISO_PLAN || job->operation == GUI_CD_ISO_WRITE) {
        bool planning = job->operation == GUI_CD_ISO_PLAN;
        if (planning) {
            if (!gui_cd_plan_take(ui,job) && job->succeeded) { job->succeeded = false; job->error = job->cancel && WaitForSingleObject(job->cancel,0) == WAIT_OBJECT_0 ? ERROR_CANCELLED : ERROR_MEDIA_CHANGED; wcscpy(job->result,job->error == ERROR_CANCELLED ? L"追加规划已取消。\r\n" : L"来源、驱动器或选项已变化，请重新规划。\r\n"); }
        } else { gui_cd_plan_clear(ui); ui->cdPlan = job->cdResult.data; ui->writeMediaReady = false; ui->writeNeedsRefresh = true; }
        if (ui->progress) SendMessageW(ui->progress,PBM_SETPOS,job->succeeded ? 100 : 0,0); if (ui->info) SetWindowTextW(ui->info,job->result); gui_log(ui,job->result);
        gui_status(ui,job->succeeded ? planning ? L"只读规划完成，请明确使用此计划" : L"独立 ISO 会话已写入并通过回读" : job->error == ERROR_CANCELLED ? L"追加操作已取消" : job->error == ERROR_NOT_SUPPORTED ? L"来源或介质不在追加扩展支持范围" : L"追加操作停止，请查看实际结果");
        if (ui->cancel) EnableWindow(ui->cancel,FALSE);
        if (ui->tool && ui->tool->operation == GUI_CD_ISO_PLAN) {
            if (ui->tool->cdClosing) DestroyWindow(ui->tool->window);
            else { gui_cd_plan_fill(ui->tool); SetWindowTextW(ui->tool->candidateStatus,job->result); }
        }
        imgburn_gui_job_free(job); gui_write_checks(ui); gui_menu(ui); return;
    }
    if (job->operation == GUI_DVD_CANDIDATES) {
        bool accepted = gui_dvd_candidates_take(ui,job);
        if (!accepted && job->succeeded) { job->succeeded = false; job->error = job->cancel && WaitForSingleObject(job->cancel,0) == WAIT_OBJECT_0 ? ERROR_CANCELLED : ERROR_MEDIA_CHANGED; wcscpy(job->result,job->error == ERROR_CANCELLED ? L"候选查询已取消。\r\n" : L"候选来源已失效，请重新查询。\r\n"); }
        if (ui->progress) SendMessageW(ui->progress,PBM_SETPOS,job->succeeded ? 100 : 0,0); if (ui->info) SetWindowTextW(ui->info,job->result); gui_log(ui,job->result);
        gui_status(ui,job->succeeded ? ui->dvdCandidateCount ? L"候选查询完成，请明确选择单元" : L"查询完成，没有符合条件的视频单元" : job->error == ERROR_CANCELLED ? L"候选查询已取消" : job->error == ERROR_NOT_SUPPORTED ? L"当前视频格式或布局不支持候选查询" : L"候选读取失败，请查看结果");
        if (ui->cancel) EnableWindow(ui->cancel,FALSE);
        if (ui->tool && ui->tool->operation == GUI_DVD_CANDIDATES) {
            if (ui->tool->candidateClosing) DestroyWindow(ui->tool->window);
            else { gui_dvd_candidates_fill(ui->tool); SetWindowTextW(ui->tool->candidateStatus,job->result); SetWindowTextW(GetDlgItem(ui->tool->window,UI_TOOL_CLOSE),L"关闭"); }
        }
        imgburn_gui_job_free(job); gui_write_checks(ui); gui_menu(ui); return;
    }
    if (job->operation == GUI_DRIVE_INFO) { ui->mediaReady = job->mediaReady; ui->writeMediaReady = job->writeMediaReady; ui->testWriteSupported = job->testWriteSupported; ui->eraseSupported = job->eraseSupported; ui->discoveryCapacity = job->discoveryCapacity; }
    if (job->operation == GUI_DRIVE_INFO && ui->mode == GUI_READ) {
        bool current = ui->selectedDrive >= 0 && ui->selectedDrive < ui->driveCount && !_wcsicmp(ui->devices[ui->selectedDrive].devicePath,job->drive);
        ui->mediaReady = current && job->succeeded && job->mediaReady; ui->readDiscText[0] = 0; if (current) wcscpy(ui->readDiscText,job->result);
    }
    if (job->operation == GUI_READ_DISC && ((job->succeeded && job->readEject) || job->error == ERROR_MEDIA_CHANGED || job->error == ERROR_NOT_READY)) { ui->mediaReady = false; ui->readDiscText[0] = 0; if (ui->mode == GUI_READ && ui->info) SetWindowTextW(ui->info,L""); }
    bool writeInspection = job->operation == GUI_INSPECT && ui->mode == GUI_WRITE; if (job->operation == GUI_INSPECT) gui_source_summary_apply(ui,job);
    if (job->operation == GUI_WRITE_DISC) ui->writeMediaReady = false;
    if (job->operation == GUI_READ_DISC || job->operation == GUI_VERIFY_DISC || job->operation == GUI_WRITE_DISC || job->operation == GUI_ERASE_QUICK || job->operation == GUI_ERASE_FULL || job->operation == GUI_DISCOVERY_DISC) { EnableWindow(ui->destination,TRUE); EnableWindow(ui->drives,TRUE); }
    if (ui->progress) SendMessageW(ui->progress, PBM_SETPOS, job->succeeded && job->operation != GUI_DRIVE_INFO ? 100 : 0, 0); if (ui->info && !writeInspection) { if (ui->mode != GUI_READ) SetWindowTextW(ui->info,job->result); else if (job->operation == GUI_DRIVE_INFO) SetWindowTextW(ui->info,ui->readDiscText); } gui_log(ui,job->result);
    if (writeInspection && ui->sourceInspectPopup && job->succeeded && job->sourceCount == 1 && !_wcsicmp(ui->sourceImage,job->sources[0].path)) MessageBoxW(ui->window,job->result,L"镜像信息",MB_OK | MB_ICONINFORMATION); if (job->operation == GUI_INSPECT) ui->sourceInspectPopup = false;
    const wchar_t* completed = job->operation == GUI_DRIVE_INFO ? ((ui->mode == GUI_READ ? ui->mediaReady : job->mediaReady) ? L"介质已就绪" : L"介质未就绪或未插入光盘") : job->operation == GUI_CALCULATE ? L"计算完成" : job->operation == GUI_INSPECT ? L"镜像信息已载入" : L"操作成功完成";
    gui_status(ui, job->succeeded ? completed : job->error == ERROR_CANCELLED ? L"操作已取消" : L"操作失败；请查看日志");
    if (ui->go) EnableWindow(ui->go, ui->mode != GUI_READ && ui->mode != GUI_DISCOVERY && !(ui->mode == GUI_BUILD && ui->outputDevice)); if (ui->cancel) EnableWindow(ui->cancel,FALSE);
    if (ui->tool) {
        if (job->succeeded || job->error == ERROR_CANCELLED) DestroyWindow(ui->tool->window);
        else { HWND toolWindow = ui->tool->window; for (HWND child = GetWindow(toolWindow,GW_CHILD); child; child = GetWindow(child,GW_HWNDNEXT)) EnableWindow(child,TRUE); SetWindowTextW(toolWindow,ui->tool->operation == GUI_CREATE_CUE ? L"生成 CUE 文件" : L"创建 MDS 文件"); if (ui->tool->layer) SendMessageW(toolWindow,WM_COMMAND,UI_TOOL_LAYER,0); MessageBoxW(toolWindow,job->result,L"ImgBurn",MB_OK | MB_ICONERROR); }
    }
    else if (!job->succeeded && job->error != ERROR_CANCELLED) MessageBoxW(ui->window,job->result,L"ImgBurn",MB_OK | MB_ICONERROR);
    bool refreshMedia = job->operation == GUI_ERASE_QUICK || job->operation == GUI_ERASE_FULL || job->operation == GUI_DISCOVERY_DISC || job->operation == GUI_WRITE_DISC;
    bool inspectSource = job->operation == GUI_DRIVE_INFO && ui->mode == GUI_WRITE && ui->sourceImage[0] && !ui->sourceInfoAttempted && !ui->tool;
    bool readDestination = job->operation == GUI_DRIVE_INFO && ui->mode == GUI_READ && !ui->tool;
    imgburn_gui_job_free(job); gui_write_checks(ui); gui_menu(ui); if (refreshMedia) gui_start(ui,GUI_DRIVE_INFO); else if (inspectSource) { ui->sourceInspectPopup = false; gui_start(ui,GUI_INSPECT); } else if (readDestination) gui_read_destination_query(ui);
}

static LRESULT CALLBACK gui_log_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    GuiWindow* ui = (GuiWindow*)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message == WM_NCCREATE) { ui = (GuiWindow*)((CREATESTRUCTW*)lp)->lpCreateParams; SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)ui); }
    if (!ui) return DefWindowProcW(window,message,wp,lp);
    if (message == WM_CREATE) {
        HMENU menu = CreateMenu(), file = CreatePopupMenu(), edit = CreatePopupMenu(); item(file,UI_LOG_SAVE,L"另存为(&S)...",true); item(file,UI_LOG,L"关闭(&C)",true); item(edit,UI_LOG_COPY,L"复制全部(&C)",true); popup(menu,file,L"文件(&F)"); popup(menu,edit,L"编辑(&E)"); SetMenu(window,menu);
        ui->logList = ctl(ui,window,L"LISTBOX",L"",WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_MULTIPLESEL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,0,0,0,633,180); SendMessageW(ui->logList,LB_SETITEMHEIGHT,0,px(ui,16)); return 0;
    }
    if (message == WM_MEASUREITEM) { ((MEASUREITEMSTRUCT*)lp)->itemHeight = px(ui,16); return TRUE; }
    if (message == WM_DRAWITEM) {
        const DRAWITEMSTRUCT* draw = (const DRAWITEMSTRUCT*)lp; RECT rect = draw->rcItem; bool selected = (draw->itemState & ODS_SELECTED) != 0; FillRect(draw->hDC,&rect,GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        if (draw->itemID != (UINT)-1) { LRESULT length = SendMessageW(draw->hwndItem,LB_GETTEXTLEN,draw->itemID,0); if (length >= 0) { wchar_t* text = calloc((size_t)length + 1,sizeof(*text)); if (text) { SendMessageW(draw->hwndItem,LB_GETTEXT,draw->itemID,(LPARAM)text); rect.left += px(ui,2); HGDIOBJ font = SelectObject(draw->hDC,ui->font); SetBkMode(draw->hDC,TRANSPARENT); SetTextColor(draw->hDC,GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT)); DrawTextW(draw->hDC,text,-1,&rect,DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX); SelectObject(draw->hDC,font); free(text); } } } if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC,&draw->rcItem); return TRUE;
    }
    if (message == WM_SIZE) { MoveWindow(ui->logList,0,0,LOWORD(lp),HIWORD(lp),TRUE); return 0; }
    if (message == WM_COMMAND) {
        if (LOWORD(wp) == UI_LOG) SendMessageW(window,WM_CLOSE,0,0);
        else if (LOWORD(wp) == UI_LOG_SAVE || LOWORD(wp) == UI_LOG_COPY) {
            LRESULT count = SendMessageW(ui->logList,LB_GETCOUNT,0,0); size_t capacity = 1;
            if (count == LB_ERR) return 0;
            for (LRESULT i = 0; i < count; i++) { LRESULT n = SendMessageW(ui->logList,LB_GETTEXTLEN,i,0); if (n < 0 || capacity > SIZE_MAX / sizeof(wchar_t) - 2 || (size_t)n > SIZE_MAX / sizeof(wchar_t) - capacity - 2) return 0; capacity += (size_t)n + 2; }
            wchar_t* text = (wchar_t*)calloc(capacity,sizeof(wchar_t)); if (!text) return 0; size_t used = 0;
            for (LRESULT i = 0; i < count; i++) { LRESULT n = SendMessageW(ui->logList,LB_GETTEXTLEN,i,0); if (n < 0 || used >= capacity || capacity - used < 3 || (size_t)n > capacity - used - 3) break; LRESULT copied = SendMessageW(ui->logList,LB_GETTEXT,i,(LPARAM)(text + used)); if (copied != n) { free(text); return 0; } used += (size_t)n; text[used++] = L'\r'; text[used++] = L'\n'; }
            if (used >= capacity) { free(text); return 0; } text[used] = 0;
            if (LOWORD(wp) == UI_LOG_SAVE) { wchar_t path[MAX_PATH] = L"ImgBurn.log"; if (gui_file(window,path,true,L"日志文件\0*.log\0\0",L"log")) { FILE* out = _wfopen(path,L"wb"); if (out) { uint16_t bom = 0xFEFF; fwrite(&bom,2,1,out); fwrite(text,sizeof(wchar_t),used,out); fclose(out); } } }
            else if (LOWORD(wp) == UI_LOG_COPY && OpenClipboard(window)) { HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE,(used + 1) * sizeof(wchar_t)); if (data) { void* p = GlobalLock(data); if (p) { memcpy(p,text,(used + 1) * sizeof(wchar_t)); GlobalUnlock(data); EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT,data)) GlobalFree(data); } else GlobalFree(data); } CloseClipboard(); }
            free(text);
        } return 0;
    }
    if (message == WM_CLOSE) { ShowWindow(window,SW_HIDE); ui->logVisible = false; gui_menu(ui); return 0; }
    return DefWindowProcW(window,message,wp,lp);
}

static LRESULT CALLBACK gui_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    GuiWindow* ui = (GuiWindow*)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message == WM_NCCREATE) { ui = (GuiWindow*)((CREATESTRUCTW*)lp)->lpCreateParams; ui->window = window; SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)ui); }
    if (!ui) return DefWindowProcW(window,message,wp,lp);
    if (message == WM_CREATE) { ui->status = CreateWindowW(STATUSCLASSNAMEW,L"Ready",WS_CHILD | WS_VISIBLE,0,0,0,0,window,NULL,ui->instance,NULL); SendMessageW(ui->status,WM_SETFONT,(WPARAM)ui->font,TRUE); gui_mode(ui,GUI_EZ); DragAcceptFiles(window,TRUE); return 0; }
    if (message == WM_SIZE) { SendMessageW(ui->status,WM_SIZE,0,0); return 0; }
    if (message == WM_DRAWITEM && wp >= UI_EZ_FIRST && wp < UI_EZ_FIRST + 6) { gui_ez_draw(ui,(DRAWITEMSTRUCT*)lp); return TRUE; }
    if (message == WM_MEASUREITEM && wp == UI_DRIVE && (ui->mode == GUI_WRITE || ui->mode == GUI_READ)) { ((MEASUREITEMSTRUCT*)lp)->itemHeight = px(ui,20); return TRUE; }
    if (message == WM_DRAWITEM && wp == UI_DRIVE && (ui->mode == GUI_WRITE || ui->mode == GUI_READ)) { gui_drive_draw(ui,(const DRAWITEMSTRUCT*)lp); return TRUE; }
    if (message == WM_DRAWITEM) { bool imagePage = ui->mode == GUI_WRITE || ui->mode == GUI_READ; GuiAssetId asset = wp == UI_GO && imagePage ? ui->mode == GUI_READ ? ASSET_READ : ASSET_WRITE : gui_control_asset((UINT)wp); DRAWITEMSTRUCT draw = *(const DRAWITEMSTRUCT*)lp; if (ui->hotControl == draw.hwndItem) draw.itemState |= ODS_HOTLIGHT; if (asset != ASSET_COUNT && (imagePage ? imgburn_gui_asset_button_dpi(&ui->assets,asset,&draw,ui->dpi,true) : imgburn_gui_asset_button(&ui->assets,asset,&draw))) return TRUE; }
    if (message == WM_NOTIFY && ((NMHDR*)lp)->idFrom == UI_TAB && ((NMHDR*)lp)->code == TCN_SELCHANGE) { gui_build_page(ui); return 0; }
    if (message == GUI_JOB_PROGRESS) { if (ui->job == (GuiJob*)lp && ui->progress) SendMessageW(ui->progress,PBM_SETPOS,wp,0); return 0; }
    if (message == GUI_JOB_STATUS) { if (ui->job == (GuiJob*)wp) { gui_status(ui,(const wchar_t*)lp); gui_log(ui,(const wchar_t*)lp); } free((void*)lp); return 0; }
    if (message == GUI_JOB_DONE) { gui_complete(ui,(GuiJob*)lp); return 0; }
    if (message == WM_DROPFILES) {
        HDROP drop = (HDROP)wp; if (!ui->job && !ui->tool) { UINT count = DragQueryFileW(drop,0xFFFFFFFF,NULL,0); for (UINT i = 0; i < count; i++) { wchar_t path[MAX_PATH]; if (DragQueryFileW(drop,i,path,MAX_PATH) >= MAX_PATH) continue; if (ui->mode == GUI_BUILD) gui_paths_add(&ui->paths,&ui->pathCount,path); else { gui_dvd_source_set(ui,path); break; } } gui_mode(ui,ui->mode); } DragFinish(drop); return 0;
    }
    if (message == WM_COMMAND) {
        UINT command = LOWORD(wp);
        if (command >= UI_EZ_FIRST && command < UI_EZ_FIRST + 6) { static const GuiMode modes[] = {GUI_WRITE,GUI_BUILD,GUI_READ,GUI_BUILD,GUI_VERIFY,GUI_DISCOVERY}; ui->outputDevice = command == UI_EZ_FIRST + 1; gui_mode(ui,modes[command - UI_EZ_FIRST]); return 0; }
        if (command == UI_LOG) { ui->logVisible = !ui->logVisible; ShowWindow(ui->logWindow,ui->logVisible ? SW_SHOWNOACTIVATE : SW_HIDE); gui_menu(ui); return 0; }
        if (ui->job) { if (command == UI_CANCEL) { SetEvent(ui->job->cancel); if (ui->job->operation == GUI_CD_ISO_PLAN || ui->job->operation == GUI_CD_ISO_WRITE) gui_cd_plan_clear(ui); EnableWindow(ui->cancel,FALSE); gui_status(ui,ui->job->operation == GUI_ERASE_QUICK || ui->job->operation == GUI_ERASE_FULL ? L"正在取消；已经开始的擦除须等待设备结束..." : L"正在取消..."); } return 0; }
        switch (command) {
            case UI_MODE_EZ: gui_mode(ui,GUI_EZ); break; case UI_MODE_READ: gui_mode(ui,GUI_READ); break; case UI_MODE_BUILD: gui_mode(ui,GUI_BUILD); break; case UI_MODE_WRITE: gui_mode(ui,GUI_WRITE); break; case UI_MODE_VERIFY: gui_mode(ui,GUI_VERIFY); break; case UI_MODE_DISCOVERY: gui_mode(ui,GUI_DISCOVERY); break;
            case UI_SOURCE_FILE: gui_source(ui,false); break; case UI_SOURCE_FOLDER: gui_source(ui,true); break;
            case UI_VERIFY_COMPARE: case UI_READ_CDTEXT: case UI_READ_RAW_MDS: case UI_SESSION_PREGAPS: gui_read_controls(ui); gui_menu(ui); break;
            case UI_READ_SPLIT: if (HIWORD(wp) == CBN_SELCHANGE) { gui_read_controls(ui); gui_menu(ui); } break;
            case UI_TEST: ui->testMode = ui->testWriteSupported && SendMessageW(GetDlgItem(window,UI_TEST),BM_GETCHECK,0,0) == BST_CHECKED; gui_write_checks(ui); break;
            case UI_VERIFY_CHECK: gui_read_controls(ui); gui_write_checks(ui); break;
            case UI_DISCOVERY_ALL: case UI_DISCOVERY_CUSTOM: gui_read_controls(ui); gui_menu(ui); break;
            case UI_DISCOVERY_SECTORS: if (HIWORD(wp) == EN_CHANGE) { gui_read_controls(ui); gui_menu(ui); } break;
            case UI_DISCOVERY_CAPACITY:
                ui->discoveryAll = false; SendMessageW(GetDlgItem(window,UI_DISCOVERY_ALL),BM_SETCHECK,BST_UNCHECKED,0); SendMessageW(GetDlgItem(window,UI_DISCOVERY_CUSTOM),BM_SETCHECK,BST_CHECKED,0); swprintf(ui->discoveryCount,32,L"%u",ui->discoveryCapacity); SetWindowTextW(ui->discoverySectors,ui->discoveryCount); gui_menu(ui); break;
            case UI_DEST_EDIT: if (HIWORD(wp) == EN_CHANGE && (ui->mode == GUI_VERIFY || ui->mode == GUI_READ) && ui->destination) { gui_read_controls(ui); gui_menu(ui); } break;
            case UI_CLEAR: free(ui->paths); ui->paths = NULL; ui->pathCount = 0; gui_mode(ui,ui->mode); break;
            case UI_REMOVE_SOURCE: if (ui->sources) { for (size_t i = ui->pathCount; i > 0; i--) if (SendMessageW(ui->sources,LB_GETSEL,i - 1,0) > 0) { memmove(ui->paths + i - 1,ui->paths + i,(ui->pathCount - i) * sizeof(*ui->paths)); ui->pathCount--; } gui_mode(ui,GUI_BUILD); } break;
            case UI_NEW:
                gui_read_controls(ui); free(ui->paths); ui->paths = NULL; ui->pathCount = 0; ui->destinationImage[0] = 0; ui->fsTypes = FS_TYPE_ALL; ui->revision = UDF_REV_102; ui->dvdVideo = ui->bluray = false; ui->deduplicate = true; wcscpy(ui->volumeLabel,L"IMGBURN_DISC"); wcscpy(ui->readSplitText,L"Auto"); wcscpy(ui->readRate,L"MAX"); wcscpy(ui->readAudioRate,L"MAX"); wcscpy(ui->readRetryText,L"20");
                ui->fs = ui->udf = ui->label = ui->dvd = ui->bd = ui->optimize = NULL; if (ui->destination) SetWindowTextW(ui->destination,L""); gui_mode(ui,GUI_BUILD); break;
            case UI_DESTINATION: { wchar_t path[MAX_PATH]; wchar_t* target = ui->mode == GUI_READ ? ui->readImage : ui->destinationImage; wcscpy(path,target); if (gui_file(window,path,true,ui->mode == GUI_READ ? readFilter : L"ISO 镜像\0*.iso\0\0",L"iso")) { if (ui->mode == GUI_READ) { ui->readDestinationReady = ui->readDestinationAttempted = false; ui->readDestinationTotalBytes = ui->readDestinationFreeBytes = 0; } wcscpy(target,path); if (ui->destination) SetWindowTextW(ui->destination,path); gui_read_controls(ui); gui_menu(ui); gui_read_destination_query(ui); } break; }
            case UI_CALCULATE: gui_start(ui,GUI_CALCULATE); break;
            case UI_BUILD: case UI_READ: case UI_WRITE: case UI_VERIFY: case UI_GO:
                if (ui->mode == GUI_BUILD && !ui->outputDevice) gui_start(ui,GUI_BUILD_IMAGE); else if (ui->mode == GUI_READ) gui_start(ui,GUI_READ_DISC); else if (ui->mode == GUI_WRITE) gui_start(ui,GUI_WRITE_DISC); else if (ui->mode == GUI_VERIFY) gui_start(ui,GUI_VERIFY_DISC); else if (ui->mode == GUI_DISCOVERY) gui_start(ui,GUI_DISCOVERY_DISC); break;
            case UI_OUTPUT_IMAGE: ui->outputDevice = false; gui_mode(ui,GUI_BUILD); break; case UI_OUTPUT_DEVICE: ui->outputDevice = true; gui_mode(ui,GUI_BUILD); break;
            case UI_REFRESH: case UI_SEARCH: gui_refresh(ui); break;
            case UI_DEVICE_REFRESH: case UI_DEVICE_CAPABILITIES: if (ui->mode == GUI_READ && command == UI_DEVICE_REFRESH) { ui->readDestinationReady = ui->readDestinationAttempted = false; gui_read_destination_info(ui); } if (ui->selectedDrive >= 0 && ui->selectedDrive < ui->driveCount) gui_start(ui,GUI_DRIVE_INFO); else if (command == UI_DEVICE_REFRESH) gui_read_destination_query(ui); break;
            case UI_DEVICE_ERASE_QUICK: gui_start(ui,GUI_ERASE_QUICK); break; case UI_DEVICE_ERASE_FULL: gui_start(ui,GUI_ERASE_FULL); break;
            case UI_DEVICE_SECTORS: imgburn_gui_sector_show(window,ui->instance,ui->devices,ui->driveCount,ui->selectedDrive,&ui->assets); break;
            case UI_READ_SECTOR_VIEWER: if (ui->mediaReady) imgburn_gui_sector_show(window,ui->instance,ui->devices,ui->driveCount,ui->selectedDrive,&ui->assets); break;
            case UI_READ_IMAGE_INFO: if (ui->mediaReady && ui->readDiscText[0]) MessageBoxW(window,ui->readDiscText,L"光盘信息",MB_OK | MB_ICONINFORMATION); break;
            case UI_TOOL_CUE: gui_tool_open(ui,GUI_CREATE_CUE); break; case UI_TOOL_MDS: gui_tool_open(ui,GUI_CREATE_MDS); break;
            case UI_WRITE_OPTIONS: gui_tool_open(ui,GUI_WRITE_OPTIONS); break;
            case UI_READ_OPTIONS: gui_tool_open(ui,GUI_READ_OPTIONS); break;
            case UI_WRITE_OPEN: case UI_READ_OPEN: {
                wchar_t folder[MAX_PATH]; DWORD count = GetFullPathNameW(command == UI_READ_OPEN ? ui->readImage : ui->sourceImage,MAX_PATH,folder,NULL); if (count && count < MAX_PATH) { wchar_t* slash = wcsrchr(folder,L'\\'); if (slash) { if (slash == folder + 2 && folder[1] == L':') slash[1] = 0; else *slash = 0; ShellExecuteW(window,L"open",folder,NULL,NULL,SW_SHOWNORMAL); } } break;
            }
            case UI_DVD: if (ui->bd && SendMessageW(ui->dvd,BM_GETCHECK,0,0) == BST_CHECKED) SendMessageW(ui->bd,BM_SETCHECK,BST_UNCHECKED,0); break;
            case UI_BD: if (ui->dvd && SendMessageW(ui->bd,BM_GETCHECK,0,0) == BST_CHECKED) SendMessageW(ui->dvd,BM_SETCHECK,BST_UNCHECKED,0); break;
            case UI_IMAGE_INFO: if (ui->sourceImage[0]) { ui->sourceInspectPopup = true; gui_start(ui,GUI_INSPECT); if (!ui->job || ui->job->operation != GUI_INSPECT) ui->sourceInspectPopup = false; } break;
            case UI_DVD_CANDIDATES: gui_dvd_candidates_open(ui); break;
            case UI_CD_ISO_OPEN: gui_cd_iso_open(ui); break;
            case UI_WRITE_LAYER: if (HIWORD(wp) == EN_CHANGE && ui->writeLayer && !ui->updatingWriteLayer) { wchar_t value[16]; GetWindowTextW(ui->writeLayer,value,16); gui_dvd_candidates_clear(ui,false); wcscpy(ui->writeLayerValue,value); } break;
            case UI_DRIVE: if (HIWORD(wp) == CBN_SELCHANGE) { gui_cd_plan_clear(ui); gui_read_controls(ui); ui->mediaReady = ui->writeMediaReady = ui->eraseSupported = ui->testWriteSupported = false; ui->discoveryCapacity = 0; if (ui->mode == GUI_READ) { ui->readDiscText[0] = 0; SetWindowTextW(ui->info,L""); } gui_menu(ui); if (ui->selectedDrive >= 0) gui_start(ui,GUI_DRIVE_INFO); } break;
            case UI_HELP_GUIDES: ShellExecuteW(window,L"open",L"https://forum.imgburn.com/forum/4-guides/",NULL,NULL,SW_SHOWNORMAL); break;
            case UI_HELP_SITE: ShellExecuteW(window,L"open",L"https://www.imgburn.com/",NULL,NULL,SW_SHOWNORMAL); break;
            case UI_HELP_FORUM: ShellExecuteW(window,L"open",L"https://forum.imgburn.com/",NULL,NULL,SW_SHOWNORMAL); break;
            case UI_ABOUT: MessageBoxW(window,L"ImgBurn\r\nC17 / Win32 复刻项目\r\n\r\n桌面功能正在逐项完善。",L"关于 ImgBurn",MB_OK | MB_ICONINFORMATION); break;
            case UI_EXIT: SendMessageW(window,WM_CLOSE,0,0); break;
        } return 0;
    }
    if (message == WM_CLOSE) { if (ui->job) { MessageBoxW(window,L"操作正在进行。请先取消或等待其完成。",L"ImgBurn",MB_OK | MB_ICONINFORMATION); return 0; } if (ui->tool) { SetForegroundWindow(ui->tool->window); return 0; } DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { if (ui->logWindow) DestroyWindow(ui->logWindow); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window,message,wp,lp);
}

int imgburn_gui_show(HINSTANCE instance, int show) {
    GuiWindow* ui = (GuiWindow*)calloc(1,sizeof(*ui)); if (!ui) return 1;
    ui->dvdSelected = SIZE_MAX;
    ui->instance = instance; ui->dpi = 96; ui->fsTypes = FS_TYPE_ALL; ui->revision = UDF_REV_102; ui->deduplicate = true; ui->verifyAfter = true; ui->verifyAgainstImage = true; ui->selectedDrive = -1; ui->discoveryAll = true; wcscpy(ui->discoveryCount,L"2295000"); wcscpy(ui->discoveryRate,L"MAX"); wcscpy(ui->volumeLabel,L"IMGBURN_DISC"); wcscpy(ui->readSplitText,L"Auto"); wcscpy(ui->readRate,L"MAX"); wcscpy(ui->readAudioRate,L"MAX"); wcscpy(ui->readRetryText,L"20");
    HDC screen = GetDC(NULL); if (screen) { ui->dpi = (unsigned)GetDeviceCaps(screen,LOGPIXELSX); ReleaseDC(NULL,screen); }
    gui_fonts(ui);
    HRESULT comResult = CoInitializeEx(NULL,COINIT_APARTMENTTHREADED); imgburn_gui_assets_load(&ui->assets,NULL); INITCOMMONCONTROLSEX controls = {sizeof(controls),ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW cls = {0}; cls.hInstance = instance; cls.hCursor = LoadCursorW(NULL,MAKEINTRESOURCEW(32512)); cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); cls.lpszClassName = L"ImgBurnMainWindow"; cls.lpfnWndProc = gui_proc; cls.hIcon = LoadIconW(instance,MAKEINTRESOURCEW(1)); if (!cls.hIcon) cls.hIcon = LoadIconW(NULL,MAKEINTRESOURCEW(32512)); bool ok = RegisterClassW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    cls.lpszClassName = L"ImgBurnCommand"; cls.lpfnWndProc = gui_tool_proc; ok = ok && (RegisterClassW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS); cls.lpszClassName = L"ImgBurnLogWindow"; cls.lpfnWndProc = gui_log_proc; ok = ok && (RegisterClassW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    RECT rect = {0,0,px(ui,GUI_MAIN_CONTENT_WIDTH),px(ui,GUI_MAIN_CONTENT_HEIGHT)}; DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX; AdjustWindowRect(&rect,style,TRUE);
    HWND window = ok ? CreateWindowW(L"ImgBurnMainWindow",L"ImgBurn",style,CW_USEDEFAULT,CW_USEDEFAULT,rect.right - rect.left,rect.bottom - rect.top,NULL,NULL,instance,ui) : NULL;
    if (window) {
        gui_main_resize(ui);
        if (ui->assets.largeIcon) SendMessageW(window,WM_SETICON,ICON_BIG,(LPARAM)ui->assets.largeIcon); if (ui->assets.smallIcon) SendMessageW(window,WM_SETICON,ICON_SMALL,(LPARAM)ui->assets.smallIcon);
        RECT main; GetWindowRect(window,&main); RECT log = {0,0,px(ui,GUI_LOG_CLIENT_WIDTH),px(ui,GUI_LOG_CLIENT_HEIGHT)}; AdjustWindowRect(&log,WS_OVERLAPPEDWINDOW,TRUE); ui->logWindow = CreateWindowW(L"ImgBurnLogWindow",L"ImgBurn 日志",WS_OVERLAPPEDWINDOW,main.left,main.bottom,log.right - log.left,log.bottom - log.top,window,NULL,instance,ui);
        ui->logVisible = true; ShowWindow(window,show); ShowWindow(ui->logWindow,SW_SHOWNOACTIVATE); gui_log(ui,L"ImgBurn C17 桌面界面已启动。"); gui_refresh(ui);
        ACCEL keys[] = {{FVIRTKEY | FCONTROL | FSHIFT,'P',UI_MODE_EZ},{FVIRTKEY | FCONTROL | FSHIFT,'R',UI_MODE_READ},{FVIRTKEY | FCONTROL | FSHIFT,'B',UI_MODE_BUILD},{FVIRTKEY | FCONTROL | FSHIFT,'W',UI_MODE_WRITE},{FVIRTKEY | FCONTROL | FSHIFT,'V',UI_MODE_VERIFY},{FVIRTKEY | FCONTROL | FSHIFT,'D',UI_MODE_DISCOVERY},{FVIRTKEY | FCONTROL,'O',UI_SOURCE_FILE},{FVIRTKEY,VK_F5,UI_REFRESH}};
        HACCEL accel = CreateAcceleratorTableW(keys,(int)(sizeof(keys) / sizeof(*keys))); MSG msg;
        while (GetMessageW(&msg,NULL,0,0) > 0) { if (!ui->tool && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == ui->sourceEdit && ui->mode == GUI_BUILD) { wchar_t path[MAX_PATH]; GetWindowTextW(ui->sourceEdit,path,MAX_PATH); if (gui_paths_add(&ui->paths,&ui->pathCount,path)) gui_mode(ui,GUI_BUILD); else MessageBoxW(window,L"无法添加该路径。",L"ImgBurn",MB_OK | MB_ICONERROR); continue; } if (ui->tool && IsDialogMessageW(ui->tool->window,&msg)) continue; if ((ui->tool || !TranslateAcceleratorW(window,accel,&msg)) && !IsDialogMessageW(window,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
        if (accel) DestroyAcceleratorTable(accel);
    }
    gui_dvd_candidates_clear(ui,false); gui_cd_plan_clear(ui); free(ui->paths); imgburn_gui_assets_free(&ui->assets); if (ui->font) DeleteObject(ui->font); if (ui->bold) DeleteObject(ui->bold); if (SUCCEEDED(comResult)) CoUninitialize(); free(ui); return window ? 0 : 1;
}
