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
    UI_TOOL_LAYER, UI_TOOL_LBA, UI_TOOL_OTP, UI_TOOL_PTP, UI_TOOL_OK, UI_TOOL_CLOSE, UI_IMAGE_INFO, UI_REMOVE_SOURCE, UI_VERIFY_COMPARE,
    UI_EZ_FIRST = 1100
};

typedef struct GuiWindow GuiWindow;
typedef struct {
    GuiWindow* parent;
    HWND window, list, layer, lba, otp, ptp, ok;
    GuiOperation operation;
    GuiPath* sources;
    size_t count;
} GuiTool;

struct GuiWindow {
    HINSTANCE instance;
    HWND window, status, logWindow, logList, sources, sourceEdit, destination, drives, info, progress, go, cancel, tabs;
    HWND fs, udf, label, dvd, bd, optimize, verify, compare;
    HFONT font, bold;
    GuiMode mode;
    unsigned dpi;
    bool outputDevice, logVisible, mediaReady;
    GuiPath* paths;
    size_t pathCount;
    wchar_t sourceImage[MAX_PATH], destinationImage[MAX_PATH], readImage[MAX_PATH], volumeLabel[127];
    FsTypeFlags fsTypes;
    UdfRevision revision;
    bool dvdVideo, bluray, deduplicate, verifyAfter, verifyAgainstImage;
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

static int px(const GuiWindow* ui, int value) { return MulDiv(value, (int)ui->dpi, 96); }
static GuiAssetId gui_control_asset(UINT id) { return id == UI_SOURCE_FILE || id == UI_DESTINATION ? ASSET_BROWSE : id == UI_SOURCE_FOLDER ? ASSET_FOLDER : id == UI_REMOVE_SOURCE ? ASSET_REMOVE : id == UI_IMAGE_INFO ? ASSET_INFO : ASSET_COUNT; }
static HWND ctl(GuiWindow* ui, HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
    GuiAssetId asset = gui_control_asset((UINT)id); if (!wcscmp(cls,L"BUTTON") && asset != ASSET_COUNT && ui->assets.images[asset]) style |= BS_OWNERDRAW;
    HWND child = CreateWindowExW(!wcscmp(cls, L"EDIT") || !wcscmp(cls, L"LISTBOX") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, px(ui,x), px(ui,y), px(ui,w), px(ui,h), parent, (HMENU)(INT_PTR)id, ui->instance, NULL);
    if (child) { SendMessageW(child, WM_SETFONT, (WPARAM)ui->font, TRUE); SetWindowPos(child,HWND_TOP,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); } return child;
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
static const wchar_t readFilter[] = L"数据镜像 (*.iso)\0*.iso\0原始 CD 镜像 (*.cue)\0*.cue\0\0";
static UINT_PTR CALLBACK gui_read_file_hook(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    (void)wp;
    if (message == WM_NOTIFY) { const OFNOTIFYW* notice = (const OFNOTIFYW*)lp; if (notice->hdr.code == CDN_TYPECHANGE || notice->hdr.code == CDN_INITDONE) SendMessageW(GetParent(window),CDM_SETDEFEXT,0,(LPARAM)(notice->lpOFN->nFilterIndex == 2 ? L"cue" : L"iso")); }
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
    if (ui->destination) GetWindowTextW(ui->destination, ui->mode == GUI_VERIFY ? ui->sourceImage : ui->mode == GUI_READ ? ui->readImage : ui->destinationImage, MAX_PATH);
    if (ui->label) GetWindowTextW(ui->label, ui->volumeLabel, 127);
    if (ui->fs) { static const FsTypeFlags flags[] = {FS_TYPE_ISO9660,FS_TYPE_ISO_JOLIET,FS_TYPE_ISO_UDF,FS_TYPE_ALL,FS_TYPE_UDF}; LRESULT i = SendMessageW(ui->fs, CB_GETCURSEL, 0, 0); if (i >= 0 && i < 5) ui->fsTypes = flags[i]; }
    if (ui->udf) ui->revision = SendMessageW(ui->udf, CB_GETCURSEL, 0, 0) == 1 ? UDF_REV_250 : UDF_REV_102;
    if (ui->dvd) ui->dvdVideo = SendMessageW(ui->dvd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ui->bd) ui->bluray = SendMessageW(ui->bd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ui->optimize) ui->deduplicate = SendMessageW(ui->optimize, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ui->verify) ui->verifyAfter = SendMessageW(ui->verify, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ui->compare) ui->verifyAgainstImage = SendMessageW(ui->compare, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (ui->drives) ui->selectedDrive = (int)SendMessageW(ui->drives, CB_GETCURSEL, 0, 0);
}

static void gui_menu(GuiWindow* ui) {
    bool build = ui->mode == GUI_BUILD, write = ui->mode == GUI_WRITE, read = ui->mode == GUI_READ, verify = ui->mode == GUI_VERIFY;
    bool busy = ui->job != NULL, idle = !busy, source = ui->pathCount != 0, drive = ui->selectedDrive >= 0 && ui->selectedDrive < ui->driveCount;
    bool discAction = idle && drive && ui->mediaReady && ((verify && !ui->verifyAgainstImage) || ui->sourceImage[0]);
    bool readAction = idle && drive && ui->mediaReady;
    if (verify) { EnableWindow(ui->compare,idle); EnableWindow(ui->destination,idle && ui->verifyAgainstImage); EnableWindow(GetDlgItem(ui->window,UI_SOURCE_FILE),idle && ui->verifyAgainstImage); }
    if (ui->go) EnableWindow(ui->go, build ? idle && source && !ui->outputDevice : read ? readAction : (write || verify) && discAction);
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu(), mode = CreatePopupMenu(), tools = CreatePopupMenu(), help = CreatePopupMenu();
    if (build || write || verify) item(file, UI_SOURCE_FILE, build ? L"浏览文件(&M)..." : L"浏览来源文件(&M)...\tCtrl+O", idle && (!verify || ui->verifyAgainstImage));
    if (build) { item(file, UI_SOURCE_FOLDER, L"浏览文件夹(&N)...", idle); item(file, UI_CLEAR, L"移除所有的项目(&O)", idle && source); separator(file); }
    if (build || read) { item(file, UI_DESTINATION, L"打开目标文件(&P)...", idle && !ui->outputDevice); separator(file); }
    if (build) { item(file, UI_CALCULATE, L"计算(&Q)", idle && source); item(file, UI_BUILD, L"构建(&R)", idle && source && !ui->outputDevice); separator(file); item(file, UI_NEW, L"新方案(&S)", idle); item(file, UI_LOAD_RECENT, L"载入最近的方案(&T)", false); item(file, UI_LOAD, L"载入方案(&U)...", false); item(file, UI_SAVE, L"储存方案(&V)...", false); separator(file); }
    if (read) { item(file, UI_READ, L"读取(&R)", readAction); separator(file); }
    if (write || ui->mode == GUI_DISCOVERY) { item(file, UI_WRITE, L"刻录(&R)", write && discAction); separator(file); }
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
    item(erase, UI_DEVICE_ERASE_QUICK, L"快速(&Q)", false); item(erase, UI_DEVICE_ERASE_FULL, L"完整(&F)", false); popup(device, erase, L"擦除光盘(&E)"); item(device, UI_DEVICE_SYNC, L"同步缓存(&S)", false);
    item(close, UI_DEVICE_CLOSE_TRACK, L"轨道(&T)", false); item(close, UI_DEVICE_CLOSE_SESSION, L"区段(&S)", false); item(close, UI_DEVICE_CLOSE_DISC, L"光盘(&D)", false); popup(device, close, L"关闭(&C)"); separator(device);
    item(device, UI_DEVICE_ADVANCED, L"更改高级设置(&A)...", false); item(device, UI_DEVICE_BOOKTYPE, L"更改 Book Type(&B)...", false); item(device, UI_DEVICE_SECTORS, L"扇区查看器(&V)...", idle && drive); item(device, UI_DEVICE_REGION, L"地区码(&R)...", false); item(device, UI_DEVICE_CAPABILITIES, L"功能(&C)...", idle && drive); item(device, UI_DEVICE_FAMILY, L"设备关系(&F)...", false); item(device, UI_DEVICE_FIRMWARE, L"检查固件更新(&U)...", false); popup(tools, device, L"驱动器(&V)");
    HMENU changer = CreatePopupMenu(); item(changer, 180, L"状态", false); item(changer, 181, L"装载", false); item(changer, 182, L"卸载", false); popup(tools, changer, L"换盘器(&W)"); separator(tools);
    item(tools, UI_TOOL_CUE, L"生成 CUE 文件(&C)...", idle); item(tools, UI_TOOL_DVD, L"创建 DVD 文件(&D)...", false); item(tools, UI_TOOL_MDS, L"生成 MDS 文件(&M)...", idle); separator(tools); item(tools, UI_SEARCH, L"寻找 SCSI / ATAPI 设备(&S)", idle); item(tools, 151, L"自动判断刻录速度(&X)...", false); item(tools, 152, L"驱动程序载入规则(&Y)...", false); item(tools, 153, L"重置 DMA(&A)", false); separator(tools); item(tools, UI_SETTINGS, L"选项(&Z)...", false);
    item(help, UI_HELP_GUIDES, L"查看 ImgBurn 使用指南(&G)", true); item(help, UI_HELP_SITE, L"访问 ImgBurn 网站(&W)", true); item(help, UI_HELP_FORUM, L"访问 ImgBurn 支持论坛(&F)", true); separator(help); item(help, UI_ABOUT, L"关于 ImgBurn(&A)...", true);
    popup(bar, tools, L"工具(&T)"); popup(bar, help, L"帮助(&H)"); HMENU old = GetMenu(ui->window); SetMenu(ui->window, bar); if (old) DestroyMenu(old); DrawMenuBar(ui->window);
}

static void gui_mode(GuiWindow* ui, GuiMode mode) {
    gui_read_controls(ui); HWND child = GetWindow(ui->window, GW_CHILD);
    while (child) { HWND next = GetWindow(child, GW_HWNDNEXT); if (child != ui->status) DestroyWindow(child); child = next; }
    ui->sources = ui->sourceEdit = ui->destination = ui->drives = ui->info = ui->progress = ui->go = ui->cancel = ui->tabs = NULL;
    ui->fs = ui->udf = ui->label = ui->dvd = ui->bd = ui->optimize = ui->verify = ui->compare = NULL; ui->mode = mode;
    if (mode == GUI_EZ) {
        ctl(ui, ui->window, L"BUTTON", L"您打算做什么？", BS_GROUPBOX, 0, 8, 8, 644, 364);
        const wchar_t* texts[] = {L"刻录镜像至光盘",L"刻录文件/文件夹至光盘",L"将光盘抓取成镜像文件",L"从文件/文件夹制作镜像",L"验证光盘",L"品质测试"};
        for (int i = 0; i < 6; i++) ctl(ui, ui->window, L"BUTTON", texts[i], WS_TABSTOP | BS_OWNERDRAW, UI_EZ_FIRST + i, 22 + i % 2 * 320, 30 + i / 2 * 112, 296, 104);
        gui_status(ui, L"Ready"); gui_menu(ui); return;
    }
    bool build = mode == GUI_BUILD; bool imageDestination = build && !ui->outputDevice;
    ctl(ui, ui->window, L"BUTTON", build ? L"来源" : mode == GUI_WRITE ? L"来源" : L"来源 / 光盘", BS_GROUPBOX, 0, 8, 8, 320, build ? 188 : 151);
    if (build) {
        ui->sourceEdit = ctl(ui, ui->window, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, UI_SOURCE_EDIT, 16, 25, 280, 23);
        ui->sources = ctl(ui, ui->window, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT, UI_SOURCE_LIST, 16, 50, 280, 132);
        for (size_t i = 0; i < ui->pathCount; i++) SendMessageW(ui->sources, LB_ADDSTRING, 0, (LPARAM)ui->paths[i].path);
        ctl(ui, ui->window, L"BUTTON", L"+", WS_TABSTOP, UI_SOURCE_FILE, 300, 25, 23, 23); ctl(ui, ui->window, L"BUTTON", L"…", WS_TABSTOP, UI_SOURCE_FOLDER, 300, 52, 23, 23); ctl(ui, ui->window, L"BUTTON", L"×", WS_TABSTOP, UI_REMOVE_SOURCE, 300, 80, 23, 23);
    } else if (mode == GUI_READ || mode == GUI_VERIFY) {
        ui->drives = ctl(ui, ui->window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, UI_DRIVE, 16, 28, 302, 180);
        ctl(ui, ui->window, L"STATIC", mode == GUI_READ ? L"读取为 ISO 或原始 CD 的 CUE/BIN" : L"检查光盘可读性，可选与镜像比较。\r\n音频轨默认只检查可读性。", 0, 0, 18, 64, 290, 55);
        if (mode == GUI_VERIFY) { ui->compare = ctl(ui,ui->window,L"BUTTON",L"与镜像文件比较",WS_TABSTOP | BS_AUTOCHECKBOX,UI_VERIFY_COMPARE,18,122,280,22); SendMessageW(ui->compare,BM_SETCHECK,ui->verifyAgainstImage ? BST_CHECKED : BST_UNCHECKED,0); }
    } else {
        ui->sourceEdit = ctl(ui, ui->window, L"EDIT", ui->sourceImage, WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY, UI_SOURCE_EDIT, 16, 28, 276, 24);
        ctl(ui, ui->window, L"BUTTON", L"…", WS_TABSTOP, UI_SOURCE_FILE, 297, 28, 23, 24);
        ctl(ui, ui->window, L"BUTTON", L"镜像信息", WS_TABSTOP, UI_IMAGE_INFO, 16, 68, 90, 26);
    }
    int targetY = build ? 205 : 170; ctl(ui, ui->window, L"BUTTON", imageDestination || mode == GUI_READ ? L"目标" : mode == GUI_VERIFY ? L"镜像文件" : L"目标驱动器", BS_GROUPBOX, 0, 8, targetY, 320, 67);
    if (imageDestination || mode == GUI_READ || mode == GUI_VERIFY) {
        ui->destination = ctl(ui, ui->window, L"EDIT", mode == GUI_VERIFY ? ui->sourceImage : mode == GUI_READ ? ui->readImage : ui->destinationImage, WS_TABSTOP | ES_AUTOHSCROLL, UI_DEST_EDIT, 16, targetY + 24, 276, 24);
        ctl(ui, ui->window, L"BUTTON", L"…", WS_TABSTOP, mode == GUI_VERIFY ? UI_SOURCE_FILE : UI_DESTINATION, 297, targetY + 24, 23, 24);
    } else ui->drives = ctl(ui, ui->window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, UI_DRIVE, 16, targetY + 24, 302, 160);
    ui->info = ctl(ui, ui->window, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 344, 38, 298, 282);
    if (build) {
        ui->tabs = ctl(ui, ui->window, WC_TABCONTROLW, L"", WS_TABSTOP, UI_TAB, 336, 8, 316, 359);
        const wchar_t* pages[] = {L"信息",L"选项",L"标签",L"高级"}; for (int i = 0; i < 4; i++) { TCITEMW tab = {0}; tab.mask = TCIF_TEXT; tab.pszText = (wchar_t*)pages[i]; TabCtrl_InsertItem(ui->tabs, i, &tab); }
        SetWindowPos(ui->tabs, HWND_BOTTOM, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ctl(ui, ui->window, L"BUTTON", L"计算", WS_TABSTOP, UI_CALCULATE, 450, 326, 90, 30);
    }
    if (mode == GUI_WRITE) { ui->verify = ctl(ui, ui->window, L"BUTTON", L"验证", WS_TABSTOP | BS_AUTOCHECKBOX, UI_VERIFY_CHECK, 110, 246, 65, 22); SendMessageW(ui->verify, BM_SETCHECK, ui->verifyAfter ? BST_CHECKED : BST_UNCHECKED, 0); HWND test = ctl(ui, ui->window, L"BUTTON", L"测试模式", BS_AUTOCHECKBOX, UI_TEST, 16, 246, 85, 22); EnableWindow(test, FALSE); }
    const wchar_t* start = build ? L"构建" : mode == GUI_READ ? L"读取" : mode == GUI_VERIFY ? L"验证" : mode == GUI_DISCOVERY ? L"品质测试" : L"刻录";
    ui->go = ctl(ui, ui->window, L"BUTTON", start, WS_TABSTOP | BS_DEFPUSHBUTTON, UI_GO, 98, 292, 140, 58);
    ui->cancel = ctl(ui, ui->window, L"BUTTON", L"取消", WS_TABSTOP, UI_CANCEL, 246, 328, 70, 24); EnableWindow(ui->cancel, FALSE);
    ui->progress = ctl(ui, ui->window, PROGRESS_CLASSW, L"", 0, 0, 16, 359, 308, 11);
    EnableWindow(ui->go, mode != GUI_READ && mode != GUI_DISCOVERY && !(build && ui->outputDevice));
    if (ui->drives) for (int i = 0; i < ui->driveCount; i++) { wchar_t text[128]; swprintf(text, 128, L"%c:  %hs %hs", ui->devices[i].driveLetter ? ui->devices[i].driveLetter : L'?', ui->devices[i].vendorId, ui->devices[i].productId); SendMessageW(ui->drives, CB_ADDSTRING, 0, (LPARAM)text); }
    if (ui->drives) SendMessageW(ui->drives, CB_SETCURSEL, ui->selectedDrive, 0);
    gui_status(ui, mode == GUI_DISCOVERY ? L"品质测试流程尚未接通" : build && ui->outputDevice ? L"直接构建到光盘尚未接通；可选择输出镜像文件" : L"Ready"); gui_menu(ui);
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
    ui->driveCount = spti_enumerate_drives(ui->devices, MAX_OPTICAL_DRIVES); ui->selectedDrive = ui->driveCount > 0 ? 0 : -1; ui->mediaReady = false;
    if (ui->drives) { SendMessageW(ui->drives, CB_RESETCONTENT, 0, 0); for (int i = 0; i < ui->driveCount; i++) { wchar_t text[128]; swprintf(text, 128, L"%c:  %hs %hs", ui->devices[i].driveLetter ? ui->devices[i].driveLetter : L'?', ui->devices[i].vendorId, ui->devices[i].productId); SendMessageW(ui->drives, CB_ADDSTRING, 0, (LPARAM)text); } SendMessageW(ui->drives, CB_SETCURSEL, ui->selectedDrive, 0); }
    wchar_t text[80]; swprintf(text, 80, L"找到 %d 个光驱。", ui->driveCount); gui_log(ui, text); gui_menu(ui);
    if (ui->drives && ui->selectedDrive >= 0) gui_start(ui,GUI_DRIVE_INFO);
}

static void gui_source(GuiWindow* ui, bool folder) {
    if (ui->mode == GUI_VERIFY && !ui->verifyAgainstImage) return;
    wchar_t path[MAX_PATH] = L""; bool build = ui->mode == GUI_BUILD;
    if (!(folder ? gui_folder(ui->window, path) : gui_file(ui->window, path, false, build ? L"所有文件\0*.*\0\0" : imageFilter, NULL))) return;
    if (build) { if (!gui_paths_add(&ui->paths, &ui->pathCount, path)) MessageBoxW(ui->window, L"无法添加该路径。", L"ImgBurn", MB_OK | MB_ICONERROR); gui_mode(ui, ui->mode); }
    else { wcscpy(ui->sourceImage, path); if (ui->sourceEdit) SetWindowTextW(ui->sourceEdit, path); if (ui->mode == GUI_VERIFY && ui->destination) SetWindowTextW(ui->destination, path); gui_start(ui, GUI_INSPECT); }
}

static GuiJob* gui_job(GuiWindow* ui, GuiOperation operation, const GuiPath* paths, size_t count) {
    GuiJob* job = (GuiJob*)calloc(1, sizeof(*job)); if (!job) return NULL;
    job->sources = (GuiPath*)calloc(count, sizeof(*paths)); job->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!job->sources || !job->cancel) { imgburn_gui_job_free(job); return NULL; }
    memcpy(job->sources, paths, count * sizeof(*paths)); job->sourceCount = count; job->owner = ui->window; job->operation = operation;
    wcscpy(job->destination, ui->mode == GUI_READ ? ui->readImage : ui->destinationImage); wcscpy(job->label, ui->volumeLabel); job->fsTypes = ui->fsTypes; job->revision = ui->revision; job->dvdVideo = ui->dvdVideo; job->bluray = ui->bluray; job->optimize = ui->deduplicate; job->verify = ui->verifyAfter; job->speed = 0xFFFF; job->oppositeTrackPath = true;
    if (ui->selectedDrive >= 0 && ui->selectedDrive < ui->driveCount) wcscpy(job->drive, ui->devices[ui->selectedDrive].devicePath); return job;
}
static bool gui_launch(GuiWindow* ui, GuiJob* job) {
    if (!job || ui->job) { imgburn_gui_job_free(job); return false; }
    ui->job = job; job->thread = CreateThread(NULL, 0, imgburn_gui_job_run, job, 0, NULL);
    if (!job->thread) { ui->job = NULL; imgburn_gui_job_free(job); return false; }
    if (ui->go) EnableWindow(ui->go, FALSE); if (ui->cancel) EnableWindow(ui->cancel, job->operation == GUI_CALCULATE || job->operation == GUI_BUILD_IMAGE || job->operation == GUI_CREATE_CUE || job->operation == GUI_READ_DISC || job->operation == GUI_VERIFY_DISC);
    if (job->operation == GUI_READ_DISC || job->operation == GUI_VERIFY_DISC) { EnableWindow(ui->destination,FALSE); EnableWindow(ui->drives,FALSE); }
    gui_status(ui, L"正在执行..."); gui_menu(ui); return true;
}

static void gui_start(GuiWindow* ui, GuiOperation operation) {
    if (ui->job) return; gui_read_controls(ui);
    if (ui->mode == GUI_VERIFY && ui->destination) GetWindowTextW(ui->destination, ui->sourceImage, MAX_PATH);
    GuiPath single = {0}; const GuiPath* sources = ui->paths; size_t count = ui->pathCount;
    if (operation == GUI_DRIVE_INFO || operation == GUI_READ_DISC || operation == GUI_VERIFY_DISC || operation == GUI_WRITE_DISC) { if (ui->selectedDrive < 0 || ui->selectedDrive >= ui->driveCount) return; }
    if (operation == GUI_INSPECT || operation == GUI_VERIFY_DISC || operation == GUI_WRITE_DISC || operation == GUI_DRIVE_INFO || operation == GUI_READ_DISC) { wcscpy(single.path, operation == GUI_DRIVE_INFO || operation == GUI_READ_DISC ? ui->devices[ui->selectedDrive].devicePath : ui->sourceImage); sources = &single; count = 1; }
    if (operation == GUI_VERIFY_DISC && !ui->verifyAgainstImage) single.path[0] = 0;
    if (!count || (!single.path[0] && sources == &single && (operation != GUI_VERIFY_DISC || ui->verifyAgainstImage))) { MessageBoxW(ui->window, L"请先选择来源。", L"ImgBurn", MB_OK | MB_ICONINFORMATION); return; }
    if (operation == GUI_BUILD_IMAGE && !ui->destinationImage[0]) {
        if (!gui_file(ui->window, ui->destinationImage, true, L"ISO 镜像 (*.iso)\0*.iso\0\0", L"iso")) return;
        if (ui->destination) SetWindowTextW(ui->destination, ui->destinationImage);
    } else if (operation == GUI_BUILD_IMAGE && GetFileAttributesW(ui->destinationImage) != INVALID_FILE_ATTRIBUTES && MessageBoxW(ui->window, L"目标文件已存在，是否替换？", L"ImgBurn", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (operation == GUI_WRITE_DISC && MessageBoxW(ui->window, L"将开始向所选光驱写入镜像，是否继续？", L"ImgBurn", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    bool replace = false;
    if (operation == GUI_READ_DISC) {
        if (!ui->readImage[0]) { if (!gui_file(ui->window,ui->readImage,true,readFilter,L"iso")) return; if (ui->destination) SetWindowTextW(ui->destination,ui->readImage); replace = GetFileAttributesW(ui->readImage) != INVALID_FILE_ATTRIBUTES; }
        else if (GetFileAttributesW(ui->readImage) != INVALID_FILE_ATTRIBUTES) { if (MessageBoxW(ui->window,L"目标文件已存在，是否替换？",L"ImgBurn",MB_YESNO | MB_ICONQUESTION) != IDYES) return; replace = true; }
    }
    GuiJob* job = gui_job(ui,operation,sources,count); if (job) job->replaceExisting = replace;
    if (!gui_launch(ui,job)) MessageBoxW(ui->window, L"无法启动操作。", L"ImgBurn", MB_OK | MB_ICONERROR);
}

static void gui_tool_open(GuiWindow* ui, GuiOperation operation) {
    if (ui->tool || ui->job) return;
    GuiTool* tool = (GuiTool*)calloc(1, sizeof(*tool)); if (!tool) return; tool->parent = ui; tool->operation = operation; ui->tool = tool;
    const wchar_t* title = operation == GUI_CREATE_CUE ? L"生成 CUE 文件" : L"创建 MDS 文件";
    RECT rect = {0,0,px(ui,520),px(ui,372)}; AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ImgBurnCommand", title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, ui->window, NULL, ui->instance, tool);
    if (!window) { ui->tool = NULL; free(tool); return; } EnableWindow(ui->window, FALSE); ShowWindow(window, SW_SHOW);
}

static void gui_tool_fill(GuiTool* tool) { SendMessageW(tool->list, LB_RESETCONTENT, 0, 0); for (size_t i = 0; i < tool->count; i++) SendMessageW(tool->list, LB_ADDSTRING, 0, (LPARAM)tool->sources[i].path); EnableWindow(tool->ok, tool->count != 0); }
static LRESULT CALLBACK gui_tool_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    GuiTool* tool = (GuiTool*)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) { tool = (GuiTool*)((CREATESTRUCTW*)lp)->lpCreateParams; tool->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)tool); }
    if (!tool) return DefWindowProcW(window, message, wp, lp); GuiWindow* ui = tool->parent;
    if (message == WM_CREATE) {
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
    if (message == WM_COMMAND) {
        if (ui->job && LOWORD(wp) != UI_TOOL_CLOSE) return 0;
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
    if (message == WM_CLOSE) { if (!ui->job) DestroyWindow(window); else { SetEvent(ui->job->cancel); EnableWindow(GetDlgItem(window,UI_TOOL_CLOSE),FALSE); SetWindowTextW(window,L"正在取消..."); } return 0; }
    if (message == WM_DESTROY) { EnableWindow(ui->window, TRUE); SetForegroundWindow(ui->window); ui->tool = NULL; free(tool->sources); free(tool); return 0; }
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
    unsigned asset = draw->CtlID - UI_EZ_FIRST; bool original = asset < 6 && imgburn_gui_asset_draw(&ui->assets,(GuiAssetId)asset,dc,(r.left + r.right - px(ui,128)) / 2,r.top + px(ui,12),px(ui,128),px(ui,48),!(draw->itemState & ODS_DISABLED));
    if (!original) {
    int center = (r.right + r.left) / 2, y = r.top + px(ui,12), size = px(ui,46); HPEN pen = CreatePen(PS_SOLID, px(ui,1), RGB(138,145,151)); HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(WHITE_BRUSH));
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
    wchar_t text[128]; GetWindowTextW(draw->hwndItem,text,128); RECT caption = {r.left,r.top + px(ui,70),r.right,r.bottom}; HGDIOBJ oldFont = SelectObject(dc,ui->bold); SetTextColor(dc,GetSysColor(COLOR_BTNTEXT)); DrawTextW(dc,text,-1,&caption,DT_CENTER | DT_VCENTER | DT_SINGLELINE); SelectObject(dc,oldFont); if (draw->itemState & ODS_FOCUS) { InflateRect(&r,-3,-3); DrawFocusRect(dc,&r); }
}

static void gui_complete(GuiWindow* ui, GuiJob* job) {
    if (ui->job != job) return; WaitForSingleObject(job->thread, INFINITE); ui->job = NULL;
    if (job->operation == GUI_DRIVE_INFO) ui->mediaReady = job->mediaReady;
    if (job->operation == GUI_READ_DISC || job->operation == GUI_VERIFY_DISC) { EnableWindow(ui->destination,TRUE); EnableWindow(ui->drives,TRUE); }
    if (ui->progress) SendMessageW(ui->progress, PBM_SETPOS, job->succeeded && job->operation != GUI_DRIVE_INFO ? 100 : 0, 0); if (ui->info) SetWindowTextW(ui->info,job->result); gui_log(ui,job->result);
    const wchar_t* completed = job->operation == GUI_DRIVE_INFO ? (job->mediaReady ? L"介质已就绪" : L"介质未就绪或未插入光盘") : job->operation == GUI_CALCULATE ? L"计算完成" : job->operation == GUI_INSPECT ? L"镜像信息已载入" : L"操作成功完成";
    gui_status(ui, job->succeeded ? completed : job->error == ERROR_CANCELLED ? L"操作已取消" : L"操作失败；请查看日志");
    if (ui->go) EnableWindow(ui->go, ui->mode != GUI_READ && ui->mode != GUI_DISCOVERY && !(ui->mode == GUI_BUILD && ui->outputDevice)); if (ui->cancel) EnableWindow(ui->cancel,FALSE);
    if (ui->tool) {
        if (job->succeeded || job->error == ERROR_CANCELLED) DestroyWindow(ui->tool->window);
        else { HWND toolWindow = ui->tool->window; for (HWND child = GetWindow(toolWindow,GW_CHILD); child; child = GetWindow(child,GW_HWNDNEXT)) EnableWindow(child,TRUE); SetWindowTextW(toolWindow,ui->tool->operation == GUI_CREATE_CUE ? L"生成 CUE 文件" : L"创建 MDS 文件"); if (ui->tool->layer) SendMessageW(toolWindow,WM_COMMAND,UI_TOOL_LAYER,0); MessageBoxW(toolWindow,job->result,L"ImgBurn",MB_OK | MB_ICONERROR); }
    }
    else if (!job->succeeded && job->error != ERROR_CANCELLED) MessageBoxW(ui->window,job->result,L"ImgBurn",MB_OK | MB_ICONERROR);
    imgburn_gui_job_free(job); gui_menu(ui);
}

static LRESULT CALLBACK gui_log_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    GuiWindow* ui = (GuiWindow*)GetWindowLongPtrW(window,GWLP_USERDATA);
    if (message == WM_NCCREATE) { ui = (GuiWindow*)((CREATESTRUCTW*)lp)->lpCreateParams; SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)ui); }
    if (!ui) return DefWindowProcW(window,message,wp,lp);
    if (message == WM_CREATE) {
        HMENU menu = CreateMenu(), file = CreatePopupMenu(), edit = CreatePopupMenu(); item(file,UI_LOG_SAVE,L"另存为(&S)...",true); item(file,UI_LOG,L"关闭(&C)",true); item(edit,UI_LOG_COPY,L"复制全部(&C)",true); popup(menu,file,L"文件(&F)"); popup(menu,edit,L"编辑(&E)"); SetMenu(window,menu);
        ui->logList = ctl(ui,window,L"LISTBOX",L"",WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT,0,0,0,660,150); return 0;
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
    if (message == WM_CREATE) { ui->status = CreateWindowW(STATUSCLASSNAMEW,L"Ready",WS_CHILD | WS_VISIBLE,0,0,0,0,window,NULL,ui->instance,NULL); gui_mode(ui,GUI_EZ); DragAcceptFiles(window,TRUE); return 0; }
    if (message == WM_SIZE) { SendMessageW(ui->status,WM_SIZE,0,0); return 0; }
    if (message == WM_DRAWITEM && wp >= UI_EZ_FIRST && wp < UI_EZ_FIRST + 6) { gui_ez_draw(ui,(DRAWITEMSTRUCT*)lp); return TRUE; }
    if (message == WM_DRAWITEM && gui_control_asset((UINT)wp) != ASSET_COUNT && imgburn_gui_asset_button(&ui->assets,gui_control_asset((UINT)wp),(DRAWITEMSTRUCT*)lp)) return TRUE;
    if (message == WM_NOTIFY && ((NMHDR*)lp)->idFrom == UI_TAB && ((NMHDR*)lp)->code == TCN_SELCHANGE) { gui_build_page(ui); return 0; }
    if (message == GUI_JOB_PROGRESS) { if (ui->job == (GuiJob*)lp && ui->progress) SendMessageW(ui->progress,PBM_SETPOS,wp,0); return 0; }
    if (message == GUI_JOB_DONE) { gui_complete(ui,(GuiJob*)lp); return 0; }
    if (message == WM_DROPFILES) {
        HDROP drop = (HDROP)wp; if (!ui->job && !ui->tool) { UINT count = DragQueryFileW(drop,0xFFFFFFFF,NULL,0); for (UINT i = 0; i < count; i++) { wchar_t path[MAX_PATH]; if (DragQueryFileW(drop,i,path,MAX_PATH) >= MAX_PATH) continue; if (ui->mode == GUI_BUILD) gui_paths_add(&ui->paths,&ui->pathCount,path); else { wcscpy(ui->sourceImage,path); break; } } gui_mode(ui,ui->mode); } DragFinish(drop); return 0;
    }
    if (message == WM_COMMAND) {
        UINT command = LOWORD(wp);
        if (command >= UI_EZ_FIRST && command < UI_EZ_FIRST + 6) { static const GuiMode modes[] = {GUI_WRITE,GUI_BUILD,GUI_READ,GUI_BUILD,GUI_VERIFY,GUI_DISCOVERY}; ui->outputDevice = command == UI_EZ_FIRST + 1; gui_mode(ui,modes[command - UI_EZ_FIRST]); return 0; }
        if (command == UI_LOG) { ui->logVisible = !ui->logVisible; ShowWindow(ui->logWindow,ui->logVisible ? SW_SHOWNOACTIVATE : SW_HIDE); gui_menu(ui); return 0; }
        if (ui->job) { if (command == UI_CANCEL) { SetEvent(ui->job->cancel); EnableWindow(ui->cancel,FALSE); gui_status(ui,L"正在取消..."); } return 0; }
        switch (command) {
            case UI_MODE_EZ: gui_mode(ui,GUI_EZ); break; case UI_MODE_READ: gui_mode(ui,GUI_READ); break; case UI_MODE_BUILD: gui_mode(ui,GUI_BUILD); break; case UI_MODE_WRITE: gui_mode(ui,GUI_WRITE); break; case UI_MODE_VERIFY: gui_mode(ui,GUI_VERIFY); break; case UI_MODE_DISCOVERY: gui_mode(ui,GUI_DISCOVERY); break;
            case UI_SOURCE_FILE: gui_source(ui,false); break; case UI_SOURCE_FOLDER: gui_source(ui,true); break;
            case UI_VERIFY_COMPARE: gui_read_controls(ui); gui_menu(ui); break;
            case UI_DEST_EDIT: if (HIWORD(wp) == EN_CHANGE && ui->mode == GUI_VERIFY && ui->destination) { gui_read_controls(ui); gui_menu(ui); } break;
            case UI_CLEAR: free(ui->paths); ui->paths = NULL; ui->pathCount = 0; gui_mode(ui,ui->mode); break;
            case UI_REMOVE_SOURCE: if (ui->sources) { for (size_t i = ui->pathCount; i > 0; i--) if (SendMessageW(ui->sources,LB_GETSEL,i - 1,0) > 0) { memmove(ui->paths + i - 1,ui->paths + i,(ui->pathCount - i) * sizeof(*ui->paths)); ui->pathCount--; } gui_mode(ui,GUI_BUILD); } break;
            case UI_NEW:
                gui_read_controls(ui); free(ui->paths); ui->paths = NULL; ui->pathCount = 0; ui->destinationImage[0] = 0; ui->fsTypes = FS_TYPE_ALL; ui->revision = UDF_REV_102; ui->dvdVideo = ui->bluray = false; ui->deduplicate = true; wcscpy(ui->volumeLabel,L"IMGBURN_DISC");
                ui->fs = ui->udf = ui->label = ui->dvd = ui->bd = ui->optimize = NULL; if (ui->destination) SetWindowTextW(ui->destination,L""); gui_mode(ui,GUI_BUILD); break;
            case UI_DESTINATION: { wchar_t path[MAX_PATH]; wchar_t* target = ui->mode == GUI_READ ? ui->readImage : ui->destinationImage; wcscpy(path,target); if (gui_file(window,path,true,ui->mode == GUI_READ ? readFilter : L"ISO 镜像\0*.iso\0\0",L"iso")) { wcscpy(target,path); if (ui->destination) SetWindowTextW(ui->destination,path); } break; }
            case UI_CALCULATE: gui_start(ui,GUI_CALCULATE); break;
            case UI_BUILD: case UI_READ: case UI_WRITE: case UI_VERIFY: case UI_GO:
                if (ui->mode == GUI_BUILD && !ui->outputDevice) gui_start(ui,GUI_BUILD_IMAGE); else if (ui->mode == GUI_READ) gui_start(ui,GUI_READ_DISC); else if (ui->mode == GUI_WRITE) gui_start(ui,GUI_WRITE_DISC); else if (ui->mode == GUI_VERIFY) gui_start(ui,GUI_VERIFY_DISC); break;
            case UI_OUTPUT_IMAGE: ui->outputDevice = false; gui_mode(ui,GUI_BUILD); break; case UI_OUTPUT_DEVICE: ui->outputDevice = true; gui_mode(ui,GUI_BUILD); break;
            case UI_REFRESH: case UI_SEARCH: gui_refresh(ui); break;
            case UI_DEVICE_REFRESH: case UI_DEVICE_CAPABILITIES: if (ui->selectedDrive >= 0 && ui->selectedDrive < ui->driveCount) gui_start(ui,GUI_DRIVE_INFO); break;
            case UI_DEVICE_SECTORS: imgburn_gui_sector_show(window,ui->instance,ui->devices,ui->driveCount,ui->selectedDrive,&ui->assets); break;
            case UI_TOOL_CUE: gui_tool_open(ui,GUI_CREATE_CUE); break; case UI_TOOL_MDS: gui_tool_open(ui,GUI_CREATE_MDS); break;
            case UI_DVD: if (ui->bd && SendMessageW(ui->dvd,BM_GETCHECK,0,0) == BST_CHECKED) SendMessageW(ui->bd,BM_SETCHECK,BST_UNCHECKED,0); break;
            case UI_BD: if (ui->dvd && SendMessageW(ui->bd,BM_GETCHECK,0,0) == BST_CHECKED) SendMessageW(ui->dvd,BM_SETCHECK,BST_UNCHECKED,0); break;
            case UI_IMAGE_INFO: if (ui->sourceImage[0]) gui_start(ui,GUI_INSPECT); break;
            case UI_DRIVE: if (HIWORD(wp) == CBN_SELCHANGE) { gui_read_controls(ui); ui->mediaReady = false; gui_menu(ui); if (ui->selectedDrive >= 0) gui_start(ui,GUI_DRIVE_INFO); } break;
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
    ui->instance = instance; ui->dpi = 96; ui->fsTypes = FS_TYPE_ALL; ui->revision = UDF_REV_102; ui->deduplicate = true; ui->verifyAfter = true; ui->verifyAgainstImage = true; ui->selectedDrive = -1; wcscpy(ui->volumeLabel,L"IMGBURN_DISC");
    HDC screen = GetDC(NULL); if (screen) { ui->dpi = (unsigned)GetDeviceCaps(screen,LOGPIXELSX); ReleaseDC(NULL,screen); }
    ui->font = CreateFontW(-MulDiv(9,(int)ui->dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI"); ui->bold = CreateFontW(-MulDiv(10,(int)ui->dpi,72),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HRESULT comResult = CoInitializeEx(NULL,COINIT_APARTMENTTHREADED); imgburn_gui_assets_load(&ui->assets,NULL); INITCOMMONCONTROLSEX controls = {sizeof(controls),ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW cls = {0}; cls.hInstance = instance; cls.hCursor = LoadCursorW(NULL,MAKEINTRESOURCEW(32512)); cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); cls.lpszClassName = L"ImgBurnMainWindow"; cls.lpfnWndProc = gui_proc; cls.hIcon = LoadIconW(instance,MAKEINTRESOURCEW(1)); if (!cls.hIcon) cls.hIcon = LoadIconW(NULL,MAKEINTRESOURCEW(32512)); bool ok = RegisterClassW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    cls.lpszClassName = L"ImgBurnCommand"; cls.lpfnWndProc = gui_tool_proc; ok = ok && (RegisterClassW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS); cls.lpszClassName = L"ImgBurnLogWindow"; cls.lpfnWndProc = gui_log_proc; ok = ok && (RegisterClassW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    RECT rect = {0,0,px(ui,660),px(ui,400)}; DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX; AdjustWindowRect(&rect,style,TRUE);
    HWND window = ok ? CreateWindowW(L"ImgBurnMainWindow",L"ImgBurn",style,CW_USEDEFAULT,CW_USEDEFAULT,rect.right - rect.left,rect.bottom - rect.top,NULL,NULL,instance,ui) : NULL;
    if (window) {
        if (ui->assets.largeIcon) SendMessageW(window,WM_SETICON,ICON_BIG,(LPARAM)ui->assets.largeIcon); if (ui->assets.smallIcon) SendMessageW(window,WM_SETICON,ICON_SMALL,(LPARAM)ui->assets.smallIcon);
        RECT main; GetWindowRect(window,&main); ui->logWindow = CreateWindowW(L"ImgBurnLogWindow",L"ImgBurn 日志",WS_OVERLAPPEDWINDOW,main.left,main.bottom,rect.right - rect.left,px(ui,180),window,NULL,instance,ui);
        ui->logVisible = true; ShowWindow(window,show); ShowWindow(ui->logWindow,SW_SHOWNOACTIVATE); gui_log(ui,L"ImgBurn C17 桌面界面已启动。"); gui_refresh(ui);
        ACCEL keys[] = {{FVIRTKEY | FCONTROL | FSHIFT,'P',UI_MODE_EZ},{FVIRTKEY | FCONTROL | FSHIFT,'R',UI_MODE_READ},{FVIRTKEY | FCONTROL | FSHIFT,'B',UI_MODE_BUILD},{FVIRTKEY | FCONTROL | FSHIFT,'W',UI_MODE_WRITE},{FVIRTKEY | FCONTROL | FSHIFT,'V',UI_MODE_VERIFY},{FVIRTKEY | FCONTROL | FSHIFT,'D',UI_MODE_DISCOVERY},{FVIRTKEY | FCONTROL,'O',UI_SOURCE_FILE},{FVIRTKEY,VK_F5,UI_REFRESH}};
        HACCEL accel = CreateAcceleratorTableW(keys,(int)(sizeof(keys) / sizeof(*keys))); MSG msg;
        while (GetMessageW(&msg,NULL,0,0) > 0) { if (!ui->tool && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == ui->sourceEdit && ui->mode == GUI_BUILD) { wchar_t path[MAX_PATH]; GetWindowTextW(ui->sourceEdit,path,MAX_PATH); if (gui_paths_add(&ui->paths,&ui->pathCount,path)) gui_mode(ui,GUI_BUILD); else MessageBoxW(window,L"无法添加该路径。",L"ImgBurn",MB_OK | MB_ICONERROR); continue; } if (ui->tool && IsDialogMessageW(ui->tool->window,&msg)) continue; if ((ui->tool || !TranslateAcceleratorW(window,accel,&msg)) && !IsDialogMessageW(window,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
        if (accel) DestroyAcceleratorTable(accel);
    }
    free(ui->paths); imgburn_gui_assets_free(&ui->assets); if (ui->font) DeleteObject(ui->font); if (ui->bold) DeleteObject(ui->bold); if (SUCCEEDED(comResult)) CoUninitialize(); free(ui); return window ? 0 : 1;
}
