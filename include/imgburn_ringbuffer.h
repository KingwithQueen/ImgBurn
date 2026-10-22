/**
 * @file imgburn_ringbuffer.h
 * @brief ImgBurn Core C - Asynchronous Burn/Read Ring Buffer
 *
 * Implements high-throughput, multi-threaded circular buffer with VirtualLock
 * to prevent buffer underruns across Windows XP to 11.
 */

#ifndef IMGBURN_RINGBUFFER_H
#define IMGBURN_RINGBUFFER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t* buffer;
    size_t capacity;
    size_t readPos;
    size_t writePos;
    size_t count;
    bool isFinished;
    bool isLockedInRam;
    bool isInitialized;             /* 临界区初始化成功后才允许访问或销毁。 */

    CRITICAL_SECTION cs;
    HANDLE hEventDataAvailable;
    HANDLE hEventSpaceAvailable;
} RingBuffer;

/**
 * @brief Initialize a ring buffer with given capacity in bytes
 * @param rb Pointer to RingBuffer
 * @param capacityBytes Total buffer size (e.g. 64MB = 64 * 1024 * 1024)
 * @param lockInRam If true, attempts to lock in physical RAM via VirtualLock
 */
bool ringbuffer_init(RingBuffer* rb, size_t capacityBytes, bool lockInRam);

/**
 * @brief Destroy and free ring buffer resources
 */
void ringbuffer_destroy(RingBuffer* rb);

/**
 * @brief Write data into ring buffer (Producer)
 * @param timeoutMs Timeout in milliseconds (or INFINITE)
 * @return Bytes written, or 0 on timeout/finish
 */
size_t ringbuffer_write(RingBuffer* rb, const void* src, size_t bytesToWrite, DWORD timeoutMs);

/**
 * @brief Read data from ring buffer (Consumer)
 * @param timeoutMs Timeout in milliseconds (or INFINITE)
 * @return Bytes read, or 0 if empty and finished
 */
size_t ringbuffer_read(RingBuffer* rb, void* dest, size_t bytesToRead, DWORD timeoutMs);

/**
 * @brief Signal that writer/producer has finished all data
 */
void ringbuffer_set_finished(RingBuffer* rb);

/* 在临界区内读取结束状态，避免并发访问未同步字段。 */
bool ringbuffer_is_finished(RingBuffer* rb);

/**
 * @brief Get current buffer level percentage (0 to 100)
 */
int ringbuffer_get_level_percent(RingBuffer* rb);

/**
 * @brief Clear / Reset ring buffer
 */
void ringbuffer_reset(RingBuffer* rb);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_RINGBUFFER_H */
