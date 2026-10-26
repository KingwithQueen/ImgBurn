#ifndef IMGBURN_GUI_INTERNAL_H
#define IMGBURN_GUI_INTERNAL_H

#include "imgburn_image.h"
#include "imgburn_spti.h"
#include "imgburn_data_source.h"
#include "imgburn_pipeline.h"
#include "imgburn_iso_session.h"

typedef enum { GUI_EZ, GUI_READ, GUI_BUILD, GUI_WRITE, GUI_VERIFY, GUI_DISCOVERY } GuiMode;
typedef enum { GUI_CALCULATE, GUI_BUILD_IMAGE, GUI_INSPECT, GUI_CREATE_CUE, GUI_CREATE_MDS, GUI_DRIVE_INFO, GUI_VERIFY_DISC, GUI_WRITE_DISC, GUI_DVD_INFO, GUI_DVD_PREVIEW, GUI_READ_DISC, GUI_ERASE_QUICK, GUI_ERASE_FULL, GUI_DISCOVERY_DISC, GUI_DVD_CANDIDATES, GUI_CD_ISO_PLAN, GUI_CD_ISO_WRITE, GUI_WRITE_OPTIONS, GUI_READ_OPTIONS, GUI_READ_DEST_INFO, GUI_BD_RE_FORMAT_LIST, GUI_BD_RE_FORMAT, GUI_BD_R_FORMAT_LIST, GUI_BD_R_FORMAT } GuiOperation;
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
    bool succeeded, builderReady, mediaReady, writeMediaReady, eraseSupported;
    DWORD error;
    char lastStatus[128];
    wchar_t result[8192];
    bool bdReMedia, bdReWritable, bdReOverwrite, bdReErase; /* 候选与本次破坏性操作意图分开，不持久接受覆盖。 */
    MmcFormatCapacities bdReCapacities;
    BdReFormatConfig bdReFormat;
    BdReFormatResult bdReFormatResult;
    BdRFormatConfig bdRFormat;
    BdRFormatResult bdRFormatResult;
    BdWriteType bdWriteType;
    bool bdDisableVnr; /* 仅本次明确的BD-R写入意图，不作为覆盖或格式化授权。 */
    bool sourceInfoReady;
    uint64_t sourceBytes;
    uint32_t sourceSectors, sourceSectorBytes;
    wchar_t sourceVolume[128], sourceImplementation[128], sourceFileSystems[64]; /* 只填来源解析可确认的字段，未知值保持空。 */
    bool readDestinationReady;
    uint64_t readDestinationFreeBytes, readDestinationTotalBytes;
    wchar_t readDestinationFileSystem[40]; /* 目标所在文件系统信息，与光盘来源文件系统分开。 */
    ImageBuilder builder;
    DvdNavigation navigation;
    DvdPreviewPlan preview;
    DvdLayerBreakCandidate* dvdCandidates;
    size_t dvdCandidateCount;
    uint32_t dvdImageSectors;
    DataImageSource dvdCandidateSource; /* 窗口接管候选时一并接管只读来源，直至派生选择失效。 */
    bool cdAllowBlank, cdFinalize, cdPlanSelected;
    MmcCdAppendPlan cdPlan;
    CdSessionResult cdResult;
    IsoSessionSource cdIsoSource; /* 规划成功转交窗口持有；新写入任务接管时重绑取消事件。 */
} GuiJob;

/* 工作者只通过消息报告结果，不从后台线程读取或修改窗口控件。 */
DWORD WINAPI imgburn_gui_job_run(void* context);
void imgburn_gui_job_free(GuiJob* job);
const wchar_t* imgburn_gui_basename(const wchar_t* path);

#endif
