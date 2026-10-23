#ifndef IMGBURN_GUI_SECTOR_H
#define IMGBURN_GUI_SECTOR_H
#include "imgburn_gui_assets.h"
#include "imgburn_spti.h"
int imgburn_gui_sector_show(HWND owner, HINSTANCE instance, const OpticalDriveInfo* devices, int count, int selected, const GuiAssets* assets);
#endif
