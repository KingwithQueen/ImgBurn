#ifndef IMGBURN_FS_INTERNAL_H
#define IMGBURN_FS_INTERNAL_H

#include "imgburn_fs.h"

/* 文件树扫描与元数据布局分离；布局只保存节点引用，不拥有源文件。 */
bool fs_layout_prepare(ImageBuilder* builder);
bool fs_layout_write(ImageBuilder* builder, HANDLE output);
void fs_layout_free(ImageBuilder* builder);
FsDirectoryNode* fs_ensure_directory(ImageBuilder* builder, const wchar_t* path);
bool fs_video_prepare(ImageBuilder* builder);
bool fs_video_finalize(ImageBuilder* builder);
int fs_video_file_order(const void* a, const void* b);
uint32_t fs_video_align_file(const ImageBuilder* builder, const FsFileNode* file, uint32_t lba);
void fs_video_patch(const FsFileNode* file, uint64_t offset, uint8_t* data, uint32_t bytes);

#endif
