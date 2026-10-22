/**
 * @file imgburn_vendor.c
 * @brief Optical Drive Vendor-Specific Extensions & BookType / Bitsetting
 *
 * Implements reverse-engineered vendor-specific SCSI/MMC commands for:
 * - BookType / Bitsetting (Lite-On, BenQ, LG, NEC/Optiarc, NuTech, Plextor, Ricoh, Samsung)
 * - Pioneer Drive Features (Optimal Writing Speed, PureRead 2/3/4, Quiet Mode acoustic control)
 * - Lite-On Advanced Controls (SmartBurn, Force/Online HyperTuning, OverSpeed, Clear OPC)
 * - BenQ Vendor Controls (SolidBurn, OverSpeed, Unlock/Lock)
 * - Samsung Magic Speed (Silent, Quiet, Maximum)
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_vendor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Helper to trim leading/trailing whitespace */
static void str_trim(char *str) {
    char *p = str;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != str) memmove(str, p, strlen(p) + 1);
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        str[--len] = '\0';
    }
}

/* Case-insensitive string search */
static bool str_contains_nocase(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (_strnicmp(&haystack[i], needle, nlen) == 0) return true;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * OEM Drive Configuration / Alias Table
 * ------------------------------------------------------------------------- */
typedef struct {
    char idSubstring[64];
    drive_vendor_t realVendor;
} OemAliasEntry;

static OemAliasEntry s_oem_aliases[32];
static int s_oem_alias_count = 0;

void imgburn_oem_register_alias(const char *driveIdSubstring, drive_vendor_t realVendor) {
    if (!driveIdSubstring || s_oem_alias_count >= 32) return;
    strncpy(s_oem_aliases[s_oem_alias_count].idSubstring, driveIdSubstring, 63);
    s_oem_aliases[s_oem_alias_count].realVendor = realVendor;
    s_oem_alias_count++;
}

void imgburn_oem_clear_aliases(void) {
    s_oem_alias_count = 0;
    memset(s_oem_aliases, 0, sizeof(s_oem_aliases));
}

/* -------------------------------------------------------------------------
 * Vendor Detection
 * ------------------------------------------------------------------------- */
drive_vendor_t imgburn_vendor_detect(const char *vendorId, const char *productId) {
    char vendor[32] = { 0 };
    char product[64] = { 0 };

    if (vendorId) {
        strncpy(vendor, vendorId, sizeof(vendor) - 1);
        str_trim(vendor);
    }
    if (productId) {
        strncpy(product, productId, sizeof(product) - 1);
        str_trim(product);
    }

    /* Check OEM Aliases first */
    for (int i = 0; i < s_oem_alias_count; i++) {
        if (str_contains_nocase(vendor, s_oem_aliases[i].idSubstring) ||
            str_contains_nocase(product, s_oem_aliases[i].idSubstring)) {
            return s_oem_aliases[i].realVendor;
        }
    }

    /* Pioneer */
    if (str_contains_nocase(vendor, "PIONEER") ||
        str_contains_nocase(product, "DVR-") ||
        str_contains_nocase(product, "BDR-") ||
        (str_contains_nocase(product, "BD-RW") && str_contains_nocase(vendor, "PION"))) {
        return DRIVE_VENDOR_PIONEER;
    }

    /* Lite-On / PLDS / OEM models */
    if (str_contains_nocase(vendor, "LITE-ON") ||
        str_contains_nocase(vendor, "LITEON") ||
        str_contains_nocase(vendor, "PLDS") ||
        str_contains_nocase(product, "iHAS") ||
        str_contains_nocase(product, "iHAP") ||
        str_contains_nocase(product, "DH16") ||
        str_contains_nocase(product, "DH-16") ||
        str_contains_nocase(product, "DH20") ||
        str_contains_nocase(product, "DH-20") ||
        str_contains_nocase(product, "DH24") ||
        str_contains_nocase(product, "DH-24") ||
        str_contains_nocase(product, "DVDRW LH-") ||
        str_contains_nocase(product, "DVD+-RW DH")) {
        return DRIVE_VENDOR_LITEON;
    }

    /* LG / HL-DT-ST */
    if (str_contains_nocase(vendor, "HL-DT-ST") ||
        str_contains_nocase(vendor, "LG") ||
        str_contains_nocase(product, "GSA-") ||
        str_contains_nocase(product, "WH16") ||
        str_contains_nocase(product, "BH16") ||
        str_contains_nocase(product, "WH14") ||
        str_contains_nocase(product, "BH14")) {
        return DRIVE_VENDOR_LG;
    }

    /* NEC / Optiarc */
    if (str_contains_nocase(vendor, "NEC") ||
        str_contains_nocase(vendor, "OPTIARC") ||
        str_contains_nocase(product, "ND-") ||
        str_contains_nocase(product, "AD-") ||
        str_contains_nocase(product, "Optiarc")) {
        return DRIVE_VENDOR_NEC;
    }

    /* BenQ / Philips */
    if (str_contains_nocase(vendor, "BENQ") ||
        str_contains_nocase(product, "DW16") ||
        str_contains_nocase(product, "DW18") ||
        str_contains_nocase(product, "DW20") ||
        str_contains_nocase(vendor, "PBDS")) {
        return DRIVE_VENDOR_BENQ;
    }

    /* Samsung / TSSTcorp */
    if (str_contains_nocase(vendor, "SAMSUNG") ||
        str_contains_nocase(vendor, "TSSTcorp") ||
        str_contains_nocase(product, "SH-S") ||
        str_contains_nocase(product, "SH-2")) {
        return DRIVE_VENDOR_SAMSUNG;
    }

    /* Plextor */
    if (str_contains_nocase(vendor, "PLEXTOR") ||
        str_contains_nocase(product, "PX-")) {
        return DRIVE_VENDOR_PLEXTOR;
    }

    /* Ricoh */
    if (str_contains_nocase(vendor, "RICOH")) {
        return DRIVE_VENDOR_RICOH;
    }

    /* NuTech */
    if (str_contains_nocase(vendor, "NUTECH")) {
        return DRIVE_VENDOR_NUTECH;
    }

    return DRIVE_VENDOR_UNKNOWN;
}

const char *imgburn_vendor_name(drive_vendor_t vendor) {
    switch (vendor) {
        case DRIVE_VENDOR_BENQ:    return "BenQ";
        case DRIVE_VENDOR_LG:      return "LG / HL-DT-ST";
        case DRIVE_VENDOR_LITEON:  return "LITE-ON";
        case DRIVE_VENDOR_NEC:     return "NEC / Optiarc";
        case DRIVE_VENDOR_NUTECH:  return "NuTech";
        case DRIVE_VENDOR_PIONEER: return "Pioneer";
        case DRIVE_VENDOR_PLEXTOR: return "Plextor";
        case DRIVE_VENDOR_RICOH:   return "Ricoh";
        case DRIVE_VENDOR_SAMSUNG: return "Samsung";
        default:                   return "Unknown Vendor";
    }
}

const char *imgburn_booktype_name(booktype_setting_t setting) {
    switch (setting) {
        case BOOKTYPE_SETTING_DVD_ROM:     return "DVD-ROM";
        case BOOKTYPE_SETTING_DVD_RAM:     return "DVD-RAM";
        case BOOKTYPE_SETTING_DVD_R:       return "DVD-R";
        case BOOKTYPE_SETTING_DVD_RW:      return "DVD-RW";
        case BOOKTYPE_SETTING_DVD_PLUS_RW: return "DVD+RW";
        case BOOKTYPE_SETTING_DVD_PLUS_R:  return "DVD+R";
        default:                           return "Unknown / Default";
    }
}

/* -------------------------------------------------------------------------
 * Internal Auto-Detect Helper
 * ------------------------------------------------------------------------- */
static drive_vendor_t resolve_vendor(OpticalDrive *drive, drive_vendor_t vendor) {
    if (vendor != DRIVE_VENDOR_UNKNOWN) return vendor;
    if (!drive) return DRIVE_VENDOR_UNKNOWN;

    /* If drive->info already has vendor/product */
    if (drive->info.vendorId[0] != '\0' || drive->info.productId[0] != '\0') {
        return imgburn_vendor_detect(drive->info.vendorId, drive->info.productId);
    }

    /* Otherwise execute inquiry */
    uint8_t inqBuf[36] = { 0 };
    if (mmc_inquiry(drive, inqBuf, sizeof(inqBuf))) {
        char v[9] = { 0 }, p[17] = { 0 };
        memcpy(v, &inqBuf[8], 8);
        memcpy(p, &inqBuf[16], 16);
        return imgburn_vendor_detect(v, p);
    }
    return DRIVE_VENDOR_UNKNOWN;
}

/* -------------------------------------------------------------------------
 * BookType / Bitsetting Implementations
 * ------------------------------------------------------------------------- */

/* BenQ Vendor Unlock: CDB: FD F1 'B' 'E' 'N' 'Q' */
bool imgburn_benq_unlock(OpticalDrive *drive) {
    if (!drive) return false;
    uint8_t cdb[12] = { 0xFD, 0xF1, 'B', 'E', 'N', 'Q', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
}

/* BenQ Vendor Lock: CDB: FD F2 'B' 'E' 'N' 'Q' */
bool imgburn_benq_lock(OpticalDrive *drive) {
    if (!drive) return false;
    uint8_t cdb[12] = { 0xFD, 0xF2, 'B', 'E', 'N', 'Q', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_booktype_query(OpticalDrive *drive,
                            drive_vendor_t vendor,
                            booktype_media_t media,
                            booktype_setting_t *out_setting) {
    if (!drive || !out_setting) return false;
    *out_setting = BOOKTYPE_SETTING_UNKNOWN;

    vendor = resolve_vendor(drive, vendor);

    switch (vendor) {
        case DRIVE_VENDOR_LITEON: {
            /* Lite-On Query: CDB 0xEB 0x01 (Query Bitsetting) */
            uint8_t cdb[12] = { 0xEB, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            uint8_t buffer[8] = { 0 };
            if (spti_send_cdb(drive, cdb, 12, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC)) {
                uint8_t raw = 0xFF;
                if (media == BOOKTYPE_MEDIA_DVD_PLUS_R) raw = buffer[0];
                else if (media == BOOKTYPE_MEDIA_DVD_PLUS_RW) raw = buffer[1];
                else if (media == BOOKTYPE_MEDIA_DVD_PLUS_R_DL) raw = buffer[2];

                if (raw == 0x00) *out_setting = BOOKTYPE_SETTING_DVD_ROM;
                else if (raw == 0x0E) *out_setting = BOOKTYPE_SETTING_DVD_PLUS_R;
                else if (raw == 0x09) *out_setting = BOOKTYPE_SETTING_DVD_PLUS_RW;
                return true;
            }

            /* Fallback: CDB 0xFF 0x00 (ImgBurn reversed 0x5B6C78) */
            uint8_t cdb_ff[12] = { 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            uint8_t buf_ff[128] = { 0 };
            if (spti_send_cdb(drive, cdb_ff, 12, buf_ff, sizeof(buf_ff), true, DEFAULT_TIMEOUT_SEC)) {
                uint8_t val = (buf_ff[0] >> 4) & 0x0F;
                *out_setting = (booktype_setting_t)val;
                return true;
            }
            return false;
        }

        case DRIVE_VENDOR_LG: {
            /* LG Query: CDB 0xE9 0x00 0x22 ... (ImgBurn reversed 0x5B9598) */
            uint8_t cdb[12] = { 0xE9, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00 };
            uint8_t buffer[8] = { 0 };
            if (spti_send_cdb(drive, cdb, 12, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC) && buffer[0] == 0x22) {
                if (media == BOOKTYPE_MEDIA_DVD_PLUS_R_DL) {
                    *out_setting = (buffer[2] & 0x02) ? BOOKTYPE_SETTING_DVD_ROM : BOOKTYPE_SETTING_DVD_PLUS_R;
                } else {
                    *out_setting = (buffer[2] & 0x01) ? BOOKTYPE_SETTING_DVD_ROM : BOOKTYPE_SETTING_DVD_PLUS_R;
                }
                return true;
            }
            return false;
        }

        case DRIVE_VENDOR_BENQ: {
            imgburn_benq_unlock(drive);
            uint8_t cdb[12] = { 0xAD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00 };
            uint8_t buffer[4] = { 0 };
            bool ok = spti_send_cdb(drive, cdb, 12, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC);
            imgburn_benq_lock(drive);
            if (ok) {
                *out_setting = (booktype_setting_t)((buffer[0] >> 4) & 0x0F);
                return true;
            }
            return false;
        }

        case DRIVE_VENDOR_NEC: {
            /* NEC Query: CDB 0xFF 0x10 0x01 (ImgBurn reversed 0x5B6CDE) */
            uint8_t cdb[12] = { 0xFF, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            uint8_t buffer[128] = { 0 };
            if (spti_send_cdb(drive, cdb, 12, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC)) {
                *out_setting = (booktype_setting_t)((buffer[0] >> 4) & 0x0F);
                return true;
            }
            return false;
        }

        case DRIVE_VENDOR_RICOH:
        case DRIVE_VENDOR_SAMSUNG:
        case DRIVE_VENDOR_PLEXTOR:
        case DRIVE_VENDOR_NUTECH: {
            /* Standard Read DVD Structure Physical Format Information */
            uint8_t cdb[12] = { 0xAD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00 };
            uint8_t buffer[4] = { 0 };
            if (spti_send_cdb(drive, cdb, 12, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC)) {
                *out_setting = (booktype_setting_t)((buffer[0] >> 4) & 0x0F);
                return true;
            }
            return false;
        }

        default:
            return false;
    }
}

bool imgburn_booktype_set(OpticalDrive *drive,
                          drive_vendor_t vendor,
                          booktype_media_t media,
                          booktype_setting_t new_setting,
                          bool permanent) {
    if (!drive) return false;
    vendor = resolve_vendor(drive, vendor);

    uint8_t target_val = (new_setting == BOOKTYPE_SETTING_DVD_ROM) ? 0x00 : 0x0E;

    switch (vendor) {
        case DRIVE_VENDOR_LITEON: {
            /* Lite-On Set BookType: CDB 0xEB 0x00
             * CDB[2]: target booktype (0x00 = DVD-ROM)
             * CDB[3]: media type (1 = DVD+R, 2 = DVD+RW, 3 = DVD+R DL)
             * CDB[4]: persistence (0 = Session, 1 = EEPROM)
             */
            uint8_t media_code = 1;
            if (media == BOOKTYPE_MEDIA_DVD_PLUS_RW) media_code = 2;
            else if (media == BOOKTYPE_MEDIA_DVD_PLUS_R_DL) media_code = 3;

            uint8_t cdb[12] = {
                0xEB, 0x00, target_val, media_code, (uint8_t)(permanent ? 1 : 0),
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            };
            return spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
        }

        case DRIVE_VENDOR_LG: {
            /* LG Set BookType: CDB 0xE9 0x10 0x22 (ImgBurn reversed 0x5B9715 & 0x5B9766)
             * Subcode 0x0A: DVD+R DL
             * Subcode 0x0E: DVD+R
             */
            uint8_t sub = (media == BOOKTYPE_MEDIA_DVD_PLUS_R_DL) ? 0x0A : 0x0E;
            uint8_t val = (new_setting == BOOKTYPE_SETTING_DVD_ROM) ? 1 : 0;
            uint8_t cdb[12] = {
                0xE9, 0x10, 0x22, sub, 0x00, val, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00
            };
            return spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
        }

        case DRIVE_VENDOR_BENQ: {
            /* BenQ: Unlock -> Mode Select -> Lock */
            imgburn_benq_unlock(drive);

            uint8_t cdb[12] = {
                0xDF, 0x0F, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            };
            bool ok = spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
            imgburn_benq_lock(drive);
            return ok;
        }

        case DRIVE_VENDOR_RICOH:
        case DRIVE_VENDOR_SAMSUNG:
        case DRIVE_VENDOR_PLEXTOR:
        case DRIVE_VENDOR_NUTECH: {
            /* Ricoh +RTB Standard (ImgBurn reversed 0x5B7177):
             * CDB: FC 00 2B 52 54 42 00 00 00 04 00 00
             * Data: [0xA0, 0x00, 0x00, 0x00] for DVD-ROM
             */
            uint8_t cdb[12] = {
                0xFC, 0x00, 0x2B, 0x52, 0x54, 0x42, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00
            };
            uint8_t payload[4] = { 0 };
            payload[0] = (new_setting == BOOKTYPE_SETTING_DVD_ROM) ? 0xA0 : 0x00;
            return spti_send_cdb(drive, cdb, 12, payload, 4, false, DEFAULT_TIMEOUT_SEC);
        }

        default:
            return false;
    }
}

bool imgburn_booktype_auto_dvd_rom(OpticalDrive *drive, booktype_media_t media) {
    if (!drive) return false;
    drive_vendor_t vendor = resolve_vendor(drive, DRIVE_VENDOR_UNKNOWN);
    if (vendor == DRIVE_VENDOR_UNKNOWN) return false;

    return imgburn_booktype_set(drive, vendor, media, BOOKTYPE_SETTING_DVD_ROM, false);
}

/* -------------------------------------------------------------------------
 * Pioneer Feature Controls
 * ------------------------------------------------------------------------- */

bool imgburn_pioneer_get_optimal_speed(OpticalDrive *drive,
                                       bool *out_enabled,
                                       bool *out_saved_to_drive) {
    if (!drive || !out_enabled || !out_saved_to_drive) return false;

    /* Pioneer READ BUFFER: CDB 0x3C, Mode 0x01, Buffer ID 0xF4, Length 256
     * (Reversed from ImgBurn 0x5B650F)
     */
    uint8_t cdb[10] = { 0x3C, 0x01, 0xF4, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
    uint8_t buffer[256] = { 0 };

    if (!spti_send_cdb(drive, cdb, 10, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC)) {
        return false;
    }

    /* Byte 19: Optimal Writing Speed current setting
     * Byte 20: Optimal Writing Speed saved-to-drive setting
     */
    *out_enabled = (buffer[19] != 0);
    *out_saved_to_drive = (buffer[20] != 0);
    return true;
}

bool imgburn_pioneer_set_optimal_speed(OpticalDrive *drive,
                                       bool enable,
                                       bool save_to_drive) {
    if (!drive) return false;

    /* Pioneer WRITE BUFFER: CDB 0x3B, Mode 0x01, Buffer ID 0xFA, Length 256
     * (Reversed from ImgBurn 0x5B6627 & 0x5B66C3)
     */
    uint8_t cdb[10] = { 0x3B, 0x01, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
    uint8_t payload[256] = { 0 };

    payload[0] = 0x09; /* Feature ID 9: Optimal Writing Speed */
    payload[1] = 0x00;
    payload[2] = enable ? 1 : 0;
    payload[3] = save_to_drive ? 1 : 0;

    return spti_send_cdb(drive, cdb, 10, payload, sizeof(payload), false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_pioneer_get_pureread(OpticalDrive *drive,
                                  pioneer_pureread_mode_t *out_mode,
                                  bool *out_supported) {
    if (!drive || !out_mode || !out_supported) return false;
    *out_supported = false;
    *out_mode = PIONEER_PUREREAD_STANDARD;

    /* Pioneer READ BUFFER: CDB 0x3C, Mode 0x01, Buffer ID 0xFB (or 0xF4) */
    uint8_t cdb[10] = { 0x3C, 0x01, 0xFB, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
    uint8_t buffer[256] = { 0 };

    if (spti_send_cdb(drive, cdb, 10, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC)) {
        *out_supported = (buffer[4] & 0x01) != 0;
        *out_mode = (pioneer_pureread_mode_t)(buffer[5] & 0x03);
        return true;
    }

    /* Fallback to Buffer ID 0xF4 */
    cdb[2] = 0xF4;
    if (spti_send_cdb(drive, cdb, 10, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC)) {
        *out_supported = (buffer[21] != 0);
        *out_mode = (pioneer_pureread_mode_t)(buffer[22] & 0x03);
        return true;
    }
    return false;
}

bool imgburn_pioneer_set_pureread(OpticalDrive *drive,
                                  pioneer_pureread_mode_t mode,
                                  bool save_to_drive) {
    if (!drive) return false;

    /* Pioneer WRITE BUFFER: CDB 0x3B, Mode 0x01, Buffer ID 0xFA, Length 256 */
    uint8_t cdb[10] = { 0x3B, 0x01, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
    uint8_t payload[256] = { 0 };

    payload[0] = 0x0A; /* PureRead Feature ID 10 */
    payload[1] = 0x00;
    payload[2] = (uint8_t)(mode & 0x03);
    payload[3] = save_to_drive ? 1 : 0;

    return spti_send_cdb(drive, cdb, 10, payload, sizeof(payload), false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_pioneer_get_quiet_mode(OpticalDrive *drive,
                                    pioneer_quiet_mode_t *out_mode) {
    if (!drive || !out_mode) return false;
    *out_mode = PIONEER_QUIET_PERFORMANCE;

    uint8_t cdb[10] = { 0x3C, 0x01, 0xF4, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
    uint8_t buffer[256] = { 0 };

    if (spti_send_cdb(drive, cdb, 10, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC)) {
        *out_mode = (pioneer_quiet_mode_t)(buffer[17] & 0x03);
        return true;
    }
    return false;
}

bool imgburn_pioneer_set_quiet_mode(OpticalDrive *drive,
                                    pioneer_quiet_mode_t mode) {
    if (!drive) return false;

    uint8_t cdb[10] = { 0x3B, 0x01, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
    uint8_t payload[256] = { 0 };

    payload[0] = 0x08; /* Quiet Mode Feature ID 8 */
    payload[1] = 0x00;
    payload[2] = (uint8_t)(mode & 0x03);

    return spti_send_cdb(drive, cdb, 10, payload, sizeof(payload), false, DEFAULT_TIMEOUT_SEC);
}

/* -------------------------------------------------------------------------
 * Lite-On Advanced Controls
 * ------------------------------------------------------------------------- */

bool imgburn_liteon_set_smartburn(OpticalDrive *drive, bool enable) {
    if (!drive) return false;
    uint8_t cdb[12] = { 0xEB, 0x80, (uint8_t)(enable ? 1 : 0), 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_liteon_set_force_hypertuning(OpticalDrive *drive, bool enable) {
    if (!drive) return false;
    uint8_t cdb[12] = { 0xEB, 0x81, (uint8_t)(enable ? 1 : 0), 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_liteon_set_online_hypertuning(OpticalDrive *drive, bool enable) {
    if (!drive) return false;
    uint8_t cdb[12] = { 0xEB, 0x82, (uint8_t)(enable ? 1 : 0), 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_liteon_set_overspeed(OpticalDrive *drive, bool enable) {
    if (!drive) return false;
    uint8_t cdb[12] = { 0xEB, 0x83, (uint8_t)(enable ? 1 : 0), 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_liteon_clear_opc_history(OpticalDrive *drive) {
    if (!drive) return false;
    uint8_t cdb[12] = { 0xEB, 0x84, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
}

/* -------------------------------------------------------------------------
 * BenQ Advanced Controls
 * ------------------------------------------------------------------------- */

bool imgburn_benq_set_solidburn(OpticalDrive *drive, bool known_media, bool enable) {
    if (!drive) return false;
    imgburn_benq_unlock(drive);

    uint8_t sub = known_media ? 0x01 : 0x02;
    uint8_t cdb[12] = { 0xFD, 0x03, sub, (uint8_t)(enable ? 1 : 0), 0, 0, 0, 0, 0, 0, 0, 0 };
    bool ok = spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);

    imgburn_benq_lock(drive);
    return ok;
}

bool imgburn_benq_set_overspeed(OpticalDrive *drive, bool enable) {
    if (!drive) return false;
    imgburn_benq_unlock(drive);

    uint8_t cdb[12] = { 0xFD, 0x04, 0x01, (uint8_t)(enable ? 1 : 0), 0, 0, 0, 0, 0, 0, 0, 0 };
    bool ok = spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);

    imgburn_benq_lock(drive);
    return ok;
}

/* -------------------------------------------------------------------------
 * Samsung Magic Speed
 * ------------------------------------------------------------------------- */

bool imgburn_samsung_set_magic_speed(OpticalDrive *drive, samsung_speed_mode_t mode) {
    if (!drive) return false;

    /* Samsung Magic Speed CDB 0xED (ImgBurn reversed 0x5B6754) */
    uint8_t val = (mode == SAMSUNG_SPEED_MAXIMUM) ? 0x00 :
                  (mode == SAMSUNG_SPEED_QUIET)   ? 0x01 : 0x02;
    uint8_t cdb[12] = { 0xED, 0x00, val, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, DEFAULT_TIMEOUT_SEC);
}

/* -------------------------------------------------------------------------
 * Plextor Advanced Controls (Mode Page 0x30 / 0x21)
 * ------------------------------------------------------------------------- */

bool imgburn_plextor_get_autostrategy(OpticalDrive *drive, bool *supported, bool *enabled) {
    if (!drive || !supported || !enabled) return false;
    *supported = false; *enabled = false;

    uint8_t cdb[10] = { 0x5A, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    uint8_t buf[24] = { 0 };
    if (!spti_send_cdb(drive, cdb, 10, buf, sizeof(buf), true, DEFAULT_TIMEOUT_SEC)) return false;

    *supported = (buf[10] & 0x02) != 0;
    *enabled = (buf[11] & 0x02) != 0;
    return true;
}

bool imgburn_plextor_set_autostrategy(OpticalDrive *drive, bool enable) {
    if (!drive) return false;
    uint8_t cdbSense[10] = { 0x5A, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    uint8_t buf[24] = { 0 };
    if (!spti_send_cdb(drive, cdbSense, 10, buf, sizeof(buf), true, DEFAULT_TIMEOUT_SEC)) return false;

    buf[0] = 0; buf[1] = 0;
    if (enable) buf[11] |= 0x02;
    else buf[11] &= ~0x02;

    uint8_t cdbSelect[10] = { 0x55, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    return spti_send_cdb(drive, cdbSelect, 10, buf, sizeof(buf), false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_plextor_get_powerrec(OpticalDrive *drive, bool *supported, bool *enabled) {
    if (!drive || !supported || !enabled) return false;
    *supported = false; *enabled = false;

    uint8_t cdb[10] = { 0x5A, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    uint8_t buf[24] = { 0 };
    if (!spti_send_cdb(drive, cdb, 10, buf, sizeof(buf), true, DEFAULT_TIMEOUT_SEC)) return false;

    *supported = (buf[10] & 0x08) != 0;
    *enabled = (buf[11] & 0x08) != 0;
    return true;
}

bool imgburn_plextor_set_powerrec(OpticalDrive *drive, bool enable) {
    if (!drive) return false;
    uint8_t cdbSense[10] = { 0x5A, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    uint8_t buf[24] = { 0 };
    if (!spti_send_cdb(drive, cdbSense, 10, buf, sizeof(buf), true, DEFAULT_TIMEOUT_SEC)) return false;

    buf[0] = 0; buf[1] = 0;
    if (enable) buf[11] |= 0x08;
    else buf[11] &= ~0x08;

    uint8_t cdbSelect[10] = { 0x55, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    return spti_send_cdb(drive, cdbSelect, 10, buf, sizeof(buf), false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_plextor_get_gigarec(OpticalDrive *drive, bool *supported, bool *enabled) {
    if (!drive || !supported || !enabled) return false;
    *supported = false; *enabled = false;

    uint8_t cdb[10] = { 0x5A, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    uint8_t buf[24] = { 0 };
    if (!spti_send_cdb(drive, cdb, 10, buf, sizeof(buf), true, DEFAULT_TIMEOUT_SEC)) return false;

    *supported = (buf[10] & 0x04) != 0;
    *enabled = (buf[11] & 0x04) != 0;
    return true;
}

bool imgburn_plextor_set_gigarec(OpticalDrive *drive, bool enable) {
    if (!drive) return false;
    uint8_t cdbSense[10] = { 0x5A, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    uint8_t buf[24] = { 0 };
    if (!spti_send_cdb(drive, cdbSense, 10, buf, sizeof(buf), true, DEFAULT_TIMEOUT_SEC)) return false;

    buf[0] = 0; buf[1] = 0;
    if (enable) buf[11] |= 0x04;
    else buf[11] &= ~0x04;

    uint8_t cdbSelect[10] = { 0x55, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    return spti_send_cdb(drive, cdbSelect, 10, buf, sizeof(buf), false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_plextor_set_varirec(OpticalDrive *drive, int offsetLevel) {
    if (!drive || offsetLevel < -4 || offsetLevel > 4) return false;
    uint8_t cdbSense[10] = { 0x5A, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    uint8_t buf[24] = { 0 };
    if (!spti_send_cdb(drive, cdbSense, 10, buf, sizeof(buf), true, DEFAULT_TIMEOUT_SEC)) return false;

    buf[0] = 0; buf[1] = 0;
    buf[12] = (uint8_t)(offsetLevel + 4);

    uint8_t cdbSelect[10] = { 0x55, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    return spti_send_cdb(drive, cdbSelect, 10, buf, sizeof(buf), false, DEFAULT_TIMEOUT_SEC);
}

bool imgburn_plextor_set_silent_mode(OpticalDrive *drive, bool enable, int speedLimit) {
    if (!drive) return false;
    uint8_t cdbSense[10] = { 0x5A, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    uint8_t buf[24] = { 0 };
    if (!spti_send_cdb(drive, cdbSense, 10, buf, sizeof(buf), true, DEFAULT_TIMEOUT_SEC)) return false;

    buf[0] = 0; buf[1] = 0;
    buf[13] = enable ? 0x01 : 0x00;
    buf[14] = (uint8_t)speedLimit;

    uint8_t cdbSelect[10] = { 0x55, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00 };
    return spti_send_cdb(drive, cdbSelect, 10, buf, sizeof(buf), false, DEFAULT_TIMEOUT_SEC);
}
