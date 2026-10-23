#ifndef IMGBURN_AUDIO_SOURCE_H
#define IMGBURN_AUDIO_SOURCE_H

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    HANDLE source, pcm;
    uint64_t dataOffset, dataBytes;
    uint32_t sectorCount;
} AudioPcmSource;

/* 完整准备可随机读取的CD音频PCM；源文件锁与两个独立句柄由close统一释放，扇区尾部由调用者补零。 */
bool audio_pcm_source_open(const wchar_t* path, HANDLE cancelEvent, AudioPcmSource* source);
void audio_pcm_source_close(AudioPcmSource* source);

#ifdef __cplusplus
}
#endif

#endif
