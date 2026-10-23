/**
 * @file imgburn_spti.h
 * @brief ImgBurn Core C - Windows SCSI Pass-Through Interface (SPTI)
 *
 * Fully portable pure C implementation for Windows XP through Windows 11.
 * Provides direct SCSI/MMC command execution, device management, and error diagnostics.
 */

#ifndef IMGBURN_SPTI_H
#define IMGBURN_SPTI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_OPTICAL_DRIVES 32
#define SENSE_BUFFER_SIZE  64
#define DEFAULT_TIMEOUT_SEC 15

/**
 * @brief Optical Disc Drive basic information
 */
typedef struct {
    int index;                      /* Internal index (0..31) */
    wchar_t driveLetter;            /* e.g., L'E' (0 if no letter) */
    wchar_t devicePath[64];         /* e.g., L"\\\\.\\CdRom0" or L"\\\\.\\E:" */
    char vendorId[9];               /* Vendor ID (8 chars + null) */
    char productId[17];             /* Product ID (16 chars + null) */
    char productRevision[5];        /* Firmware rev (4 chars + null) */
    uint32_t busType;               /* STORAGE_BUS_TYPE (SCSI, ATAPI, USB, SATA, NVMe) */
    bool isLockable;
} OpticalDriveInfo;

/**
 * @brief SCSI Sense data representation
 */
typedef struct {
    uint8_t responseCode;           /* 0x70 = Current error, 0x71 = Deferred */
    uint8_t senseKey;               /* 0x00=No Sense, 0x02=Not Ready, 0x03=Medium Error, 0x05=Illegal Request... */
    uint8_t asc;                    /* Additional Sense Code */
    uint8_t ascq;                   /* Additional Sense Code Qualifier */
    uint8_t senseBuffer[SENSE_BUFFER_SIZE];
    uint8_t senseLength;
    const char* senseKeyStr;        /* Human-readable Sense Key */
    const char* ascAscqStr;         /* Human-readable ASC/ASCQ description */
} ScsiSenseResult;

/**
 * @brief Drive Handle and state
 */
typedef struct {
    HANDLE hDevice;
    OpticalDriveInfo info;
    bool isLocked;
    bool isMcnDisabled;             /* Media Change Notification disabled */
    ScsiSenseResult lastSense;
    uint32_t lastError;
    uint32_t lastTransferBytes;     /* 最近一次成功命令实际传输的字节数。 */
} OpticalDrive;

/**
 * @brief Enumerate all optical disc drives on the system
 * @param drives Output array of OpticalDriveInfo
 * @param maxDrives Maximum count to return
 * @return Number of drives found
 */
int spti_enumerate_drives(OpticalDriveInfo* drives, int maxDrives);

/**
 * @brief Open an optical drive
 * @param drive Pointer to OpticalDrive struct to initialize
 * @param devicePath Path, e.g. L"\\\\.\\CdRom0" or L"\\\\.\\E:"
 * @param exclusiveLock True to request exclusive volume lock
 * @return True on success, false on failure
 */
bool spti_open_drive(OpticalDrive* drive, const wchar_t* devicePath, bool exclusiveLock);

/**
 * @brief Close the optical drive handle and release locks
 */
void spti_close_drive(OpticalDrive* drive);

/**
 * @brief Lock or unlock the drive volume (IOCTL_CDROM_EXCLUSIVE_ACCESS)
 */
bool spti_lock_drive(OpticalDrive* drive, bool lock);

/**
 * @brief Disable or enable Windows Media Change Notification (Autorun / AutoPlay)
 */
bool spti_set_mcn_control(OpticalDrive* drive, bool disable);

/**
 * @brief Core SCSI CDB execution via IOCTL_SCSI_PASS_THROUGH_DIRECT
 *
 * @param drive Open optical drive
 * @param cdb Pointer to Command Descriptor Block (6, 10, 12, or 16 bytes)
 * @param cdbLength Length of CDB
 * @param dataBuffer Data buffer for read or write (or NULL)
 * @param dataLength Length of data transfer in bytes
 * @param isDataIn True = Data from device to host (Read), False = Host to device (Write)
 * @param timeoutSeconds Timeout in seconds
 * @return True if SCSI status is GOOD (0x00), False on error or CHECK CONDITION
 */
bool spti_send_cdb(OpticalDrive* drive,
                   const uint8_t* cdb,
                   uint8_t cdbLength,
                   void* dataBuffer,
                   uint32_t dataLength,
                   bool isDataIn,
                   uint32_t timeoutSeconds);

/**
 * @brief Format sense data into human readable string
 */
void spti_format_sense_error(const ScsiSenseResult* sense, char* outBuffer, size_t maxLen);

#ifdef __cplusplus
}
#endif

#endif /* IMGBURN_SPTI_H */
