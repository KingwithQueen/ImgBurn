#include "imgburn_cd_sector.h"
#include <string.h>

static uint8_t cd_bcd(uint32_t value) {
    return (uint8_t)((value / 10) * 16 + value % 10);
}

static uint32_t cd_edc(const uint8_t* bytes, size_t count) {
    uint32_t remainder = 0;
    /* ECMA-130的两个16次多项式之积，按最低位优先使用反射值0xD8018001，初值为0，末值不异或。 */
    for (size_t i = 0; i < count; i++) {
        remainder ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) remainder = (remainder >> 1) ^ ((remainder & 1) ? UINT32_C(0xD8018001) : 0);
    }
    return remainder;
}

static void cd_store_edc(uint8_t* destination, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) destination[i] = (uint8_t)(value >> (i * 8));
}

static uint8_t cd_gf_alpha(uint8_t value) {
    /* GF(256)由x^8+x^4+x^3+x^2+1生成，原始元素为2。 */
    return (uint8_t)(((unsigned)value << 1) ^ ((value & 0x80) ? 0x11D : 0));
}

static uint8_t cd_gf_multiply(uint8_t left, uint8_t right) {
    uint8_t result = 0;
    while (right) { if (right & 1) result ^= left; left = cd_gf_alpha(left); right >>= 1; }
    return result;
}

static void cd_parity_vector(const uint8_t* source, unsigned sourceBytes, unsigned start, unsigned step, unsigned count, uint8_t* first, uint8_t* second) {
    uint8_t sum = 0, weighted = 0;
    unsigned index = start;
    for (unsigned i = 0; i < count; i++) {
        uint8_t value = source[index];
        sum ^= value; weighted = cd_gf_alpha((uint8_t)(weighted ^ value));
        index += step; if (index >= sourceBytes) index -= sourceBytes;
    }
    /* 两个校验方程分别要求全向量异或为0，以及以alpha的降幂加权后异或为0。 */
    weighted = cd_gf_alpha(weighted);
    *first = cd_gf_multiply((uint8_t)(weighted ^ sum), 0xF4); /* 0xF4为alpha+1即3的乘法逆元。 */
    *second = (uint8_t)(sum ^ *first);
}

static void cd_ecc(uint8_t frame[2352]) {
    /* P按43列的两个字节平面生成，Q再沿包括P的26条对角线生成。 */
    for (unsigned column = 0; column < 86; column++) cd_parity_vector(frame + 12, 2064, column, 86, 24, frame + 2076 + column, frame + 2162 + column);
    for (unsigned diagonal = 0; diagonal < 52; diagonal++) {
        unsigned start = (diagonal / 2) * 86 + diagonal % 2;
        cd_parity_vector(frame + 12, 2236, start, 88, 43, frame + 2248 + diagonal, frame + 2300 + diagonal);
    }
}

bool imgburn_cd_normalize_sector(TrackDataMode mode, uint32_t lba, uint8_t frame[2352]) {
    if (!frame) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    if (mode == TRACK_MODE_AUDIO) return true;
    if (mode != TRACK_MODE_MODE1_2352 && mode != TRACK_MODE_MODE2_2352) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (lba >= 100 * 60 * 75 - 150) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    if (mode == TRACK_MODE_MODE2_2352 && memcmp(frame + 16, frame + 20, 4)) { SetLastError(ERROR_INVALID_DATA); return false; }
    uint32_t address = lba + 150;
    frame[0] = 0; memset(frame + 1, 0xFF, 10); frame[11] = 0;
    frame[12] = cd_bcd(address / (60 * 75)); frame[13] = cd_bcd(address / 75 % 60); frame[14] = cd_bcd(address % 75);
    frame[15] = mode == TRACK_MODE_MODE1_2352 ? 1 : 2;
    if (mode == TRACK_MODE_MODE1_2352) {
        cd_store_edc(frame + 2064, cd_edc(frame, 2064)); memset(frame + 2068, 0, 8); cd_ecc(frame);
    } else if (frame[18] & 0x20) cd_store_edc(frame + 2348, cd_edc(frame + 16, 2332));
    else {
        cd_store_edc(frame + 2072, cd_edc(frame + 16, 2056));
        uint8_t header[4]; memcpy(header, frame + 12, 4); memset(frame + 12, 0, 4);
        cd_ecc(frame); memcpy(frame + 12, header, 4);
    }
    return true;
}

bool imgburn_cd_zero_sector(TrackDataMode mode, uint32_t lba, uint8_t frame[2352]) {
    if (!frame) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    if (mode != TRACK_MODE_AUDIO && mode != TRACK_MODE_MODE1_2352 && mode != TRACK_MODE_MODE2_2352) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (mode != TRACK_MODE_AUDIO && lba >= 100 * 60 * 75 - 150) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    memset(frame, 0, 2352);
    if (mode == TRACK_MODE_MODE2_2352) frame[18] = frame[22] = 0x08;
    return imgburn_cd_normalize_sector(mode, lba, frame);
}
