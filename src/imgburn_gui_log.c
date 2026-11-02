#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_gui_log.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

_Static_assert(sizeof(wchar_t) == 2, "Win32 log text requires UTF-16 wchar_t");

static DWORD log_export_last_error(DWORD fallback) {
    DWORD error = GetLastError();
    return error ? error : fallback;
}

static bool log_export_result(DWORD code, DWORD* error) {
    if (error) *error = code;
    SetLastError(code);
    return code == ERROR_SUCCESS;
}

/* 同步写完每一段；成功的短写继续，成功但零进展视为写入失败。 */
static DWORD log_export_write_all(HANDLE file, const void* data, size_t bytes) {
    const BYTE* cursor = (const BYTE*)data;
    while (bytes) {
        DWORD wanted = bytes > MAXDWORD ? MAXDWORD : (DWORD)bytes, actual = 0;
        SetLastError(ERROR_SUCCESS);
        if (!WriteFile(file,cursor,wanted,&actual,NULL)) return log_export_last_error(ERROR_WRITE_FAULT);
        if (!actual || actual > wanted) return ERROR_WRITE_FAULT;
        cursor += actual;
        bytes -= actual;
    }
    return ERROR_SUCCESS;
}

bool imgburn_gui_log_save(const wchar_t* path, const wchar_t* text, size_t chars, DWORD* error) {
    if (!path || !path[0] || !text) return log_export_result(ERROR_INVALID_PARAMETER,error);
    if (chars > SIZE_MAX / sizeof(*text)) return log_export_result(ERROR_ARITHMETIC_OVERFLOW,error);
    if (wcsnlen(path,MAX_PATH) >= MAX_PATH) return log_export_result(ERROR_FILENAME_EXCED_RANGE,error);
    wchar_t destination[MAX_PATH], temporary[MAX_PATH];
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetFullPathNameW(path,MAX_PATH,destination,NULL);
    if (!length) return log_export_result(log_export_last_error(ERROR_INVALID_NAME),error);
    if (length >= MAX_PATH) return log_export_result(ERROR_FILENAME_EXCED_RANGE,error);

    static volatile LONG sequence = 0;
    DWORD code = ERROR_SUCCESS, process = GetCurrentProcessId();
    HANDLE file = INVALID_HANDLE_VALUE;
    bool created = false;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        LONG number = InterlockedIncrement(&sequence);
        int count = swprintf(temporary,MAX_PATH,L"%ls.partial.%lu.%ld",destination,process,number);
        if (count < 0 || count >= MAX_PATH) return log_export_result(ERROR_FILENAME_EXCED_RANGE,error);
        SetLastError(ERROR_SUCCESS);
        file = CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if (file != INVALID_HANDLE_VALUE) { created = true; break; }
        code = log_export_last_error(ERROR_WRITE_FAULT);
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) return log_export_result(code,error);
    }
    if (!created) return log_export_result(code,error);

    /* 只在临时文件完整落盘并关闭后发布；失败从不打开或截断既有目标。 */
    const BYTE bom[] = {0xFF,0xFE};
    code = log_export_write_all(file,bom,sizeof(bom));
    if (!code) code = log_export_write_all(file,text,chars * sizeof(*text));
    if (!code) {
        SetLastError(ERROR_SUCCESS);
        if (!FlushFileBuffers(file)) code = log_export_last_error(ERROR_WRITE_FAULT);
    }
    if (!code) {
        SetLastError(ERROR_SUCCESS);
        if (!CloseHandle(file)) code = log_export_last_error(ERROR_WRITE_FAULT);
        else file = INVALID_HANDLE_VALUE;
    }
    if (!code) {
        SetLastError(ERROR_SUCCESS);
        if (!MoveFileExW(temporary,destination,MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) code = log_export_last_error(ERROR_WRITE_FAULT);
    }
    /* 保留首个故障；正常Close失败时仍尝试清理句柄及临时文件。 */
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (code) DeleteFileW(temporary);
    return log_export_result(code,error);
}

bool imgburn_gui_log_copy(HWND owner, const wchar_t* text, size_t chars, DWORD* error) {
    if (!owner || !text) return log_export_result(ERROR_INVALID_PARAMETER,error);
    if (chars > SIZE_MAX / sizeof(*text) - 1) return log_export_result(ERROR_ARITHMETIC_OVERFLOW,error);
    SIZE_T bytes = (chars + 1) * sizeof(*text);
    SetLastError(ERROR_SUCCESS);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE,bytes);
    if (!memory) return log_export_result(log_export_last_error(ERROR_NOT_ENOUGH_MEMORY),error);
    DWORD code = ERROR_SUCCESS;
    bool locked = false, opened = false;
    SetLastError(ERROR_SUCCESS);
    wchar_t* target = (wchar_t*)GlobalLock(memory);
    if (!target) code = log_export_last_error(ERROR_NOT_ENOUGH_MEMORY);
    else {
        locked = true;
        if (chars) memcpy(target,text,chars * sizeof(*text));
        target[chars] = L'\0';
        /* 最后一次成功解锁也返回零，只有非零LastError才表示失败。 */
        SetLastError(ERROR_SUCCESS);
        BOOL remaining = GlobalUnlock(memory);
        DWORD unlockError = GetLastError();
        if (!remaining && unlockError) code = unlockError;
        else locked = false;
    }
    if (!code) {
        SetLastError(ERROR_SUCCESS);
        if (!OpenClipboard(owner)) code = log_export_last_error(ERROR_GEN_FAILURE);
        else opened = true;
    }
    if (!code) {
        SetLastError(ERROR_SUCCESS);
        if (!EmptyClipboard()) code = log_export_last_error(ERROR_GEN_FAILURE);
    }
    if (!code) {
        SetLastError(ERROR_SUCCESS);
        if (!SetClipboardData(CF_UNICODETEXT,memory)) code = log_export_last_error(ERROR_GEN_FAILURE);
        else memory = NULL;
    }
    /* Set成功即移交内存，即使随后Close失败也不能再释放系统持有的数据。 */
    if (memory) {
        if (locked) GlobalUnlock(memory);
        GlobalFree(memory);
    }
    if (opened) {
        SetLastError(ERROR_SUCCESS);
        if (!CloseClipboard()) {
            if (!code) code = log_export_last_error(ERROR_GEN_FAILURE);
            /* 保留首次失败，不让一次暂态关闭故障继续占用剪贴板。 */
            CloseClipboard();
        }
    }
    return log_export_result(code,error);
}
