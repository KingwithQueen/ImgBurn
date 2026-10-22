/**
 * @file imgburn_vendor.h
 * @brief Optical Drive Vendor-Specific Extensions & BookType / Bitsetting
 *
 * Implements reverse-engineered vendor-specific SCSI/MMC commands for:
 * - BookType / Bitsetting (Lite-On, BenQ, LG, NEC/Optiarc, NuTech, Plextor, Ricoh, Samsung)
 * - Pioneer Drive Features (Optimal Writing Speed, PureRead 2/3/4, Quiet Mode acoustic control)
 * - Lite-On Advanced Controls (SmartBurn, Force/Online HyperTuning, OverSpeed, Clear OPC)
 * - BenQ Vendor Controls (SolidBurn, OverSpeed, Unlock/Lock)
 * - Samsung Magic Speed (Silent, Quiet, Maximum)
 *
 * Pure C99/C11, Win32 SPTI. No external dependencies.
 */

#ifndef IMGBURN_VENDOR_H
#define IMGBURN_VENDOR_H

#include "imgburn_spti.h"
#include "imgburn_mmc.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Drive Vendor Enumeration
 * ------------------------------------------------------------------------- */
typedef enum {
    DRIVE_VENDOR_UNKNOWN = 0,
    DRIVE_VENDOR_BENQ,       /* BenQ (QSuite, SolidBurn, OverSpeed) */
    DRIVE_VENDOR_LG,         /* LG Electronics / HL-DT-ST */
    DRIVE_VENDOR_LITEON,     /* Lite-On / PLDS / HP / Sony / TEAC (SmartBurn) */
    DRIVE_VENDOR_NEC,        /* NEC / Optiarc */
    DRIVE_VENDOR_NUTECH,     /* NuTech */
    DRIVE_VENDOR_PIONEER,    /* Pioneer (PureRead, QuietMode, Optimal Write Speed) */
    DRIVE_VENDOR_PLEXTOR,    /* Plextor (PlexTools, AutoStrategy, PowerRec) */
    DRIVE_VENDOR_RICOH,      /* Ricoh (+RTB bitsetting) */
    DRIVE_VENDOR_SAMSUNG     /* Samsung / TSSTcorp (Magic Speed) */
} drive_vendor_t;

/* -------------------------------------------------------------------------
 * BookType / Bitsetting Definitions
 * ------------------------------------------------------------------------- */
typedef enum {
    BOOKTYPE_MEDIA_DVD_PLUS_R    = 0,  /* DVD+R Single Layer */
    BOOKTYPE_MEDIA_DVD_PLUS_RW   = 1,  /* DVD+RW ReWritable */
    BOOKTYPE_MEDIA_DVD_PLUS_R_DL = 2   /* DVD+R Double Layer */
} booktype_media_t;

typedef enum {
    BOOKTYPE_SETTING_DVD_ROM     = 0x0, /* DVD-ROM (Maximum compatibility) */
    BOOKTYPE_SETTING_DVD_RAM     = 0x1, /* DVD-RAM */
    BOOKTYPE_SETTING_DVD_R       = 0x2, /* DVD-R */
    BOOKTYPE_SETTING_DVD_RW      = 0x3, /* DVD-RW */
    BOOKTYPE_SETTING_DVD_PLUS_RW = 0x9, /* DVD+RW */
    BOOKTYPE_SETTING_DVD_PLUS_R  = 0xE, /* DVD+R / DVD+R DL */
    BOOKTYPE_SETTING_UNKNOWN     = 0xFF
} booktype_setting_t;

/* -------------------------------------------------------------------------
 * Pioneer PureRead & Quiet Mode Definitions
 * ------------------------------------------------------------------------- */
typedef enum {
    PIONEER_PUREREAD_STANDARD = 0, /* Standard mode: audio interpolation active */
    PIONEER_PUREREAD_MASTER   = 1, /* Master mode: maximum retries before interpolation */
    PIONEER_PUREREAD_PERFECT  = 2  /* Perfect mode: bit-perfect only, fail on uncorrectable */
} pioneer_pureread_mode_t;

typedef enum {
    PIONEER_QUIET_PERFORMANCE = 0, /* Full speed, standard acoustic profile */
    PIONEER_QUIET_QUIET       = 1, /* Spindle speed throttled for minimum noise */
    PIONEER_QUIET_SMOOTH      = 2  /* Auto profile: silent during AV playback, fast for data */
} pioneer_quiet_mode_t;

/* -------------------------------------------------------------------------
 * Samsung Magic Speed Definitions
 * ------------------------------------------------------------------------- */
typedef enum {
    SAMSUNG_SPEED_MAXIMUM = 0, /* Full read/write speed */
    SAMSUNG_SPEED_QUIET   = 1, /* Medium speed / low acoustic profile */
    SAMSUNG_SPEED_SILENT  = 2  /* Minimum spindle noise / low speed */
} samsung_speed_mode_t;

/* -------------------------------------------------------------------------
 * Drive Vendor Detection API
 * ------------------------------------------------------------------------- */

/**
 * @brief Detect optical drive vendor from vendor & product strings
 * @param vendorId 8-char vendor string (e.g. "PIONEER ", "hp      ", "LITE-ON ")
 * @param productId 16-char product string (e.g. "DVD A  DH16ACSHR")
 * @return Detected drive vendor enum
 */
drive_vendor_t imgburn_vendor_detect(const char *vendorId, const char *productId);

/**
 * @brief Get human-readable vendor name string
 */
const char *imgburn_vendor_name(drive_vendor_t vendor);

/**
 * @brief Get human-readable booktype setting name string
 */
const char *imgburn_booktype_name(booktype_setting_t setting);

/* -------------------------------------------------------------------------
 * BookType / Bitsetting API
 * ------------------------------------------------------------------------- */

/**
 * @brief Query current BookType setting for specified media on the drive
 * @param drive Active SPTI drive handle
 * @param vendor Drive vendor (or DRIVE_VENDOR_UNKNOWN for auto-detect)
 * @param media Media category (DVD+R, DVD+RW, DVD+R DL)
 * @param out_setting Output pointer receiving current booktype setting
 * @return true on success, false otherwise
 */
bool imgburn_booktype_query(OpticalDrive *drive,
                            drive_vendor_t vendor,
                            booktype_media_t media,
                            booktype_setting_t *out_setting);

/**
 * @brief Set BookType setting for specified media on the drive
 * @param drive Active SPTI drive handle
 * @param vendor Drive vendor (or DRIVE_VENDOR_UNKNOWN for auto-detect)
 * @param media Media category to change
 * @param new_setting Target setting (typically BOOKTYPE_SETTING_DVD_ROM)
 * @param permanent True to write into drive non-volatile EEPROM; False for current session
 * @return true on success, false otherwise
 */
bool imgburn_booktype_set(OpticalDrive *drive,
                          drive_vendor_t vendor,
                          booktype_media_t media,
                          booktype_setting_t new_setting,
                          bool permanent);

/**
 * @brief Automatically query vendor and bitset DVD+R / DVD+R DL to DVD-ROM if supported
 * @param drive Active SPTI drive handle
 * @param media Media category currently loaded in drive
 * @return true on success, false if failed or unsupported
 */
bool imgburn_booktype_auto_dvd_rom(OpticalDrive *drive, booktype_media_t media);

/* -------------------------------------------------------------------------
 * Pioneer Feature Control API
 * ------------------------------------------------------------------------- */

/**
 * @brief Query Pioneer Optimal Writing Speed state
 * @param drive Active SPTI drive handle
 * @param out_enabled Returns true if Optimal Writing Speed is currently active
 * @param out_saved_to_drive Returns true if saved persistently into drive EEPROM
 */
bool imgburn_pioneer_get_optimal_speed(OpticalDrive *drive,
                                       bool *out_enabled,
                                       bool *out_saved_to_drive);

/**
 * @brief Configure Pioneer Optimal Writing Speed
 * @param drive Active SPTI drive handle
 * @param enable True to enable adaptive optimal write power, False to disable
 * @param save_to_drive True to save persistently into EEPROM
 */
bool imgburn_pioneer_set_optimal_speed(OpticalDrive *drive,
                                       bool enable,
                                       bool save_to_drive);

/**
 * @brief Query Pioneer PureRead audio error management mode
 * @param drive Active SPTI drive handle
 * @param out_mode Receives current PureRead mode (Standard, Master, Perfect)
 * @param out_supported Receives true if drive supports PureRead
 */
bool imgburn_pioneer_get_pureread(OpticalDrive *drive,
                                  pioneer_pureread_mode_t *out_mode,
                                  bool *out_supported);

/**
 * @brief Configure Pioneer PureRead audio error management mode
 * @param drive Active SPTI drive handle
 * @param mode Target PureRead mode
 * @param save_to_drive True to save persistently into EEPROM
 */
bool imgburn_pioneer_set_pureread(OpticalDrive *drive,
                                  pioneer_pureread_mode_t mode,
                                  bool save_to_drive);

/**
 * @brief Query Pioneer Quiet Mode (spindle acoustic management)
 */
bool imgburn_pioneer_get_quiet_mode(OpticalDrive *drive,
                                    pioneer_quiet_mode_t *out_mode);

/**
 * @brief Set Pioneer Quiet Mode
 */
bool imgburn_pioneer_set_quiet_mode(OpticalDrive *drive,
                                    pioneer_quiet_mode_t mode);

/* -------------------------------------------------------------------------
 * Lite-On Advanced Control API
 * ------------------------------------------------------------------------- */

/**
 * @brief Configure Lite-On SmartBurn (protects against buffer underrun & adjusts write strategy)
 */
bool imgburn_liteon_set_smartburn(OpticalDrive *drive, bool enable);

/**
 * @brief Configure Lite-On Force HyperTuning (forces dynamic recalibration on all media)
 */
bool imgburn_liteon_set_force_hypertuning(OpticalDrive *drive, bool enable);

/**
 * @brief Configure Lite-On Online HyperTuning (real-time walking OPC while burning)
 */
bool imgburn_liteon_set_online_hypertuning(OpticalDrive *drive, bool enable);

/**
 * @brief Configure Lite-On OverSpeed (allows burning above rated media speed)
 */
bool imgburn_liteon_set_overspeed(OpticalDrive *drive, bool enable);

/**
 * @brief Clear Lite-On OPC learning calibration history in EEPROM
 */
bool imgburn_liteon_clear_opc_history(OpticalDrive *drive);

/* -------------------------------------------------------------------------
 * BenQ Advanced Control API
 * ------------------------------------------------------------------------- */

/**
 * @brief BenQ vendor handshake unlock (CDB: FD F1 'BENQ')
 */
bool imgburn_benq_unlock(OpticalDrive *drive);

/**
 * @brief BenQ vendor handshake lock (CDB: FD F2 'BENQ')
 */
bool imgburn_benq_lock(OpticalDrive *drive);

/**
 * @brief Configure BenQ SolidBurn (WOPC learning for unknown / known media)
 */
bool imgburn_benq_set_solidburn(OpticalDrive *drive, bool known_media, bool enable);

/**
 * @brief Configure BenQ OverSpeed
 */
bool imgburn_benq_set_overspeed(OpticalDrive *drive, bool enable);

/* -------------------------------------------------------------------------
 * Samsung Magic Speed API
 * ------------------------------------------------------------------------- */

/**
 * @brief Set Samsung drive spindle acoustic profile / speed limit
 */
bool imgburn_samsung_set_magic_speed(OpticalDrive *drive, samsung_speed_mode_t mode);

/* -------------------------------------------------------------------------
 * Plextor Advanced Controls (PlexTools Commands)
 * ------------------------------------------------------------------------- */

/**
 * @brief Query Plextor AutoStrategy (dynamic write strategy generation)
 */
bool imgburn_plextor_get_autostrategy(OpticalDrive *drive, bool *supported, bool *enabled);

/**
 * @brief Set Plextor AutoStrategy
 */
bool imgburn_plextor_set_autostrategy(OpticalDrive *drive, bool enable);

/**
 * @brief Query Plextor PowerRec (optimum laser power verification)
 */
bool imgburn_plextor_get_powerrec(OpticalDrive *drive, bool *supported, bool *enabled);

/**
 * @brief Set Plextor PowerRec
 */
bool imgburn_plextor_set_powerrec(OpticalDrive *drive, bool enable);

/**
 * @brief Query Plextor GigaRec (CD capacity multiplication)
 */
bool imgburn_plextor_get_gigarec(OpticalDrive *drive, bool *supported, bool *enabled);

/**
 * @brief Set Plextor GigaRec
 */
bool imgburn_plextor_set_gigarec(OpticalDrive *drive, bool enable);

/**
 * @brief Configure Plextor VariRec (laser power fine offset: -4 to +4)
 */
bool imgburn_plextor_set_varirec(OpticalDrive *drive, int offsetLevel);

/**
 * @brief Configure Plextor SilentMode (spindle acoustic management)
 */
bool imgburn_plextor_set_silent_mode(OpticalDrive *drive, bool enable, int speedLimit);

/* -------------------------------------------------------------------------
 * OEM Drive Configuration / Alias Table API
 * ------------------------------------------------------------------------- */

/**
 * @brief Register custom OEM drive alias mapping (e.g. SONY DRU-830A -> Samsung)
 */
void imgburn_oem_register_alias(const char *driveIdSubstring, drive_vendor_t realVendor);

/**
 * @brief Clear all registered OEM drive aliases
 */
void imgburn_oem_clear_aliases(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_VENDOR_H */
