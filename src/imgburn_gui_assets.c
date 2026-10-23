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
    L"ui_images\\tfrmsectorviewer\\btnPreviousSector.png", L"ui_images\\tfrmsectorviewer\\btnNextSector.png", L"ui_images\\tfrmsectorviewer\\btnRefreshSector.png", L"ui_images\\tfrmsectorviewer\\btnLBAFromMedia.png"
};
static HBITMAP assets_png(IWICImagingFactory* factory, const wchar_t* path) {
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
    if (FAILED(hr) && bitmap) { DeleteObject(bitmap); bitmap = NULL; }
    if (converter) IWICFormatConverter_Release(converter); if (frame) IWICBitmapFrameDecode_Release(frame); if (decoder) IWICBitmapDecoder_Release(decoder); return bitmap;
}
void imgburn_gui_assets_load(GuiAssets* assets, const wchar_t* root) {
    if (!assets) return; ZeroMemory(assets,sizeof(*assets)); wchar_t base[MAX_PATH], path[MAX_PATH];
    if (root) { DWORD count = GetFullPathNameW(root,MAX_PATH,base,NULL); if (!count || count >= MAX_PATH) return; }
    else { DWORD n = GetModuleFileNameW(NULL,base,MAX_PATH); if (!n || n >= MAX_PATH) return; wchar_t* slash = wcsrchr(base,L'\\'); if (!slash) return; *slash = 0; if (wcslen(base) + 11 >= MAX_PATH) return; wcscat_s(base,MAX_PATH,L"\\resources"); }
    if (swprintf(path,MAX_PATH,L"%ls\\icons\\MAINICON.ico",base) > 0) { assets->largeIcon = (HICON)LoadImageW(NULL,path,IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_LOADFROMFILE); assets->smallIcon = (HICON)LoadImageW(NULL,path,IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_LOADFROMFILE); }
    IWICImagingFactory* factory = NULL; if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void**)&factory))) return;
    for (unsigned i = 0; i < ASSET_COUNT; i++) if (swprintf(path,MAX_PATH,L"%ls\\%ls",base,imageNames[i]) > 0) { assets->images[i] = assets_png(factory,path); if (assets->images[i]) assets->loaded++; }
    IWICImagingFactory_Release(factory);
}
void imgburn_gui_assets_free(GuiAssets* assets) {
    if (!assets) return; for (unsigned i = 0; i < ASSET_COUNT; i++) if (assets->images[i]) DeleteObject(assets->images[i]);
    if (assets->largeIcon) DestroyIcon(assets->largeIcon); if (assets->smallIcon) DestroyIcon(assets->smallIcon); ZeroMemory(assets,sizeof(*assets));
}
bool imgburn_gui_asset_draw(const GuiAssets* assets, GuiAssetId id, HDC dc, int x, int y, int width, int height, bool enabled) {
    if (!assets || (unsigned)id >= ASSET_COUNT || !assets->images[id] || width <= 0 || height <= 0) return false; BITMAP info;
    if (!GetObjectW(assets->images[id],sizeof(info),&info)) return false; HDC source = CreateCompatibleDC(dc); if (!source) return false;
    HGDIOBJ previous = SelectObject(source,assets->images[id]); BLENDFUNCTION blend = {AC_SRC_OVER,0,enabled ? 255 : 100,AC_SRC_ALPHA};
    bool ok = AlphaBlend(dc,x,y,width,height,source,0,0,info.bmWidth,info.bmHeight,blend) != FALSE; SelectObject(source,previous); DeleteDC(source); return ok;
}
bool imgburn_gui_asset_button(const GuiAssets* assets, GuiAssetId id, const DRAWITEMSTRUCT* draw) {
    if (!assets || (unsigned)id >= ASSET_COUNT || !assets->images[id] || !draw) return false; RECT r = draw->rcItem;
    UINT flags = DFCS_BUTTONPUSH | (draw->itemState & ODS_SELECTED ? DFCS_PUSHED : 0) | (draw->itemState & ODS_DISABLED ? DFCS_INACTIVE : 0); DrawFrameControl(draw->hDC,&r,DFC_BUTTON,flags);
    int size = r.bottom - r.top - 6; if (size > r.right - r.left - 6) size = r.right - r.left - 6; if (size < 1) return true;
    int offset = draw->itemState & ODS_SELECTED ? 1 : 0; imgburn_gui_asset_draw(assets,id,draw->hDC,(r.left + r.right - size) / 2 + offset,(r.top + r.bottom - size) / 2 + offset,size,size,!(draw->itemState & ODS_DISABLED));
    if (draw->itemState & ODS_FOCUS) { InflateRect(&r,-3,-3); DrawFocusRect(draw->hDC,&r); } return true;
}
