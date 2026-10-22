/**
 * @file imgburn_changer.h
 * @brief Optical Disc Medium Changer, Embedded Changer & Autoloader Controls
 *
 * Implements:
 * - SCSI Medium Changer commands (MOVE MEDIUM 0xA5, READ ELEMENT STATUS 0xB8, INITIALIZE ELEMENT STATUS 0x07)
 * - MMC Embedded Changer commands (LOAD/UNLOAD MEDIUM 0xA6, MECHANISM STATUS 0xBD)
 * - Robotics Autoloader Commands (Load, Accept, Reject, Calibrate)
 *
 * Pure C99/C11, Win32 SPTI. No external dependencies.
 */

#ifndef IMGBURN_CHANGER_H
#define IMGBURN_CHANGER_H

#include "imgburn_spti.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CHANGER_SLOTS 64

/* -------------------------------------------------------------------------
 * Embedded Changer Definitions
 * ------------------------------------------------------------------------- */
typedef enum {
    CHANGER_ACTION_UNLOAD = 0,
    CHANGER_ACTION_LOAD   = 1
} ChangerAction;

typedef struct {
    uint8_t currentSlot;        /* Currently loaded slot index (0..N-1) */
    uint8_t totalSlots;         /* Total number of slots available */
    bool isDoorOpen;            /* Changer door open */
    bool isMechanismBusy;       /* Mechanism in motion */
    bool slotOccupied[MAX_CHANGER_SLOTS]; /* True if slot contains a disc */
} EmbeddedChangerStatus;

/* -------------------------------------------------------------------------
 * SCSI Medium Changer (Jukebox / Library) Definitions
 * ------------------------------------------------------------------------- */
typedef enum {
    ELEMENT_TYPE_ALL        = 0,
    ELEMENT_TYPE_TRANSPORT  = 1, /* Robot picker arm */
    ELEMENT_TYPE_STORAGE    = 2, /* Disc slot / magazine */
    ELEMENT_TYPE_IMPORT_EXPORT = 3, /* Mail slot / load tray */
    ELEMENT_TYPE_DRIVE      = 4  /* Optical drive data transfer element */
} ChangerElementType;

typedef struct {
    uint16_t elementAddress;
    ChangerElementType type;
    bool isFull;                 /* Disc present in slot */
    bool isAccessible;
    bool isExceptional;          /* In error state */
    uint16_t sourceElement;      /* Original slot address */
} ChangerElementDescriptor;

/* -------------------------------------------------------------------------
 * Autoloader Robotics Definitions
 * ------------------------------------------------------------------------- */
typedef enum {
    AUTOLOADER_CMD_LOAD = 0,     /* Pick from input spindle to drive */
    AUTOLOADER_CMD_ACCEPT,       /* Move finished disc from drive to output spindle */
    AUTOLOADER_CMD_REJECT,       /* Move bad/failed disc to reject bin */
    AUTOLOADER_CMD_CALIBRATE,    /* Reset & home robotics arm */
    AUTOLOADER_CMD_SELF_TEST     /* Robotics self test */
} AutoloaderCommand;

/* -------------------------------------------------------------------------
 * Embedded Changer APIs (MMC 0xA6 / 0xBD)
 * ------------------------------------------------------------------------- */

/**
 * @brief Query embedded multi-disc changer mechanism status (MMC CDB 0xBD)
 * @param drive Open OpticalDrive handle
 * @param outStatus Receives current changer status
 * @return true on success, false if drive is not a multi-disc changer
 */
bool imgburn_changer_get_status(OpticalDrive *drive, EmbeddedChangerStatus *outStatus);

/**
 * @brief Load a specific slot into the optical pickup mechanism (MMC CDB 0xA6)
 * @param drive Open OpticalDrive handle
 * @param slotIndex Target slot index (0..totalSlots-1)
 * @return true on success, false on error
 */
bool imgburn_changer_load_slot(OpticalDrive *drive, uint8_t slotIndex);

/**
 * @brief Unload current disc back into its storage magazine slot (MMC CDB 0xA6)
 * @param drive Open OpticalDrive handle
 * @return true on success, false on error
 */
bool imgburn_changer_unload(OpticalDrive *drive);

/* -------------------------------------------------------------------------
 * SCSI Medium Changer APIs (SMC 0xA5 / 0xB8 / 0x07)
 * ------------------------------------------------------------------------- */

/**
 * @brief Initialize Medium Changer element status (CDB 0x07)
 * Causes library robot to scan all slots and update presence sensors
 */
bool imgburn_medium_changer_initialize(OpticalDrive *drive);

/**
 * @brief Move disc from source slot to destination drive or slot (CDB 0xA5)
 * @param drive Open OpticalDrive / Changer handle
 * @param transportAddress Address of picker arm (usually 0)
 * @param sourceAddress Starting slot / drive address
 * @param destAddress Target slot / drive address
 * @param invert Invert disc during transport (for double-sided media)
 */
bool imgburn_medium_changer_move(OpticalDrive *drive,
                                 uint16_t transportAddress,
                                 uint16_t sourceAddress,
                                 uint16_t destAddress,
                                 bool invert);

/**
 * @brief Read element status from changer (CDB 0xB8)
 * @param drive Open OpticalDrive / Changer handle
 * @param elementType Type of elements to query
 * @param startingAddress Starting element address
 * @param elementCount Maximum number of elements to query
 * @param outDescriptors Array of element descriptors
 * @param outCount Actual count returned
 */
bool imgburn_medium_changer_read_elements(OpticalDrive *drive,
                                          ChangerElementType elementType,
                                          uint16_t startingAddress,
                                          uint16_t elementCount,
                                          ChangerElementDescriptor *outDescriptors,
                                          int *outCount);

/* -------------------------------------------------------------------------
 * Robotics Autoloader APIs
 * ------------------------------------------------------------------------- */

/**
 * @brief Send robotics autoloader command
 */
bool imgburn_autoloader_execute(OpticalDrive *drive, AutoloaderCommand cmd);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_CHANGER_H */
