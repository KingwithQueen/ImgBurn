/* 独立ISO9660会话的只读地址视图；源文件内容与物理长度保持不变。 */
#ifndef IMGBURN_ISO_SESSION_H
#define IMGBURN_ISO_SESSION_H
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    HANDLE file, cancelEvent;
    uint32_t sectors, volumeSectors, startLba;
    struct IsoSessionPatch* patches;
    size_t patchCount;
    bool joliet;
} IsoSessionSource;

/* 输入必须是从0寻址的独立2048字节ISO；不导入旧树，UDF/引导/未知目录扩展返回不支持。 */
bool imgburn_iso_session_open(IsoSessionSource* source, const wchar_t* path, HANDLE cancelEvent);
/* 每次均基于源地址设定，失败不改变原基址；Volume Space Size仍为本会话长度。 */
bool imgburn_iso_session_set_start(IsoSessionSource* source, uint32_t startLba);
bool imgburn_iso_session_read(IsoSessionSource* source, uint32_t sourceLba, uint16_t sectors, void* buffer);
void imgburn_iso_session_close(IsoSessionSource* source);
#endif
