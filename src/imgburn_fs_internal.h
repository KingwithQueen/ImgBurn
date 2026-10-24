#ifndef IMGBURN_FS_INTERNAL_H
#define IMGBURN_FS_INTERNAL_H

#include "imgburn_image.h"

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

/* 成品镜像保留文件树中的真实extent，经同一导航校验读取IFO与NAV。 */
typedef bool (*FsDvdReadCallback)(const FsFileNode* file, uint64_t offset, void* data, size_t bytes, void* context);
bool fs_dvd_read_navigation_ex(const ImageBuilder* builder, DvdNavigation* navigation, FsDvdReadCallback read, void* context);
bool fs_dvd_layer_break_candidates_fixed(const ImageBuilder* builder, const DvdMediaProfile* profile, DvdLayerBreakCandidate* candidates, size_t capacity, size_t* count, FsDvdReadCallback read, void* context);

#endif
