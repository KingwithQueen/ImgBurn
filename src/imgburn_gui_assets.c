/* 可选原版图像加载：WIC 转成预乘 BGRA，GDI 使用透明混合绘制。 */
#define COBJMACROS
#include "imgburn_gui_assets.h"
#include <wincodec.h>
#include <stdio.h>
#include <wchar.h>
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"uuid.lib")
#pragma comment(lib,"msimg32.lib")

static const wchar_t* const imageNames[ASSET_COUNT] = {
    L"ui_images\\tfrmmain\\Panel_PICKER_WriteButton.png", L"ui_images\\tfrmmain\\Panel_PICKER_BuildDeviceButton.png", L"ui_images\\tfrmmain\\Panel_PICKER_ReadButton.png",
    L"ui_images\\tfrmmain\\Panel_PICKER_BuildImageFileButton.png", L"ui_images\\tfrmmain\\Panel_PICKER_VerifyButton.png", L"ui_images\\tfrmmain\\Panel_PICKER_DiscoveryButton.png",
    L"ui_images\\tfrmmain\\Panel_ISOREAD_Destination_Browse.png", L"ui_images\\tfrmmain\\Panel_ISOBUILD_Source_BrowseFolder.png", L"ui_images\\tfrmmain\\Panel_ISOBUILD_Source_Remove.png", L"ui_images\\tfrmmain\\Panel_ISOVERIFY_ImageInformation.png",
    L"ui_images\\tfrmsectorviewer\\btnPreviousSector.png", L"ui_images\\tfrmsectorviewer\\btnNextSector.png", L"ui_images\\tfrmsectorviewer\\btnRefreshSector.png", L"ui_images\\tfrmsectorviewer\\btnLBAFromMedia.png",
    L"ui_images\\tfrmmain\\Panel_ISOWRITE_SourceBrowseAdd.png", L"ui_images\\tfrmmain\\Panel_ISOWRITE_SourceOpen.png", L"ui_images\\tfrmmain\\Panel_ISOWRITE_SourceCreateCUE.png",
    L"ui_images\\tfrmmain\\Panel_ISOWRITE_DestinationLoad.png", L"ui_images\\tfrmmain\\Panel_ISOWRITE_DestinationEject.png", L"ui_images\\tfrmmain\\Panel_ISOWRITE_DestinationErase.png", L"ui_images\\tfrmmain\\Panel_ISOWRITE_DestinationChangerControl.png",
    L"ui_images\\tfrmmain\\Panel_ISOWRITE_SettingsBookType.png", L"ui_images\\tfrmmain\\Panel_ISOWRITE_SettingsAdvancedSettings.png", L"ui_images\\tfrmmain\\Panel_ISOWRITE_SettingsAutomaticWriteSpeed.png",
    L"ui_images\\tfrmmain\\Panel_ISOWRITE_GraphDataDisplay.png", L"ui_images\\tfrmmain\\Panel_ISOWRITE_GraphDataExport.png", L"ui_images\\tfrmmain\\PngImageList32.png", L"ui_images\\tfrmmain\\PngImageList32_2.png", L"ui_images\\tfrmmain\\ImageList.png"
};

/* 整数化的Round近似沿用原组件的最近偶数规则，避免在正好半值时一律向上。 */
static BYTE assets_round_even(unsigned value, unsigned divisor) {
    unsigned whole = value / divisor, remainder = value % divisor; if (remainder * 2 > divisor || (remainder * 2 == divisor && (whole & 1))) whole++; return (BYTE)whole;
}
static HBITMAP assets_variant(IWICImagingFactory* factory, IWICBitmapFrameDecode* frame, UINT width, UINT height, bool disabled) {
    IWICFormatConverter* converter = NULL; HBITMAP bitmap = NULL; BYTE* pixels = NULL;
    HRESULT hr = IWICImagingFactory_CreateFormatConverter(factory,&converter);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(converter,(IWICBitmapSource*)frame,&GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        BITMAPINFO info = {0}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = (LONG)width; info.bmiHeader.biHeight = -(LONG)height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(NULL,&info,DIB_RGB_COLORS,(void**)&pixels,NULL,0); if (!bitmap || !pixels) hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_CopyPixels(converter,NULL,width * 4,width * height * 4,pixels);
    if (SUCCEEDED(hr)) for (size_t i = 0; i < (size_t)width * height; i++) {
        BYTE* p = pixels + i * 4; BYTE alpha = assets_round_even((unsigned)p[3] * (disabled ? 128 : 193),256);
        if (disabled) { BYTE gray = assets_round_even((unsigned)p[2] * 30 + (unsigned)p[1] * 59 + (unsigned)p[0] * 11,100); p[0] = p[1] = p[2] = gray; }
        for (unsigned channel = 0; channel < 3; channel++) p[channel] = (BYTE)(((unsigned)p[channel] * alpha + 127) / 255); p[3] = alpha;
    }
    if (FAILED(hr) && bitmap) { DeleteObject(bitmap); bitmap = NULL; } if (converter) IWICFormatConverter_Release(converter); return bitmap;
}
static HBITMAP assets_png(IWICImagingFactory* factory, const wchar_t* path, HBITMAP* disabled, HBITMAP* blended) {
    IWICBitmapDecoder* decoder = NULL; IWICBitmapFrameDecode* frame = NULL; IWICFormatConverter* converter = NULL; HBITMAP bitmap = NULL; void* pixels = NULL; UINT width = 0, height = 0;
    HRESULT hr = IWICImagingFactory_CreateDecoderFromFilename(factory,path,NULL,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(decoder,0,&frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetSize(frame,&width,&height);
    if (SUCCEEDED(hr) && (!width || !height || width > 4096 || height > 4096)) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(factory,&converter);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(converter,(IWICBitmapSource*)frame,&GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        BITMAPINFO info = {0}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = (LONG)width; info.bmiHeader.biHeight = -(LONG)height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(NULL,&info,DIB_RGB_COLORS,&pixels,NULL,0); if (!bitmap || !pixels) hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_CopyPixels(converter,NULL,width * 4,width * height * 4,pixels);
    if (SUCCEEDED(hr)) { *disabled = assets_variant(factory,frame,width,height,true); *blended = assets_variant(factory,frame,width,height,false); if (!*disabled || !*blended) hr = E_OUTOFMEMORY; }
    if (FAILED(hr)) { if (*disabled) DeleteObject(*disabled); if (*blended) DeleteObject(*blended); *disabled = *blended = NULL; }
    if (FAILED(hr) && bitmap) { DeleteObject(bitmap); bitmap = NULL; }
    if (converter) IWICFormatConverter_Release(converter); if (frame) IWICBitmapFrameDecode_Release(frame); if (decoder) IWICBitmapDecoder_Release(decoder); return bitmap;
}
void imgburn_gui_assets_load(GuiAssets* assets, const wchar_t* root) {
    if (!assets) return; ZeroMemory(assets,sizeof(*assets)); wchar_t base[MAX_PATH], path[MAX_PATH];
    if (root) { DWORD count = GetFullPathNameW(root,MAX_PATH,base,NULL); if (!count || count >= MAX_PATH) return; }
    else { DWORD n = GetModuleFileNameW(NULL,base,MAX_PATH); if (!n || n >= MAX_PATH) return; wchar_t* slash = wcsrchr(base,L'\\'); if (!slash) return; *slash = 0; if (wcslen(base) + 11 >= MAX_PATH) return; wcscat_s(base,MAX_PATH,L"\\resources"); }
    if (swprintf(path,MAX_PATH,L"%ls\\icons\\MAINICON.ico",base) > 0) { assets->largeIcon = (HICON)LoadImageW(NULL,path,IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_LOADFROMFILE); assets->smallIcon = (HICON)LoadImageW(NULL,path,IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_LOADFROMFILE); }
    IWICImagingFactory* factory = NULL; if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void**)&factory))) return;
    for (unsigned i = 0; i < ASSET_COUNT; i++) if (swprintf(path,MAX_PATH,L"%ls\\%ls",base,imageNames[i]) > 0) { assets->images[i] = assets_png(factory,path,&assets->disabledImages[i],&assets->blendedImages[i]); if (assets->images[i]) assets->loaded++; }
    IWICImagingFactory_Release(factory);
}
void imgburn_gui_assets_free(GuiAssets* assets) {
    if (!assets) return; for (unsigned i = 0; i < ASSET_COUNT; i++) { if (assets->images[i]) DeleteObject(assets->images[i]); if (assets->disabledImages[i]) DeleteObject(assets->disabledImages[i]); if (assets->blendedImages[i]) DeleteObject(assets->blendedImages[i]); }
    if (assets->largeIcon) DestroyIcon(assets->largeIcon); if (assets->smallIcon) DestroyIcon(assets->smallIcon); ZeroMemory(assets,sizeof(*assets));
}
static bool assets_draw_bitmap(HBITMAP bitmap, HDC dc, int x, int y, int width, int height, BYTE opacity) {
    if (!bitmap || width <= 0 || height <= 0) return false; BITMAP info;
    if (!GetObjectW(bitmap,sizeof(info),&info)) return false; HDC source = CreateCompatibleDC(dc); if (!source) return false;
    HGDIOBJ previous = SelectObject(source,bitmap); BLENDFUNCTION blend = {AC_SRC_OVER,0,opacity,AC_SRC_ALPHA};
    bool ok = AlphaBlend(dc,x,y,width,height,source,0,0,info.bmWidth,info.bmHeight,blend) != FALSE; SelectObject(source,previous); DeleteDC(source); return ok;
}
bool imgburn_gui_asset_draw(const GuiAssets* assets, GuiAssetId id, HDC dc, int x, int y, int width, int height, bool enabled) {
    if (!assets || (unsigned)id >= ASSET_COUNT) return false; return assets_draw_bitmap(assets->images[id],dc,x,y,width,height,enabled ? 255 : 100);
}
bool imgburn_gui_asset_draw_state(const GuiAssets* assets, GuiAssetId id, HDC dc, int x, int y, int width, int height, bool enabled, bool hot, bool blendOnEnabled) {
    if (!assets || (unsigned)id >= ASSET_COUNT) return false;
    HBITMAP bitmap = !enabled ? assets->disabledImages[id] : blendOnEnabled && !hot ? assets->blendedImages[id] : assets->images[id];
    return assets_draw_bitmap(bitmap,dc,x,y,width,height,255);
}
bool imgburn_gui_asset_button(const GuiAssets* assets, GuiAssetId id, const DRAWITEMSTRUCT* draw) {
    if (!assets || (unsigned)id >= ASSET_COUNT || !assets->images[id] || !draw) return false; RECT r = draw->rcItem;
    UINT flags = DFCS_BUTTONPUSH | (draw->itemState & ODS_SELECTED ? DFCS_PUSHED : 0) | (draw->itemState & ODS_DISABLED ? DFCS_INACTIVE : 0); DrawFrameControl(draw->hDC,&r,DFC_BUTTON,flags);
    int size = r.bottom - r.top - 6; if (size > r.right - r.left - 6) size = r.right - r.left - 6; if (size < 1) return true;
    int offset = draw->itemState & ODS_SELECTED ? 1 : 0; imgburn_gui_asset_draw(assets,id,draw->hDC,(r.left + r.right - size) / 2 + offset,(r.top + r.bottom - size) / 2 + offset,size,size,!(draw->itemState & ODS_DISABLED));
    if (draw->itemState & ODS_FOCUS) { InflateRect(&r,-3,-3); DrawFocusRect(draw->hDC,&r); } return true;
}
bool imgburn_gui_asset_button_dpi(const GuiAssets* assets, GuiAssetId id, const DRAWITEMSTRUCT* draw, unsigned dpi, bool flat) {
    if (!assets || (unsigned)id >= ASSET_COUNT || !assets->images[id] || !draw) return false; if (!dpi) dpi = 96;
    RECT r = draw->rcItem; bool enabled = !(draw->itemState & ODS_DISABLED), selected = (draw->itemState & ODS_SELECTED) != 0, hot = (draw->itemState & ODS_HOTLIGHT) != 0;
    FillRect(draw->hDC,&r,GetSysColorBrush(COLOR_BTNFACE));
    if (!flat) { UINT flags = DFCS_BUTTONPUSH | (selected ? DFCS_PUSHED : 0) | (enabled ? 0 : DFCS_INACTIVE); DrawFrameControl(draw->hDC,&r,DFC_BUTTON,flags); }
    else if (selected || (enabled && hot)) DrawEdge(draw->hDC,&r,selected ? EDGE_SUNKEN : EDGE_RAISED,BF_RECT);
    BITMAP info; if (!GetObjectW(assets->images[id],sizeof(info),&info)) return false;
    int padding = MulDiv(2,(int)dpi,96), availableWidth = r.right - r.left - padding * 2, availableHeight = r.bottom - r.top - padding * 2;
    int width = MulDiv(info.bmWidth,(int)dpi,96), height = MulDiv(info.bmHeight,(int)dpi,96);
    if (availableWidth < 1 || availableHeight < 1) return true;
    if (width > availableWidth) { width = availableWidth; height = MulDiv(info.bmHeight,width,info.bmWidth); }
    if (height > availableHeight) { height = availableHeight; width = MulDiv(info.bmWidth,height,info.bmHeight); }
    int offset = selected ? MulDiv(1,(int)dpi,96) : 0;
    bool ok = imgburn_gui_asset_draw_state(assets,id,draw->hDC,(r.left + r.right - width) / 2 + offset,(r.top + r.bottom - height) / 2 + offset,width,height,enabled,hot || selected,false);
    if (draw->itemState & ODS_FOCUS) { InflateRect(&r,-MulDiv(3,(int)dpi,96),-MulDiv(3,(int)dpi,96)); DrawFocusRect(draw->hDC,&r); } return ok;
}
