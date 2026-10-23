#ifndef IMGBURN_GUI_INTERNAL_H
#define IMGBURN_GUI_INTERNAL_H

#include "imgburn_image.h"
#include "imgburn_spti.h"

typedef enum { GUI_EZ, GUI_READ, GUI_BUILD, GUI_WRITE, GUI_VERIFY, GUI_DISCOVERY } GuiMode;
typedef enum { GUI_CALCULATE, GUI_BUILD_IMAGE, GUI_INSPECT, GUI_CREATE_CUE, GUI_CREATE_MDS, GUI_DRIVE_INFO, GUI_VERIFY_DISC, GUI_WRITE_DISC, GUI_DVD_INFO, GUI_DVD_PREVIEW, GUI_READ_DISC } GuiOperation;
typedef struct { wchar_t path[MAX_PATH]; } GuiPath;
enum { GUI_JOB_PROGRESS = WM_APP + 40, GUI_JOB_DONE };

typedef struct {
    GuiOperation operation;
    HWND owner;
    HANDLE thread, cancel;
    GuiPath* sources;
    size_t sourceCount;
    wchar_t destination[MAX_PATH], drive[MAX_PATH], label[127];
    FsTypeFlags fsTypes;
    UdfRevision revision;
    bool dvdVideo, bluray, optimize, verify, seamless, oppositeTrackPath, replaceExisting;
    uint16_t speed;
    uint32_t layerBreak;
    bool succeeded, builderReady, mediaReady;
    DWORD error;
    wchar_t result[8192];
    ImageBuilder builder;
    DvdNavigation navigation;
    DvdPreviewPlan preview;
} GuiJob;

/* 工作者只通过消息报告结果，不从后台线程读取或修改窗口控件。 */
DWORD WINAPI imgburn_gui_job_run(void* context);
void imgburn_gui_job_free(GuiJob* job);
const wchar_t* imgburn_gui_basename(const wchar_t* path);

#endif
