/**
 * @file imgburn_mmc.h
 * @brief ImgBurn Core C - SCSI Multimedia Commands (MMC) Implementation
 *
 * Implements standard MMC-5 / MMC-6 commands for optical disc discovery,
 * track management, media status query, speed control, cache flush, and burning.
 */

#ifndef IMGBURN_MMC_H
#define IMGBURN_MMC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "imgburn_spti.h"
#include <stdint.h>
#include <stdbool.h>

/* Optical Disc Profiles (MMC-5 Table 94) */
typedef enum {
    MMC_PROFILE_NONE            = 0x0000,
    MMC_PROFILE_CD_ROM          = 0x0008,
    MMC_PROFILE_CD_R            = 0x0009,
    MMC_PROFILE_CD_RW           = 0x000A,
    MMC_PROFILE_DVD_ROM         = 0x0010,
    MMC_PROFILE_DVD_R_SEQ       = 0x0011,
    MMC_PROFILE_DVD_RAM         = 0x0012,
    MMC_PROFILE_DVD_RW_RO       = 0x0013,
    MMC_PROFILE_DVD_RW_SEQ      = 0x0014,
    MMC_PROFILE_DVD_R_DL_SEQ    = 0x0015,
    MMC_PROFILE_DVD_R_DL_JUMP   = 0x0016,
    MMC_PROFILE_DVD_RW_DL       = 0x0017,
    MMC_PROFILE_DVD_PLUS_RW     = 0x001A,
    MMC_PROFILE_DVD_PLUS_R      = 0x001B,
    MMC_PROFILE_DVD_PLUS_RW_DL  = 0x002A,
    MMC_PROFILE_DVD_PLUS_R_DL   = 0x002B,
    MMC_PROFILE_BD_ROM          = 0x0040,
    MMC_PROFILE_BD_R_SRM        = 0x0041,
    MMC_PROFILE_BD_R_RRM        = 0x0042,
    MMC_PROFILE_BD_RE           = 0x0043,
    MMC_PROFILE_UNKNOWN         = 0xFFFF
} MmcProfile;

/* Disc Status (READ DISC INFORMATION) */
typedef enum {
    DISC_STATUS_EMPTY           = 0,
    DISC_STATUS_APPENDABLE      = 1,
    DISC_STATUS_COMPLETE        = 2,
    DISC_STATUS_OTHER           = 3
} MmcDiscStatus;

/**
 * @brief Disc Information structure
 */
typedef struct {
    MmcDiscStatus discStatus;
    bool isErasable;
    uint16_t numSessions;
    uint8_t firstTrack;
    uint16_t lastTrack;
    uint8_t lastSessionStatus;
    uint32_t leadInLba;
    uint32_t leadOutLba;
    MmcProfile currentProfile;
    const char* profileName;
    uint16_t firstTrackLastSession; /* 包括尚未记录的最后会话首轨，来自响应字节5/10。 */
} MmcDiscInfo;

/**
 * @brief Track Information structure
 */
typedef struct {
    uint16_t trackNumber;
    uint16_t sessionNumber;
    bool isDamage;
    bool isCopy;
    bool isPacket;
    bool isBlank;
    bool nwaValid;
    uint32_t trackStartLba;
    uint32_t nextWritableAddress;
    uint32_t freeBlocks;
    uint32_t trackSize;
    bool isReserved, fixedPacket; /* RT/FP：区分不可见轨与已预留轨及固定包地址。 */
} MmcTrackInfo;

/**
 * @brief Test if unit is ready (SCSI 0x00)
 */
bool mmc_test_unit_ready(OpticalDrive* drive);

/**
 * @brief Standard SCSI Inquiry (SCSI 0x12)
 */
bool mmc_inquiry(OpticalDrive* drive, uint8_t* outBuffer, uint32_t bufSize);

/**
 * @brief Get Current Media Profile (SCSI 0x46 GET CONFIGURATION)
 */
MmcProfile mmc_get_current_profile(OpticalDrive* drive);

/**
 * @brief Get human-readable name of MMC profile
 */
const char* mmc_get_profile_name(MmcProfile profile);

/**
 * @brief Read Disc Capacity (SCSI 0x25 READ CAPACITY)
 */
bool mmc_read_capacity(OpticalDrive* drive, uint32_t* outLastLba, uint32_t* outBlockSize);

/* 只读 DVD 物理格式信息，返回介质实际层数。 */
bool mmc_read_dvd_layer_count(OpticalDrive* drive, unsigned* outLayers);
/* 将 PFI 的物理地址换算为逻辑层布局，并与已读取的数据容量核对。 */
typedef struct { unsigned layerCount; bool oppositeTrackPath; uint32_t layerBreakLba; } MmcDvdLayout;
bool mmc_read_dvd_layout(OpticalDrive* drive, uint32_t totalSectors, MmcDvdLayout* layout);
/* DVD+R DL写入能力和最小半径收尾；DVD±R DL共用L0容量查询，-R DL的容量固定。 */
typedef struct { uint32_t sectors; bool changeable; } MmcDvdLayerCapacity;
bool mmc_get_dvd_plus_r_dl_write_feature(OpticalDrive* drive);
bool mmc_read_dvd_layer_capacity(OpticalDrive* drive, MmcDvdLayerCapacity* capacity);
bool mmc_set_dvd_layer_capacity(OpticalDrive* drive, uint32_t sectors);
bool mmc_finalize_dvd_plus_r_dl(OpticalDrive* drive);

/**
 * @brief Read Disc Information (SCSI 0x51 READ DISC INFORMATION)
 */
bool mmc_read_disc_information(OpticalDrive* drive, MmcDiscInfo* outInfo);

/**
 * @brief Read Track Information (SCSI 0x52 READ TRACK INFORMATION)
 */
bool mmc_read_track_information(OpticalDrive* drive, uint32_t lbaOrTrack, bool isLba, MmcTrackInfo* outInfo);

/**
 * @brief Set Read/Write Speed in KB/sec (SCSI 0xBB SET CD SPEED)
 * @param readSpeedKbps 0xFFFF for Maximum
 * @param writeSpeedKbps 0xFFFF for Maximum
 */
bool mmc_set_speed(OpticalDrive* drive, uint16_t readSpeedKbps, uint16_t writeSpeedKbps);
/* CD/DVD使用SET CD SPEED，BD使用32位SET STREAMING；另一方向使用MAX参数。 */
bool mmc_set_read_speed(OpticalDrive* drive, MmcProfile profile, uint32_t readSpeedKbps);
/* 蓝光使用 SET STREAMING 的 32 位速率，CD/DVD 保留 SET CD SPEED。 */
bool mmc_set_burn_speed(OpticalDrive* drive, MmcProfile profile, uint32_t writeSpeedKbps);
/* 当前 BD Write 能力；返回设备是否支持 WRITE (12) 的 VNR 位。 */
bool mmc_get_bd_write_features(OpticalDrive* drive, bool* verifyNotRequired);
bool mmc_write_bd_blocks(OpticalDrive* drive, uint32_t startLba, uint16_t blockCount, const void* buffer, bool verifyNotRequired);

/**
 * @brief Synchronize Cache / Flush FIFO (SCSI 0x35)
 */
bool mmc_synchronize_cache(OpticalDrive* drive);

/**
 * @brief Close Track or Session (SCSI 0x5B)
 * @param closeType 1 为关闭轨道，2 为关闭会话，6 为结束 DVD+R/BD-R 光盘；含义需按介质区分。
 */
bool mmc_close_track_session(OpticalDrive* drive, uint8_t closeType, uint16_t trackNumber);

/**
 * @brief Blank / Erase rewritable media (SCSI 0xA1 BLANK)
 * @param blankType 0 = Blank entire disc, 1 = Minimal / Quick blank
 */
bool mmc_blank_disc(OpticalDrive* drive, uint8_t blankType);
/* 后台擦除仅支持整盘/最小擦除；进度使用 REQUEST SENSE，兼容忙时 TUR 返回成功的旧设备。 */
typedef struct { bool active, valid; uint16_t value; } MmcEraseProgress;
bool mmc_start_blank_disc(OpticalDrive* drive, bool full);
bool mmc_read_erase_progress(OpticalDrive* drive, MmcEraseProgress* progress);

/**
 * @brief Eject or Load Disc Tray (SCSI 0x1B START STOP UNIT)
 * @param eject True = Open Tray, False = Close Tray / Spin up
 */
bool mmc_load_eject(OpticalDrive* drive, bool eject);

/**
 * @brief Reserve Track (SCSI 0x53)
 */
bool mmc_reserve_track(OpticalDrive* drive, uint32_t reservationSizeLba);
/* 刻录期间禁止手动弹出托盘，结束后恢复。 */
bool mmc_prevent_removal(OpticalDrive* drive, bool prevent);
/* DVD-R（含DL）DAO能力和可恢复写入参数页；数据不包含MODE SELECT头，DL要求RDL位。 */
typedef struct { bool testWrite, bufferUnderrunFree; } MmcDvdWriteFeatures;
typedef struct { uint16_t size; uint8_t page[56]; } MmcWriteParameters;
bool mmc_get_dvd_write_features(OpticalDrive* drive, MmcDvdWriteFeatures* features);
/* CD-R 单轨 MODE1 SAO：设备生成导入、两秒预间隙和导出区。 */
typedef struct { bool testWrite, bufferUnderrunFree; uint32_t maxCueSheetBytes; bool rawRwSubchannel; } MmcCdWriteFeatures;
bool mmc_get_cd_write_features(OpticalDrive* drive, MmcCdWriteFeatures* features);
bool mmc_read_cd_write_capacity(OpticalDrive* drive, uint32_t* sectors);
bool mmc_send_cd_mode1_cue(OpticalDrive* drive, uint32_t sectors);
/* CD-TEXT 导入区从 track 0 查询，负 LBA 按 MMC 的 100 分钟回绕换算为绝对 MSF。 */
typedef struct { int32_t startLba; uint32_t sectorCount; uint8_t minute, second, frame; } MmcCdTextLeadIn;
bool mmc_read_cd_text_leadin(OpticalDrive* drive, MmcCdTextLeadIn* leadIn);
/* SAO 的 CUE form 41h 由设备生成主通道，每扇区只接收 96 字节原始 P-W。 */
bool mmc_write_cd_text_blocks(OpticalDrive* drive, int32_t startLba, uint16_t blockCount, const void* buffer);
/* SAO 的完整 CUE、负 LBA 两秒生成间隙及 CD-DA 主通道写入。 */
bool mmc_send_cd_cue(OpticalDrive* drive, const uint8_t* cue, uint32_t bytes);
bool mmc_write_cd_leadin_gap(OpticalDrive* drive);
bool mmc_write_cd_audio_blocks(OpticalDrive* drive, uint32_t startLba, uint16_t blockCount, const void* buffer);
bool mmc_write_cd_mode1_blocks(OpticalDrive* drive, uint32_t startLba, uint16_t blockCount, const void* buffer);
bool mmc_finish_cd_sao(OpticalDrive* drive);
bool mmc_get_write_parameters(OpticalDrive* drive, MmcWriteParameters* parameters);
bool mmc_set_write_parameters(OpticalDrive* drive, const MmcWriteParameters* parameters);
bool mmc_perform_opc(OpticalDrive* drive);

/**
 * @brief WRITE (10) 写入 2048 字节数据扇区，每次最多 32 扇区。
 */
bool mmc_write_blocks(OpticalDrive* drive, uint32_t startLba, uint16_t blockCount, const void* buffer);

/**
 * @brief Read 10/12-byte blocks (SCSI 0x28 READ 10 / 0xA8 READ 12)
 */
bool mmc_read_blocks(OpticalDrive* drive, uint32_t startLba, uint16_t blockCount, void* buffer);

/* 核对单会话、单轨 MODE1 CD 的 TOC、结束位置及原始扇区头。 */
bool mmc_check_iso_cd(OpticalDrive* drive, uint32_t sectors);

typedef struct { uint32_t startLba; uint8_t number, control; } MmcCdTrack;
typedef struct { unsigned trackCount; uint32_t leadOutLba; MmcCdTrack tracks[99]; } MmcCdToc;

/* Full TOC 保留每个完整会话的轨道范围和独立导出区，轨道索引指向下面的统一数组。 */
typedef struct { uint32_t leadOutLba; unsigned firstTrackIndex, trackCount; uint8_t number, firstTrack, lastTrack, discType; } MmcCdSession;
typedef struct { unsigned sessionCount, trackCount; uint32_t leadOutLba; MmcCdSession sessions[99]; MmcCdTrack tracks[99]; } MmcCdFullToc;
bool mmc_read_cd_full_toc(OpticalDrive* drive, MmcCdFullToc* toc);

/* 只读的下一会话数据范围；dataEndLba为排他终点，余量仅为报告的用户数据块预算。 */
typedef struct {
    uint16_t sessionNumber, trackNumber;
    uint32_t previousLeadOutLba, nextWritableAddress, dataStartLba, dataEndLba;
    uint32_t sourceSectors, availableDataSectors, remainingDataSectors, sectorBytes;
    uint8_t writeType, trackMode, dataBlockType;
} MmcCdAppendPlan;
/* 输入须为同一盘状态下的已解析快照，包括当前写参数页；不读取设备或选择写模式。
 * 仅确认TAO模式下空不可见轨的MODE1/2048数据范围；SAO的NWA/预间隙解释尚未支持。
 * 不生成导入区、预间隙或结束策略，不保证完整会话开销，不启用追加执行或合并旧文件树。 */
bool mmc_plan_cd_mode1_append(const MmcDiscInfo* disc, const MmcTrackInfo* nextTrack, const MmcCdFullToc* toc, const MmcWriteParameters* parameters, uint64_t sourceSectors, MmcCdAppendPlan* plan);

/* 单会话 CD 的完整 TOC；READ CD 同时返回原始主通道与格式化 Q。 */
bool mmc_read_cd_toc(OpticalDrive* drive, MmcCdToc* toc);
/* 返回去除READ TOC头的18字节CD-TEXT包；无文本时成功返回NULL和0，调用者用free释放。 */
bool mmc_read_cd_text(OpticalDrive* drive, uint8_t** packs, uint32_t* bytes);
/* READ SUB-CHANNEL 的盘级MCN及指定音轨ISRC；无有效值返回空串，失败也清空输出。 */
bool mmc_read_cd_catalog(OpticalDrive* drive, char catalog[14]);
bool mmc_read_cd_isrc(OpticalDrive* drive, unsigned track, char isrc[13]);
bool mmc_read_cd_blocks(OpticalDrive* drive, uint32_t lba, uint16_t count, void* buffer);
/* Verify 只请求 2352 字节主通道，不要求设备额外返回 Q。 */
bool mmc_read_cd_main_blocks(OpticalDrive* drive, uint32_t lba, uint16_t count, void* buffer);
bool mmc_read_cd_raw_sector(OpticalDrive* drive, uint32_t lba, bool withSubChannel, void* buffer);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_MMC_H */
