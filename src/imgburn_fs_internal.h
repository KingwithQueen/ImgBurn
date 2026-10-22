#ifndef IMGBURN_FS_INTERNAL_H
#define IMGBURN_FS_INTERNAL_H

#include "imgburn_fs.h"

/* 文件树扫描与元数据布局分离；布局只保存节点引用，不拥有源文件。 */
bool fs_layout_prepare(ImageBuilder* builder);
bool fs_layout_write(ImageBuilder* builder, HANDLE output);
void fs_layout_free(ImageBuilder* builder);

#endif
