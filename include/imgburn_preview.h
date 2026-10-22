#ifndef IMGBURN_PREVIEW_H
#define IMGBURN_PREVIEW_H

#include "imgburn_image.h"

typedef struct {
    HANDLE process, output;
    unsigned width, height;
    DWORD exitCode;
    bool ended, failed;
} DvdFrameDecoder;

/* FFmpeg只接收本程序提取的单元数据，不调用原版程序或依赖原版安装路径。 */
bool imgburn_frame_decoder_open(DvdFrameDecoder* decoder, const wchar_t* video, const wchar_t* ffmpeg, unsigned width, unsigned height, uint64_t firstFrame);
bool imgburn_frame_decoder_read(DvdFrameDecoder* decoder, void* bgr, size_t capacity);
void imgburn_frame_decoder_close(DvdFrameDecoder* decoder);
int imgburn_dvd_preview_show(const ImageBuilder* builder, const DvdPreviewPlan* plan, const wchar_t* ffmpeg);

#endif
