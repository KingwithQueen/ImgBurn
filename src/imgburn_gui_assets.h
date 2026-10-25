#ifndef IMGBURN_GUI_ASSETS_H
#define IMGBURN_GUI_ASSETS_H
#include <windows.h>
#include <stdbool.h>

typedef enum {
    ASSET_WRITE, ASSET_BUILD_DEVICE, ASSET_READ, ASSET_BUILD_IMAGE, ASSET_VERIFY, ASSET_DISCOVERY, ASSET_BROWSE, ASSET_FOLDER, ASSET_REMOVE, ASSET_INFO,
    ASSET_SECTOR_PREVIOUS, ASSET_SECTOR_NEXT, ASSET_SECTOR_REFRESH, ASSET_SECTOR_LAST,
    ASSET_WRITE_QUEUE, ASSET_OPEN_FOLDER, ASSET_CREATE_CUE, ASSET_DEVICE_LOAD, ASSET_DEVICE_EJECT, ASSET_DEVICE_ERASE, ASSET_DEVICE_CHANGER,
    ASSET_BOOKTYPE, ASSET_ADVANCED, ASSET_AWS, ASSET_GRAPH_DISPLAY, ASSET_GRAPH_EXPORT, ASSET_STOP, ASSET_STOP_LIGHT, ASSET_DRIVE, ASSET_COUNT
} GuiAssetId;
typedef struct { HBITMAP images[ASSET_COUNT], disabledImages[ASSET_COUNT], blendedImages[ASSET_COUNT]; HICON largeIcon, smallIcon; unsigned loaded; } GuiAssets;
/* root 为空时只从程序目录的 resources 子目录加载，不依赖当前工作目录。 */
void imgburn_gui_assets_load(GuiAssets* assets, const wchar_t* root);
void imgburn_gui_assets_free(GuiAssets* assets);
bool imgburn_gui_asset_draw(const GuiAssets* assets, GuiAssetId id, HDC dc, int x, int y, int width, int height, bool enabled);
bool imgburn_gui_asset_button(const GuiAssets* assets, GuiAssetId id, const DRAWITEMSTRUCT* draw);
/* 新主窗按原图逻辑尺寸和比例绘制；禁用灰化半透明，Picker普通态可选择启用混合。 */
bool imgburn_gui_asset_draw_state(const GuiAssets* assets, GuiAssetId id, HDC dc, int x, int y, int width, int height, bool enabled, bool hot, bool blendOnEnabled);
bool imgburn_gui_asset_button_dpi(const GuiAssets* assets, GuiAssetId id, const DRAWITEMSTRUCT* draw, unsigned dpi, bool flat);
#endif
