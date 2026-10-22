/**
 * @file imgburn_ringbuffer.c
 * @brief ImgBurn Core C - Asynchronous Burn/Read Ring Buffer Implementation
 */

#include "imgburn_ringbuffer.h"
#include <stdlib.h>
#include <string.h>

bool ringbuffer_init(RingBuffer* rb, size_t capacityBytes, bool lockInRam) {
    if (!rb || capacityBytes == 0) return false;
    memset(rb, 0, sizeof(*rb));

    /* Round up to 64KB boundary */
    size_t align = 64 * 1024;
    if (capacityBytes > SIZE_MAX - (align - 1)) return false;
    capacityBytes = (capacityBytes + align - 1) & ~(align - 1);

    rb->capacity = capacityBytes;
    rb->buffer = (uint8_t*)VirtualAlloc(NULL, capacityBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rb->buffer) {
        return false;
    }

    if (lockInRam) {
        /* Try to lock in physical RAM to prevent swapping */
        if (VirtualLock(rb->buffer, capacityBytes)) {
            rb->isLockedInRam = true;
        }
    }

    if (!InitializeCriticalSectionAndSpinCount(&rb->cs, 0)) {
        if (rb->isLockedInRam) VirtualUnlock(rb->buffer, rb->capacity);
        VirtualFree(rb->buffer, 0, MEM_RELEASE);
        memset(rb, 0, sizeof(*rb));
        return false;
    }
    rb->isInitialized = true;
    rb->hEventDataAvailable = CreateEventW(NULL, FALSE, FALSE, NULL);
    rb->hEventSpaceAvailable = CreateEventW(NULL, FALSE, TRUE, NULL);
    if (!rb->hEventDataAvailable || !rb->hEventSpaceAvailable) { ringbuffer_destroy(rb); return false; }

    return true;
}

void ringbuffer_destroy(RingBuffer* rb) {
    if (!rb) return;

    if (rb->buffer) {
        if (rb->isLockedInRam) {
            VirtualUnlock(rb->buffer, rb->capacity);
        }
        VirtualFree(rb->buffer, 0, MEM_RELEASE);
        rb->buffer = NULL;
    }

    if (rb->hEventDataAvailable) {
        CloseHandle(rb->hEventDataAvailable);
        rb->hEventDataAvailable = NULL;
    }

    if (rb->hEventSpaceAvailable) {
        CloseHandle(rb->hEventSpaceAvailable);
        rb->hEventSpaceAvailable = NULL;
    }

    if (rb->isInitialized) DeleteCriticalSection(&rb->cs);
    memset(rb, 0, sizeof(*rb));
}

size_t ringbuffer_write(RingBuffer* rb, const void* src, size_t bytesToWrite, DWORD timeoutMs) {
    if (!rb || !rb->isInitialized || !src || bytesToWrite == 0) return 0;
    const uint8_t* pSrc = (const uint8_t*)src;
    size_t totalWritten = 0;
    DWORD started = GetTickCount();

    while (bytesToWrite > 0) {
        EnterCriticalSection(&rb->cs);
        if (rb->isFinished) {
            LeaveCriticalSection(&rb->cs);
            break;
        }

        size_t space = rb->capacity - rb->count;
        if (space == 0) {
            LeaveCriticalSection(&rb->cs);
            DWORD elapsed = GetTickCount() - started;
            DWORD remaining = timeoutMs == INFINITE ? INFINITE : (elapsed < timeoutMs ? timeoutMs - elapsed : 0);
            DWORD waitRes = WaitForSingleObject(rb->hEventSpaceAvailable, remaining);
            if (waitRes != WAIT_OBJECT_0) {
                break; /* Timeout or error */
            }
            continue;
        }

        size_t chunk = (bytesToWrite < space) ? bytesToWrite : space;
        size_t part1 = rb->capacity - rb->writePos;
        if (chunk <= part1) {
            memcpy(rb->buffer + rb->writePos, pSrc, chunk);
            rb->writePos = (rb->writePos + chunk) % rb->capacity;
        } else {
            memcpy(rb->buffer + rb->writePos, pSrc, part1);
            memcpy(rb->buffer, pSrc + part1, chunk - part1);
            rb->writePos = chunk - part1;
        }

        rb->count += chunk;
        pSrc += chunk;
        bytesToWrite -= chunk;
        totalWritten += chunk;

        SetEvent(rb->hEventDataAvailable);
        LeaveCriticalSection(&rb->cs);
    }

    return totalWritten;
}

size_t ringbuffer_read(RingBuffer* rb, void* dest, size_t bytesToRead, DWORD timeoutMs) {
    if (!rb || !rb->isInitialized || !dest || bytesToRead == 0) return 0;
    uint8_t* pDest = (uint8_t*)dest;
    size_t totalRead = 0;
    DWORD started = GetTickCount();

    while (bytesToRead > 0) {
        EnterCriticalSection(&rb->cs);
        if (rb->count == 0) {
            if (rb->isFinished) {
                LeaveCriticalSection(&rb->cs);
                break; /* No more data will ever arrive */
            }
            LeaveCriticalSection(&rb->cs);
            DWORD elapsed = GetTickCount() - started;
            DWORD remaining = timeoutMs == INFINITE ? INFINITE : (elapsed < timeoutMs ? timeoutMs - elapsed : 0);
            DWORD waitRes = WaitForSingleObject(rb->hEventDataAvailable, remaining);
            if (waitRes != WAIT_OBJECT_0) {
                break; /* Timeout */
            }
            continue;
        }

        size_t chunk = (bytesToRead < rb->count) ? bytesToRead : rb->count;
        size_t part1 = rb->capacity - rb->readPos;
        if (chunk <= part1) {
            memcpy(pDest, rb->buffer + rb->readPos, chunk);
            rb->readPos = (rb->readPos + chunk) % rb->capacity;
        } else {
            memcpy(pDest, rb->buffer + rb->readPos, part1);
            memcpy(pDest + part1, rb->buffer, chunk - part1);
            rb->readPos = chunk - part1;
        }

        rb->count -= chunk;
        pDest += chunk;
        bytesToRead -= chunk;
        totalRead += chunk;

        SetEvent(rb->hEventSpaceAvailable);
        LeaveCriticalSection(&rb->cs);
    }

    return totalRead;
}

void ringbuffer_set_finished(RingBuffer* rb) {
    if (!rb || !rb->isInitialized) return;
    EnterCriticalSection(&rb->cs);
    rb->isFinished = true;
    SetEvent(rb->hEventDataAvailable);
    SetEvent(rb->hEventSpaceAvailable);
    LeaveCriticalSection(&rb->cs);
}

bool ringbuffer_is_finished(RingBuffer* rb) {
    if (!rb || !rb->isInitialized) return true;
    EnterCriticalSection(&rb->cs);
    bool finished = rb->isFinished;
    LeaveCriticalSection(&rb->cs);
    return finished;
}

int ringbuffer_get_level_percent(RingBuffer* rb) {
    if (!rb || !rb->isInitialized || rb->capacity == 0) return 0;
    EnterCriticalSection(&rb->cs);
    int pct = (int)((rb->count * 100ULL) / rb->capacity);
    LeaveCriticalSection(&rb->cs);
    return pct;
}

void ringbuffer_reset(RingBuffer* rb) {
    if (!rb || !rb->isInitialized) return;
    EnterCriticalSection(&rb->cs);
    rb->readPos = 0;
    rb->writePos = 0;
    rb->count = 0;
    rb->isFinished = false;
    ResetEvent(rb->hEventDataAvailable);
    SetEvent(rb->hEventSpaceAvailable);
    LeaveCriticalSection(&rb->cs);
}
