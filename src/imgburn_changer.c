/**
 * @file imgburn_changer.c
 * @brief Optical Disc Medium Changer, Embedded Changer & Autoloader Controls
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_changer.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * Embedded Changer Implementations (MMC 0xA6 / 0xBD)
 * ------------------------------------------------------------------------- */

bool imgburn_changer_get_status(OpticalDrive *drive, EmbeddedChangerStatus *outStatus) {
    if (!drive || !outStatus) return false;
    memset(outStatus, 0, sizeof(*outStatus));

    /* MMC CDB 0xBD: MECHANISM STATUS
     * CDB: BD 00 00 00 00 00 00 00 [AllocLen MSB] [AllocLen LSB] 00 00
     */
    uint8_t cdb[12] = { 0xBD, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x40, 0, 0 };
    uint8_t buffer[64] = { 0 };

    if (!spti_send_cdb(drive, cdb, 12, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC)) {
        return false;
    }

    outStatus->currentSlot = buffer[0] & 0x1F;
    outStatus->isMechanismBusy = (buffer[1] & 0xE0) != 0;
    outStatus->isDoorOpen = (buffer[1] & 0x10) != 0;
    outStatus->totalSlots = buffer[4];

    if (outStatus->totalSlots > MAX_CHANGER_SLOTS) {
        outStatus->totalSlots = MAX_CHANGER_SLOTS;
    }

    /* Slot status table starts at byte 8 (4 bytes per slot) */
    for (uint8_t i = 0; i < outStatus->totalSlots; i++) {
        uint8_t offset = 8 + (i * 4);
        if (offset < sizeof(buffer)) {
            outStatus->slotOccupied[i] = (buffer[offset] & 0x80) != 0;
        }
    }

    return true;
}

bool imgburn_changer_load_slot(OpticalDrive *drive, uint8_t slotIndex) {
    if (!drive) return false;

    /* MMC CDB 0xA6: LOAD/UNLOAD MEDIUM
     * CDB: A6 00 00 00 [SlotIndex] 00 00 00 [Action: 1=Load] 00 00 00
     */
    uint8_t cdb[12] = {
        0xA6, 0x00, 0x00, 0x00, slotIndex, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00
    };

    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, 30);
}

bool imgburn_changer_unload(OpticalDrive *drive) {
    if (!drive) return false;

    /* MMC CDB 0xA6: LOAD/UNLOAD MEDIUM with Action 0 = Unload */
    uint8_t cdb[12] = {
        0xA6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };

    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, 30);
}

/* -------------------------------------------------------------------------
 * SCSI Medium Changer (Jukebox / Library) Implementations (SMC 0xA5 / 0xB8 / 0x07)
 * ------------------------------------------------------------------------- */

bool imgburn_medium_changer_initialize(OpticalDrive *drive) {
    if (!drive) return false;

    /* SMC CDB 0x07: INITIALIZE ELEMENT STATUS */
    uint8_t cdb[6] = { 0x07, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return spti_send_cdb(drive, cdb, 6, NULL, 0, false, 60);
}

bool imgburn_medium_changer_move(OpticalDrive *drive,
                                 uint16_t transportAddress,
                                 uint16_t sourceAddress,
                                 uint16_t destAddress,
                                 bool invert) {
    if (!drive) return false;

    /* SMC CDB 0xA5: MOVE MEDIUM
     * Byte 2..3: Transport Element Address
     * Byte 4..5: Source Element Address
     * Byte 6..7: Destination Element Address
     * Byte 10: Invert flag (bit 0)
     */
    uint8_t cdb[12] = { 0 };
    cdb[0] = 0xA5;
    cdb[2] = (uint8_t)(transportAddress >> 8);
    cdb[3] = (uint8_t)(transportAddress & 0xFF);
    cdb[4] = (uint8_t)(sourceAddress >> 8);
    cdb[5] = (uint8_t)(sourceAddress & 0xFF);
    cdb[6] = (uint8_t)(destAddress >> 8);
    cdb[7] = (uint8_t)(destAddress & 0xFF);
    cdb[10] = invert ? 0x01 : 0x00;

    return spti_send_cdb(drive, cdb, 12, NULL, 0, false, 60);
}

bool imgburn_medium_changer_read_elements(OpticalDrive *drive,
                                          ChangerElementType elementType,
                                          uint16_t startingAddress,
                                          uint16_t elementCount,
                                          ChangerElementDescriptor *outDescriptors,
                                          int *outCount) {
    if (!drive || !outDescriptors || !outCount || elementCount == 0) return false;
    *outCount = 0;

    /* SMC CDB 0xB8: READ ELEMENT STATUS
     * Byte 1: Element type code (bits 0..3) | VolTag (bit 4)
     * Byte 2..3: Starting Element Address
     * Byte 4..5: Number of Elements
     * Byte 7..9: Allocation length
     */
    uint8_t cdb[12] = { 0 };
    cdb[0] = 0xB8;
    cdb[1] = (uint8_t)(elementType & 0x0F);
    cdb[2] = (uint8_t)(startingAddress >> 8);
    cdb[3] = (uint8_t)(startingAddress & 0xFF);
    cdb[4] = (uint8_t)(elementCount >> 8);
    cdb[5] = (uint8_t)(elementCount & 0xFF);
    cdb[8] = 0x01; /* 256 bytes */
    cdb[9] = 0x00;

    uint8_t buffer[256] = { 0 };
    if (!spti_send_cdb(drive, cdb, 12, buffer, sizeof(buffer), true, DEFAULT_TIMEOUT_SEC)) {
        return false;
    }

    /* Element Status Page header (8 bytes) + Element Descriptor blocks (12 bytes each) */
    uint16_t byteCount = (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
    if (byteCount > 8) {
        int descOffset = 8 + 8; /* Header (8) + Element Status Page Header (8) */
        while (descOffset + 12 <= sizeof(buffer) && *outCount < (int)elementCount) {
            ChangerElementDescriptor *d = &outDescriptors[(*outCount)++];
            d->elementAddress = (buffer[descOffset] << 8) | buffer[descOffset + 1];
            d->isFull = (buffer[descOffset + 2] & 0x01) != 0;
            d->isExceptional = (buffer[descOffset + 2] & 0x04) != 0;
            d->isAccessible = (buffer[descOffset + 2] & 0x08) != 0;
            d->sourceElement = (buffer[descOffset + 10] << 8) | buffer[descOffset + 11];
            d->type = elementType;
            descOffset += 12;
        }
    }

    return true;
}

/* -------------------------------------------------------------------------
 * Robotics Autoloader Implementations
 * ------------------------------------------------------------------------- */

bool imgburn_autoloader_execute(OpticalDrive *drive, AutoloaderCommand cmd) {
    if (!drive) return false;

    switch (cmd) {
        case AUTOLOADER_CMD_LOAD: {
            /* Primera / Microboards Autoloader Load command (CDB 0xEA 0x01) */
            uint8_t cdb[6] = { 0xEA, 0x01, 0x00, 0x00, 0x00, 0x00 };
            return spti_send_cdb(drive, cdb, 6, NULL, 0, false, 45);
        }
        case AUTOLOADER_CMD_ACCEPT: {
            /* Eject tray and robotics arm moves disc to output spindle (CDB 0xEA 0x02) */
            uint8_t cdb[6] = { 0xEA, 0x02, 0x00, 0x00, 0x00, 0x00 };
            return spti_send_cdb(drive, cdb, 6, NULL, 0, false, 45);
        }
        case AUTOLOADER_CMD_REJECT: {
            /* Robotics arm moves failed disc to reject bin (CDB 0xEA 0x03) */
            uint8_t cdb[6] = { 0xEA, 0x03, 0x00, 0x00, 0x00, 0x00 };
            return spti_send_cdb(drive, cdb, 6, NULL, 0, false, 45);
        }
        case AUTOLOADER_CMD_CALIBRATE: {
            /* Robotics arm home and calibration (CDB 0xEA 0x04) */
            uint8_t cdb[6] = { 0xEA, 0x04, 0x00, 0x00, 0x00, 0x00 };
            return spti_send_cdb(drive, cdb, 6, NULL, 0, false, 60);
        }
        case AUTOLOADER_CMD_SELF_TEST: {
            /* Robotics diagnostic self test (CDB 0xEA 0x05) */
            uint8_t cdb[6] = { 0xEA, 0x05, 0x00, 0x00, 0x00, 0x00 };
            return spti_send_cdb(drive, cdb, 6, NULL, 0, false, 60);
        }
        default:
            return false;
    }
}
