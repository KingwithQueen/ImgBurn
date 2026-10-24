/**
 * @file imgburn_cli.c
 * @brief ImgBurn Console CLI - Unified Command-Line Optical Disc Suite
 *
 * Implements command-line operations for:
 * - Device discovery & capability inspection (`devices`, `info`)
 * - ISO/Joliet/UDF Filesystem Image Building (`build`)
 * - Disc Burning with real-time telemetry (`burn`)
 * - Verification / Sector comparison (`verify`)
 * - BookType / Bitsetting control (`booktype`)
 * - Pioneer drive controls (PureRead, QuietMode, Optimal Speed) (`pioneer`)
 * - Multi-disc Changer & Autoloader control (`changer`)
 * - Image inspection (`cue`, `mds`)
 *
 * Pure C99/C11, Win32 API.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "imgburn_spti.h"
#include "imgburn_mmc.h"
#include "imgburn_fs.h"
#include "imgburn_udf.h"
#include "imgburn_pipeline.h"
#include "imgburn_sector.h"
#include "imgburn_vendor.h"
#include "imgburn_image.h"
#include "imgburn_cdtext.h"
#include "imgburn_changer.h"
#include "imgburn_gui.h"
#include "imgburn_preview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

/* 内部路径统一使用UTF-8；过长或非法参数不得留下未初始化的宽字符缓冲区。 */
static bool utf8_path(const char* input, wchar_t* output, int capacity) {
    if (!input || !output || capacity <= 0) return false;
    output[0] = L'\0';
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, output, capacity) > 0) return true;
    output[0] = L'\0';
    printf("Error: Invalid or unsupported path length.\n");
    return false;
}

static void print_banner(void) {
    printf("=================================================================\n");
    printf(" ImgBurn Core CLI - Optical Disc Authoring & Burn Suite (Pure C)\n");
    printf(" Win32 Native Edition - C17 Reimplementation In Progress\n");
    printf("=================================================================\n\n");
}

static void print_usage(void) {
    printf("Usage: imgburn <command> [arguments...]\n\n");
    printf("Device & Information Commands:\n");
    printf("  devices                        List all optical drives on system\n");
    printf("  info <drive>                   Detailed drive & loaded media inspection\n");
    printf("  booktype <drive> [query|set <dvd_rom|default> [dvd+r|dvd+r_dl]]\n");
    printf("                                 Query or set drive BookType / Bitsetting\n");
    printf("  pioneer <drive> <feature> [value]\n");
    printf("                                 Configure Pioneer OptimalSpeed / PureRead / QuietMode\n");
    printf("  changer <drive> <status|load <slot>|unload>\n");
    printf("                                 Embedded changer / jukebox controls\n\n");
    printf("Authoring & Image Commands:\n");
    printf("  build <out.iso> <folder> [--label <vol>] [--udf <1.02|2.50>]\n");
    printf("        [--dvd-video | --bluray]  Build DVD-Video or Blu-ray layout\n");
    printf("        [--dvd-break <VTS:PGC:Cell> | --dvd-menu-break <VTS:LU:PGC:Cell>]\n");
    printf("        [--seamless] [--show-interleaved]\n");
    printf("        [--l0-min <sectors>] [--l0-max <sectors>] [--capacity <sectors>]\n");
    printf("                                 Build ISO9660 + Joliet + UDF disc image\n");
    printf("  cue <file.cue>                 Parse and display CUE sheet tracks & CD-Text\n");
    printf("  cdtext-info <file.cdt>         Inspect raw CD-TEXT packs and supported text blocks\n");
    printf("  cdtext-build <file.cue> <out.cdt> [--replace]\n");
    printf("                                 Export CDTEXTFILE or encode supported CUE text\n");
    printf("  mds <file.mds>                 Parse and display MDS metadata & Layer Break\n\n");
    printf("  dvd-layers <folder> [--show-interleaved] [--l0-min <n>] [--l0-max <n>] [--capacity <n>]\n");
    printf("                                 Inspect DVD navigation and layer break cells\n\n");
    printf("  dvd-preview <folder> --title <VTS:PGC:Cell> | --menu <VTS:LU:PGC:Cell> [--ffmpeg <path>]\n");
    printf("Burning & Verification Commands:\n");
    printf("  sector <drive> <lba|--last> [--raw|--raw-sub]\n");
    printf("                                 Inspect one sector as hexadecimal and ASCII\n");
    printf("  cd-toc <drive>                Display complete CD sessions, tracks and lead-outs\n");
    printf("  read <drive> <out.iso|out.mds|out.cue> [--retries <0..20>] [--replace] [--cdtext] [--split <MiB>]\n");
    printf("                                 Read data ISO or raw CD BIN/CUE with Q indexes\n");
    printf("                                 MDS destination saves MODE1 CD or single/dual-layer DVD data files\n");
    printf("                                 --split sets a per-file MiB limit for MDS output; 0 disables splitting\n");
    printf("  burn <drive> <image> [--speed <Nx|MAX>] [--test] [--verify] [--reserve] [--no-cycle] [--opc]\n");
    printf("                                 Burn ISO/BIN, data MDS or supported CD CUE images\n");
    printf("                                 DVD+R DL requires an OTP MDS with an aligned layer break\n");
    printf("  erase <drive> [--quick|--full]   Erase CD-RW/DVD-RW; cancellation waits for the device\n");
    printf("  discovery <drive> [--sectors <n>] [--speed <Nx|MAX>] [--test]\n");
    printf("        [--verify|--no-verify] [--no-cycle] [--opc]\n");
    printf("                                 Write zero data; all free sectors and verify by default\n");
    printf("  verify <drive> <image|--readable> [--retries <0..20>] [--compare-audio]\n");
    printf("                                 Verify data ISO/BIN/MDS or raw CD CUE; audio defaults to readability\n\n");
    printf("Examples:\n");
    printf("  imgburn devices\n");
    printf("  imgburn info I:\n");
    printf("  imgburn build my_disc.iso D:\\MyFolder --label \"BACKUP_2026\"\n");
    printf("  imgburn burn I: my_disc.iso --speed 8x --verify\n");
    printf("  imgburn booktype I: set dvd_rom dvd+r_dl\n");
    printf("  imgburn pioneer I: pureread master\n");
}

/* Helper to parse drive string (e.g. "I:", "I", "\\.\I:", "\\.\CdRom0") */
static bool parse_drive_path(const char *arg, wchar_t *outPath, size_t maxLen) {
    if (!arg || !outPath || maxLen < 32) return false;

    if (arg[0] == '\\' && arg[1] == '\\') {
        return utf8_path(arg, outPath, (int)maxLen);
    }

    char letter = 0;
    if (strlen(arg) == 1 && isalpha((unsigned char)arg[0])) {
        letter = (char)toupper((unsigned char)arg[0]);
    } else if (strlen(arg) >= 2 && isalpha((unsigned char)arg[0]) && arg[1] == ':') {
        /* 完整文件路径不能被截断为卷设备，避免向错误的盘符发送命令。 */
        if (strlen(arg) != 2) { printf("Error: Invalid drive path; use a drive letter or an explicit device path.\n"); return false; }
        letter = (char)toupper((unsigned char)arg[0]);
    }

    if (letter) {
        swprintf(outPath, maxLen, L"\\\\.\\%c:", letter);
        return true;
    }

    return utf8_path(arg, outPath, (int)maxLen);
}

/* -------------------------------------------------------------------------
 * Command: devices
 * ------------------------------------------------------------------------- */
static int cmd_devices(void) {
    OpticalDriveInfo drives[MAX_OPTICAL_DRIVES];
    int count = spti_enumerate_drives(drives, MAX_OPTICAL_DRIVES);
    if (count <= 0) {
        printf("No optical disc drives found on this system.\n");
        return 0;
    }

    printf("Discovered %d Optical Drive(s):\n", count);
    printf("-----------------------------------------------------------------\n");
    printf(" #  Letter  Path          Vendor   Product           Rev    Bus\n");
    printf("-----------------------------------------------------------------\n");

    for (int i = 0; i < count; i++) {
        char busName[16] = "Unknown";
        if (drives[i].busType == 0x08) strcpy(busName, "SATA/ATAPI");
        else if (drives[i].busType == 0x07) strcpy(busName, "USB");
        else if (drives[i].busType == 0x01) strcpy(busName, "SCSI");

        drive_vendor_t v = imgburn_vendor_detect(drives[i].vendorId, drives[i].productId);

        printf("[%d]  %c:     %-12ls  %-8s %-16s  %-4s   %s (%s)\n",
               i,
               drives[i].driveLetter ? drives[i].driveLetter : '?',
               drives[i].devicePath,
               drives[i].vendorId[0] ? drives[i].vendorId : "N/A",
               drives[i].productId[0] ? drives[i].productId : "N/A",
               drives[i].productRevision[0] ? drives[i].productRevision : "N/A",
               busName,
               imgburn_vendor_name(v));
    }
    printf("-----------------------------------------------------------------\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Command: info <drive>
 * ------------------------------------------------------------------------- */
static int cmd_info(const char *driveArg) {
    wchar_t path[MAX_PATH];
    if (!parse_drive_path(driveArg, path, MAX_PATH)) {
        printf("Invalid drive specified: '%s'\n", driveArg);
        return 1;
    }

    wprintf(L"Opening optical device: %ls ...\n", path);
    OpticalDrive drive;
    if (!spti_open_drive(&drive, path, false)) {
        printf("Error: Failed to open drive (error code = %u)\n", drive.lastError);
        return 1;
    }

    char vendor[9] = { 0 }, product[17] = { 0 }, revision[5] = { 0 };
    uint8_t inq[36] = { 0 };
    if (mmc_inquiry(&drive, inq, sizeof(inq))) {
        memcpy(vendor, &inq[8], 8);
        memcpy(product, &inq[16], 16);
        memcpy(revision, &inq[32], 4);
    } else {
        strncpy(vendor, drive.info.vendorId, sizeof(vendor) - 1);
        strncpy(product, drive.info.productId, sizeof(product) - 1);
        strncpy(revision, drive.info.productRevision, sizeof(revision) - 1);
    }

    drive_vendor_t v = imgburn_vendor_detect(vendor, product);

    printf("\nDevice Hardware Information:\n");
    printf("  Vendor ID:            '%s'\n", vendor);
    printf("  Product ID:           '%s'\n", product);
    printf("  Firmware Revision:    '%s'\n", revision);
    printf("  Vendor Profile:       %s\n", imgburn_vendor_name(v));

    /* Test Unit Ready */
    bool ready = mmc_test_unit_ready(&drive);
    printf("  Drive Status:         %s\n", ready ? "READY (Disc Present)" : "NOT READY (Tray Open / Empty)");

    if (ready) {
        MmcProfile prof = mmc_get_current_profile(&drive);
        printf("  Current Media:        %s (Profile 0x%04X)\n", mmc_get_profile_name(prof), prof);

        uint32_t lastLba = 0, blockSize = 0;
        if (mmc_read_capacity(&drive, &lastLba, &blockSize)) {
            uint64_t totalBytes = (uint64_t)(lastLba + 1) * blockSize;
            printf("  Media Capacity:       %u Sectors (%.2f GB / %.2f MB)\n",
                   lastLba + 1,
                   (double)totalBytes / (1024.0 * 1024.0 * 1024.0),
                   (double)totalBytes / (1024.0 * 1024.0));
        }
    }

    /* Query BookType */
    booktype_setting_t btPlusR = BOOKTYPE_SETTING_UNKNOWN;
    booktype_setting_t btPlusRDL = BOOKTYPE_SETTING_UNKNOWN;
    imgburn_booktype_query(&drive, v, BOOKTYPE_MEDIA_DVD_PLUS_R, &btPlusR);
    imgburn_booktype_query(&drive, v, BOOKTYPE_MEDIA_DVD_PLUS_R_DL, &btPlusRDL);
    printf("  DVD+R BookType:       %s\n", imgburn_booktype_name(btPlusR));
    printf("  DVD+R DL BookType:    %s\n", imgburn_booktype_name(btPlusRDL));

    /* If Pioneer */
    if (v == DRIVE_VENDOR_PIONEER) {
        bool opt_en = false, opt_save = false;
        if (imgburn_pioneer_get_optimal_speed(&drive, &opt_en, &opt_save)) {
            printf("  Optimal Write Speed:  %s (Saved to drive: %s)\n",
                   opt_en ? "ON" : "OFF", opt_save ? "YES" : "NO");
        }
        pioneer_pureread_mode_t pmode = PIONEER_PUREREAD_STANDARD;
        bool psupp = false;
        if (imgburn_pioneer_get_pureread(&drive, &pmode, &psupp) && psupp) {
            const char *pmodeStr = (pmode == PIONEER_PUREREAD_PERFECT) ? "Perfect" :
                                   (pmode == PIONEER_PUREREAD_MASTER)  ? "Master" : "Standard";
            printf("  Pioneer PureRead:     %s Mode (Active)\n", pmodeStr);
        }
    }

    /* Embedded Changer status */
    EmbeddedChangerStatus cs;
    if (imgburn_changer_get_status(&drive, &cs)) {
        printf("  Embedded Changer:     Total Slots = %u, Current Slot = %u\n",
               cs.totalSlots, cs.currentSlot);
    }

    spti_close_drive(&drive);
    return 0;
}

/* -------------------------------------------------------------------------
 * Command: build <out.iso> <folder> [options]
 * ------------------------------------------------------------------------- */
static void on_cli_build_progress(int percent, uint32_t currentLba, uint32_t totalLba, void* userData) {
    (void)userData;
    printf("\r  Building Image: %3d%% [Sector %u / %u]", percent, currentLba, totalLba);
    fflush(stdout);
}

static bool dvd_profile_option(const char* option, const char* value, DvdMediaProfile* profile) {
    if (!value || !value[0] || strspn(value, "0123456789") != strlen(value) || strlen(value) > 8) return false;
    unsigned long number = strtoul(value, NULL, 10); if (number > 4173824 || (!number && _stricmp(option, "--l0-min"))) return false;
    if (!_stricmp(option, "--l0-min")) profile->minimumL0 = (uint32_t)number;
    else if (!_stricmp(option, "--l0-max")) profile->maximumL0 = (uint32_t)number;
    else if (!_stricmp(option, "--capacity")) profile->mediaCapacity = (uint32_t)number;
    else return false;
    return true;
}

static DvdLayerBreakCandidate* dvd_candidates(ImageBuilder* builder, const DvdMediaProfile* profile, unsigned options, size_t* count) {
    if (!imgburn_dvd_layer_break_candidates_ex(builder, profile, options, NULL, 0, count) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) return NULL;
    if (!*count || *count > SIZE_MAX / sizeof(DvdLayerBreakCandidate)) return NULL;
    DvdLayerBreakCandidate* candidates = (DvdLayerBreakCandidate*)calloc(*count, sizeof(*candidates));
    if (candidates && !imgburn_dvd_layer_break_candidates_ex(builder, profile, options, candidates, *count, count)) { free(candidates); return NULL; }
    return candidates;
}

static int cmd_dvd_layers(int argc, char** argv) {
    DvdMediaProfile profile = {0,2086912,4173824}; wchar_t folder[MAX_PATH]; unsigned options = 0;
    if (argc < 3 || !utf8_path(argv[2], folder, MAX_PATH)) return 1;
    for (int i = 3; i < argc; i++) {
        if (!_stricmp(argv[i], "--show-interleaved")) options |= DVD_LAYER_SHOW_INTERLEAVED;
        else if (i + 1 == argc || !dvd_profile_option(argv[i], argv[i + 1], &profile)) { printf("Error: Invalid DVD media profile.\n"); return 1; }
        else i++;
    }
    ImageBuilder b; if (!image_builder_init(&b, "DVD_NAVIGATION")) return 1;
    b.config.dvdVideoCompliance = true; b.config.fsTypes = FS_TYPE_ISO_UDF;
    bool ok = image_builder_add_folder(&b, folder, L"") && image_builder_calculate_layout(&b); size_t count = 0;
    DvdLayerBreakCandidate* candidates = ok ? dvd_candidates(&b, &profile, options, &count) : NULL;
    if (candidates) {
        printf("DVD image sectors: %u; candidates: %zu\n", b.config.totalImageSectors, count);
        printf("VTS:PGC:Cell or M<VTS:LU:PGC:Cell>  VOB:Cell  VOB-sector  L0-sectors  Padding  Total  Chapter  SPLIP  Language  ILVU  Angle\n");
        for (size_t i = 0; i < count; i++) {
            const DvdLayerBreakCandidate* c = &candidates[i];
            if (c->domain == DVD_DOMAIN_TITLE) printf("%u:%u:%u", c->titleSet, c->pgc, c->cell); else printf("M%u:%u:%u:%u", c->titleSet, c->languageUnit, c->pgc, c->cell);
            printf("  %u:%u  %u  %u  %u  %u  %u:%u  %s  %04X  %s  %s\n", c->vobId, c->cellId, c->vobSector, c->lba, c->paddingSectors, c->imageSectors, c->title, c->chapter, c->isSeamless ? "Yes" : "No", c->languageCode, c->isInterleaved ? "Yes" : "No", c->isAngleBlock ? "Yes" : "No");
        }
    } else printf("Error: Invalid DVD navigation, media profile or no usable layer break cells.\n");
    bool success = candidates != NULL; free(candidates); image_builder_destroy(&b); return success ? 0 : 1;
}

static int cmd_dvd_preview(int argc, char** argv) {
    if (argc < 5) return 1; wchar_t folder[MAX_PATH], ffmpeg[MAX_PATH]; const wchar_t* decoder = NULL; unsigned vts = 0, lu = 0, pgc = 0, cell = 0; DvdNavigationDomain domain; char extra;
    if (!utf8_path(argv[2], folder, MAX_PATH)) return 1;
    if (!_stricmp(argv[3], "--title") && sscanf(argv[4], "%3u:%4u:%3u%c", &vts, &pgc, &cell, &extra) == 3) domain = DVD_DOMAIN_TITLE;
    else if (!_stricmp(argv[3], "--menu") && sscanf(argv[4], "%3u:%3u:%4u:%3u%c", &vts, &lu, &pgc, &cell, &extra) == 4) domain = vts ? DVD_DOMAIN_VTS_MENU : DVD_DOMAIN_VMG_MENU;
    else return 1;
    if (argc == 7 && !_stricmp(argv[5], "--ffmpeg") && utf8_path(argv[6], ffmpeg, MAX_PATH)) decoder = ffmpeg; else if (argc != 5) return 1;
    ImageBuilder b; if (!image_builder_init(&b, "DVD_PREVIEW")) return 1; b.config.dvdVideoCompliance = true; b.config.fsTypes = FS_TYPE_ISO_UDF; DvdPreviewPlan plan;
    bool ok = image_builder_add_folder(&b, folder, L"") && image_builder_calculate_layout(&b) && imgburn_dvd_preview_plan(&b, domain, vts, lu, pgc, cell, &plan);
    int result = ok ? imgburn_dvd_preview_show(&b, &plan, decoder) : 1; if (result) printf("Error: DVD preview failed. Check the selected cell and FFmpeg decoder.\n"); image_builder_destroy(&b); return result;
}

static int cmd_build(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: imgburn build <out.iso> <folder> [--label <vol>] [--udf <1.02|2.50>] [--dvd-video | --bluray]\n");
        return 1;
    }

    const char *outIsoA = argv[2];
    const char *folderA = argv[3];
    const char *labelA = "IMGBURN_DISC";
    UdfRevision udfRev = UDF_REV_102;
    bool dvdVideo = false, bluray = false, seamless = false, menuBreak = false; unsigned dvdOptions = 0;
    const char* breakCell = NULL; DvdMediaProfile profile = {0,2086912,4173824};

    for (int i = 4; i < argc; i++) {
        if (_stricmp(argv[i], "--label") == 0 && i + 1 < argc) {
            labelA = argv[++i];
        } else if (_stricmp(argv[i], "--udf") == 0 && i + 1 < argc) {
            const char *ver = argv[++i];
            if (strcmp(ver, "2.50") == 0) udfRev = UDF_REV_250;
            else if (strcmp(ver, "1.02") != 0) { printf("Error: Unsupported UDF revision.\n"); return 1; }
        } else if (!_stricmp(argv[i], "--dvd-video")) dvdVideo = true;
        else if (!_stricmp(argv[i], "--bluray")) bluray = true;
        else if ((!_stricmp(argv[i], "--dvd-break") || !_stricmp(argv[i], "--dvd-menu-break")) && i + 1 < argc) { if (breakCell) return 1; menuBreak = !_stricmp(argv[i], "--dvd-menu-break"); breakCell = argv[++i]; }
        else if (!_stricmp(argv[i], "--seamless")) seamless = true;
        else if (!_stricmp(argv[i], "--show-interleaved")) dvdOptions |= DVD_LAYER_SHOW_INTERLEAVED;
        else if ((!_stricmp(argv[i], "--l0-min") || !_stricmp(argv[i], "--l0-max") || !_stricmp(argv[i], "--capacity")) && i + 1 < argc) { if (!dvd_profile_option(argv[i], argv[i + 1], &profile)) return 1; i++; }
        else {
            printf("Error: Unknown or incomplete build option: %s\n", argv[i]);
            return 1;
        }
    }
    if (dvdVideo && bluray) { printf("Error: Choose one video layout.\n"); return 1; }
    if (((breakCell || dvdOptions) && !dvdVideo) || (seamless && !breakCell)) { printf("Error: A DVD-Video layer break cell is required.\n"); return 1; }

    wchar_t outIsoW[MAX_PATH], folderW[MAX_PATH], mdsPath[MAX_PATH] = L"";
    if (!utf8_path(outIsoA, outIsoW, MAX_PATH) || !utf8_path(folderA, folderW, MAX_PATH)) return 1;

    printf("Initializing ImageBuilder with Volume Label: '%s'\n", labelA);
    ImageBuilder b;
    if (!image_builder_init(&b, labelA)) {
        printf("Failed to initialize ImageBuilder.\n");
        return 1;
    }
    b.config.udfRev = udfRev;
    b.config.dvdVideoCompliance = dvdVideo; b.config.blurayCompliance = bluray;
    if (dvdVideo) { b.config.fsTypes = FS_TYPE_ISO_UDF; b.config.udfRev = UDF_REV_102; }
    if (bluray) { b.config.fsTypes = FS_TYPE_UDF; b.config.udfRev = UDF_REV_250; }

    wprintf(L"Scanning source folder: %ls ...\n", folderW);
    if (!image_builder_add_folder(&b, folderW, L"")) {
        printf("Error: Cannot scan the complete source directory.\n");
        image_builder_destroy(&b);
        return 1;
    }

    printf("Calculating filesystem layout and deduplication...\n");
    uint32_t sectors = image_builder_calculate_layout(&b);
    if (sectors == 0) { printf("Error: Cannot calculate image layout.\n"); image_builder_destroy(&b); return 1; }
    if (breakCell) {
        unsigned vts = 0, lu = 0, pgc = 0, cell = 0; char trailing; size_t count = 0; bool selected = false;
        int fields = menuBreak ? sscanf(breakCell, "%3u:%3u:%4u:%3u%c", &vts, &lu, &pgc, &cell, &trailing) : sscanf(breakCell, "%3u:%4u:%3u%c", &vts, &pgc, &cell, &trailing);
        if (fields != (menuBreak ? 4 : 3) || vts > 99 || lu > 99 || pgc > 999 || cell > 255) { printf("Error: Invalid DVD layer break cell.\n"); image_builder_destroy(&b); return 1; }
        DvdNavigationDomain domain = !menuBreak ? DVD_DOMAIN_TITLE : vts ? DVD_DOMAIN_VTS_MENU : DVD_DOMAIN_VMG_MENU;
        DvdLayerBreakCandidate* candidates = dvd_candidates(&b, &profile, dvdOptions, &count);
        for (size_t i = 0; candidates && i < count; i++) if (candidates[i].titleSet == vts && candidates[i].domain == domain && candidates[i].languageUnit == lu && candidates[i].pgc == pgc && candidates[i].cell == cell) selected = imgburn_dvd_select_layer_break(&b, &profile, &candidates[i], seamless);
        free(candidates);
        if (!selected || !(sectors = image_builder_calculate_layout(&b))) { printf("Error: The selected DVD cell cannot be used with this layout and media profile.\n"); image_builder_destroy(&b); return 1; }
        printf("  Layer Break:             %u (padding %u sectors)\n", b.config.dvdLayerBreakLba, b.config.dvdBreakPadding);
    }
    if (dvdVideo && !breakCell && sectors > 2295104) { printf("Error: Select a DVD layer break cell before building this double layer image.\n"); image_builder_destroy(&b); return 1; }
    if (dvdVideo) {
        DWORD chars = GetFullPathNameW(outIsoW, MAX_PATH, mdsPath, NULL);
        if (!chars || chars >= MAX_PATH) { image_builder_destroy(&b); return 1; }
        wchar_t* dot = wcsrchr(mdsPath, L'.'); wchar_t* slash = wcsrchr(mdsPath, L'\\');
        if (!dot || (slash && dot < slash)) dot = mdsPath + wcslen(mdsPath);
        if ((size_t)(dot - mdsPath) + 4 >= MAX_PATH) { image_builder_destroy(&b); return 1; }
        bool samePath = !_wcsicmp(dot, L".mds"); wcscpy(dot, L".mds");
        /* 自动生成的MDS也必须在写入ISO之前接受源文件保护检查。 */
        if (samePath || !image_builder_check_destination(&b, mdsPath)) { printf("Error: The MDS destination conflicts with an input file.\n"); image_builder_destroy(&b); return 1; }
    }
    printf("  Total Files:             %u\n", b.config.totalFiles);
    printf("  Total Directories:       %u\n", b.config.totalDirectories);
    printf("  Source Data Size:        %.2f MB (%llu bytes)\n",
           (double)b.config.totalSourceBytes / (1024.0 * 1024.0), b.config.totalSourceBytes);
    printf("  Deduplication Saved:     %.2f MB (%llu bytes)\n",
           (double)b.config.deduplicatedBytesSaved / (1024.0 * 1024.0), b.config.deduplicatedBytesSaved);
    printf("  Total Image Sectors:     %u LBA (%.2f MB)\n",
           sectors, ((double)sectors * 2048.0) / (1024.0 * 1024.0));

    wprintf(L"Writing ISO image to: %ls\n", outIsoW);
    bool ok = image_builder_create_iso(&b, outIsoW, on_cli_build_progress, NULL);
    printf("\n");

    if (ok && dvdVideo) {
        wchar_t full[MAX_PATH]; DWORD chars = GetFullPathNameW(outIsoW, MAX_PATH, full, NULL);
        ok = chars && chars < MAX_PATH && image_builder_check_destination(&b, mdsPath) && imgburn_mds_create(mdsPath, full, sectors, b.config.dvdLayerBreakLba);
        if (!ok) printf("Error: The ISO was created but its MDS descriptor could not be written.\n");
    }

    image_builder_destroy(&b);

    if (ok) {
        printf("Image built successfully: %s\n", outIsoA);
        return 0;
    } else {
        printf("Error: Failed to write ISO image.\n");
        return 1;
    }
}

/* -------------------------------------------------------------------------
 * Command: burn <drive> <image> [options]
 * ------------------------------------------------------------------------- */
static void on_cli_burn_progress(const PipelineStats* p, void* userData) {
    (void)userData;
    printf("\r  [%s] %3d%% | LBA %u/%u | %4.1f MB/s (%4.1fx) | Buf: %3d%% / Dev: %3d%% | ETA: %02u:%02u",
           p->statusMessage[0] ? p->statusMessage : "Writing",
           p->progressPercent,
           p->currentLba, p->totalSectors,
           p->currentSpeedKBps / 1024.0,
           p->speedMultiplier,
           p->hostBufferPercent, p->deviceBufferPercent,
           p->remainingTimeSec / 60, p->remainingTimeSec % 60);
    fflush(stdout);
}

/* Read 与界面共用同一数据读取流程；显式参数控制重试和目标替换。 */
static HANDLE cliReadCancel;
static SRWLOCK cliReadLock = SRWLOCK_INIT;
static BOOL WINAPI cli_read_control(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    AcquireSRWLockShared(&cliReadLock); BOOL handled = cliReadCancel != NULL; if (handled) SetEvent(cliReadCancel); ReleaseSRWLockShared(&cliReadLock); return handled;
}
static int cmd_read(int argc, char** argv) {
    if (argc < 4) { printf("Usage: imgburn read <drive> <out.iso|out.mds|out.cue> [--retries <0..20>] [--replace] [--cdtext] [--split <MiB>]\n"); return 1; }
    ReadConfig config = {0}; config.retryCount = 20;
    for (int i = 4; i < argc; i++) {
        if (!_stricmp(argv[i],"--replace")) config.replaceExisting = true;
        else if (!_stricmp(argv[i],"--cdtext")) config.readCdText = true;
        else if (!_stricmp(argv[i],"--split") && i + 1 < argc) { char* end; const char* value = argv[++i]; unsigned long n = strtoul(value,&end,10); if (!value[0] || *end || n > UINT32_MAX / 512 || value[0] == '-') { printf("Error: Split size must be a whole MiB value from 0 to %u.\n",UINT32_MAX / 512); return 1; } config.splitSectors = (uint32_t)n * 512; }
        else if (!_stricmp(argv[i],"--retries") && i + 1 < argc) { char* end; const char* value = argv[++i]; unsigned long n = strtoul(value,&end,10); if (!value[0] || *end || n > 20 || value[0] == '-') { printf("Error: Retry count must be 0..20.\n"); return 1; } config.retryCount = (unsigned)n; }
        else { printf("Error: Unknown or incomplete read option: %s\n",argv[i]); return 1; }
    }
    wchar_t device[MAX_PATH], image[MAX_PATH]; if (!parse_drive_path(argv[2],device,MAX_PATH) || !utf8_path(argv[3],image,MAX_PATH)) return 1;
    const wchar_t* extension = wcsrchr(image,L'.'); bool rawCd = extension && !_wcsicmp(extension,L".cue");
    if (config.readCdText && !rawCd) { printf("Error: --cdtext requires a CUE destination.\n"); return 1; }
    if (config.splitSectors && (!extension || _wcsicmp(extension,L".mds"))) { printf("Error: --split requires an MDS destination.\n"); return 1; }
    OpticalDrive drive; if (!spti_open_drive(&drive,device,false)) { printf("Error: Cannot open source drive (%lu).\n",drive.lastError); return 1; }
    if (drive.accessKnown && (drive.deviceAccess & (GENERIC_READ | GENERIC_WRITE)) != (GENERIC_READ | GENERIC_WRITE)) { printf("Error: Source device opened without read/write command access. SPTI requires both access rights; try running as administrator.\n"); spti_close_drive(&drive); return 1; }
    HANDLE cancel = CreateEventW(NULL,TRUE,FALSE,NULL); if (!cancel) { spti_close_drive(&drive); return 1; }
    AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = cancel; ReleaseSRWLockExclusive(&cliReadLock);
    bool control = SetConsoleCtrlHandler(cli_read_control,TRUE) != FALSE;
    config.drive = &drive; config.imagePath = image; config.cancelEvent = cancel;
    bool ok = rawCd ? pipeline_read_cd(&config,on_cli_burn_progress,NULL) : pipeline_read_disc(&config,on_cli_burn_progress,NULL); DWORD error = GetLastError();
    if (control) SetConsoleCtrlHandler(cli_read_control,FALSE); AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = NULL; CloseHandle(cancel); ReleaseSRWLockExclusive(&cliReadLock); spti_close_drive(&drive);
    if (ok) printf(extension && !_wcsicmp(extension,L".mds") ? "\nDisc image and MDS descriptor created successfully; the MDS lists its data files in order.\n" : "\nDisc image read successfully.\n");
    else printf("\nDisc read failed (error %lu).\n",error); return ok ? 0 : 1;
}

/* 整盘擦除使用同一后台命令流程；Ctrl+C 不打断正在进行的物理擦除。 */
static int cmd_erase(int argc, char** argv) {
    if (argc < 3 || argc > 4 || (argc == 4 && _stricmp(argv[3],"--quick") && _stricmp(argv[3],"--full"))) { printf("Usage: imgburn erase <drive> [--quick|--full]\n"); return 1; }
    wchar_t path[MAX_PATH]; if (!parse_drive_path(argv[2],path,MAX_PATH)) return 1;
    OpticalDrive drive; if (!spti_open_drive(&drive,path,false)) { printf("Error: Failed to open drive (error %lu).\n",GetLastError()); return 1; }
    HANDLE cancel = CreateEventW(NULL,TRUE,FALSE,NULL); if (!cancel) { spti_close_drive(&drive); return 1; }
    EraseConfig config = {&drive,cancel,argc == 4 && !_stricmp(argv[3],"--full")};
    AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = cancel; ReleaseSRWLockExclusive(&cliReadLock); bool control = SetConsoleCtrlHandler(cli_read_control,TRUE) != FALSE;
    bool ok = pipeline_erase_disc(&config,on_cli_burn_progress,NULL); DWORD error = GetLastError();
    if (control) SetConsoleCtrlHandler(cli_read_control,FALSE); AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = NULL; CloseHandle(cancel); ReleaseSRWLockExclusive(&cliReadLock); spti_close_drive(&drive);
    printf(ok ? "\nDisc erase completed.\n" : "\nDisc erase failed or was cancelled (error %lu).\n",error); return ok ? 0 : 1;
}

static int cmd_burn(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: imgburn burn <drive> <image> [--speed <Nx|MAX>] [--test] [--verify] [--reserve] [--no-cycle] [--opc]\n");
        return 1;
    }

    const char *driveArg = argv[2];
    const char *imageArg = argv[3];
    bool testMode = false;
    bool verifyAfter = false;
    double speed = 0; bool reserve = false, cycle = true, opc = false;

    for (int i = 4; i < argc; i++) {
        if (_stricmp(argv[i], "--test") == 0) testMode = true;
        else if (_stricmp(argv[i], "--verify") == 0) verifyAfter = true;
        else if (_stricmp(argv[i], "--reserve") == 0) reserve = true;
        else if (_stricmp(argv[i], "--no-cycle") == 0) cycle = false;
        else if (_stricmp(argv[i], "--opc") == 0) opc = true;
        else if (_stricmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            const char* value = argv[++i]; if (!_stricmp(value,"MAX")) speed = 0;
            else { char* end; speed = strtod(value,&end); if ((*end == 'x' || *end == 'X') && !end[1]) end++; if (!value[0] || *end || !isfinite(speed) || speed < 1 || speed * 4495.5 + 0.5 >= UINT32_MAX) { printf("Error: Invalid write speed.\n"); return 1; } }
        } else { printf("Error: Unknown or incomplete burn option.\n"); return 1; }
    }

    wchar_t drivePath[MAX_PATH], imagePath[MAX_PATH];
    if (!parse_drive_path(driveArg, drivePath, MAX_PATH) || !utf8_path(imageArg, imagePath, MAX_PATH)) return 1;

    wprintf(L"Target Drive: %ls\n", drivePath);
    wprintf(L"Source Image: %ls\n", imagePath);
    printf("Options: TestMode=%s, VerifyAfterBurn=%s, Speed=%.2fx (0=MAX)\n\n",
           testMode ? "YES" : "NO", verifyAfter && !testMode ? "YES" : "NO", speed);

    OpticalDrive drive;
    if (!spti_open_drive(&drive, drivePath, true)) {
        printf("Error: Failed to open drive with exclusive lock.\n");
        return 1;
    }

    BurnConfig config;
    memset(&config, 0, sizeof(config));
    config.drive = &drive;
    config.imagePath = imagePath;
    config.testMode = testMode;
    config.verifyAfterWrite = verifyAfter;
    MmcProfile speedProfile = speed > 0 ? mmc_get_current_profile(&drive) : MMC_PROFILE_NONE;
    if (speed > 0 && speedProfile == MMC_PROFILE_UNKNOWN) { printf("Error: Cannot identify media for write speed.\n"); spti_close_drive(&drive); return 1; }
    config.writeSpeedKbps = speed > 0 ? (uint32_t)(speed * (speedProfile == MMC_PROFILE_BD_R_SRM ? 4495.5 : speedProfile == MMC_PROFILE_CD_R ? 176.4 : 1385.0) + 0.5) : UINT32_MAX;
    config.ringBufferBytes = 32 * 1024 * 1024;
    config.reserveTrack = reserve; config.cycleTrayBeforeVerify = cycle; config.performOpc = opc;
    HANDLE cancel = CreateEventW(NULL,TRUE,FALSE,NULL); if (!cancel) { spti_close_drive(&drive); return 1; } config.cancelEvent = cancel;
    AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = cancel; ReleaseSRWLockExclusive(&cliReadLock); bool control = SetConsoleCtrlHandler(cli_read_control,TRUE) != FALSE;

    printf("Starting burn operation...\n");
    bool ok = pipeline_burn_image(&config, on_cli_burn_progress, NULL);
    DWORD error = GetLastError(); if (control) SetConsoleCtrlHandler(cli_read_control,FALSE); AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = NULL; CloseHandle(cancel); ReleaseSRWLockExclusive(&cliReadLock);
    printf("\n");

    if (ok) {
        printf(testMode ? "Device test write completed; recorded-data verification was skipped.\n" : "Burn operation completed successfully!\n");
    } else {
        printf("Burn operation failed (error %lu).\n",error);
    }

    spti_close_drive(&drive);
    return ok ? 0 : 1;
}

/* Verify 共用有界重试和取消机制；没有镜像时只检查光盘可读性。 */
static void on_cli_verify_progress(const PipelineStats* stats, void* context) { *(PipelineStats*)context = *stats; on_cli_burn_progress(stats,NULL); }
static int cmd_verify(int argc, char **argv) {
    if (argc < 4) { printf("Usage: imgburn verify <drive> <image|--readable> [--retries <0..20>] [--compare-audio]\n"); return 1; }
    VerifyConfig config = {0};
    for (int i = 4; i < argc; i++) {
        if (!_stricmp(argv[i],"--retries") && i + 1 < argc) { char* end; const char* value = argv[++i]; unsigned long n = strtoul(value,&end,10); if (!value[0] || *end || n > 20 || value[0] == '-') { printf("Error: Retry count must be 0..20.\n"); return 1; } config.retryCount = (unsigned)n; }
        else if (!_stricmp(argv[i],"--compare-audio")) config.compareAudio = true;
        else { printf("Error: Unknown or incomplete verify option: %s\n",argv[i]); return 1; }
    }
    wchar_t device[MAX_PATH], image[MAX_PATH]; bool readable = !_stricmp(argv[3],"--readable");
    if (!parse_drive_path(argv[2],device,MAX_PATH) || (!readable && !utf8_path(argv[3],image,MAX_PATH))) return 1;
    OpticalDrive drive; if (!spti_open_drive(&drive,device,false)) { printf("Error: Cannot open source drive (%lu).\n",drive.lastError); return 1; }
    if (drive.accessKnown && (drive.deviceAccess & (GENERIC_READ | GENERIC_WRITE)) != (GENERIC_READ | GENERIC_WRITE)) { printf("Error: Source device opened without read/write command access. SPTI requires both access rights; try running as administrator.\n"); spti_close_drive(&drive); return 1; }
    HANDLE cancel = CreateEventW(NULL,TRUE,FALSE,NULL); if (!cancel) { spti_close_drive(&drive); return 1; }
    AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = cancel; ReleaseSRWLockExclusive(&cliReadLock);
    bool control = SetConsoleCtrlHandler(cli_read_control,TRUE) != FALSE;
    config.drive = &drive; config.imagePath = readable ? NULL : image; config.cancelEvent = cancel;
    PipelineStats report = {0}; bool ok = pipeline_verify_disc_ex(&config,on_cli_verify_progress,&report); DWORD error = GetLastError();
    if (control) SetConsoleCtrlHandler(cli_read_control,FALSE); AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = NULL; CloseHandle(cancel); ReleaseSRWLockExclusive(&cliReadLock); spti_close_drive(&drive);
    if (ok) printf("\nVerify passed: %u sectors compared, %u sectors checked for readability only.\n",report.comparedSectors,report.readableOnlySectors);
    else printf("\nVerify %s (error %lu, %u mismatched sectors).\n",error == ERROR_CANCELLED ? "cancelled" : error == ERROR_CRC ? "found mismatched sectors" : "failed",error,report.miscomparesCount);
    return ok ? 0 : 1;
}

/* 品质测试直接写入零数据；默认使用全部可写容量，并在写入后回读验证。 */
static int cmd_discovery(int argc, char** argv) {
    if (argc < 3) { printf("Usage: imgburn discovery <drive> [--sectors <n>] [--speed <Nx|MAX>] [--test] [--verify|--no-verify] [--no-cycle] [--opc]\n"); return 1; }
    DiscoveryConfig config = {0}; config.verifyAfterWrite = true; config.cycleTrayBeforeVerify = true; config.writeSpeedKbps = UINT32_MAX; config.ringBufferBytes = 32 * 1024 * 1024; double speed = 0;
    for (int i = 3; i < argc; i++) {
        if (!_stricmp(argv[i],"--test")) config.testMode = true;
        else if (!_stricmp(argv[i],"--verify")) config.verifyAfterWrite = true;
        else if (!_stricmp(argv[i],"--no-verify")) config.verifyAfterWrite = false;
        else if (!_stricmp(argv[i],"--no-cycle")) config.cycleTrayBeforeVerify = false;
        else if (!_stricmp(argv[i],"--opc")) config.performOpc = true;
        else if (!_stricmp(argv[i],"--sectors") && i + 1 < argc) { char* end; const char* value = argv[++i]; uint64_t count = _strtoui64(value,&end,10); if (!value[0] || *end || !count || count > UINT32_MAX || value[0] == '-') { printf("Error: Invalid custom sector count.\n"); return 1; } config.sectors = (uint32_t)count; }
        else if (!_stricmp(argv[i],"--speed") && i + 1 < argc) {
            const char* value = argv[++i]; if (!_stricmp(value,"MAX")) speed = 0;
            else { char* end; speed = strtod(value,&end); if ((*end == 'x' || *end == 'X') && !end[1]) end++; if (!value[0] || *end || !isfinite(speed) || speed < 1 || speed * 4495.5 + 0.5 >= UINT32_MAX) { printf("Error: Invalid write speed.\n"); return 1; } }
        } else { printf("Error: Unknown or incomplete discovery option: %s\n",argv[i]); return 1; }
    }
    wchar_t device[MAX_PATH]; if (!parse_drive_path(argv[2],device,MAX_PATH)) return 1;
    OpticalDrive drive; if (!spti_open_drive(&drive,device,true)) { printf("Error: Cannot open target drive exclusively (%lu).\n",GetLastError()); return 1; }
    if (speed > 0) { MmcProfile profile = mmc_get_current_profile(&drive); if (profile == MMC_PROFILE_UNKNOWN) { printf("Error: Cannot identify media for write speed.\n"); spti_close_drive(&drive); return 1; } config.writeSpeedKbps = (uint32_t)(speed * (profile == MMC_PROFILE_BD_R_SRM ? 4495.5 : profile == MMC_PROFILE_CD_R ? 176.4 : 1385.0) + 0.5); }
    HANDLE cancel = CreateEventW(NULL,TRUE,FALSE,NULL); if (!cancel) { spti_close_drive(&drive); return 1; }
    config.drive = &drive; config.cancelEvent = cancel;
    AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = cancel; ReleaseSRWLockExclusive(&cliReadLock); bool control = SetConsoleCtrlHandler(cli_read_control,TRUE) != FALSE;
    printf("Discovery: %s sectors, TestMode=%s, Verify=%s, Speed=%.2fx (0=MAX)\n",config.sectors ? "custom" : "all free",config.testMode ? "YES" : "NO",config.verifyAfterWrite && !config.testMode ? "YES" : "NO",speed);
    PipelineStats report = {0}; bool ok = pipeline_discover_disc(&config,on_cli_verify_progress,&report); DWORD error = GetLastError();
    if (control) SetConsoleCtrlHandler(cli_read_control,FALSE); AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = NULL; CloseHandle(cancel); ReleaseSRWLockExclusive(&cliReadLock); spti_close_drive(&drive);
    if (ok && config.testMode) printf("\nDiscovery device test write completed; recorded-data verification was skipped.\n");
    else if (ok) printf("\nDiscovery completed: %u sectors written, %u sectors compared.\n",report.totalSectors,report.comparedSectors);
    else printf("\nDiscovery %s (error %lu, %u mismatched sectors).\n",error == ERROR_CANCELLED ? "cancelled" : "failed",error,report.miscomparesCount);
    return ok ? 0 : 1;
}

/* 与扇区查看器窗口使用同一只读接口，输出支持原始 CD 的 96 字节子通道。 */
static int cmd_sector(int argc, char** argv) {
    if (argc < 4 || argc > 5) { printf("Usage: imgburn sector <drive> <lba|--last> [--raw|--raw-sub]\n"); return 1; }
    SectorRequest request = {0}; request.useLast = !_stricmp(argv[3],"--last");
    if (!request.useLast) { char* end; unsigned long long n = _strtoui64(argv[3],&end,10); if (!argv[3][0] || *end || strspn(argv[3],"0123456789") != strlen(argv[3]) || n > UINT32_MAX) { printf("Error: Invalid LBA.\n"); return 1; } request.lba = (uint32_t)n; }
    if (argc == 5) { if (!_stricmp(argv[4],"--raw")) request.mode = SECTOR_RAW; else if (!_stricmp(argv[4],"--raw-sub")) request.mode = SECTOR_RAW_SUBCHANNEL; else { printf("Error: Unknown sector option.\n"); return 1; } }
    wchar_t device[MAX_PATH]; if (!parse_drive_path(argv[2],device,MAX_PATH)) return 1; OpticalDrive drive;
    if (!spti_open_drive(&drive,device,false)) { printf("Error: Cannot open source drive (%lu).\n",drive.lastError); return 1; }
    HANDLE cancel = CreateEventW(NULL,TRUE,FALSE,NULL); if (!cancel) { spti_close_drive(&drive); return 1; } request.cancelEvent = cancel;
    AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = cancel; ReleaseSRWLockExclusive(&cliReadLock); bool control = SetConsoleCtrlHandler(cli_read_control,TRUE) != FALSE;
    SectorData data; bool ok = imgburn_sector_read(&drive,&request,&data); DWORD error = GetLastError();
    if (control) SetConsoleCtrlHandler(cli_read_control,FALSE); AcquireSRWLockExclusive(&cliReadLock); cliReadCancel = NULL; CloseHandle(cancel); ReleaseSRWLockExclusive(&cliReadLock); spti_close_drive(&drive);
    if (ok) { wchar_t* wide = malloc(16384 * sizeof(wchar_t)); char* utf8 = malloc(16384); ok = wide && utf8 && imgburn_sector_format(&data,wide,16384) && WideCharToMultiByte(CP_UTF8,0,wide,-1,utf8,16384,NULL,NULL) != 0; if (ok) printf("LBA %u / %u, %u bytes\n%s",data.lba,data.lastLba,data.bytes,utf8); else printf("Error: Cannot format sector for display.\n"); free(wide); free(utf8); }
    else printf(error == ERROR_ACCESS_DENIED ? "Error: SPTI requires device read/write access; try running as administrator (%lu).\n" : "Error: Sector read failed (%lu).\n",error); return ok ? 0 : 1;
}
/* 查询或更改支持的驱动器 Book Type。 */
static int cmd_booktype(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: imgburn booktype <drive> [query | set <dvd_rom|default> [dvd+r|dvd+r_dl]]\n");
        return 1;
    }

    wchar_t drivePath[MAX_PATH];
    parse_drive_path(argv[2], drivePath, MAX_PATH);

    OpticalDrive drive;
    if (!spti_open_drive(&drive, drivePath, false)) {
        printf("Failed to open drive: %s\n", argv[2]);
        return 1;
    }

    drive_vendor_t v = imgburn_vendor_detect(drive.info.vendorId, drive.info.productId);
    printf("Drive Vendor: %s (%s)\n", drive.info.vendorId, imgburn_vendor_name(v));

    if (argc == 3 || _stricmp(argv[3], "query") == 0) {
        booktype_setting_t rSet = BOOKTYPE_SETTING_UNKNOWN;
        booktype_setting_t dlSet = BOOKTYPE_SETTING_UNKNOWN;
        imgburn_booktype_query(&drive, v, BOOKTYPE_MEDIA_DVD_PLUS_R, &rSet);
        imgburn_booktype_query(&drive, v, BOOKTYPE_MEDIA_DVD_PLUS_R_DL, &dlSet);

        printf("Current BookType Settings:\n");
        printf("  DVD+R:     %s\n", imgburn_booktype_name(rSet));
        printf("  DVD+R DL:  %s\n", imgburn_booktype_name(dlSet));
    } else if (_stricmp(argv[3], "set") == 0 && argc >= 5) {
        booktype_setting_t target = (_stricmp(argv[4], "dvd_rom") == 0) ?
                                    BOOKTYPE_SETTING_DVD_ROM : BOOKTYPE_SETTING_DVD_PLUS_R;
        booktype_media_t media = BOOKTYPE_MEDIA_DVD_PLUS_R;
        if (argc >= 6 && _stricmp(argv[5], "dvd+r_dl") == 0) {
            media = BOOKTYPE_MEDIA_DVD_PLUS_R_DL;
        }

        printf("Setting BookType for %s to %s ...\n",
               (media == BOOKTYPE_MEDIA_DVD_PLUS_R_DL) ? "DVD+R DL" : "DVD+R",
               imgburn_booktype_name(target));

        bool ok = imgburn_booktype_set(&drive, v, media, target, false);
        printf("Result: %s\n", ok ? "SUCCESS" : "COMMAND FAILED (Unsupported by drive firmware)");
    }

    spti_close_drive(&drive);
    return 0;
}

/* -------------------------------------------------------------------------
 * Command: pioneer <drive> <feature> [value]
 * ------------------------------------------------------------------------- */
static int cmd_pioneer(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: imgburn pioneer <drive> <optimal_speed|pureread|quiet> [value]\n");
        return 1;
    }

    wchar_t drivePath[MAX_PATH];
    parse_drive_path(argv[2], drivePath, MAX_PATH);

    OpticalDrive drive;
    if (!spti_open_drive(&drive, drivePath, false)) {
        printf("Failed to open drive: %s\n", argv[2]);
        return 1;
    }

    const char *feature = argv[3];

    if (_stricmp(feature, "optimal_speed") == 0) {
        if (argc >= 5) {
            bool enable = (_stricmp(argv[4], "on") == 0 || strcmp(argv[4], "1") == 0);
            bool ok = imgburn_pioneer_set_optimal_speed(&drive, enable, false);
            printf("Set Optimal Write Speed to %s: %s\n", enable ? "ON" : "OFF", ok ? "SUCCESS" : "FAILED");
        } else {
            bool en = false, saved = false;
            if (imgburn_pioneer_get_optimal_speed(&drive, &en, &saved)) {
                printf("Optimal Write Speed: %s (Saved to drive: %s)\n", en ? "ON" : "OFF", saved ? "YES" : "NO");
            } else {
                printf("Failed to query Optimal Write Speed (Not a Pioneer drive?)\n");
            }
        }
    } else if (_stricmp(feature, "pureread") == 0) {
        if (argc >= 5) {
            pioneer_pureread_mode_t mode = PIONEER_PUREREAD_STANDARD;
            if (_stricmp(argv[4], "master") == 0) mode = PIONEER_PUREREAD_MASTER;
            else if (_stricmp(argv[4], "perfect") == 0) mode = PIONEER_PUREREAD_PERFECT;

            bool ok = imgburn_pioneer_set_pureread(&drive, mode, false);
            printf("Set PureRead Mode: %s\n", ok ? "SUCCESS" : "FAILED");
        } else {
            pioneer_pureread_mode_t mode;
            bool supp = false;
            if (imgburn_pioneer_get_pureread(&drive, &mode, &supp) && supp) {
                const char *s = (mode == PIONEER_PUREREAD_PERFECT) ? "Perfect" :
                                (mode == PIONEER_PUREREAD_MASTER)  ? "Master" : "Standard";
                printf("PureRead: Supported, Mode = %s (%d)\n", s, mode);
            } else {
                printf("PureRead not supported by this drive.\n");
            }
        }
    } else if (_stricmp(feature, "quiet") == 0) {
        if (argc >= 5) {
            pioneer_quiet_mode_t qmode = PIONEER_QUIET_PERFORMANCE;
            if (_stricmp(argv[4], "quiet") == 0) qmode = PIONEER_QUIET_QUIET;
            else if (_stricmp(argv[4], "smooth") == 0) qmode = PIONEER_QUIET_SMOOTH;

            bool ok = imgburn_pioneer_set_quiet_mode(&drive, qmode);
            printf("Set Quiet Mode: %s\n", ok ? "SUCCESS" : "FAILED");
        } else {
            pioneer_quiet_mode_t qmode;
            if (imgburn_pioneer_get_quiet_mode(&drive, &qmode)) {
                const char *s = (qmode == PIONEER_QUIET_QUIET)  ? "Quiet" :
                                (qmode == PIONEER_QUIET_SMOOTH) ? "Smooth (Auto)" : "Performance";
                printf("Quiet Mode: %s (%d)\n", s, qmode);
            }
        }
    }

    spti_close_drive(&drive);
    return 0;
}

/* -------------------------------------------------------------------------
 * Command: changer <drive> <status|load <slot>|unload>
 * ------------------------------------------------------------------------- */
static int cmd_changer(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: imgburn changer <drive> <status | load <slot> | unload>\n");
        return 1;
    }

    wchar_t drivePath[MAX_PATH];
    parse_drive_path(argv[2], drivePath, MAX_PATH);

    OpticalDrive drive;
    if (!spti_open_drive(&drive, drivePath, false)) {
        printf("Failed to open drive: %s\n", argv[2]);
        return 1;
    }

    const char *sub = argv[3];
    if (_stricmp(sub, "status") == 0) {
        EmbeddedChangerStatus cs;
        if (imgburn_changer_get_status(&drive, &cs)) {
            printf("Changer Status:\n");
            printf("  Total Slots:    %u\n", cs.totalSlots);
            printf("  Current Slot:   %u\n", cs.currentSlot);
            printf("  Door Open:      %s\n", cs.isDoorOpen ? "YES" : "NO");
            printf("  Busy:           %s\n", cs.isMechanismBusy ? "YES" : "NO");
            printf("  Slot Matrix:\n");
            for (uint8_t i = 0; i < cs.totalSlots; i++) {
                printf("    Slot %02u: %s\n", i, cs.slotOccupied[i] ? "[DISC PRESENT]" : "[EMPTY]");
            }
        } else {
            printf("Drive is not a recognized multi-disc changer.\n");
        }
    } else if (_stricmp(sub, "load") == 0 && argc >= 5) {
        uint8_t slot = (uint8_t)atoi(argv[4]);
        printf("Loading slot %u ...\n", slot);
        bool ok = imgburn_changer_load_slot(&drive, slot);
        printf("Result: %s\n", ok ? "SUCCESS" : "FAILED");
    } else if (_stricmp(sub, "unload") == 0) {
        printf("Unloading current slot...\n");
        bool ok = imgburn_changer_unload(&drive);
        printf("Result: %s\n", ok ? "SUCCESS" : "FAILED");
    }

    spti_close_drive(&drive);
    return 0;
}

/* -------------------------------------------------------------------------
 * Command: cue <file.cue>
 * ------------------------------------------------------------------------- */
static int cmd_cue(const char* cueFile) {
    wchar_t path[MAX_PATH];
    if (!utf8_path(cueFile, path, MAX_PATH)) return 1;
    CueSheet* cue = (CueSheet*)calloc(1, sizeof(*cue));
    if (!cue) return 1;
    if (!imgburn_cue_parse(path, cue)) { printf("Error: Failed to parse CUE sheet '%s'\n", cueFile); free(cue); return 1; }
    printf("CUE Sheet Metadata:\n  Disc Title: '%s'\n  Disc Performer: '%s'\n  Media Catalog Number: '%s'\n  Sessions: %u\n  Total Tracks: %d\n  Total Sectors: %u\n", cue->discTitle, cue->discPerformer, cue->catalogNumber, cue->sessionCount, cue->trackCount, cue->totalSectors);
    printf("  Composer: '%s' | Arranger: '%s' | Message: '%s'\n  Disc ID: '%s' | CD-TEXT UPC: '%s'\n",cue->composer,cue->arranger,cue->message,cue->discId,cue->cdTextUpc);
    static const char* modes[] = {"AUDIO","MODE1/2048","MODE1/2352","MODE2/2336","MODE2/2352","MODE2/2048","MODE2/2324","CDI/2336","CDI/2352","CDG"};
    for (int i = 0; i < cue->trackCount; i++) {
        const CueTrack* t = &cue->tracks[i];
        printf("  Session %u Track %02d: %s | LBA %u | Sectors %u | Source Offset %llu | Flags %u | Indices %d\n", t->sessionNumber, t->trackNumber, modes[t->mode], t->startLba, t->sectorCount, t->sourceByteOffset, t->flags, t->indexCount);
    }
    free(cue); return 0;
}

/* CDT使用不带READ TOC响应头的18字节pack流；锁定输入，禁止发布时覆盖CUE、音源或已有CDTEXTFILE。 */
static bool cli_cdtext_load(HANDLE file, CdText* text) {
    if (GetFileType(file) != FILE_TYPE_DISK) { SetLastError(ERROR_INVALID_DATA); return false; }
    LARGE_INTEGER length; if (!GetFileSizeEx(file,&length)) return false;
    if (length.QuadPart < IMGBURN_CDTEXT_PACK_BYTES || length.QuadPart > IMGBURN_CDTEXT_MAX_BYTES || length.QuadPart % IMGBURN_CDTEXT_PACK_BYTES) { SetLastError(ERROR_INVALID_DATA); return false; }
    DWORD bytes = (DWORD)length.QuadPart, got = 0; uint8_t* data = (uint8_t*)malloc(bytes); if (!data) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    bool ok = ReadFile(file,data,bytes,&got,NULL) != FALSE; DWORD error = GetLastError();
    if (ok && got != bytes) { ok = false; error = ERROR_HANDLE_EOF; }
    if (ok) { ok = imgburn_cdtext_parse(data,bytes,text); error = GetLastError(); }
    free(data); SetLastError(error); return ok;
}

static bool cli_cdtext_same_file(const wchar_t* source, HANDLE locked, const wchar_t* output) {
    wchar_t a[MAX_PATH], b[MAX_PATH]; DWORD na = GetFullPathNameW(source,MAX_PATH,a,NULL), nb = GetFullPathNameW(output,MAX_PATH,b,NULL);
    if (!na || na >= MAX_PATH || !nb || nb >= MAX_PATH) return true;
    if (!_wcsicmp(a,b)) return true;
    HANDLE other = CreateFileW(output,FILE_READ_ATTRIBUTES,FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if (other == INVALID_HANDLE_VALUE) return GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND;
    BY_HANDLE_FILE_INFORMATION first = {0}, second = {0}; bool known = GetFileInformationByHandle(locked,&first) && GetFileInformationByHandle(other,&second); CloseHandle(other);
    return !known || (first.dwVolumeSerialNumber == second.dwVolumeSerialNumber && first.nFileIndexHigh == second.nFileIndexHigh && first.nFileIndexLow == second.nFileIndexLow);
}

static bool cli_cdtext_publish(const wchar_t* output, const CdText* text, bool replace) {
    static LONG serial; wchar_t temporary[MAX_PATH]; HANDLE file = INVALID_HANDLE_VALUE;
    if (wcslen(output) + 48 >= MAX_PATH) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        if (swprintf(temporary,MAX_PATH,L"%ls.partial.%lu.%ld",output,GetCurrentProcessId(),InterlockedIncrement(&serial)) < 0) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return false; }
        file = CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; bool ok = WriteFile(file,text->packs,(DWORD)text->bytes,&written,NULL) != FALSE; DWORD error = GetLastError();
    if (ok && written != text->bytes) { ok = false; error = ERROR_WRITE_FAULT; }
    if (ok && !FlushFileBuffers(file)) { ok = false; error = GetLastError(); }
    if (!CloseHandle(file) && ok) { ok = false; error = GetLastError(); }
    if (ok) { ok = MoveFileExW(temporary,output,MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)) != FALSE; error = GetLastError(); }
    if (!ok) DeleteFileW(temporary); SetLastError(error); return ok;
}

static int cmd_cdtext_info(int argc, char** argv) {
    if (argc != 3) { printf("Usage: imgburn cdtext-info <file.cdt>\n"); return 1; }
    wchar_t path[MAX_PATH]; if (!utf8_path(argv[2],path,MAX_PATH)) return 1;
    HANDLE file = CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if (file == INVALID_HANDLE_VALUE) { printf("Error: Cannot open CDT (%lu).\n",GetLastError()); return 1; }
    CdText text = {0}; bool ok = cli_cdtext_load(file,&text); DWORD error = GetLastError(); CloseHandle(file);
    if (!ok) { printf("Error: Invalid or unsupported CDT pack stream (%lu).\n",error); return 1; }
    printf("CD-TEXT: %zu packs, %u blocks\n",text.bytes / IMGBURN_CDTEXT_PACK_BYTES,text.blockCount);
    CueSheet* sheet = (CueSheet*)calloc(1,sizeof(*sheet)); if (!sheet) { imgburn_cdtext_free(&text); return 1; }
    for (unsigned block = 0; block < 8; block++) {
        const CdTextBlock* info = &text.blocks[block]; if (!info->packCount) continue;
        printf("  Block %u: language %u, character code %u, tracks %u..%u, %u packs, %u omitted CRCs%s%s\n",block,info->language,info->characterCode,info->firstTrack,info->lastTrack,info->packCount,info->omittedCrcCount,info->doubleByte ? ", double byte" : "",info->hasUnmapped ? ", extra pack types preserved" : "");
        memset(sheet,0,sizeof(*sheet)); sheet->trackCount = info->lastTrack - info->firstTrack + 1;
        for (int i = 0; i < sheet->trackCount; i++) sheet->tracks[i].trackNumber = (uint8_t)(info->firstTrack + i);
        if (!imgburn_cdtext_extract(&text,block,sheet)) { printf("    Text conversion unavailable; raw packs retained.\n"); continue; }
        printf("    TITLE '%s' | PERFORMER '%s' | SONGWRITER '%s' | UPC '%s'\n",sheet->discTitle,sheet->discPerformer,sheet->songwriter,sheet->cdTextUpc);
        printf("    COMPOSER '%s' | ARRANGER '%s' | MESSAGE '%s' | DISCID '%s'\n",sheet->composer,sheet->arranger,sheet->message,sheet->discId);
        for (int i = 0; i < sheet->trackCount; i++) {
            printf("    Track %02u: TITLE '%s' | PERFORMER '%s' | SONGWRITER '%s' | ISRC '%s'\n",sheet->tracks[i].trackNumber,sheet->tracks[i].title,sheet->tracks[i].performer,sheet->tracks[i].songwriter,sheet->tracks[i].isrc);
            printf("      COMPOSER '%s' | ARRANGER '%s' | MESSAGE '%s'\n",sheet->tracks[i].composer,sheet->tracks[i].arranger,sheet->tracks[i].message);
        }
    }
    free(sheet); imgburn_cdtext_free(&text); return 0;
}

static int cmd_cdtext_build(int argc, char** argv) {
    if (argc < 4 || argc > 5 || (argc == 5 && _stricmp(argv[4],"--replace"))) { printf("Usage: imgburn cdtext-build <file.cue> <out.cdt> [--replace]\n"); return 1; }
    wchar_t source[MAX_PATH], output[MAX_PATH]; if (!utf8_path(argv[2],source,MAX_PATH) || !utf8_path(argv[3],output,MAX_PATH)) return 1;
    const wchar_t* extension = wcsrchr(output,L'.'); if (!extension || _wcsicmp(extension,L".cdt")) { printf("Error: CD-TEXT output must use .cdt.\n"); return 1; }
    HANDLE locks[101]; unsigned count = 0; CdText text = {0}; CueSheet* sheet = (CueSheet*)calloc(1,sizeof(*sheet)); bool ok = false; DWORD error = ERROR_NOT_ENOUGH_MEMORY;
    if (!sheet) goto done;
    locks[count] = CreateFileW(source,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
    if (locks[count] == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; } count++;
    if (!imgburn_cue_parse(source,sheet)) { error = GetLastError(); goto done; }
    if (cli_cdtext_same_file(source,locks[0],output)) { error = ERROR_INVALID_PARAMETER; goto done; }
    for (int i = 0; i < sheet->trackCount; i++) {
        locks[count] = CreateFileW(sheet->tracks[i].sourceFilePath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if (locks[count] == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; } count++;
        if (cli_cdtext_same_file(sheet->tracks[i].sourceFilePath,locks[count - 1],output)) { error = ERROR_INVALID_PARAMETER; goto done; }
    }
    if (sheet->cdTextFile[0]) {
        locks[count] = CreateFileW(sheet->cdTextFile,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if (locks[count] != INVALID_HANDLE_VALUE) {
            count++; if (cli_cdtext_same_file(sheet->cdTextFile,locks[count - 1],output)) { error = ERROR_INVALID_PARAMETER; goto done; }
            if (!cli_cdtext_load(locks[count - 1],&text)) { error = GetLastError(); goto done; }
        } else { error = GetLastError(); if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) goto done; sheet->cdTextFile[0] = 0; }
    }
    if (!text.packs && !imgburn_cdtext_generate(sheet,&text)) { error = GetLastError(); goto done; }
    ok = cli_cdtext_publish(output,&text,argc == 5); error = GetLastError();
done:
    for (unsigned i = 0; i < count; i++) CloseHandle(locks[i]);
    imgburn_cdtext_free(&text); free(sheet);
    if (ok) printf("CD-TEXT exported successfully.\n"); else printf("Error: CD-TEXT export failed (%lu).\n",error); SetLastError(error); return ok ? 0 : 1;
}

static int cmd_cd_toc(int argc, char** argv) {
    if (argc != 3) { printf("Usage: imgburn cd-toc <drive>\n"); return 1; }
    wchar_t path[MAX_PATH]; if (!parse_drive_path(argv[2],path,MAX_PATH)) return 1;
    OpticalDrive drive; if (!spti_open_drive(&drive,path,false)) { printf("Error: Cannot open source drive (%lu).\n",GetLastError()); return 1; }
    MmcCdFullToc* toc = calloc(1,sizeof(*toc)); bool ok = false;
    if (!toc) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto done; }
    if (!mmc_test_unit_ready(&drive)) goto done;
    MmcProfile profile = mmc_get_current_profile(&drive);
    if (profile != MMC_PROFILE_CD_ROM && profile != MMC_PROFILE_CD_R && profile != MMC_PROFILE_CD_RW) { SetLastError(ERROR_NOT_SUPPORTED); goto done; }
    if (!mmc_read_cd_full_toc(&drive,toc)) goto done;
    printf("CD Full TOC: %u complete sessions, %u tracks; final lead-out LBA %u\n",toc->sessionCount,toc->trackCount,toc->leadOutLba);
    for (unsigned i = 0; i < toc->sessionCount; i++) {
        const MmcCdSession* session = &toc->sessions[i]; printf("  Session %u: tracks %u-%u, type 0x%02X, lead-out LBA %u\n",session->number,session->firstTrack,session->lastTrack,session->discType,session->leadOutLba);
        for (unsigned j = 0; j < session->trackCount; j++) { const MmcCdTrack* track = &toc->tracks[session->firstTrackIndex + j]; printf("    Track %u: INDEX01 LBA %u, control 0x%X\n",track->number,track->startLba,track->control); }
    }
    ok = true;
done:
    if (!ok) printf("Error: Cannot read complete CD session layout (%lu).\n",GetLastError()); free(toc); spti_close_drive(&drive); return ok ? 0 : 1;
}
static int cmd_mds(const char* mdsFile) {
    wchar_t path[MAX_PATH];
    if (!utf8_path(mdsFile, path, MAX_PATH)) return 1;
    MdsSheet* mds = (MdsSheet*)calloc(1, sizeof(*mds));
    if (!mds) return 1;
    if (!imgburn_mds_parse(path, mds)) { printf("Error: Failed to parse MDS file '%s'\n", mdsFile); free(mds); return 1; }
    const char* medium = mds->mediumType >= 0x10 ? (mds->layerBreakLba ? "DVD-DL" : "DVD") : mds->mediumType == 2 ? "CD-RW" : mds->mediumType == 1 ? "CD-R" : "CD-ROM";
    printf("MDS Sheet Metadata:\n  Medium Type: %s (%u)\n  Total Sectors: %u\n  Sessions: %u\n  Tracks: %d\n  Data File References: %u\n  Layer Break: %u\n", medium, mds->mediumType, mds->totalSectors, mds->sessionCount, mds->trackCount, mds->dataFileCount, mds->layerBreakLba);
    if (mds->layerCount) printf("  Layers: %u | Track Path: %s\n", mds->layerCount, mds->oppositeTrackPath ? "OTP" : "PTP");
    for (int i = 0; i < mds->trackCount; i++) {
        const MdsTrack* t = &mds->tracks[i];
        printf("  Session %u Track %u: LBA %d | Sectors %u | Sector Size %u | Pregap %u | Files %u\n", t->sessionNumber, t->trackNumber, t->startLba, t->sectorCount, t->sectorSize, t->pregapSectors, t->fileCount);
    }
    free(mds); return 0;
}

/* -------------------------------------------------------------------------
 * Main CLI Dispatcher
 * ------------------------------------------------------------------------- */
static int run_cli(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    print_banner();

    if (argc < 2) {
        print_usage();
        return 0;
    }

    const char *cmd = argv[1];

    if (_stricmp(cmd, "devices") == 0 || _stricmp(cmd, "list") == 0) {
        return cmd_devices();
    } else if (_stricmp(cmd, "info") == 0 && argc >= 3) {
        return cmd_info(argv[2]);
    } else if (_stricmp(cmd, "build") == 0) {
        return cmd_build(argc, argv);
    } else if (_stricmp(cmd, "burn") == 0) {
        return cmd_burn(argc, argv);
    } else if (_stricmp(cmd, "erase") == 0) {
        return cmd_erase(argc,argv);
    } else if (_stricmp(cmd, "discovery") == 0) {
        return cmd_discovery(argc,argv);
    } else if (_stricmp(cmd, "read") == 0) {
        return cmd_read(argc,argv);
    } else if (_stricmp(cmd, "verify") == 0) {
        return cmd_verify(argc, argv);
    } else if (_stricmp(cmd, "sector") == 0) {
        return cmd_sector(argc,argv);
    } else if (_stricmp(cmd, "cd-toc") == 0) {
        return cmd_cd_toc(argc,argv);
    } else if (_stricmp(cmd, "booktype") == 0) {
        return cmd_booktype(argc, argv);
    } else if (_stricmp(cmd, "pioneer") == 0) {
        return cmd_pioneer(argc, argv);
    } else if (_stricmp(cmd, "changer") == 0) {
        return cmd_changer(argc, argv);
    } else if (_stricmp(cmd, "cue") == 0 && argc >= 3) {
        return cmd_cue(argv[2]);
    } else if (_stricmp(cmd, "cdtext-info") == 0) {
        return cmd_cdtext_info(argc,argv);
    } else if (_stricmp(cmd, "cdtext-build") == 0) {
        return cmd_cdtext_build(argc,argv);
    } else if (_stricmp(cmd, "mds") == 0 && argc >= 3) {
        return cmd_mds(argv[2]);
    } else if (_stricmp(cmd, "dvd-layers") == 0) {
        return cmd_dvd_layers(argc, argv);
    } else if (_stricmp(cmd, "dvd-preview") == 0) {
        return cmd_dvd_preview(argc, argv);
    } else if (_stricmp(cmd, "help") == 0 || _stricmp(cmd, "--help") == 0 || _stricmp(cmd, "-h") == 0) {
        print_usage();
        return 0;
    } else {
        printf("Unknown command: '%s'\n\n", cmd);
        print_usage();
        return 1;
    }
}

/* 从Windows宽字符入口读取参数，统一转换成内部UTF-8，避免系统代码页丢失中文路径。 */
int wmain(int argc, wchar_t** wideArgs) {
    if (argc == 1 || (argc == 2 && (!_wcsicmp(wideArgs[1], L"gui") || !_wcsicmp(wideArgs[1], L"--gui")))) return imgburn_gui_show(GetModuleHandleW(NULL), SW_SHOWNORMAL);
    HANDLE standardOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((!standardOutput || standardOutput == INVALID_HANDLE_VALUE || GetFileType(standardOutput) == FILE_TYPE_UNKNOWN) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = NULL; _wfreopen_s(&stream,L"CONOUT$",L"w",stdout); _wfreopen_s(&stream,L"CONOUT$",L"w",stderr);
    }
    char** args = (char**)calloc((size_t)argc + 1, sizeof(char*));
    if (!args) return 1;
    int result = 1;
    for (int i = 0; i < argc; i++) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArgs[i], -1, NULL, 0, NULL, NULL);
        if (bytes <= 0 || !(args[i] = (char*)malloc((size_t)bytes))) goto cleanup;
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArgs[i], -1, args[i], bytes, NULL, NULL)) goto cleanup;
    }
    result = run_cli(argc, args);
cleanup:
    for (int i = 0; i < argc; i++) free(args[i]);
    free(args);
    return result;
}
