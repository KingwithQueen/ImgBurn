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
    uint8_t numSessions;
    uint8_t firstTrack;
    uint8_t lastTrack;
    uint32_t leadInLba;
    uint32_t leadOutLba;
    MmcProfile currentProfile;
    const char* profileName;
} MmcDiscInfo;

/**
 * @brief Track Information structure
 */
typedef struct {
    uint8_t trackNumber;
    uint8_t sessionNumber;
    bool isDamage;
    bool isCopy;
    bool isPacket;
    bool isBlank;
    uint32_t trackStartLba;
    uint32_t nextWritableAddress;
    uint32_t freeBlocks;
    uint32_t trackSize;
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

/**
 * @brief Synchronize Cache / Flush FIFO (SCSI 0x35)
 */
bool mmc_synchronize_cache(OpticalDrive* drive);

/**
 * @brief Close Track or Session (SCSI 0x5B)
 * @param closeType 1 = Close Track, 2 = Close Session / Finalize
 */
bool mmc_close_track_session(OpticalDrive* drive, uint8_t closeType, uint16_t trackNumber);

/**
 * @brief Blank / Erase rewritable media (SCSI 0xA1 BLANK)
 * @param blankType 0 = Blank entire disc, 1 = Minimal / Quick blank
 */
bool mmc_blank_disc(OpticalDrive* drive, uint8_t blankType);

/**
 * @brief Eject or Load Disc Tray (SCSI 0x1B START STOP UNIT)
 * @param eject True = Open Tray, False = Close Tray / Spin up
 */
bool mmc_load_eject(OpticalDrive* drive, bool eject);

/**
 * @brief Reserve Track (SCSI 0x53)
 */
bool mmc_reserve_track(OpticalDrive* drive, uint32_t reservationSizeLba);

/**
 * @brief Write 10/12-byte blocks (SCSI 0x2A WRITE 10 / 0xAA WRITE 12)
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

/* 单会话 CD 的完整 TOC；READ CD 同时返回原始主通道与格式化 Q。 */
bool mmc_read_cd_toc(OpticalDrive* drive, MmcCdToc* toc);
bool mmc_read_cd_blocks(OpticalDrive* drive, uint32_t lba, uint16_t count, void* buffer);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_MMC_H */
