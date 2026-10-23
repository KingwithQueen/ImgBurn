/**
 * @file imgburn_spti.c
 * @brief ImgBurn Core C - Windows SCSI Pass-Through Interface (SPTI) Implementation
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_spti.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SCSI Status Codes */
#define SCSI_STATUS_GOOD             0x00
#define SCSI_STATUS_CHECK_CONDITION  0x02
#define SCSI_STATUS_CONDITION_MET    0x04
#define SCSI_STATUS_BUSY             0x08
#define SCSI_STATUS_INTERMEDIATE     0x10
#define SCSI_STATUS_RESERVATION_CONF 0x18
#define SCSI_STATUS_TASK_SET_FULL    0x28
#define SCSI_STATUS_ACA_ACTIVE       0x30

/* Structure holding SCSI_PASS_THROUGH_DIRECT with sense buffer */
typedef struct {
    SCSI_PASS_THROUGH_DIRECT sptd;
    ULONG Filler;
    UCHAR SenseBuf[SENSE_BUFFER_SIZE];
} SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER;

/* Sense Key Names */
static const char* SENSE_KEY_STRINGS[] = {
    "NO SENSE (0x00)",
    "RECOVERED ERROR (0x01)",
    "NOT READY (0x02)",
    "MEDIUM ERROR (0x03)",
    "HARDWARE ERROR (0x04)",
    "ILLEGAL REQUEST (0x05)",
    "UNIT ATTENTION (0x06)",
    "DATA PROTECT (0x07)",
    "BLANK CHECK (0x08)",
    "VENDOR SPECIFIC (0x09)",
    "COPY ABORTED (0x0A)",
    "ABORTED COMMAND (0x0B)",
    "EQUAL (0x0C)",
    "VOLUME OVERFLOW (0x0D)",
    "MISCOMPARE (0x0E)",
    "COMPLETED (0x0F)"
};

/* ASC/ASCQ Common Lookup Dictionary */
typedef struct {
    uint8_t asc;
    uint8_t ascq;
    const char* text;
} AscAscqEntry;

static const AscAscqEntry ASC_ASCQ_TABLE[] = {
    { 0x00, 0x00, "No Additional Sense Information" },
    { 0x00, 0x11, "Audio Play Operation in Progress" },
    { 0x00, 0x12, "Audio Play Operation Paused" },
    { 0x00, 0x13, "Audio Play Operation Successfully Completed" },
    { 0x00, 0x14, "Audio Play Operation Stopped Due to Error" },
    { 0x00, 0x15, "No Current Audio Status to Return" },
    { 0x02, 0x00, "No Seek Complete" },
    { 0x04, 0x00, "Logical Unit Not Ready, Cause Not Reportable" },
    { 0x04, 0x01, "Logical Unit is in Process of Becoming Ready" },
    { 0x04, 0x02, "Logical Unit Not Ready, Initializing Command Required" },
    { 0x04, 0x03, "Logical Unit Not Ready, Manual Intervention Required" },
    { 0x04, 0x04, "Logical Unit Not Ready, Format in Progress" },
    { 0x04, 0x07, "Logical Unit Not Ready, Operation in Progress" },
    { 0x04, 0x08, "Logical Unit Not Ready, Long Write in Progress" },
    { 0x06, 0x00, "No Reference Position Found" },
    { 0x08, 0x00, "Logical Unit Communication Failure" },
    { 0x09, 0x00, "Track Following Error" },
    { 0x0C, 0x00, "Write Error" },
    { 0x0C, 0x07, "Write Error - Recovery Needed" },
    { 0x0C, 0x08, "Write Error - Recovery Failed" },
    { 0x0C, 0x09, "Write Error - Loss of Streaming" },
    { 0x11, 0x00, "Unrecovered Read Error" },
    { 0x11, 0x05, "L-EC Uncorrectable Error" },
    { 0x11, 0x06, "CIRC Unrecovered Error" },
    { 0x15, 0x00, "Random Positioning Error" },
    { 0x15, 0x01, "Mechanical Positioning Error" },
    { 0x15, 0x02, "Positioning Error Detected by Read of Medium" },
    { 0x20, 0x00, "Invalid Command Operation Code" },
    { 0x21, 0x00, "Logical Block Address Out of Range" },
    { 0x21, 0x02, "Invalid Address for Write" },
    { 0x24, 0x00, "Invalid Field in CDB" },
    { 0x25, 0x00, "Logical Unit Not Supported" },
    { 0x26, 0x00, "Invalid Field in Parameter List" },
    { 0x27, 0x00, "Write Protected" },
    { 0x28, 0x00, "Not Ready to Ready Change, Medium May Have Changed" },
    { 0x29, 0x00, "Power On, Reset, or Bus Device Reset Occurred" },
    { 0x2A, 0x00, "Parameters Changed" },
    { 0x2A, 0x01, "Mode Parameters Changed" },
    { 0x2C, 0x00, "Command Sequence Error" },
    { 0x2E, 0x00, "Insufficient Time for Operation" },
    { 0x30, 0x00, "Incompatible Medium Installed" },
    { 0x30, 0x01, "Cannot Read Medium - Unknown Format" },
    { 0x30, 0x02, "Cannot Read Medium - Incompatible Format" },
    { 0x30, 0x05, "Cannot Write Medium - Incompatible Format" },
    { 0x30, 0x06, "Cannot Format Medium - Incompatible Medium" },
    { 0x30, 0x07, "Cleaning Cartridge Installed" },
    { 0x31, 0x00, "Medium Format Corrupted" },
    { 0x31, 0x01, "Format Command Failed" },
    { 0x35, 0x02, "Enclosure Services Unavailable" },
    { 0x38, 0x00, "Event Status Notification" },
    { 0x3A, 0x00, "Medium Not Present" },
    { 0x3A, 0x01, "Medium Not Present - Tray Closed" },
    { 0x3A, 0x02, "Medium Not Present - Tray Open" },
    { 0x3E, 0x00, "Logical Unit Has Not Self-Configured Yet" },
    { 0x3F, 0x00, "Target Operating Conditions Have Changed" },
    { 0x3F, 0x01, "Microcode Has Been Changed" },
    { 0x44, 0x00, "Internal Target Failure" },
    { 0x51, 0x00, "Erase Failure" },
    { 0x53, 0x00, "Media Load or Eject Failed" },
    { 0x57, 0x00, "Unable to Recover Table of Contents" },
    { 0x5A, 0x00, "Operator Request or State Change Input" },
    { 0x5A, 0x01, "Operator Medium Removal Requested" },
    { 0x64, 0x00, "Illegal Mode for This Track" },
    { 0x72, 0x00, "Session Fixation Error" },
    { 0x72, 0x01, "Session Fixation Error - Writing Lead-in" },
    { 0x72, 0x02, "Session Fixation Error - Writing Lead-out" },
    { 0x72, 0x03, "Session Fixation Error - Incomplete Track in Session" },
    { 0x72, 0x04, "Empty or Partially Written Reserved Track" },
    { 0x72, 0x05, "No More Track Reservations Allowed" },
    { 0x73, 0x00, "CD Control Error" },
    { 0x73, 0x01, "Power Calibration Area Almost Full" },
    { 0x73, 0x02, "Power Calibration Area is Full" },
    { 0x73, 0x03, "Power Calibration Area Error" },
    { 0x73, 0x04, "Program Memory Area Update Failure" },
    { 0x73, 0x05, "Program Memory Area is Full" },
    { 0x73, 0x06, "RMA/PMA is Almost Full" }
};

static const char* lookup_asc_ascq(uint8_t asc, uint8_t ascq) {
    size_t count = sizeof(ASC_ASCQ_TABLE) / sizeof(ASC_ASCQ_TABLE[0]);
    for (size_t i = 0; i < count; i++) {
        if (ASC_ASCQ_TABLE[i].asc == asc && ASC_ASCQ_TABLE[i].ascq == ascq) {
            return ASC_ASCQ_TABLE[i].text;
        }
    }
    return "Unknown Error Code";
}

static void query_device_descriptor(HANDLE hDevice, OpticalDriveInfo* info) {
    STORAGE_PROPERTY_QUERY query;
    memset(&query, 0, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    BYTE buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    DWORD bytesReturned = 0;

    if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
                        &query, sizeof(query),
                        buffer, sizeof(buffer),
                        &bytesReturned, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR* desc = (STORAGE_DEVICE_DESCRIPTOR*)buffer;
        info->busType = desc->BusType;

        if (desc->VendorIdOffset != 0 && desc->VendorIdOffset < bytesReturned) {
            const char* v = (const char*)(buffer + desc->VendorIdOffset);
            strncpy(info->vendorId, v, sizeof(info->vendorId) - 1);
            /* Trim trailing whitespace */
            for (int i = (int)strlen(info->vendorId) - 1; i >= 0 && info->vendorId[i] == ' '; i--) {
                info->vendorId[i] = '\0';
            }
        }
        if (desc->ProductIdOffset != 0 && desc->ProductIdOffset < bytesReturned) {
            const char* p = (const char*)(buffer + desc->ProductIdOffset);
            strncpy(info->productId, p, sizeof(info->productId) - 1);
            for (int i = (int)strlen(info->productId) - 1; i >= 0 && info->productId[i] == ' '; i--) {
                info->productId[i] = '\0';
            }
        }
        if (desc->ProductRevisionOffset != 0 && desc->ProductRevisionOffset < bytesReturned) {
            const char* r = (const char*)(buffer + desc->ProductRevisionOffset);
            strncpy(info->productRevision, r, sizeof(info->productRevision) - 1);
            for (int i = (int)strlen(info->productRevision) - 1; i >= 0 && info->productRevision[i] == ' '; i--) {
                info->productRevision[i] = '\0';
            }
        }
    }
}

int spti_enumerate_drives(OpticalDriveInfo* drives, int maxDrives) {
    if (!drives || maxDrives <= 0) return 0;
    int foundCount = 0;

    /* Method 1: Enumerate logical drive letters (D: to Z:) */
    DWORD driveMask = GetLogicalDrives();
    for (int i = 0; i < 26 && foundCount < maxDrives; i++) {
        if (!(driveMask & (1 << i))) continue;

        wchar_t rootPath[4] = { (wchar_t)(L'A' + i), L':', L'\\', L'\0' };
        UINT driveType = GetDriveTypeW(rootPath);
        if (driveType == DRIVE_CDROM) {
            OpticalDriveInfo* info = &drives[foundCount];
            memset(info, 0, sizeof(*info));
            info->index = foundCount;
            info->driveLetter = (wchar_t)(L'A' + i);
            swprintf(info->devicePath, 64, L"\\\\.\\%c:", info->driveLetter);

            /* Attempt to open and query details */
            HANDLE h = CreateFileW(info->devicePath,
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                /* Try read-only */
                h = CreateFileW(info->devicePath,
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
            }
            if (h == INVALID_HANDLE_VALUE) {
                /* Try query access (0) */
                h = CreateFileW(info->devicePath,
                                0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, 0, NULL);
            }

            if (h != INVALID_HANDLE_VALUE) {
                query_device_descriptor(h, info);
                CloseHandle(h);
            }
            foundCount++;
        }
    }

    /* Method 2: If no drives found via letters, check \\.\CdRom0 to \\.\CdRom31 */
    if (foundCount == 0) {
        for (int i = 0; i < 32 && foundCount < maxDrives; i++) {
            wchar_t cdromPath[32];
            swprintf(cdromPath, 32, L"\\\\.\\CdRom%d", i);

            HANDLE h = CreateFileW(cdromPath,
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                OpticalDriveInfo* info = &drives[foundCount];
                memset(info, 0, sizeof(*info));
                info->index = foundCount;
                info->driveLetter = 0;
                wcscpy(info->devicePath, cdromPath);
                query_device_descriptor(h, info);
                CloseHandle(h);
                foundCount++;
            }
        }
    }

    return foundCount;
}

bool spti_open_drive(OpticalDrive* drive, const wchar_t* devicePath, bool exclusiveLock) {
    if (!drive || !devicePath || wcslen(devicePath) >= sizeof(drive->info.devicePath) / sizeof(drive->info.devicePath[0])) return false;
    memset(drive, 0, sizeof(*drive));

    drive->hDevice = CreateFileW(devicePath,
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL,
                                 NULL);

    if (drive->hDevice == INVALID_HANDLE_VALUE) {
        /* Fallback 1: try without write sharing */
        drive->hDevice = CreateFileW(devicePath,
                                     GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ,
                                     NULL,
                                     OPEN_EXISTING,
                                     0,
                                     NULL);
    }

    if (drive->hDevice == INVALID_HANDLE_VALUE) {
        /* Fallback 2: try Read-Only access (allows inquiry/read on standard user tokens) */
        drive->hDevice = CreateFileW(devicePath,
                                     GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL,
                                     OPEN_EXISTING,
                                     0,
                                     NULL);
    }

    if (drive->hDevice == INVALID_HANDLE_VALUE) {
        /* Fallback 3: try with 0 access (query permissions) */
        drive->hDevice = CreateFileW(devicePath,
                                     0,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL,
                                     OPEN_EXISTING,
                                     0,
                                     NULL);
    }

    if (drive->hDevice == INVALID_HANDLE_VALUE) {
        drive->lastError = GetLastError();
        return false;
    }

    /* Populate basic info */
    wcscpy(drive->info.devicePath, devicePath);
    query_device_descriptor(drive->hDevice, &drive->info);

    /* Disable AutoPlay/MCN during operation */
    spti_set_mcn_control(drive, true);

    if (exclusiveLock) {
        if (!spti_lock_drive(drive, true)) {
            DWORD error = GetLastError();
            spti_close_drive(drive);
            drive->lastError = error;
            SetLastError(error);
            return false;
        }
    }

    return true;
}

void spti_close_drive(OpticalDrive* drive) {
    if (!drive || drive->hDevice == INVALID_HANDLE_VALUE || !drive->hDevice) {
        return;
    }

    if (drive->isLocked) {
        spti_lock_drive(drive, false);
    }

    if (drive->isMcnDisabled) {
        spti_set_mcn_control(drive, false);
    }

    CloseHandle(drive->hDevice);
    drive->hDevice = INVALID_HANDLE_VALUE;
}

bool spti_lock_drive(OpticalDrive* drive, bool lock) {
    if (!drive || drive->hDevice == INVALID_HANDLE_VALUE) return false;
    DWORD bytesReturned = 0;
    DWORD ioctlCode = lock ? FSCTL_LOCK_VOLUME : FSCTL_UNLOCK_VOLUME;

    BOOL ok = DeviceIoControl(drive->hDevice, ioctlCode,
                              NULL, 0, NULL, 0,
                              &bytesReturned, NULL);
    if (ok) {
        drive->isLocked = lock;
    }
    return ok != FALSE;
}

bool spti_set_mcn_control(OpticalDrive* drive, bool disable) {
    if (!drive || drive->hDevice == INVALID_HANDLE_VALUE) return false;
    PREVENT_MEDIA_REMOVAL pmr;
    memset(&pmr, 0, sizeof(pmr));
    pmr.PreventMediaRemoval = disable ? TRUE : FALSE;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(drive->hDevice, IOCTL_STORAGE_MCN_CONTROL,
                              &pmr, sizeof(pmr),
                              NULL, 0,
                              &bytesReturned, NULL);
    if (ok) {
        drive->isMcnDisabled = disable;
    }
    return ok != FALSE;
}

static void parse_sense_data(OpticalDrive* drive, const uint8_t* senseBuf, uint8_t senseLen) {
    ScsiSenseResult* res = &drive->lastSense;
    memset(res, 0, sizeof(*res));

    if (!senseBuf || senseLen < 4) {
        return;
    }

    res->senseLength = (senseLen > SENSE_BUFFER_SIZE) ? SENSE_BUFFER_SIZE : senseLen;
    memcpy(res->senseBuffer, senseBuf, res->senseLength);

    res->responseCode = senseBuf[0] & 0x7F;
    if (res->responseCode == 0x70 || res->responseCode == 0x71) {
        /* Standard fixed format sense data */
        res->senseKey = senseBuf[2] & 0x0F;
        if (senseLen >= 13) {
            res->asc = senseBuf[12];
        }
        if (senseLen >= 14) {
            res->ascq = senseBuf[13];
        }
    } else if (res->responseCode == 0x72 || res->responseCode == 0x73) {
        /* Descriptor format sense data */
        res->senseKey = senseBuf[1] & 0x0F;
        if (senseLen >= 3) {
            res->asc = senseBuf[2];
        }
        if (senseLen >= 4) {
            res->ascq = senseBuf[3];
        }
    }

    if (res->senseKey < sizeof(SENSE_KEY_STRINGS) / sizeof(SENSE_KEY_STRINGS[0])) {
        res->senseKeyStr = SENSE_KEY_STRINGS[res->senseKey];
    } else {
        res->senseKeyStr = "UNKNOWN SENSE KEY";
    }

    res->ascAscqStr = lookup_asc_ascq(res->asc, res->ascq);
}

bool spti_send_cdb(OpticalDrive* drive,
                   const uint8_t* cdb,
                   uint8_t cdbLength,
                   void* dataBuffer,
                   uint32_t dataLength,
                   bool isDataIn,
                   uint32_t timeoutSeconds) {
    if (drive) drive->lastTransferBytes = 0;
    if (!drive || !drive->hDevice || drive->hDevice == INVALID_HANDLE_VALUE || !cdb ||
        (cdbLength != 6 && cdbLength != 10 && cdbLength != 12 && cdbLength != 16) || (dataLength > 0 && !dataBuffer)) {
        if (drive) drive->lastError = ERROR_INVALID_PARAMETER;
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER sptdwb;
    memset(&sptdwb, 0, sizeof(sptdwb));

    sptdwb.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    sptdwb.sptd.CdbLength = cdbLength;
    memcpy(sptdwb.sptd.Cdb, cdb, cdbLength);

    sptdwb.sptd.DataIn = (dataLength == 0) ? SCSI_IOCTL_DATA_UNSPECIFIED :
                         (isDataIn ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT);
    sptdwb.sptd.DataTransferLength = dataLength;
    sptdwb.sptd.DataBuffer = dataBuffer;

    sptdwb.sptd.TimeOutValue = (timeoutSeconds == 0) ? DEFAULT_TIMEOUT_SEC : timeoutSeconds;
    sptdwb.sptd.SenseInfoLength = SENSE_BUFFER_SIZE;
    sptdwb.sptd.SenseInfoOffset = (ULONG)((ULONG_PTR)&sptdwb.SenseBuf[0] - (ULONG_PTR)&sptdwb.sptd);

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(drive->hDevice,
                              IOCTL_SCSI_PASS_THROUGH_DIRECT,
                              &sptdwb,
                              sizeof(sptdwb),
                              &sptdwb,
                              sizeof(sptdwb),
                              &bytesReturned,
                              NULL);

    if (!ok) {
        drive->lastError = GetLastError();
        return false;
    }

    if (sptdwb.sptd.ScsiStatus != SCSI_STATUS_GOOD) {
        /* Parse sense buffer if available */
        if (sptdwb.sptd.SenseInfoLength > 0) {
            parse_sense_data(drive, sptdwb.SenseBuf, sptdwb.sptd.SenseInfoLength);
        }
        drive->lastError = ERROR_IO_DEVICE; SetLastError(ERROR_IO_DEVICE); return false;
    }

    /* Success: clear last sense */
    memset(&drive->lastSense, 0, sizeof(drive->lastSense));
    drive->lastTransferBytes = sptdwb.sptd.DataTransferLength;
    drive->lastError = ERROR_SUCCESS;
    return true;
}

void spti_format_sense_error(const ScsiSenseResult* sense, char* outBuffer, size_t maxLen) {
    if (!sense || !outBuffer || maxLen == 0) return;
    snprintf(outBuffer, maxLen,
             "Sense Key: %s\n"
             "ASC: 0x%02X, ASCQ: 0x%02X\n"
             "Interpretation: %s",
             sense->senseKeyStr ? sense->senseKeyStr : "None",
             sense->asc, sense->ascq,
             sense->ascAscqStr ? sense->ascAscqStr : "Unknown");
}
