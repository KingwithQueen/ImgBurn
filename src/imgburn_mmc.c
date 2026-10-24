/**
 * @file imgburn_mmc.c
 * @brief ImgBurn Core C - SCSI Multimedia Commands (MMC) Implementation
 */

#include "imgburn_mmc.h"
#include <stdlib.h>
#include <string.h>

/* Endian helpers for SCSI CDB (SCSI uses Big-Endian) */
static inline void write_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

static inline uint16_t read_be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

bool mmc_test_unit_ready(OpticalDrive* drive) {
    uint8_t cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
    return spti_send_cdb(drive, cdb, 6, NULL, 0, true, 5);
}

bool mmc_inquiry(OpticalDrive* drive, uint8_t* outBuffer, uint32_t bufSize) {
    if (!outBuffer || bufSize < 36) return false;
    uint8_t cdb[6] = { 0x12, 0, 0, 0, (uint8_t)bufSize, 0 };
    return spti_send_cdb(drive, cdb, 6, outBuffer, bufSize, true, 5);
}

MmcProfile mmc_get_current_profile(OpticalDrive* drive) {
    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x46; /* GET CONFIGURATION */
    cdb[1] = 0x01; /* RT = 01b (Query current profile) */
    cdb[8] = 8;    /* Allocation length: header is 8 bytes */

    uint8_t buffer[8] = { 0 };
    if (!spti_send_cdb(drive, cdb, 10, buffer, 8, true, 5)) {
        return MMC_PROFILE_UNKNOWN;
    }
    if (drive->lastTransferBytes != sizeof(buffer)) { drive->lastError = ERROR_READ_FAULT; SetLastError(ERROR_READ_FAULT); return MMC_PROFILE_UNKNOWN; }

    uint16_t profile = read_be16(&buffer[6]);
    return (MmcProfile)profile;
}

const char* mmc_get_profile_name(MmcProfile profile) {
    switch (profile) {
        case MMC_PROFILE_NONE:            return "No Profile (Empty Drive)";
        case MMC_PROFILE_CD_ROM:          return "CD-ROM";
        case MMC_PROFILE_CD_R:            return "CD-R";
        case MMC_PROFILE_CD_RW:           return "CD-RW";
        case MMC_PROFILE_DVD_ROM:         return "DVD-ROM";
        case MMC_PROFILE_DVD_R_SEQ:       return "DVD-R Sequential";
        case MMC_PROFILE_DVD_RAM:         return "DVD-RAM";
        case MMC_PROFILE_DVD_RW_RO:       return "DVD-RW Restricted Overwrite";
        case MMC_PROFILE_DVD_RW_SEQ:      return "DVD-RW Sequential";
        case MMC_PROFILE_DVD_R_DL_SEQ:    return "DVD-R Dual Layer Sequential";
        case MMC_PROFILE_DVD_R_DL_JUMP:   return "DVD-R Dual Layer Layer Jump";
        case MMC_PROFILE_DVD_RW_DL:       return "DVD-RW Dual Layer";
        case MMC_PROFILE_DVD_PLUS_RW:     return "DVD+RW";
        case MMC_PROFILE_DVD_PLUS_R:      return "DVD+R";
        case MMC_PROFILE_DVD_PLUS_RW_DL:  return "DVD+RW Dual Layer";
        case MMC_PROFILE_DVD_PLUS_R_DL:   return "DVD+R Dual Layer";
        case MMC_PROFILE_BD_ROM:          return "BD-ROM";
        case MMC_PROFILE_BD_R_SRM:        return "BD-R Sequential (SRM)";
        case MMC_PROFILE_BD_R_RRM:        return "BD-R Random (RRM)";
        case MMC_PROFILE_BD_RE:           return "BD-RE";
        default:                          return "Unknown Optical Media";
    }
}

bool mmc_read_capacity(OpticalDrive* drive, uint32_t* outLastLba, uint32_t* outBlockSize) {
    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x25; /* READ CAPACITY */

    uint8_t buffer[8] = { 0 };
    if (!spti_send_cdb(drive, cdb, 10, buffer, 8, true, 10)) {
        return false;
    }
    if (drive->lastTransferBytes != sizeof(buffer)) { drive->lastError = ERROR_READ_FAULT; SetLastError(ERROR_READ_FAULT); return false; }

    if (outLastLba) *outLastLba = read_be32(&buffer[0]);
    if (outBlockSize) *outBlockSize = read_be32(&buffer[4]);
    return true;
}

bool mmc_read_disc_information(OpticalDrive* drive, MmcDiscInfo* outInfo) {
    if (!outInfo) return false;
    memset(outInfo, 0, sizeof(*outInfo));

    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x51; /* READ DISC INFORMATION */
    cdb[8] = 34;   /* Standard disc information length */

    uint8_t buffer[34] = { 0 };
    if (!spti_send_cdb(drive, cdb, 10, buffer, sizeof(buffer), true, 10)) {
        return false;
    }

    if (drive->lastTransferBytes != sizeof(buffer) || read_be16(buffer) < 32) { drive->lastError = ERROR_READ_FAULT; SetLastError(ERROR_READ_FAULT); return false; }

    /* Byte 2: Status bits */
    uint8_t discStatusBits = buffer[2] & 0x03;
    outInfo->discStatus = (MmcDiscStatus)discStatusBits;
    outInfo->isErasable = (buffer[2] & 0x10) != 0;

    outInfo->firstTrack = buffer[3];
    outInfo->numSessions = (uint16_t)((uint16_t)buffer[9] << 8 | buffer[4]);
    outInfo->lastTrack = (uint16_t)((uint16_t)buffer[11] << 8 | buffer[6]);
    outInfo->lastSessionStatus = (buffer[2] >> 2) & 3;

    /* Byte 16-19: Lead-in LBA */
    outInfo->leadInLba = read_be32(&buffer[16]);
    /* Byte 20-23: Last possible Lead-out LBA */
    outInfo->leadOutLba = read_be32(&buffer[20]);

    outInfo->currentProfile = mmc_get_current_profile(drive);
    if (outInfo->currentProfile == MMC_PROFILE_UNKNOWN) { memset(outInfo,0,sizeof(*outInfo)); return false; }
    outInfo->profileName = mmc_get_profile_name(outInfo->currentProfile);

    return true;
}

bool mmc_read_track_information(OpticalDrive* drive, uint32_t lbaOrTrack, bool isLba, MmcTrackInfo* outInfo) {
    if (!outInfo) return false;
    memset(outInfo, 0, sizeof(*outInfo));

    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x52; /* READ TRACK INFORMATION */
    cdb[1] = isLba ? 0x00 : 0x01; /* Address type: 0 = LBA, 1 = Track Number */
    write_be32(&cdb[2], lbaOrTrack);
    cdb[8] = 36; /* Track information length */

    uint8_t buffer[36] = { 0 };
    if (!spti_send_cdb(drive, cdb, 10, buffer, sizeof(buffer), true, 10)) {
        return false;
    }

    uint32_t declared = (uint32_t)read_be16(buffer) + 2, required = declared < sizeof(buffer) ? declared : sizeof(buffer);
    if (drive->lastTransferBytes < required || drive->lastTransferBytes > sizeof(buffer) || declared < 32) { drive->lastError = ERROR_READ_FAULT; SetLastError(ERROR_READ_FAULT); return false; }

    outInfo->trackNumber = buffer[2];
    outInfo->sessionNumber = buffer[3];
    if (drive->lastTransferBytes >= 34 && read_be16(buffer) >= 32) { outInfo->trackNumber |= (uint16_t)buffer[32] << 8; outInfo->sessionNumber |= (uint16_t)buffer[33] << 8; }
    outInfo->isDamage = (buffer[5] & 0x20) != 0;
    outInfo->isCopy = (buffer[5] & 0x10) != 0;
    outInfo->isPacket = (buffer[6] & 0x20) != 0;
    outInfo->isBlank = (buffer[6] & 0x40) != 0;
    outInfo->nwaValid = (buffer[7] & 0x01) != 0;

    outInfo->trackStartLba = read_be32(&buffer[8]);
    outInfo->nextWritableAddress = read_be32(&buffer[12]);
    outInfo->freeBlocks = read_be32(&buffer[16]);
    outInfo->trackSize = read_be32(&buffer[24]);

    return true;
}

bool mmc_set_speed(OpticalDrive* drive, uint16_t readSpeedKbps, uint16_t writeSpeedKbps) {
    uint8_t cdb[12] = { 0 };
    cdb[0] = 0xBB; /* SET CD SPEED */
    write_be16(&cdb[2], readSpeedKbps);
    write_be16(&cdb[4], writeSpeedKbps);

    return spti_send_cdb(drive, cdb, 12, NULL, 0, true, 10);
}

bool mmc_synchronize_cache(OpticalDrive* drive) {
    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x35; /* SYNCHRONIZE CACHE */
    return spti_send_cdb(drive, cdb, 10, NULL, 0, true, 60);
}

bool mmc_close_track_session(OpticalDrive* drive, uint8_t closeType, uint16_t trackNumber) {
    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x5B; /* CLOSE TRACK SESSION */
    /* 关闭功能由调用者按介质选择，DVD+R 的会话关闭和光盘结束使用不同值。 */
    cdb[2] = closeType & 0x07;
    write_be16(&cdb[4], trackNumber);

    /* Closing track/session can take up to 2-3 minutes */
    return spti_send_cdb(drive, cdb, 10, NULL, 0, true, 180);
}

bool mmc_blank_disc(OpticalDrive* drive, uint8_t blankType) {
    uint8_t cdb[12] = { 0 };
    cdb[0] = 0xA1; /* BLANK */
    cdb[1] = blankType & 0x07; /* 0 = Entire disc, 1 = Minimal */

    /* Erasing a full disc can take 15+ minutes */
    return spti_send_cdb(drive, cdb, 12, NULL, 0, true, 900);
}

bool mmc_start_blank_disc(OpticalDrive* drive, bool full) { uint8_t cdb[12] = {0xA1}; cdb[1] = (uint8_t)(0x10 | (full ? 0 : 1)); return spti_send_cdb(drive,cdb,12,NULL,0,true,30); }

bool mmc_read_erase_progress(OpticalDrive* drive, MmcEraseProgress* progress) {
    if (!progress) { SetLastError(ERROR_INVALID_PARAMETER); return false; } memset(progress,0,sizeof(*progress));
    uint8_t cdb[6] = {3,0,0,0,252}, data[252] = {0};
    if (!spti_send_cdb(drive,cdb,6,data,sizeof(data),true,10)) return false;
    DWORD error = ERROR_INVALID_DATA; uint32_t size = drive->lastTransferBytes; uint8_t key = 0, asc = 0, ascq = 0;
    if (size < 8 || size > sizeof(data)) { error = ERROR_READ_FAULT; goto invalid; }
    uint32_t declared = (uint32_t)data[7] + 8; if (declared > size) { error = ERROR_READ_FAULT; goto invalid; }
    uint8_t format = data[0] & 0x7F;
    if (format == 0x70 || format == 0x71) {
        if (declared < 18) { error = ERROR_READ_FAULT; goto invalid; } key = data[2] & 15; asc = data[12]; ascq = data[13];
        if (data[15] & 0x80) { progress->valid = true; progress->value = read_be16(data + 16); }
    } else if (format == 0x72 || format == 0x73) {
        key = data[1] & 15; asc = data[2]; ascq = data[3];
        for (uint32_t offset = 8; offset < declared;) {
            if (declared - offset < 2 || (uint32_t)data[offset + 1] + 2 > declared - offset) goto invalid;
            if (data[offset] == 2) {
                if (data[offset + 1] != 6 || progress->valid || !(data[offset + 4] & 0x80)) goto invalid;
                progress->valid = true; progress->value = read_be16(data + offset + 5);
            } else if (data[offset] == 0x0A) {
                if (data[offset + 1] != 6) goto invalid;
                /* 另一操作的进度有独立 SK/ASC/ASCQ，不能仅由响应头判断已经结束。 */
                if ((data[offset + 2] & 15) == 2 && data[offset + 3] == 4 && data[offset + 4] == 7) { if (progress->valid) goto invalid; progress->active = progress->valid = true; progress->value = read_be16(data + offset + 6); }
            }
            offset += (uint32_t)data[offset + 1] + 2;
        }
    } else goto invalid;
    if (!key && !asc && !ascq) { progress->active = progress->valid; return true; }
    if (key == 2 && asc == 4 && ascq == 7) { progress->active = true; return true; }
    error = key == 7 ? ERROR_WRITE_PROTECT : key == 2 ? ERROR_NOT_READY : ERROR_IO_DEVICE;
invalid:
    memset(progress,0,sizeof(*progress)); drive->lastError = error; SetLastError(error); return false;
}

bool mmc_load_eject(OpticalDrive* drive, bool eject) {
    uint8_t cdb[6] = { 0 };
    cdb[0] = 0x1B; /* START STOP UNIT */
    /* LoEj bit: 0x02 = Load/Eject operation
     * Start bit: 0x00 = Eject/Stop, 0x01 = Load/Start
     */
    cdb[4] = eject ? 0x02 : 0x03;

    return spti_send_cdb(drive, cdb, 6, NULL, 0, true, 15);
}

bool mmc_reserve_track(OpticalDrive* drive, uint32_t reservationSizeLba) {
    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x53; /* RESERVE TRACK */
    write_be32(&cdb[5], reservationSizeLba);

    return spti_send_cdb(drive, cdb, 10, NULL, 0, true, 15);
}

bool mmc_prevent_removal(OpticalDrive* drive, bool prevent) { uint8_t cdb[6] = {0x1E}; cdb[4] = prevent ? 1 : 0; return spti_send_cdb(drive,cdb,6,NULL,0,true,10); }

static bool mmc_write_parameter_error(OpticalDrive* drive, DWORD error) { if (drive) drive->lastError = error; SetLastError(error); return false; }
bool mmc_get_dvd_write_features(OpticalDrive* drive, MmcDvdWriteFeatures* features) {
    if (!features) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER); memset(features,0,sizeof(*features));
    uint8_t cdb[10] = {0x46,2,0,0x2F}, data[16] = {0}; cdb[8] = sizeof(data);
    if (!spti_send_cdb(drive,cdb,10,data,sizeof(data),true,10)) return false;
    if (drive->lastTransferBytes == 8 && read_be32(data) == 4) return mmc_write_parameter_error(drive,ERROR_NOT_SUPPORTED);
    if (drive->lastTransferBytes != sizeof(data) || read_be32(data) < 12) return mmc_write_parameter_error(drive,ERROR_READ_FAULT);
    if (read_be16(data + 6) != MMC_PROFILE_DVD_R_SEQ || read_be16(data + 8) != 0x2F || !(data[10] & 1) || data[11] != 4) return mmc_write_parameter_error(drive,ERROR_NOT_SUPPORTED);
    features->testWrite = (data[12] & 4) != 0; features->bufferUnderrunFree = (data[12] & 0x40) != 0; return true;
}
bool mmc_get_cd_write_features(OpticalDrive* drive, MmcCdWriteFeatures* features) {
    if (!features) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER); memset(features,0,sizeof(*features));
    uint8_t cdb[10] = {0x46,2,0,0x2E}, data[16] = {0}; cdb[8] = sizeof(data);
    if (!spti_send_cdb(drive,cdb,10,data,sizeof(data),true,10)) return false;
    if (drive->lastTransferBytes == 8 && read_be32(data) == 4) return mmc_write_parameter_error(drive,ERROR_NOT_SUPPORTED);
    if (drive->lastTransferBytes != sizeof(data) || read_be32(data) < 12) return mmc_write_parameter_error(drive,ERROR_READ_FAULT);
    if (read_be16(data + 6) != MMC_PROFILE_CD_R || read_be16(data + 8) != 0x2E || !(data[10] & 1) || data[11] != 4 || !(data[12] & 0x20)) return mmc_write_parameter_error(drive,ERROR_NOT_SUPPORTED);
    features->testWrite = (data[12] & 4) != 0; features->bufferUnderrunFree = (data[12] & 0x40) != 0; features->maxCueSheetBytes = (uint32_t)data[13] << 16 | (uint32_t)data[14] << 8 | data[15]; features->rawRwSubchannel = (data[12] & 1) != 0;
    if (features->maxCueSheetBytes < 32) return mmc_write_parameter_error(drive,ERROR_NOT_SUPPORTED); return true;
}
bool mmc_read_cd_write_capacity(OpticalDrive* drive, uint32_t* sectors) {
    if (!sectors) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER); *sectors = 0;
    uint8_t cdb[10] = {0x43,0,4}, data[32] = {0}; cdb[8] = sizeof(data);
    if (!spti_send_cdb(drive,cdb,10,data,sizeof(data),true,10)) return false;
    uint32_t bytes = (uint32_t)read_be16(data) + 2;
    if (drive->lastTransferBytes < 16 || drive->lastTransferBytes > sizeof(data) || bytes < 16 || bytes > drive->lastTransferBytes) return mmc_write_parameter_error(drive,ERROR_READ_FAULT);
    /* 仅普通、非受限 CD-R；ATIP 的时间为二进制 MSF，不进行 BCD 转换。 */
    if (!(data[5] & 0x40) || (data[6] & 0xF8) != 0x80) return mmc_write_parameter_error(drive,ERROR_NOT_SUPPORTED);
    if (data[12] > 79 || data[13] >= 60 || data[14] >= 75) return mmc_write_parameter_error(drive,ERROR_INVALID_DATA);
    uint32_t end = ((uint32_t)data[12] * 60 + data[13]) * 75 + data[14];
    if (end <= 150) return mmc_write_parameter_error(drive,ERROR_INVALID_DATA); *sectors = end - 150; return true;
}
bool mmc_send_cd_mode1_cue(OpticalDrive* drive, uint32_t sectors) {
    if (sectors < 300 || sectors > 79 * 60 * 75 + 59 * 75 + 74 - 150) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER);
    uint8_t data[32] = {0x41,0,0,1,0,0,0,0, 0x41,1,0,0x14,0,0,0,0, 0x41,1,1,0x10,0,0,2,0, 0x41,0xAA,1,1,0,0,0,0};
    uint32_t end = sectors + 150; data[29] = (uint8_t)(end / (60 * 75)); data[30] = (uint8_t)(end / 75 % 60); data[31] = (uint8_t)(end % 75);
    return mmc_send_cd_cue(drive,data,sizeof(data));
}
bool mmc_read_cd_text_leadin(OpticalDrive* drive, MmcCdTextLeadIn* leadIn) {
    if (!leadIn) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER); memset(leadIn,0,sizeof(*leadIn));
    if (!drive) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER);
    MmcTrackInfo track; if (!mmc_read_track_information(drive,0,false,&track)) return false;
    /* MMC-5 6.44.3.3 仅将 -45150..-1 解释为负地址；预间隙 -150..-1 不属于导入区。 */
    if (track.trackNumber || track.trackStartLba < UINT32_C(0xFFFF4FA2) || track.trackStartLba > UINT32_C(0xFFFFFF69)) return mmc_write_parameter_error(drive,ERROR_INVALID_DATA);
    int32_t start = (int32_t)((int64_t)track.trackStartLba - INT64_C(4294967296)); uint32_t absolute = (uint32_t)(start + 450150);
    leadIn->startLba = start; leadIn->sectorCount = (uint32_t)(-150 - start);
    leadIn->minute = (uint8_t)(absolute / 4500); leadIn->second = (uint8_t)(absolute / 75 % 60); leadIn->frame = (uint8_t)(absolute % 75); return true;
}
bool mmc_write_cd_text_blocks(OpticalDrive* drive, int32_t startLba, uint16_t blockCount, const void* buffer) {
    if (!drive || !buffer || !blockCount || blockCount > 682 || startLba < -45150 || startLba >= -150 || (uint32_t)blockCount > (uint32_t)(-150 - startLba)) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER);
    /* SAO 的传输长度由 CUE 决定，01h 主通道不传数据，41h 只追加 96 字节原始子通道。 */
    uint8_t cdb[10] = {0x2A}; write_be32(cdb + 2,(uint32_t)startLba); write_be16(cdb + 7,blockCount); uint32_t bytes = (uint32_t)blockCount * 96;
    if (!spti_send_cdb(drive,cdb,10,(void*)buffer,bytes,false,180)) return false;
    if (drive->lastTransferBytes != bytes) return mmc_write_parameter_error(drive,ERROR_WRITE_FAULT); return true;
}
bool mmc_send_cd_cue(OpticalDrive* drive, const uint8_t* cue, uint32_t bytes) {
    if (!cue || bytes < 32 || bytes > 0xFFFFFF || bytes % 8) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER);
    uint8_t cdb[10] = {0x5D}; cdb[6] = (uint8_t)(bytes >> 16); cdb[7] = (uint8_t)(bytes >> 8); cdb[8] = (uint8_t)bytes;
    if (!spti_send_cdb(drive,cdb,10,(void*)cue,bytes,false,10)) return false;
    if (drive->lastTransferBytes != bytes) return mmc_write_parameter_error(drive,ERROR_WRITE_FAULT); return true;
}
bool mmc_write_cd_leadin_gap(OpticalDrive* drive) {
    /* 即使 CUE 指定由设备生成零数据，程序区的每一扇区也必须有对应 WRITE。 */
    uint8_t cdb[10] = {0x2A}; write_be32(cdb + 2,(uint32_t)-150); write_be16(cdb + 7,150);
    if (!spti_send_cdb(drive,cdb,10,NULL,0,false,180)) return false;
    if (drive->lastTransferBytes) return mmc_write_parameter_error(drive,ERROR_WRITE_FAULT); return true;
}
bool mmc_write_cd_audio_blocks(OpticalDrive* drive, uint32_t startLba, uint16_t blockCount, const void* buffer) {
    if (!buffer || !blockCount || blockCount > 27 || startLba > UINT32_MAX - ((uint32_t)blockCount - 1)) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER);
    uint8_t cdb[10] = {0x2A}; write_be32(cdb + 2,startLba); write_be16(cdb + 7,blockCount); uint32_t bytes = (uint32_t)blockCount * 2352;
    if (!spti_send_cdb(drive,cdb,10,(void*)buffer,bytes,false,startLba ? 30 : 180)) return false;
    if (drive->lastTransferBytes != bytes) return mmc_write_parameter_error(drive,ERROR_WRITE_FAULT); return true;
}
bool mmc_write_cd_mode1_blocks(OpticalDrive* drive, uint32_t startLba, uint16_t blockCount, const void* buffer) {
    if (!buffer || !blockCount || blockCount > 32 || startLba > UINT32_MAX - ((uint32_t)blockCount - 1)) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER);
    uint8_t cdb[10] = {0x2A}; write_be32(cdb + 2,startLba); write_be16(cdb + 7,blockCount); uint32_t bytes = (uint32_t)blockCount * 2048;
    /* 首次 WRITE 还要生成导入区；不能沿用普通数据块的短超时。 */
    if (!spti_send_cdb(drive,cdb,10,(void*)buffer,bytes,false,startLba ? 30 : 180)) return false;
    if (drive->lastTransferBytes != bytes) return mmc_write_parameter_error(drive,ERROR_WRITE_FAULT); return true;
}
bool mmc_finish_cd_sao(OpticalDrive* drive) {
    uint8_t cdb[10] = {0x35}; return spti_send_cdb(drive,cdb,10,NULL,0,true,180);
}
bool mmc_get_write_parameters(OpticalDrive* drive, MmcWriteParameters* parameters) {
    if (!parameters) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER); memset(parameters,0,sizeof(*parameters));
    uint8_t cdb[10] = {0x5A,8,5}, data[128] = {0}; cdb[8] = sizeof(data);
    if (!spti_send_cdb(drive,cdb,10,data,sizeof(data),true,10)) return false;
    if (drive->lastTransferBytes < 10 || drive->lastTransferBytes > sizeof(data)) return mmc_write_parameter_error(drive,ERROR_READ_FAULT);
    uint32_t declared = (uint32_t)read_be16(data) + 2;
    if (declared < 10 || declared > drive->lastTransferBytes) return mmc_write_parameter_error(drive,ERROR_READ_FAULT);
    /* MMC 不使用块描述符或模式子页；只接受完整的标准长度或带厂商尾部的页。 */
    if (read_be16(data + 6) || (data[8] & 0x7F) != 5 || (data[9] != 0x32 && data[9] != 0x36)) return mmc_write_parameter_error(drive,ERROR_INVALID_DATA);
    uint16_t size = (uint16_t)(data[9] + 2); if ((uint32_t)size + 8 > declared) return mmc_write_parameter_error(drive,ERROR_READ_FAULT);
    parameters->size = size; memcpy(parameters->page,data + 8,size); return true;
}
bool mmc_set_write_parameters(OpticalDrive* drive, const MmcWriteParameters* parameters) {
    if (!parameters || (parameters->size != 52 && parameters->size != 56) || (parameters->page[0] & 0x7F) != 5 || parameters->page[1] + 2 != parameters->size) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER);
    uint8_t cdb[10] = {0x55,0x10}, data[64] = {0}; uint16_t size = (uint16_t)(parameters->size + 8); write_be16(cdb + 7,size); memcpy(data + 8,parameters->page,parameters->size); data[8] &= 0x7F;
    if (!spti_send_cdb(drive,cdb,10,data,size,false,10)) return false;
    if (drive->lastTransferBytes != size) return mmc_write_parameter_error(drive,ERROR_WRITE_FAULT); return true;
}
bool mmc_perform_opc(OpticalDrive* drive) { uint8_t cdb[10] = {0x54,1}; return spti_send_cdb(drive,cdb,10,NULL,0,true,180); }

bool mmc_get_bd_write_features(OpticalDrive* drive, bool* verifyNotRequired) {
    if (!verifyNotRequired) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER); *verifyNotRequired = false;
    uint8_t cdb[10] = {0x46,2,0,0x41}, data[32] = {0}; cdb[8] = sizeof(data);
    if (!spti_send_cdb(drive,cdb,10,data,sizeof(data),true,10)) return false;
    if (drive->lastTransferBytes == 8 && read_be32(data) == 4) return mmc_write_parameter_error(drive,ERROR_NOT_SUPPORTED);
    if (drive->lastTransferBytes != sizeof(data) || read_be32(data) < 28) return mmc_write_parameter_error(drive,ERROR_READ_FAULT);
    if (read_be16(data + 6) != MMC_PROFILE_BD_R_SRM || read_be16(data + 8) != 0x41 || !(data[10] & 1) || data[11] != 20) return mmc_write_parameter_error(drive,ERROR_NOT_SUPPORTED);
    bool writable = false; for (unsigned i = 24; i < 32; i++) writable = writable || data[i] != 0;
    if (!writable) return mmc_write_parameter_error(drive,ERROR_NOT_SUPPORTED); *verifyNotRequired = (data[12] & 1) != 0; return true;
}
bool mmc_set_burn_speed(OpticalDrive* drive, MmcProfile profile, uint32_t writeSpeedKbps) {
    if (profile != MMC_PROFILE_BD_R_SRM && profile != MMC_PROFILE_DVD_R_SEQ && profile != MMC_PROFILE_DVD_PLUS_R && profile != MMC_PROFILE_CD_R) return mmc_write_parameter_error(drive,ERROR_NOT_SUPPORTED);
    uint32_t speed = !writeSpeedKbps || writeSpeedKbps == 0xFFFF ? UINT32_MAX : writeSpeedKbps;
    if (profile != MMC_PROFILE_BD_R_SRM) {
        if (speed != UINT32_MAX && speed >= 0xFFFF) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER);
        return mmc_set_speed(drive,0xFFFF,speed == UINT32_MAX ? 0xFFFF : (uint16_t)speed);
    }
    uint8_t cdb[12] = {0xB6}, data[28] = {0}; write_be16(cdb + 9,sizeof(data)); write_be32(data + 8,UINT32_MAX); write_be32(data + 12,UINT32_MAX); write_be32(data + 16,1000); write_be32(data + 20,speed); write_be32(data + 24,1000);
    if (!spti_send_cdb(drive,cdb,12,data,sizeof(data),false,10)) return false;
    if (drive->lastTransferBytes != sizeof(data)) return mmc_write_parameter_error(drive,ERROR_WRITE_FAULT); return true;
}
bool mmc_write_bd_blocks(OpticalDrive* drive, uint32_t startLba, uint16_t blockCount, const void* buffer, bool verifyNotRequired) {
    if (!verifyNotRequired) return mmc_write_blocks(drive,startLba,blockCount,buffer);
    if (!buffer || !blockCount || blockCount > 32 || startLba > UINT32_MAX - ((uint32_t)blockCount - 1)) return mmc_write_parameter_error(drive,ERROR_INVALID_PARAMETER);
    uint8_t cdb[12] = {0xAA}; write_be32(cdb + 2,startLba); write_be32(cdb + 6,blockCount); cdb[10] = 0x40;
    uint32_t size = (uint32_t)blockCount * 2048; if (!spti_send_cdb(drive,cdb,12,(void*)buffer,size,false,30)) return false;
    if (drive->lastTransferBytes != size) return mmc_write_parameter_error(drive,ERROR_WRITE_FAULT); return true;
}

bool mmc_write_blocks(OpticalDrive* drive, uint32_t startLba, uint16_t blockCount, const void* buffer) {
    if (!buffer || blockCount == 0 || blockCount > 32 || startLba > UINT32_MAX - ((uint32_t)blockCount - 1)) { if (drive) drive->lastError = ERROR_INVALID_PARAMETER; SetLastError(ERROR_INVALID_PARAMETER); return false; }

    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x2A; /* WRITE (10) */
    write_be32(&cdb[2], startLba);
    write_be16(&cdb[7], blockCount);

    uint32_t transferBytes = blockCount * 2048; /* Standard optical block size = 2048 */
    if (!spti_send_cdb(drive,cdb,10,(void*)buffer,transferBytes,false,30)) return false;
    if (drive->lastTransferBytes != transferBytes) { drive->lastError = ERROR_WRITE_FAULT; SetLastError(ERROR_WRITE_FAULT); return false; } return true;
}

bool mmc_read_blocks(OpticalDrive* drive, uint32_t startLba, uint16_t blockCount, void* buffer) {
    if (!buffer || !blockCount || startLba > UINT32_MAX - ((uint32_t)blockCount - 1)) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    if (!buffer || blockCount == 0) return false;

    uint8_t cdb[10] = { 0 };
    cdb[0] = 0x28; /* READ (10) */
    write_be32(&cdb[2], startLba);
    write_be16(&cdb[7], blockCount);

    uint32_t transferBytes = blockCount * 2048;
    if (!spti_send_cdb(drive, cdb, 10, buffer, transferBytes, true, 30)) return false;
    if (drive->lastTransferBytes != transferBytes) { drive->lastError = ERROR_READ_FAULT; SetLastError(ERROR_READ_FAULT); return false; }
    return true;
}

/* ISO 读取只接收一个完整会话中的单条 MODE1 数据轨；其他 CD 需要保留原始轨道布局。 */
bool mmc_check_iso_cd(OpticalDrive* drive, uint32_t sectors) {
    uint8_t cdb[12] = {0}, session[12] = {0}, toc[20] = {0}, sector[2352] = {0};
    cdb[0] = 0x43; cdb[2] = 1; cdb[8] = sizeof(session);
    if (!spti_send_cdb(drive,cdb,10,session,sizeof(session),true,10)) return false;
    if (drive->lastTransferBytes != sizeof(session) || read_be16(session) != 10 || session[2] != 1 || session[3] != 1) goto unsupported;
    cdb[2] = 0; cdb[8] = sizeof(toc);
    if (!spti_send_cdb(drive,cdb,10,toc,sizeof(toc),true,10)) return false;
    if (drive->lastTransferBytes != sizeof(toc) || read_be16(toc) != 18 || toc[2] != 1 || toc[3] != 1 || toc[6] != 1 || !(toc[5] & 4) || read_be32(toc + 8) || toc[14] != 0xAA || read_be32(toc + 16) != sectors) goto unsupported;
    memset(cdb,0,sizeof(cdb)); cdb[0] = 0xBE; cdb[8] = 1; cdb[9] = 0xF8;
    if (!spti_send_cdb(drive,cdb,12,sector,sizeof(sector),true,30)) return false;
    if (drive->lastTransferBytes != sizeof(sector) || sector[0] || sector[11] || sector[15] != 1) goto unsupported;
    for (unsigned i = 1; i < 11; i++) if (sector[i] != 0xFF) goto unsupported;
    return true;
unsupported:
    drive->lastError = ERROR_NOT_SUPPORTED; SetLastError(ERROR_NOT_SUPPORTED); return false;
}

static bool mmc_cd_text_failure(OpticalDrive* drive, DWORD error) {
    if (drive) drive->lastError = error; SetLastError(error); return false;
}
static bool mmc_cd_text_command(OpticalDrive* drive, uint8_t* data, uint16_t bytes) {
    uint8_t cdb[10] = {0x43,0,5}; write_be16(cdb + 7,bytes);
    /* 传输失败可能不刷新sense，不能把旧的“不支持”掩盖为本次结果。 */
    memset(&drive->lastSense,0,sizeof(drive->lastSense));
    if (spti_send_cdb(drive,cdb,10,data,bytes,true,10)) return true;
    const ScsiSenseResult* sense = &drive->lastSense;
    if (sense->senseKey == 5 && ((sense->asc == 0x20 && !sense->ascq) || (sense->asc == 0x24 && !sense->ascq))) return mmc_cd_text_failure(drive,ERROR_NOT_SUPPORTED);
    return false;
}
bool mmc_read_cd_text(OpticalDrive* drive, uint8_t** packs, uint32_t* bytes) {
    if (packs) *packs = NULL; if (bytes) *bytes = 0;
    if (!drive || !packs || !bytes) return mmc_cd_text_failure(drive,ERROR_INVALID_PARAMETER);
    uint8_t header[4] = {0};
    if (!mmc_cd_text_command(drive,header,sizeof(header))) return false;
    if (drive->lastTransferBytes != sizeof(header)) return mmc_cd_text_failure(drive,ERROR_READ_FAULT);
    uint32_t total = (uint32_t)read_be16(header) + 2;
    if (total < 4 || total > UINT16_MAX || (total - 4) % 18 || header[2] || header[3]) return mmc_cd_text_failure(drive,ERROR_INVALID_DATA);
    if (total == 4) { SetLastError(ERROR_SUCCESS); return true; }
    uint8_t* data = malloc(total);
    if (!data) return mmc_cd_text_failure(drive,ERROR_NOT_ENOUGH_MEMORY);
    if (!mmc_cd_text_command(drive,data,(uint16_t)total)) { DWORD error = GetLastError(); free(data); SetLastError(error); return false; }
    DWORD error = ERROR_SUCCESS;
    if (drive->lastTransferBytes != total) error = ERROR_READ_FAULT;
    else if (memcmp(data,header,sizeof(header))) error = ERROR_MEDIA_CHANGED;
    if (error) { free(data); return mmc_cd_text_failure(drive,error); }
    memmove(data,data + 4,total - 4); *packs = data; *bytes = total - 4; SetLastError(ERROR_SUCCESS); return true;
}

static bool mmc_read_cd_identifier(OpticalDrive* drive, unsigned track, bool isrc, char* value) {
    unsigned chars = isrc ? 12 : 13;
    if (value) memset(value,0,chars + 1);
    if (!drive || !value || (isrc && (!track || track > 99))) return mmc_cd_text_failure(drive,ERROR_INVALID_PARAMETER);
    uint8_t cdb[10] = {0x42,0,0x40}, data[24] = {0}; cdb[3] = isrc ? 3 : 2; cdb[6] = (uint8_t)track; cdb[8] = sizeof(data);
    /* 清除前一条命令的状态，避免将传输故障误判成设备明确不支持。 */
    memset(&drive->lastSense,0,sizeof(drive->lastSense)); drive->lastTransferBytes = 0; drive->lastError = ERROR_SUCCESS; SetLastError(ERROR_SUCCESS);
    if (!spti_send_cdb(drive,cdb,10,data,sizeof(data),true,10)) {
        const ScsiSenseResult* sense = &drive->lastSense;
        DWORD error = sense->senseKey == 5 && !sense->ascq && (sense->asc == 0x20 || sense->asc == 0x24) ? ERROR_NOT_SUPPORTED : drive->lastError;
        if (!error) error = GetLastError(); return mmc_cd_text_failure(drive,error ? error : ERROR_IO_DEVICE);
    }
    if (drive->lastTransferBytes < 4 || drive->lastTransferBytes > sizeof(data)) return mmc_cd_text_failure(drive,ERROR_READ_FAULT);
    /* 仅返回头且数据长度为零时没有编号；不读取未传输的有效标志。 */
    if (!read_be16(data + 2)) { drive->lastError = ERROR_SUCCESS; SetLastError(ERROR_SUCCESS); return true; }
    if (read_be16(data + 2) != sizeof(data) - 4) return mmc_cd_text_failure(drive,ERROR_INVALID_DATA);
    if (drive->lastTransferBytes != sizeof(data)) return mmc_cd_text_failure(drive,ERROR_READ_FAULT);
    if (data[4] != cdb[3] || (isrc && data[6] != track)) return mmc_cd_text_failure(drive,ERROR_INVALID_DATA);
    if (data[8] & 0x80) {
        bool empty = true, zeros = true;
        for (unsigned i = 0; i < chars; i++) { if (data[9 + i]) empty = false; if (data[9 + i] != '0') zeros = false; }
        if (!empty && !zeros) {
            for (unsigned i = 0; i < chars; i++) {
                unsigned c = data[9 + i]; if (isrc && c >= 'a' && c <= 'z') c -= 'a' - 'A';
                bool digit = c >= '0' && c <= '9', letter = c >= 'A' && c <= 'Z';
                if (isrc ? (i < 2 ? !letter : i < 5 ? !letter && !digit : !digit) : !digit) { memset(value,0,chars + 1); return mmc_cd_text_failure(drive,ERROR_INVALID_DATA); }
                value[i] = (char)c;
            }
        }
    }
    drive->lastError = ERROR_SUCCESS; SetLastError(ERROR_SUCCESS); return true;
}
bool mmc_read_cd_catalog(OpticalDrive* drive, char catalog[14]) { return mmc_read_cd_identifier(drive,0,false,catalog); }
bool mmc_read_cd_isrc(OpticalDrive* drive, unsigned track, char isrc[13]) { return mmc_read_cd_identifier(drive,track,true,isrc); }

bool mmc_read_cd_toc(OpticalDrive* drive, MmcCdToc* toc) {
    if (!toc) { SetLastError(ERROR_INVALID_PARAMETER); return false; } memset(toc,0,sizeof(*toc));
    uint8_t cdb[10] = {0x43,0,1}, session[12] = {0}, data[804] = {0}; cdb[8] = sizeof(session);
    if (!spti_send_cdb(drive,cdb,10,session,sizeof(session),true,10)) return false;
    if (drive->lastTransferBytes != sizeof(session) || read_be16(session) != 10) goto invalid;
    if (session[2] != 1 || session[3] != 1) { drive->lastError = ERROR_NOT_SUPPORTED; SetLastError(ERROR_NOT_SUPPORTED); return false; }
    cdb[2] = 0; write_be16(cdb + 7,sizeof(data));
    if (!spti_send_cdb(drive,cdb,10,data,sizeof(data),true,10)) return false;
    unsigned bytes = (unsigned)read_be16(data) + 2, count = data[3];
    if (data[2] != 1 || !count || count > 99 || bytes != 4 + (count + 1) * 8 || drive->lastTransferBytes < bytes || drive->lastTransferBytes > sizeof(data)) goto invalid;
    for (unsigned i = 0; i <= count; i++) {
        const uint8_t* entry = data + 4 + i * 8; uint32_t start = read_be32(entry + 4);
        if ((entry[1] >> 4) != 1 || entry[2] != (i == count ? 0xAA : i + 1) || start >= 100 * 60 * 75 || (i && start <= toc->tracks[i - 1].startLba)) goto invalid;
        if (i == count) toc->leadOutLba = start;
        else { toc->tracks[i].number = (uint8_t)i + 1; toc->tracks[i].control = entry[1] & 15; toc->tracks[i].startLba = start; }
    }
    toc->trackCount = count; return true;
invalid:
    memset(toc,0,sizeof(*toc)); drive->lastError = ERROR_INVALID_DATA; SetLastError(ERROR_INVALID_DATA); return false;
}

static bool mmc_read_cd_transfer(OpticalDrive* drive, uint32_t lba, uint16_t count, void* buffer, unsigned subChannel) {
    uint32_t stride = subChannel == 1 ? 2448 : subChannel == 2 ? 2368 : 2352;
    if (!buffer || !count || subChannel > 2 || count > 65536 / stride || lba > UINT32_MAX - ((uint32_t)count - 1)) { if (drive) drive->lastError = ERROR_INVALID_PARAMETER; SetLastError(ERROR_INVALID_PARAMETER); return false; }
    uint8_t cdb[12] = {0xBE}; write_be32(cdb + 2,lba); cdb[7] = (uint8_t)(count >> 8); cdb[8] = (uint8_t)count; cdb[9] = 0xF8; cdb[10] = (uint8_t)subChannel;
    uint32_t bytes = (uint32_t)count * stride;
    if (!spti_send_cdb(drive,cdb,12,buffer,bytes,true,30)) return false;
    if (drive->lastTransferBytes != bytes) { drive->lastError = ERROR_READ_FAULT; SetLastError(ERROR_READ_FAULT); return false; }
    return true;
}
bool mmc_read_cd_blocks(OpticalDrive* drive, uint32_t lba, uint16_t count, void* buffer) { return mmc_read_cd_transfer(drive,lba,count,buffer,2); }
bool mmc_read_cd_main_blocks(OpticalDrive* drive, uint32_t lba, uint16_t count, void* buffer) { return mmc_read_cd_transfer(drive,lba,count,buffer,0); }
bool mmc_read_cd_raw_sector(OpticalDrive* drive, uint32_t lba, bool withSubChannel, void* buffer) { return mmc_read_cd_transfer(drive,lba,1,buffer,withSubChannel ? 1 : 0); }
