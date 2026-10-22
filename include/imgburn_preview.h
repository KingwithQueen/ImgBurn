#ifndef IMGBURN_PREVIEW_H
#define IMGBURN_PREVIEW_H

#include "imgburn_image.h"

typedef struct {
    uint64_t number;
    int64_t timestampUs, byteOffset;
    char pictureType;
    bool interlaced, valid;
} DvdPreviewFrameInfo;

typedef struct {
    HANDLE process, output;
    unsigned width, height;
    DWORD exitCode;
    bool ended, failed;
    void* metadata;
    uint64_t frameIndex;
    DvdPreviewFrameInfo info;
} DvdFrameDecoder;

typedef struct { HANDLE process, output; DWORD exitCode; bool ended, failed; } DvdPcmDecoder;
typedef struct DvdAudioPlayer DvdAudioPlayer;
typedef struct {
    unsigned left, right, top, bottom, up, down, previous, next;
    bool autoAction;
    uint8_t command[8];
} DvdPreviewButton;
typedef struct {
    uint32_t sourceSector, startTime, endTime;
    unsigned vobId, cellId, buttonCount, forcedSelect, forcedActivate, highlightStatus;
    DvdPreviewButton buttons[36];
} DvdPreviewPacketInfo;

/* FFmpeg只接收本程序提取的单元数据，不调用原版程序或依赖原版安装路径。 */
bool imgburn_frame_decoder_open(DvdFrameDecoder* decoder, const wchar_t* video, const wchar_t* ffmpeg, unsigned width, unsigned height, uint64_t firstFrame);
bool imgburn_frame_decoder_read(DvdFrameDecoder* decoder, void* bgr, size_t capacity);
void imgburn_frame_decoder_close(DvdFrameDecoder* decoder);
bool imgburn_pcm_decoder_open(DvdPcmDecoder* decoder, const wchar_t* video, const wchar_t* ffmpeg, unsigned streamId, uint64_t firstSample);
size_t imgburn_pcm_decoder_read(DvdPcmDecoder* decoder, void* pcm, size_t capacity);
void imgburn_pcm_decoder_close(DvdPcmDecoder* decoder);
/* 预览输出统一为48 kHz、16位双声道PCM，保持原始音轨的时间戳对齐。 */
DvdAudioPlayer* imgburn_preview_audio_open(const wchar_t* video, const wchar_t* ffmpeg, unsigned streamId, uint64_t firstSample);
void imgburn_preview_audio_pause(DvdAudioPlayer* player, bool pause);
bool imgburn_preview_audio_position(DvdAudioPlayer* player, uint64_t* played, bool* drained, bool* failed);
void imgburn_preview_audio_close(DvdAudioPlayer* player);
bool imgburn_preview_save_bmp(const wchar_t* path, const void* bgr, unsigned width, unsigned height, bool replace);
/* 返回当前NAV包的按钮项；HLI继承、按钮命令执行由后续导航上下文处理。 */
bool imgburn_preview_packet_info(const wchar_t* video, int64_t byteOffset, DvdPreviewPacketInfo* info);
int imgburn_dvd_preview_show(const ImageBuilder* builder, const DvdPreviewPlan* plan, const wchar_t* ffmpeg);

#endif
