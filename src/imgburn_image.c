/**
 * @file imgburn_image.c
 * @brief Optical Disc Image Parsers & Generators (CUE, BIN, MDS, ISO, DVD-Video IFO)
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Convert MM:SS:FF string to LBA */
static uint32_t msf_to_lba(const char *msf) {
    int m = 0, s = 0, f = 0;
    if (sscanf(msf, "%d:%d:%d", &m, &s, &f) == 3) {
        return (uint32_t)((m * 60 + s) * 75 + f);
    }
    return 0;
}

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

/* Helper to strip quotes */
static void strip_quotes(char *str) {
    str_trim(str);
    size_t len = strlen(str);
    if (len >= 2 && str[0] == '"' && str[len - 1] == '"') {
        str[len - 1] = '\0';
        memmove(str, str + 1, len - 1);
    }
}

/* -------------------------------------------------------------------------
 * Image Format Detection
 * ------------------------------------------------------------------------- */
DiscImageType imgburn_image_detect_type(const wchar_t *filePath) {
    if (!filePath) return IMAGE_TYPE_UNKNOWN;

    /* Check if directory */
    DWORD attr = GetFileAttributesW(filePath);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        wchar_t checkPath[MAX_PATH];
        swprintf(checkPath, MAX_PATH, L"%ls\\VIDEO_TS", filePath);
        if (GetFileAttributesW(checkPath) != INVALID_FILE_ATTRIBUTES) {
            return IMAGE_TYPE_DVD_FOLDER;
        }
        swprintf(checkPath, MAX_PATH, L"%ls\\BDMV", filePath);
        if (GetFileAttributesW(checkPath) != INVALID_FILE_ATTRIBUTES) {
            return IMAGE_TYPE_DVD_FOLDER;
        }
        return IMAGE_TYPE_UNKNOWN;
    }

    /* Check extension */
    const wchar_t *dot = wcsrchr(filePath, L'.');
    if (dot) {
        if (_wcsicmp(dot, L".cue") == 0) return IMAGE_TYPE_CUE;
        if (_wcsicmp(dot, L".mds") == 0) return IMAGE_TYPE_MDS;
        if (_wcsicmp(dot, L".iso") == 0) return IMAGE_TYPE_ISO;
        if (_wcsicmp(dot, L".img") == 0) return IMAGE_TYPE_ISO;
        if (_wcsicmp(dot, L".bin") == 0) return IMAGE_TYPE_BIN;
        if (_wcsicmp(dot, L".mdf") == 0) return IMAGE_TYPE_BIN;
    }

    /* Content magic inspection */
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return IMAGE_TYPE_UNKNOWN;

    uint8_t header[64] = { 0 };
    DWORD readBytes = 0;
    ReadFile(hFile, header, sizeof(header), &readBytes, NULL);

    /* Check MDS magic */
    if (readBytes >= 16 && memcmp(header, "MEDIA DESCRIPTOR", 16) == 0) {
        CloseHandle(hFile);
        return IMAGE_TYPE_MDS;
    }

    /* Check CUE text */
    if (readBytes >= 4) {
        if (memcmp(header, "FILE", 4) == 0 ||
            memcmp(header, "TRACK", 5) == 0 ||
            memcmp(header, "CATALOG", 7) == 0 ||
            memcmp(header, "TITLE", 5) == 0 ||
            memcmp(header, "REM", 3) == 0) {
            CloseHandle(hFile);
            return IMAGE_TYPE_CUE;
        }
    }

    /* Check ISO9660 / UDF header at sector 16 (offset 32768) */
    LARGE_INTEGER offset;
    offset.QuadPart = 16 * 2048;
    if (SetFilePointerEx(hFile, offset, NULL, FILE_BEGIN)) {
        uint8_t s16[16] = { 0 };
        ReadFile(hFile, s16, sizeof(s16), &readBytes, NULL);
        if (readBytes >= 6 && memcmp(&s16[1], "CD001", 5) == 0) {
            CloseHandle(hFile);
            return IMAGE_TYPE_ISO;
        }
        if (readBytes >= 6 && memcmp(&s16[1], "BEA01", 5) == 0) {
            CloseHandle(hFile);
            return IMAGE_TYPE_ISO;
        }
    }

    CloseHandle(hFile);
    return IMAGE_TYPE_BIN;
}

/* -------------------------------------------------------------------------
 * CUE Sheet Parser
 * ------------------------------------------------------------------------- */
bool imgburn_cue_parse(const wchar_t *cueFilePath, CueSheet *outCue) {
    if (!cueFilePath || !outCue) return false;
    memset(outCue, 0, sizeof(*outCue));
    wcscpy(outCue->cueFilePath, cueFilePath);

    FILE *f = _wfopen(cueFilePath, L"r");
    if (!f) return false;

    /* Extract directory of CUE file for resolving relative paths */
    wchar_t cueDir[MAX_PATH] = { 0 };
    wcscpy(cueDir, cueFilePath);
    wchar_t *lastSlash = wcsrchr(cueDir, L'\\');
    if (!lastSlash) lastSlash = wcsrchr(cueDir, L'/');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    else cueDir[0] = L'\0';

    char line[512];
    wchar_t currentFile[MAX_PATH] = { 0 };
    CueTrack *currentTrack = NULL;

    while (fgets(line, sizeof(line), f)) {
        str_trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        if (_strnicmp(line, "REM", 3) == 0) {
            /* Comment / Metadata */
            continue;
        }

        if (_strnicmp(line, "TITLE", 5) == 0) {
            char val[256];
            strncpy(val, line + 5, sizeof(val) - 1);
            strip_quotes(val);
            if (currentTrack) strncpy(currentTrack->title, val, sizeof(currentTrack->title) - 1);
            else strncpy(outCue->discTitle, val, sizeof(outCue->discTitle) - 1);
            continue;
        }

        if (_strnicmp(line, "PERFORMER", 9) == 0) {
            char val[256];
            strncpy(val, line + 9, sizeof(val) - 1);
            strip_quotes(val);
            if (currentTrack) strncpy(currentTrack->performer, val, sizeof(currentTrack->performer) - 1);
            else strncpy(outCue->discPerformer, val, sizeof(outCue->discPerformer) - 1);
            continue;
        }

        if (_strnicmp(line, "CATALOG", 7) == 0) {
            char val[256];
            strncpy(val, line + 7, sizeof(val) - 1);
            strip_quotes(val);
            strncpy(outCue->catalogNumber, val, sizeof(outCue->catalogNumber) - 1);
            continue;
        }

        if (_strnicmp(line, "FILE", 4) == 0) {
            char fnA[256] = { 0 };
            char typeA[32] = { 0 };
            /* Format: FILE "filename" TYPE */
            const char *startQuote = strchr(line, '"');
            if (startQuote) {
                const char *endQuote = strchr(startQuote + 1, '"');
                if (endQuote) {
                    size_t fnLen = endQuote - startQuote - 1;
                    if (fnLen < sizeof(fnA)) {
                        memcpy(fnA, startQuote + 1, fnLen);
                        fnA[fnLen] = '\0';
                    }
                }
            } else {
                sscanf(line + 4, "%255s %31s", fnA, typeA);
            }

            wchar_t fnW[MAX_PATH] = { 0 };
            MultiByteToWideChar(CP_UTF8, 0, fnA, -1, fnW, MAX_PATH);

            if (wcschr(fnW, L':') || fnW[0] == L'\\') {
                wcscpy(currentFile, fnW);
            } else {
                swprintf(currentFile, MAX_PATH, L"%ls%ls", cueDir, fnW);
            }
            continue;
        }

        if (_strnicmp(line, "TRACK", 5) == 0) {
            if (outCue->trackCount >= MAX_CUE_TRACKS) continue;
            currentTrack = &outCue->tracks[outCue->trackCount++];
            int trkNum = 1;
            char modeStr[64] = { 0 };
            sscanf(line + 5, "%d %63s", &trkNum, modeStr);
            currentTrack->trackNumber = trkNum;
            wcscpy(currentTrack->sourceFilePath, currentFile);

            if (_stricmp(modeStr, "AUDIO") == 0) currentTrack->mode = TRACK_MODE_AUDIO;
            else if (_stricmp(modeStr, "MODE1/2048") == 0) currentTrack->mode = TRACK_MODE_MODE1_2048;
            else if (_stricmp(modeStr, "MODE1/2352") == 0) currentTrack->mode = TRACK_MODE_MODE1_2352;
            else if (_stricmp(modeStr, "MODE2/2336") == 0) currentTrack->mode = TRACK_MODE_MODE2_2336;
            else if (_stricmp(modeStr, "MODE2/2352") == 0) currentTrack->mode = TRACK_MODE_MODE2_2352;
            else currentTrack->mode = TRACK_MODE_AUDIO;
            continue;
        }

        if (_strnicmp(line, "INDEX", 5) == 0 && currentTrack) {
            if (currentTrack->indexCount >= MAX_CUE_INDICES) continue;
            int idxNum = 1;
            char msfStr[32] = { 0 };
            sscanf(line + 5, "%d %31s", &idxNum, msfStr);
            CueIndex *ci = &currentTrack->indices[currentTrack->indexCount++];
            ci->indexNumber = idxNum;
            ci->lba = msf_to_lba(msfStr);
            strncpy(ci->msfStr, msfStr, sizeof(ci->msfStr) - 1);

            if (idxNum == 1) {
                currentTrack->startLba = ci->lba;
            }
            continue;
        }

        if (_strnicmp(line, "PREGAP", 6) == 0 && currentTrack) {
            char msfStr[32] = { 0 };
            sscanf(line + 6, "%31s", msfStr);
            currentTrack->pregapSectors = msf_to_lba(msfStr);
            continue;
        }

        if (_strnicmp(line, "POSTGAP", 7) == 0 && currentTrack) {
            char msfStr[32] = { 0 };
            sscanf(line + 7, "%31s", msfStr);
            currentTrack->postgapSectors = msf_to_lba(msfStr);
            continue;
        }
    }

    fclose(f);

    /* Calculate total sectors from last track */
    if (outCue->trackCount > 0) {
        CueTrack *last = &outCue->tracks[outCue->trackCount - 1];
        outCue->totalSectors = last->startLba + last->sectorCount;
    }

    return (outCue->trackCount > 0);
}

/* -------------------------------------------------------------------------
 * Media Descriptor Sheet (.MDS) Parser & Creator
 * ------------------------------------------------------------------------- */
bool imgburn_mds_parse(const wchar_t *mdsFilePath, MdsSheet *outMds) {
    if (!mdsFilePath || !outMds) return false;
    memset(outMds, 0, sizeof(*outMds));
    wcscpy(outMds->mdsFilePath, mdsFilePath);

    HANDLE hFile = CreateFileW(mdsFilePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    MdsHeader hdr;
    DWORD readBytes = 0;
    if (!ReadFile(hFile, &hdr, sizeof(hdr), &readBytes, NULL) || readBytes < sizeof(hdr)) {
        CloseHandle(hFile);
        return false;
    }

    if (memcmp(hdr.signature, "MEDIA DESCRIPTOR", 16) != 0) {
        CloseHandle(hFile);
        return false;
    }

    outMds->mediumType = hdr.mediumType;

    /* Associate data file (.mdf or .iso) */
    wcscpy(outMds->dataFilePath, mdsFilePath);
    wchar_t *dot = wcsrchr(outMds->dataFilePath, L'.');
    if (dot) {
        wcscpy(dot, L".mdf");
        if (GetFileAttributesW(outMds->dataFilePath) == INVALID_FILE_ATTRIBUTES) {
            wcscpy(dot, L".iso");
        }
    }

    /* Read session blocks */
    if (hdr.sessionsOffset != 0 && hdr.sessionCount > 0) {
        SetFilePointer(hFile, hdr.sessionsOffset, NULL, FILE_BEGIN);
        MdsSessionBlock sb;
        if (ReadFile(hFile, &sb, sizeof(sb), &readBytes, NULL)) {
            outMds->totalSectors = (uint32_t)(sb.sessionEndLba - sb.sessionStartLba);
        }
    }

    /* If DVD-DL, search for Layer Break in Disc Structures block */
    if (hdr.mediumType == 2 && hdr.discStructuresOffset != 0) {
        SetFilePointer(hFile, hdr.discStructuresOffset, NULL, FILE_BEGIN);
        uint8_t dsb[64] = { 0 };
        if (ReadFile(hFile, dsb, sizeof(dsb), &readBytes, NULL)) {
            /* Layer Break is stored as Layer 0 End Sector (e.g. 0x001F0000) */
            uint32_t l0End = (dsb[4] << 24) | (dsb[5] << 16) | (dsb[6] << 8) | dsb[7];
            if (l0End > 0x030000) {
                outMds->layerBreakLba = l0End - 0x030000 + 1;
            }
        }
    }

    CloseHandle(hFile);
    return true;
}

bool imgburn_mds_create(const wchar_t *mdsFilePath,
                        const wchar_t *dataFileName,
                        uint32_t totalSectors,
                        uint32_t layerBreakLba) {
    if (!mdsFilePath || !dataFileName || totalSectors == 0) return false;

    HANDLE hFile = CreateFileW(mdsFilePath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    MdsHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.signature, "MEDIA DESCRIPTOR", 16);
    hdr.version[0] = 1;
    hdr.version[1] = 3;
    hdr.mediumType = (layerBreakLba > 0) ? 2 : 1; /* 1 = DVD, 2 = DVD-DL */
    hdr.sessionCount = 1;

    hdr.sessionsOffset = sizeof(MdsHeader);
    hdr.tracksOffset = hdr.sessionsOffset + sizeof(MdsSessionBlock);
    hdr.discStructuresOffset = (layerBreakLba > 0) ? (hdr.tracksOffset + sizeof(MdsTrackBlock)) : 0;

    MdsSessionBlock sb;
    memset(&sb, 0, sizeof(sb));
    sb.sessionStartLba = 0;
    sb.sessionEndLba = (int32_t)totalSectors;
    sb.sessionNumber = 1;
    sb.totalTracks = 1;
    sb.firstTrack = 1;
    sb.lastTrack = 1;

    MdsTrackBlock tb;
    memset(&tb, 0, sizeof(tb));
    tb.trackMode = 0xA9; /* DVD Mode 1 */
    tb.trackNumber = 1;
    tb.sectorSize = 2048;
    tb.startLba = 0;
    tb.startByteOffset = 0;

    DWORD written = 0;
    WriteFile(hFile, &hdr, sizeof(hdr), &written, NULL);
    WriteFile(hFile, &sb, sizeof(sb), &written, NULL);
    WriteFile(hFile, &tb, sizeof(tb), &written, NULL);

    /* If Dual-Layer DVD, write DVD Physical Format Information containing Layer 0 capacity */
    if (layerBreakLba > 0) {
        uint8_t dsb[64] = { 0 };
        /* DVD-ROM / DVD+R DL Layer 0 end sector address (0x30000 + layerBreakLba - 1) */
        uint32_t l0End = 0x030000 + layerBreakLba - 1;
        dsb[0] = 0x01; /* DVD Physical Format */
        dsb[1] = 0x02; /* Dual layer */
        dsb[2] = 0x21; /* Opposite Track Path (OTP) */
        dsb[4] = (uint8_t)(l0End >> 24);
        dsb[5] = (uint8_t)(l0End >> 16);
        dsb[6] = (uint8_t)(l0End >> 8);
        dsb[7] = (uint8_t)(l0End & 0xFF);
        WriteFile(hFile, dsb, sizeof(dsb), &written, NULL);
    }

    CloseHandle(hFile);
    return true;
}

/* -------------------------------------------------------------------------
 * DVD-Video IFO Parser & Layer Break Candidate Analyzer
 * ------------------------------------------------------------------------- */
bool imgburn_dvd_find_layer_breaks(const wchar_t *videoTsPath,
                                   uint32_t totalImageSectors,
                                   DvdLayerBreakCandidate *outCandidates,
                                   int maxCandidates,
                                   int *outCount) {
    if (!videoTsPath || !outCandidates || maxCandidates <= 0 || !outCount) return false;
    *outCount = 0;

    uint32_t midPoint = totalImageSectors / 2;
    /* Minimum L0 is midPoint; Maximum L0 for DVD+R DL is 2,086,912 */
    uint32_t minL0 = midPoint > 50000 ? (midPoint - 50000) : midPoint;
    uint32_t maxL0 = 2086912;
    if (totalImageSectors < maxL0 * 2) {
        maxL0 = (totalImageSectors > 2086912) ? 2086912 : totalImageSectors;
    }

    /* Enumerate all VTS_*.IFO files */
    WIN32_FIND_DATAW fd;
    wchar_t searchPattern[MAX_PATH];
    swprintf(searchPattern, MAX_PATH, L"%ls\\VTS_*_0.IFO", videoTsPath);

    HANDLE hFind = FindFirstFileW(searchPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        /* Try without _0 suffix */
        swprintf(searchPattern, MAX_PATH, L"%ls\\VIDEO_TS.IFO", videoTsPath);
        hFind = FindFirstFileW(searchPattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE) return false;
    }

    do {
        wchar_t ifoPath[MAX_PATH];
        swprintf(ifoPath, MAX_PATH, L"%ls\\%ls", videoTsPath, fd.cFileName);

        HANDLE hIfo = CreateFileW(ifoPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                  OPEN_EXISTING, 0, NULL);
        if (hIfo == INVALID_HANDLE_VALUE) continue;

        /* Verify DVDVIDEO signature */
        uint8_t ifoHdr[256];
        DWORD readBytes = 0;
        if (ReadFile(hIfo, ifoHdr, sizeof(ifoHdr), &readBytes, NULL) && readBytes == 256) {
            if (memcmp(ifoHdr, "DVDVIDEO-VTS", 12) == 0) {
                /* Cell Address Table (VTS_C_ADT) pointer at offset 0x00E0 */
                uint32_t cAdtSector = (ifoHdr[0xE0] << 24) | (ifoHdr[0xE1] << 16) |
                                      (ifoHdr[0xE2] << 8)  | ifoHdr[0xE3];

                if (cAdtSector > 0) {
                    LARGE_INTEGER pos;
                    pos.QuadPart = (int64_t)cAdtSector * 2048;
                    SetFilePointerEx(hIfo, pos, NULL, FILE_BEGIN);

                    uint8_t cAdtHdr[8];
                    if (ReadFile(hIfo, cAdtHdr, 8, &readBytes, NULL) && readBytes == 8) {
                        uint16_t cellCount = (cAdtHdr[0] << 8) | cAdtHdr[1];
                        if (cellCount > 256) cellCount = 256;

                        for (uint16_t c = 0; c < cellCount && *outCount < maxCandidates; c++) {
                            uint8_t cellRec[12];
                            if (ReadFile(hIfo, cellRec, 12, &readBytes, NULL) && readBytes == 12) {
                                uint16_t vobId = (cellRec[0] << 8) | cellRec[1];
                                uint16_t cellId = cellRec[2];
                                uint32_t startSec = (cellRec[4] << 24) | (cellRec[5] << 16) |
                                                    (cellRec[6] << 8)  | cellRec[7];

                                /* Check if cell falls within valid Layer Break boundary */
                                if (startSec >= minL0 && startSec <= maxL0) {
                                    DvdLayerBreakCandidate *cand = &outCandidates[(*outCount)++];
                                    cand->lba = startSec;
                                    cand->vobId = vobId;
                                    cand->cellId = cellId;
                                    cand->isSeamless = (cellRec[3] & 0x02) != 0;
                                    cand->isChapterStart = (cellId == 1 || (cellRec[3] & 0x01) != 0);

                                    /* Suitability rating */
                                    int score = 70;
                                    if (!cand->isSeamless) score += 20;
                                    if (cand->isChapterStart) score += 10;
                                    cand->suitabilityRating = score;

                                    snprintf(cand->description, sizeof(cand->description),
                                             "VOB %u, Cell %u (%s)",
                                             vobId, cellId,
                                             cand->isChapterStart ? "Chapter Start" : "Inter-Scene");
                                }
                            }
                        }
                    }
                }
            }
        }
        CloseHandle(hIfo);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    /* If no IFO cells found (e.g. data image), provide automatic midpoint candidate */
    if (*outCount == 0 && totalImageSectors > 2000000) {
        DvdLayerBreakCandidate *cand = &outCandidates[(*outCount)++];
        cand->lba = midPoint;
        cand->vobId = 1;
        cand->cellId = 1;
        cand->isSeamless = false;
        cand->isChapterStart = true;
        cand->suitabilityRating = 80;
        snprintf(cand->description, sizeof(cand->description), "Midpoint Auto L0 Alignment");
    }

    return (*outCount > 0);
}
