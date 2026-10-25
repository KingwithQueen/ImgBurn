#ifndef IMGBURN_GUI_INTERNAL_H
#define IMGBURN_GUI_INTERNAL_H

#include "imgburn_image.h"
#include "imgburn_spti.h"
#include "imgburn_data_source.h"

typedef enum { GUI_EZ, GUI_READ, GUI_BUILD, GUI_WRITE, GUI_VERIFY, GUI_DISCOVERY } GuiMode;
typedef enum { GUI_CALCULATE, GUI_BUILD_IMAGE, GUI_INSPECT, GUI_CREATE_CUE, GUI_CREATE_MDS, GUI_DRIVE_INFO, GUI_VERIFY_DISC, GUI_WRITE_DISC, GUI_DVD_INFO, GUI_DVD_PREVIEW, GUI_READ_DISC, GUI_ERASE_QUICK, GUI_ERASE_FULL, GUI_DISCOVERY_DISC, GUI_DVD_CANDIDATES } GuiOperation;
typedef struct { wchar_t path[MAX_PATH]; } GuiPath;
enum { GUI_JOB_PROGRESS = WM_APP + 40, GUI_JOB_DONE, GUI_JOB_STATUS };

typedef struct {
    GuiOperation operation;
    HWND owner;
    HANDLE thread, cancel;
    GuiPath* sources;
    size_t sourceCount;
    wchar_t destination[MAX_PATH], drive[MAX_PATH], label[127];
    FsTypeFlags fsTypes;
    UdfRevision revision;
    bool dvdVideo, bluray, optimize, verify, seamless, oppositeTrackPath, replaceExisting, testMode, testWriteSupported, readCdText, rawCdMds, includeSessionPregaps;
    uint32_t speed, discoverySectors, discoveryCapacity;
    uint32_t readSplitSectors;
    unsigned readRetries;
    bool readAutoSplit, readEject, setReadSpeeds;
    double readSpeedMultiplier, audioReadSpeedMultiplier;
    double discoverySpeedMultiplier;
    uint32_t layerBreak, writeLayerBreak;
    uint32_t comparedSectors, readableOnlySectors, miscompares;
    bool succeeded, builderReady, mediaReady, eraseSupported;
    DWORD error;
    char lastStatus[128];
    wchar_t result[8192];
    ImageBuilder builder;
    DvdNavigation navigation;
    DvdPreviewPlan preview;
    DvdLayerBreakCandidate* dvdCandidates;
    size_t dvdCandidateCount;
    uint32_t dvdImageSectors;
    DataImageSource dvdCandidateSource; /* 窗口接管候选时一并接管只读来源，直至派生选择失效。 */
} GuiJob;

/* 工作者只通过消息报告结果，不从后台线程读取或修改窗口控件。 */
DWORD WINAPI imgburn_gui_job_run(void* context);
void imgburn_gui_job_free(GuiJob* job);
const wchar_t* imgburn_gui_basename(const wchar_t* path);

#endif
