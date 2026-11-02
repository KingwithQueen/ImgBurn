#ifndef IMGBURN_GUI_LOG_H
#define IMGBURN_GUI_LOG_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

/* chars为UTF-16正文单元数，不含结尾NUL；error可空，返回时同步设置LastError。 */
bool imgburn_gui_log_save(const wchar_t* path, const wchar_t* text, size_t chars, DWORD* error);
/* 仅true表示复制全过程成功；Set成功而Close失败时，内存仍已归系统所有。 */
bool imgburn_gui_log_copy(HWND owner, const wchar_t* text, size_t chars, DWORD* error);

#endif
