/**
 * @file imgburn_pipeline.h
 * @brief ImgBurn Core C - Burn & Verify Streaming Pipelines
 *
 * Implements the asynchronous burning and verification pipeline,
 * linking Image/Audio input -> Ring Buffer -> MMC SCSI writer thread,
 * with real-time transfer stats (rate, buffer levels, ETA).
 */

#ifndef IMGBURN_PIPELINE_H
#define IMGBURN_PIPELINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "imgburn_spti.h"
#include "imgburn_mmc.h"
#include "imgburn_ringbuffer.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PIPELINE_STATE_IDLE = 0,
    PIPELINE_STATE_INITIALIZING,
    PIPELINE_STATE_WRITING,
    PIPELINE_STATE_FLUSHING_CACHE,
    PIPELINE_STATE_CLOSING_SESSION,
    PIPELINE_STATE_VERIFYING,
    PIPELINE_STATE_COMPLETED,
    PIPELINE_STATE_ABORTED,
    PIPELINE_STATE_FAILED,
    PIPELINE_STATE_READING,
    PIPELINE_STATE_ERASING
} PipelineState;

/**
 * @brief Real-time burn/verify telemetry stats
 */
typedef struct {
    PipelineState state;
    uint32_t currentLba;
    uint32_t totalSectors;
    int progressPercent;            /* 0 .. 100 */

    uint32_t hostBufferPercent;     /* 0 .. 100 (Ring buffer level) */
    uint32_t deviceBufferPercent;   /* 0 .. 100 (Hardware drive FIFO level) */

    double currentSpeedKBps;        /* Write / read rate */
    double speedMultiplier;         /* e.g. 4.0x, 8.0x, 16.0x */

    uint32_t elapsedTimeSec;
    uint32_t remainingTimeSec;

    uint32_t miscomparesCount;      /* For Verify mode */
    uint32_t retriesCount;
    uint32_t comparedSectors, readableOnlySectors; /* 分开记录内容比较与仅检查可读性的扇区。 */
    char statusMessage[128];
} PipelineStats;

typedef void (*PipelineProgressCallback)(const PipelineStats* stats, void* userData);

/**
 * @brief Burn Pipeline Configuration
 */
typedef struct {
    OpticalDrive* drive;
    const wchar_t* imagePath;       /* 2048 字节 ISO/BIN 或受支持的音频 CUE。 */
    uint32_t writeSpeedKbps;        /* 0、0xFFFF 或 0xFFFFFFFF 为最高速度；蓝光支持 32 位速率。 */
    bool testMode;                  /* 支持具备 Test Write 能力的 CD-R SAO、DVD-R DAO。 */
    bool verifyAfterWrite;
    bool ejectWhenDone;
    size_t ringBufferBytes;         /* e.g. 32 MB or 64 MB */
    HANDLE cancelEvent;             /* 借用的取消事件；不打断设备正在执行的命令。 */
    bool reserveTrack;              /* DVD+R 可选；当前 DVD-R DAO/BD-R SRM 路径总是预留。 */
    bool cycleTrayBeforeVerify;     /* 写后校验前按原版默认流程重新装载介质。 */
    bool performOpc;                /* 显式请求写入前功率校准，默认关闭。 */
} BurnConfig;

/**
 * @brief Execute Write Mode: burn an image file to optical disc
 *
 * 同步执行，内部使用源读取线程；调用者可自行在工作线程运行并接收进度回调。
 *
 * @param config Burn parameters
 * @param callback Progress notification function
 * @param userData Context passed to callback
 * @return True if burn (and verify if enabled) completed successfully
 */
bool pipeline_burn_image(const BurnConfig* config,
                         PipelineProgressCallback callback,
                         void* userData);

/* CD-RW/DVD-RW 的整盘擦除。命令开始后的取消须等待设备结束，不能中断物理擦除。 */
typedef struct { OpticalDrive* drive; HANDLE cancelEvent; bool full; } EraseConfig;
bool pipeline_erase_disc(const EraseConfig* config, PipelineProgressCallback callback, void* userData);

/**
 * @brief Execute Verify Mode: verify optical disc against an image file
 */
bool pipeline_verify_disc(OpticalDrive* drive,
                          const wchar_t* sourceImagePath,
                          PipelineProgressCallback callback,
                          void* userData);

/* Read 主体：完整读取 2048 字节数据扇区，失败或取消时保护既有目标。 */
typedef struct {
    OpticalDrive* drive;
    const wchar_t* imagePath;
    HANDLE cancelEvent;            /* 借用的事件；调用者负责关闭。 */
    unsigned retryCount;            /* 单扇区额外重试次数，最多 20 次。 */
    bool replaceExisting;
} ReadConfig;

bool pipeline_read_disc(const ReadConfig* config, PipelineProgressCallback callback, void* userData);

/* 原始 CD：2352 字节主通道和经 Q 校验的索引，最后发布 CUE。 */
bool pipeline_read_cd(const ReadConfig* config, PipelineProgressCallback callback, void* userData);

/* Verify：空镜像检查可读性，ISO/BIN 比较数据扇区，CUE 比较受支持的原始 CD。 */
typedef struct { OpticalDrive* drive; const wchar_t* imagePath; HANDLE cancelEvent; unsigned retryCount; bool compareAudio; } VerifyConfig;
bool pipeline_verify_disc_ex(const VerifyConfig* config, PipelineProgressCallback callback, void* userData);
bool pipeline_verify_cd(const VerifyConfig* config, PipelineProgressCallback callback, void* userData);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_PIPELINE_H */
