/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define _CRT_SECURE_NO_WARNINGS
#define CORE_INTERNAL
#include "FbneoRuntime.hpp"
#include "Error.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// FBNeo public headers use a TCHAR type. RMG-Core is non-UNICODE, so
// force TCHAR == char regardless of any platform headers pulled in.
#ifndef _TCHAR_DEFINED
typedef char TCHAR;
#define _TCHAR_DEFINED
#endif
#ifndef _T
#define _T(x) x
#endif

extern "C" {
#include "burn.h"
}

// BurnStateCompress/BurnStateDecompress live in statec.cpp and are declared in
// no public header. They are NOT wrapped in extern "C" there, so they get C++
// linkage (verified in libfbneo_core.a: _Z17BurnStateCompressPPhPii and
// _Z19BurnStateDecompressPhii). Declaring them as extern "C" here would ask the
// linker for unmangled names and fail, so they must keep C++ linkage.
extern INT32 BurnStateCompress(UINT8** pDef, INT32* pnDefLen, INT32 bAll);
extern INT32 BurnStateDecompress(UINT8* Def, INT32 nDefLen, INT32 bAll);

// Diagnostics sink (rmgk_gekko.cpp): one line per audible frame so a captured
// session can be compared frame-by-frame against the peer's log (an RMS
// mismatch means the audio stream diverged even if the game state did not).
extern void write_gekko_log(const std::string& message);

static int rmgkSndFrameIndex = 0;

static void rmgkSndLog(const std::string& message)
{
    write_gekko_log("rmgk " + message);
}

// Global variable at global scope: the Itanium ABI does not mangle these, so the
// symbol is literally "nRMGKNetgameMode" on both sides (no extern "C" needed).
// Defined in FBNeo's src/burner/rmgk/frontend.cpp; backing storage for
// is_netgame_or_recording(). Must be set BEFORE fbneo_rt::Initialize().
extern int nRMGKNetgameMode;

// Full-loop-iteration phase mark (defined in rmgk_gekko.cpp). Indices mirror
// RmgkLoopMark: 3 = RunFrame entry, 4 = RunFrame exit, 5 = present entry,
// 6 = present exit. No-op unless the pacing trace is enabled.
extern "C" void rmgk_gekko_trace_loop_mark(int which);

// The zip helpers live in src/burner/zipfn.cpp, which the lib frontend links in.
// Same story as above: C++ linkage, so no extern "C". ZipGetList()/ZipLoadFile()
// let BurnExtLoadRom resolve a ROM by CRC and copy it *clamped* to the driver's
// declared size (ZipLoadOneFile would read the whole entry and overflow the
// caller's buffer whenever the archive held a larger file). The declarations
// must match src/burner/burner.h verbatim or the C++ name mangling won't resolve.
struct ZipEntry { char* szName; UINT32 nLen; UINT32 nCrc; };
extern INT32 ZipOpen(char* szZip);
extern INT32 ZipClose();
extern INT32 ZipGetList(struct ZipEntry** pList, INT32* pnListCount);
extern INT32 ZipLoadFile(UINT8* Dest, INT32 nLen, INT32* pnWrote, INT32 nEntry);

// RMG-K driver-table accessors (src/burn/burn.cpp, rmgk patch). The public
// BurnDrv* API reads the global nBurnDrvActive, so using it to scan the table
// (for the name -> full name cache, or to map a ROM's CRC32 to its driver)
// would make a game running on the emulation thread execute another driver's
// frame. These read the table in place. C linkage, so no name mangling issue.
extern "C" INT32       RmgkDriverCount();
extern "C" const char* RmgkDriverShortName(INT32 i);
extern "C" const char* RmgkDriverFullName(INT32 i);
extern "C" const char* RmgkDriverParentName(INT32 i);
extern "C" INT32       RmgkDriverFlags(INT32 i);
extern "C" INT32       RmgkDriverRomInfo(INT32 i, UINT32 n, UINT32* pCrc, UINT32* pLen, UINT32* pType);

namespace fbneo_rt
{
namespace
{
using clock_ = std::chrono::steady_clock;

bool         g_libInited     = false;
bool         g_driverInited  = false;
int          g_players       = 2;
std::vector<uint32_t> g_localMasks;
// The frontend writes local input from the GUI thread (key/button events) while
// the emulation thread reads it every frame in SampleInput, so the mask array
// is guarded by this mutex.
std::mutex   g_localInputMutex;

int (*g_inputCallback)(void*, int, int) = nullptr;
int  g_inputPlayers  = 0;
int  g_inputSize     = 4;
std::vector<uint8_t> g_latchedInput;

FrameCallback g_presentCallback = nullptr;
void*         g_presentUser     = nullptr;
int           g_orientationFlags = 0;

// Archives that make up the romset. g_archives holds the *open name* (extension
// stripped, because ZipOpen() re-appends it); g_archiveFiles keeps the original
// path for diagnostics.
std::vector<std::string> g_archives;
std::vector<std::string> g_archiveFiles;
std::string              g_driverName;

// FBNeo's zip layer (src/burner/zipfn.cpp) keeps one global open archive, and
// g_archives is walked by fbneo_load_rom() on the emulation thread while the ROM
// browser may resolve another set on the search thread. Serialize the open /
// list / read / close sequence.
std::mutex g_zipMutex;

// Console systems (NES/SNES/Mega Drive/...) are usually shipped as one loose ROM
// file per title instead of a romset archive. Those paths must not go through
// ZipOpen() (it would append ".zip"), so they are kept here together with the
// CRC32/size computed once at Initialize(): fbneo_load_rom() then satisfies a
// driver ROM entry by content, exactly like a zip entry. `smd` marks a Mega Drive
// copier image: crc/len describe the decoded body, not the file on disk.
struct RawRomFile { std::string path; uint32_t crc; uint32_t len; bool smd; };
std::vector<RawRomFile> g_rawFiles;

// Fallback table for ROM entries the game file itself does not carry. FBNeo
// console drivers list their BIOS as an ordinary ROM entry of the same driver
// (cv_coleco needs "coleco.rom", FDS needs "fdsbios.nes", ...), and the user
// keeps those beside the games under FBNeo's canonical file name. Every regular
// file of the ROM's own folder (and of the roms root above it) is remembered by
// lower-case name so such a companion ROM can still be found.
std::unordered_map<std::string, std::string> g_dirFiles;

// One runtime input slot. For a plain digital input `targets` holds a single
// driver input index; for a FBNeo-style combination macro (e.g. "P1 Buttons AB")
// it holds every index the macro drives at once.
struct PlayerInputMap { std::vector<std::vector<UINT32> > targets; std::vector<std::string> names; };
std::vector<PlayerInputMap> g_playerInputMap;
// g_playerInputMap is built when a session starts (emulation thread) but the
// frontend reads the input names from the GUI thread while building key maps,
// so the table is guarded by this mutex.
std::mutex g_playerInputMapMutex;

// Driver input indices behind the cabinet test switch. Built together with the
// player input map: the "diag"/"diagnostic"/"test" input opens the game's
// configuration menu on boards that have one, while drivers without a separate
// test switch use the "service" input instead (the same rule the FBNeo headless
// frontend applies). The hotkey presses it on every frame while held.
std::vector<UINT32> g_testInputTargets;
std::atomic<bool>   g_testPressed(false);
// Remaining emulated frames of a frontend-triggered test-switch tap. The tap is
// counted in frames instead of wall-clock time on purpose: when the click lands
// while the emulation is paused (pause-on-focus-loss) or while the emulation
// thread is a frame behind, a timer-based release expires before the driver
// ever samples the switch, and the game ignores the press entirely.
std::atomic<int>    g_testPulseFrames(0);
// True when the current tap may be delivered even though the rollback layer owns
// the input stream (local sessions: one actor, no remote peer).
std::atomic<bool>   g_testAllowLocalRollback(false);

// Driver short-name -> full display name. The rom browser queries this for every
// archive in a directory, and walking the driver table mutates the global
// nBurnDrvActive. So the table is built once into a cache; later lookups (which
// may run on the search thread while a game is running) are pure reads.
// Recursive because content-based driver resolution (ResolveDriver) holds it for
// the whole lookup and internally reuses driver_lookup().
std::recursive_mutex                         g_driverNameMutex;
std::unordered_map<std::string, std::string> g_driverNameCache;
// Driver short-name -> parent short name (empty when the driver has no parent).
// Filled by the same driver-table walk as g_driverNameCache.
std::unordered_map<std::string, std::string> g_driverParentCache;

// FBNeo's BurnLibInit() calls BurnLibExit() internally, and BurnLibExit() (via
// BurnGameListExit) frees the driver-name tables without nulling the pointers.
// Invoking BurnLibInit() again after a BurnLibExit() therefore double-frees and
// corrupts the heap (STATUS_HEAP_CORRUPTION) as soon as the second game starts.
// Keep the coarse library tables for the whole process and release them once at
// exit, so BurnLibInit() only ever runs against a clean state.
void fbneo_lib_atexit()
{
    if (g_libInited)
    {
        BurnLibExit();
        g_libInited = false;
    }
}

bool ensure_lib_init()
{
    if (g_libInited)
    {
        return true;
    }
    if (BurnLibInit() != 0)
    {
        return false;
    }
    g_libInited = true;
    std::atexit(fbneo_lib_atexit);
    return true;
}

std::vector<uint8_t>   g_videoBuffer;
VideoFrame             g_videoFrame;

// Drivers flagged BDF_16BIT_ONLY (Mega Drive/32X/Sega CD, CPS3, Cave, GBA...)
// render RGB565 straight into pBurnDraw; everyone else renders XRGB8888. The
// render buffer keeps the driver's native depth and the presenter always gets
// 32 bpp, so g_displayBuffer holds the converted frame when the native depth is
// 16 (see convert_native_to_display). Feeding RGB565 to the 32 bpp presenter
// packs two source pixels into one word: the picture comes out split in two
// (one half per scanline) with the channels scrambled.
std::vector<uint8_t>   g_displayBuffer;
int                    g_nativeBpp = 4;
int                    g_fullWidth = 0;
int                    g_fullHeight = 0;

std::vector<int16_t>   g_soundBuffer;
int                    g_soundFrames = 0;

m64p_rollback_run_frame_stats g_stats = {};

// Serializes access to the live driver state so GUI-thread commands (reset,
// save/load state) never run in the middle of a BurnDrvFrame() stepped on the
// emulation thread. Frames hold it for their whole body; state commands hold it
// while they read/write the driver, so they execute cleanly between frames.
std::mutex g_frameMutex;

// A reset requested from another thread (the GUI / System menu). It is applied
// on the emulation thread at the top of the next RunFrame, while the frame lock
// is held, so the driver is never torn down underneath a running frame or the
// presenter (which reads pBurnDraw without the lock).
std::atomic_bool g_pendingReset{false};

// Pause flag for arcade sessions. When set, RunFrame keeps the presenter fed
// with the last rendered frame (and silence) but skips stepping the driver, so
// the window never goes black and the rollback/pacing loop stays alive. Written
// from the GUI thread (System > Pause), read on the emulation thread.
std::atomic_bool g_paused{false};

// DIP switch byte overrides the frontend asked to keep (BurnInputInfo index ->
// value), applied on top of the driver defaults so they survive a driver reset.
// Guarded by g_dipMutex (never hold g_frameMutex while acquiring it in reverse;
// callers that touch driver bytes take g_frameMutex first, then g_dipMutex).
std::vector<std::pair<int, uint8_t> > g_dipOverrides;
std::mutex g_dipMutex;

int lower_c(int c) { return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c; }

std::string basename_lower(const std::string& p)
{
    std::string out;
    for (char c : p) out.push_back((char)lower_c((unsigned char)c));
    std::string::size_type slash = out.find_last_of("/\\");
    return slash == std::string::npos ? out : out.substr(slash + 1);
}

uint32_t fnv1a(const uint8_t* data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 16777619u; }
    return h;
}

// Standard CRC-32 (IEEE 802.3), the same value FBNeo stores in BurnRomInfo::nCrc
// and in the zip central directory. Used to identify a loose ROM file by its
// content instead of by the file name: a No-Intro set names the file after the
// game ("Sonic the Hedgehog (USA).md"), while the driver only knows the CRC.
struct Crc32Table
{
    uint32_t t[256];
    Crc32Table()
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len)
{
    static const Crc32Table tbl;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = tbl.t[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// CRC32 of a whole file. Returns false when the file cannot be read.
bool crc32_file(const std::string& path, uint32_t& outCrc, uint32_t& outLen)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    uint32_t crc = 0;
    uint64_t total = 0;
    uint8_t buf[64 * 1024];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        crc = crc32_update(crc, buf, got);
        total += got;
    }
    const bool ok = (ferror(f) == 0);
    fclose(f);
    if (!ok || total > 0xFFFFFFFFull) return false;
    outCrc = crc;
    outLen = (uint32_t)total;
    return true;
}

// Low level helper for the .smd path: read a whole file into memory.
bool read_file_bytes(const std::string& path, size_t maxLen, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    bool ok = false;
    if (fseek(f, 0, SEEK_END) == 0)
    {
        const long size = ftell(f);
        if (size > 0 && (size_t)size <= maxLen && fseek(f, 0, SEEK_SET) == 0)
        {
            out.resize((size_t)size);
            ok = fread(out.data(), 1, (size_t)size, f) == (size_t)size;
        }
    }
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

bool has_ext(const std::string& lowerName, const char* ext)
{
    const size_t n = strlen(ext);
    return lowerName.size() >= n && lowerName.compare(lowerName.size() - n, n, ext) == 0;
}

// Mega Drive images dumped by the Super Magic Drive (.smd) are byte-interleaved
// 16 KiB blocks, usually behind a 512-byte copier header. FBNeo's Mega Drive
// driver takes raw .bin layout, so such a dump never matches the driver's CRC
// and, if loaded as-is, boots to garbage. Decode in place (same algorithm as
// Genesis Plus GX: out[2i] = block[0x2000 + i], out[2i + 1] = block[i]), then
// drop the copier header, which turns the body into the canonical .bin bytes.
void smd_deinterleave(std::vector<uint8_t>& data)
{
    if (data.size() < 0x4000) return;

    // A copier header is only present when the file is not a plain ROM:
    // Genesis Plus GX treats "size/512 odd" + no SEGA header as the marker.
    size_t off = 0;
    if ((data.size() % 512) == 0 && ((data.size() / 512) & 1) &&
        !(data.size() >= 0x104 && memcmp(data.data() + 0x100, "SEGA", 4) == 0))
    {
        off = 512;
    }

    const size_t body  = data.size() - off;
    const size_t block = 0x4000;
    const size_t count = body / block;
    if (count == 0) return;

    std::vector<uint8_t> copy(block);
    for (size_t b = 0; b < count; b++)
    {
        uint8_t* p = data.data() + off + b * block;
        memcpy(copy.data(), p, block);
        for (size_t i = 0; i < block / 2; i++)
        {
            p[i * 2 + 0] = copy[block / 2 + i];
            p[i * 2 + 1] = copy[i];
        }
    }

    if (off > 0) data.erase(data.begin(), data.begin() + (std::ptrdiff_t)off);
}

// Opt-in trace for the content-based driver resolution. Set RMGK_FBNEO_TRACE=1
// to see, on stderr, why a console ROM did or did not map to a driver.
void fbneo_trace(const char* fmt, ...)
{
    static const bool enabled = std::getenv("RMGK_FBNEO_TRACE") != nullptr;
    if (!enabled) return;
    va_list args;
    va_start(args, fmt);
    std::fputs("[FBNEO] ", stderr);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    va_end(args);
}

bool is_archive_path(const std::string& path)
{
    const std::string name = basename_lower(path);
    return (name.size() > 4 &&
            (name.compare(name.size() - 4, 4, ".zip") == 0 ||
             name.compare(name.size() - 4, 4, ".7z") == 0));
}

// Copy exactly `len` bytes from `path` into `dest`. The caller's buffer is sized
// to the driver's declared ROM length, so a short (or long) file is a miss.
bool read_exact(const std::string& path, UINT8* dest, uint32_t len)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    const size_t got = fread(dest, 1, len, f);
    fclose(f);
    return got == (size_t)len;
}

// Deterministic 128-bit driver identifier, formatted as 32 hex chars (see
// DriverRomId). Built from four salted FNV-1a passes so both peers always
// derive the same value from the driver's normalized short name.
std::string driver_rom_id(const std::string& normalizedName)
{
    static const uint32_t seeds[4] = { 0x811c9dc5u, 0x01000193u, 0x9e3779b9u, 0x85ebca6bu };
    char out[33];
    for (int s = 0; s < 4; s++)
    {
        uint32_t h = seeds[s];
        for (unsigned char c : normalizedName) { h ^= c; h *= 16777619u; }
        std::snprintf(out + s * 8, 9, "%08x", (unsigned)h);
    }
    return std::string(out, 32);
}

// burn.h exposes BurnHighCol as a function pointer whose default initialiser
// (burn.cpp) is a filler that returns 0xffffffff -- pure white. The interactive
// frontends replace it through SetBurnHighCol(), which lives in the burner/misc
// layer that the headless 'lib' frontend does not build. Left alone, every
// palette entry collapses to white, so the whole screen is white. Point it at a
// real 32-bpp RGB converter (identical to misc.cpp's HighCol24) instead.
UINT32 __cdecl fbneo_highcol32(INT32 r, INT32 g, INT32 b, INT32 /* i */)
{
    return ((UINT32)r << 16) | ((UINT32)g << 8) | (UINT32)b;
}

// 16-bpp counterpart for BDF_16BIT_ONLY drivers. Their palette arrays are
// UINT16, so a 32-bpp converter gets truncated and every colour comes out
// wrong (the Mega Drive stores RGB565 samples in UINT16 LUTs). Same component
// order as FBNeo's own HighCol16 / misc.cpp.
UINT32 __cdecl fbneo_highcol16(INT32 r, INT32 g, INT32 b, INT32 /* i */)
{
    return ((UINT32)(r << 8) & 0xf800) | ((UINT32)(g << 3) & 0x07e0) | ((UINT32)(b >> 3) & 0x001f);
}

// RGB565 -> 0x00RRGGBB, matching fbneo_highcol32's component order.
static inline uint32_t rgb565_to_xrgb8888(uint16_t p)
{
    const uint32_t r = ((p >> 11) & 0x1f);
    const uint32_t g = ((p >> 5) & 0x3f);
    const uint32_t b = (p & 0x1f);
    return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

// FBNeo's BurnLoadRomExt() asks the application to decode ROM index i into the
// caller's Dest buffer, sized to the driver's declared length. Real FBNeo
// frontends resolve a ROM by its CRC32 (the archive cache is keyed by CRC), and
// that is also how romsets that store a file under a different name still load
// -- e.g. the NeoGeo BIOS "sp-s3.sp1" is shipped as "asia-s3.rom" in the very
// same neogeo.zip. We therefore match by CRC32 first and fall back to the name.
//
// We must never copy more than the driver's declared nLen: the destination is
// exactly that size, so loading a larger archive entry straight into it would
// overflow and corrupt the heap. ZipLoadFile() clamps the read to nLen; the old
// ZipLoadOneFile() did not, which crashed the emulator on mismatched romsets.
//
// NOTE on the ".zip" suffix: ZipOpen() rebuilds the path as "<name>.zip" /
// "<name>.7z", so the archive must be handed over WITHOUT its extension.
// g_archives stores exactly that "open name"; g_archiveFiles keeps the original
// path for diagnostics.
INT32 __cdecl fbneo_load_rom(UINT8* dest, INT32* pnWrote, INT32 i)
{
    struct BurnRomInfo ri;
    memset(&ri, 0, sizeof(ri));
    BurnDrvGetRomInfo(&ri, i);
    if (ri.nType == 0 || ri.nLen == 0) { if (pnWrote) *pnWrote = 0; return 1; }

    char* name = nullptr;
    BurnDrvGetRomName(&name, i, 0);
    const std::string want = basename_lower(name ? name : ri.szName ? ri.szName : "");

    std::lock_guard<std::mutex> zipLock(g_zipMutex);
    for (size_t a = 0; a < g_archives.size(); a++)
    {
        if (ZipOpen(const_cast<char*>(g_archives[a].c_str())) != 0)
            continue;

        struct ZipEntry* list = nullptr;
        INT32 count = 0;
        if (ZipGetList(&list, &count) != 0)
        {
            ZipClose();
            continue;
        }

        INT32 byCrc = -1, byName = -1;
        for (INT32 e = 0; e < count; e++)
        {
            if (list[e].nLen != ri.nLen) continue;
            if (byCrc < 0 && ri.nCrc != 0 && list[e].nCrc == ri.nCrc) { byCrc = e; break; }
            if (byName < 0 && !want.empty() && basename_lower(list[e].szName ? list[e].szName : "") == want)
                byName = e;
        }

        // Interleaved .smd dump (packed Mega Drive sets are half .smd): decode
        // each such entry and compare the resulting body with the driver's
        // CRC/size. Only looks at entries whose stored size could hold the body.
        std::vector<uint8_t> smdBody;
        if (byCrc < 0 && byName < 0 && ri.nLen != 0 && (ri.nLen % 0x4000) == 0 &&
            ri.nLen <= (16u << 20))
        {
            for (INT32 e = 0; e < count; e++)
            {
                const std::string entryName = basename_lower(list[e].szName ? list[e].szName : "");
                if (!has_ext(entryName, ".smd")) continue;
                if (list[e].nLen == 0 || list[e].nLen > ((16u << 20) + 512)) continue;

                std::vector<uint8_t> body(list[e].nLen);
                INT32 got = 0;
                if (ZipLoadFile(body.data(), (INT32)body.size(), &got, e) != 0 ||
                    got != (INT32)body.size())
                {
                    continue;
                }
                smd_deinterleave(body);
                if (body.size() != (size_t)ri.nLen) continue;
                if (ri.nCrc == 0 || crc32_update(0, body.data(), body.size()) != ri.nCrc) continue;

                smdBody.swap(body);
                break;
            }
        }

        const INT32 entry = (byCrc >= 0) ? byCrc : byName;

        for (INT32 e = 0; e < count; e++) free(list[e].szName);
        free(list);

        if (!smdBody.empty())
        {
            memcpy(dest, smdBody.data(), smdBody.size());
            ZipClose();
            if (pnWrote) *pnWrote = ri.nLen;
            return 0;
        }

        if (entry < 0) { ZipClose(); continue; }

        INT32 wrote = 0;
        const INT32 loadRet = ZipLoadFile(dest, (INT32)ri.nLen, &wrote, entry);
        ZipClose();

        if (loadRet == 0 && wrote == (INT32)ri.nLen)
        {
            if (pnWrote) *pnWrote = wrote;
            return 0;
        }
    }

    // Console game shipped as a loose ROM file (or a loose companion ROM passed
    // alongside the archive): match the driver's entry by CRC + size first, and
    // fall back to the canonical name.
    for (const RawRomFile& raw : g_rawFiles)
    {
        if (raw.len != (uint32_t)ri.nLen) continue;
        const bool crcOk  = ri.nCrc != 0 && raw.crc == ri.nCrc;
        const bool nameOk = !want.empty() && basename_lower(raw.path) == want;

        if (raw.smd)
        {
            // Interleaved .smd: crc/len already describe the decoded body, so the
            // name can never match the driver's .bin entry; only content counts.
            if (!crcOk) continue;
            std::vector<uint8_t> body;
            if (!read_file_bytes(raw.path, 64ull << 20, body)) continue;
            smd_deinterleave(body);
            if (body.size() != (size_t)ri.nLen) continue;
            memcpy(dest, body.data(), body.size());
            if (pnWrote) *pnWrote = ri.nLen;
            return 0;
        }

        if (!crcOk && !nameOk) continue;
        if (read_exact(raw.path, dest, (uint32_t)ri.nLen))
        {
            if (pnWrote) *pnWrote = ri.nLen;
            return 0;
        }
    }

    // BIOS / companion ROM the game file does not carry ("coleco.rom",
    // "fdsbios.nes", "msx.rom", ...), found beside the game by canonical name.
    if (!want.empty())
    {
        auto it = g_dirFiles.find(want);
        if (it != g_dirFiles.end() && read_exact(it->second, dest, (uint32_t)ri.nLen))
        {
            if (pnWrote) *pnWrote = ri.nLen;
            return 0;
        }
    }

    if (pnWrote) *pnWrote = 0;
    if (getenv("RMGK_FBNEO_DEBUG_ROMS")) {
        fprintf(stderr, "[fbneo] ROM NOT FOUND: '%s' crc=%08x len=%u in %zu archive(s)\n",
                name ? name : "(null)", (unsigned)ri.nCrc, (unsigned)ri.nLen, g_archives.size());
    }
    return 1;
}

// Strip a single trailing archive extension: ZipOpen() re-appends ".zip"/".7z"
// to whatever it is given, so <rom>.zip must be handed over as "<rom>".
std::string archive_open_name(const std::string& path)
{
    std::string::size_type slash = path.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);

    const char* exts[] = { ".zip", ".7z" };
    for (const char* e : exts)
    {
        if (file.size() > 4)
        {
            std::string lower;
            for (char c : file) lower.push_back((char)lower_c((unsigned char)c));
            std::string needle = e;
            if (lower.size() >= needle.size() &&
                lower.compare(lower.size() - needle.size(), needle.size(), needle) == 0)
            {
                file.erase(file.size() - needle.size());
                break;
            }
        }
    }
    return dir + file;
}

int find_driver_index(const std::string& gameName)
{
    if (gameName.empty()) return -1;

    int idx = BurnDrvGetIndex(const_cast<char*>(gameName.c_str()));
    if (idx >= 0) return idx;

    // The host may pass a zip file name ("sf2ce.zip") or a differently-cased
    // driver name. Walk the driver table comparing the game name to each
    // driver's short name (read-only, see RmgkDriverShortName).
    const std::string want = basename_lower(gameName);
    const INT32 count = RmgkDriverCount();
    for (INT32 i = 0; i < count; i++)
    {
        const char* shortName = RmgkDriverShortName(i);
        if (shortName && *shortName && basename_lower(shortName) == want)
        {
            return i;
        }
    }
    return -1;
}

void reset_all_digital_inputs()
{
    struct BurnInputInfo bii;
    for (UINT32 i = 0;; i++)
    {
        memset(&bii, 0, sizeof(bii));
        if (BurnDrvGetInputInfo(&bii, i) != 0) break;
        if (bii.nType == BIT_DIGITAL && bii.pVal) *bii.pVal = 0;
    }
}

// Collects the driver inputs behind the cabinet test switch. Boards that have a
// dedicated test input ("diag"/"diagnostic"/"test") use it to open the settings
// menu; drivers without one fall back to their "service" input, matching what
// the FBNeo headless frontend does for its service/test button.
void build_test_input_targets()
{
    std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
    g_testInputTargets.clear();

    UINT32 serviceTarget = UINT32_MAX;
    struct BurnInputInfo bii;
    for (UINT32 i = 0;; i++)
    {
        memset(&bii, 0, sizeof(bii));
        if (BurnDrvGetInputInfo(&bii, i) != 0) break;
        if (bii.nType != BIT_DIGITAL || bii.pVal == nullptr || bii.szInfo == nullptr) continue;

        if (!strcmp(bii.szInfo, "diag") || !strcmp(bii.szInfo, "diagnostic") ||
            !strcmp(bii.szInfo, "diagnostics") || !strcmp(bii.szInfo, "test"))
        {
            g_testInputTargets.push_back(i);
        }
        else if (!strcmp(bii.szInfo, "service"))
        {
            serviceTarget = i;
        }
    }

    if (g_testInputTargets.empty() && serviceTarget != UINT32_MAX)
    {
        g_testInputTargets.push_back(serviceTarget);
    }
}

void apply_input_masks(const std::vector<uint32_t>& masks)
{
    reset_all_digital_inputs();
    struct BurnInputInfo bii;
    std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
    for (size_t p = 0; p < masks.size() && p < g_playerInputMap.size(); p++)
    {
        const uint32_t m = masks[p];
        const auto& list = g_playerInputMap[p].targets;
        for (size_t b = 0; b < list.size() && b < 32; b++)
        {
            if (((m >> b) & 1u) == 0) continue;
            for (size_t t = 0; t < list[b].size(); t++)
            {
                memset(&bii, 0, sizeof(bii));
                if (BurnDrvGetInputInfo(&bii, list[b][t]) != 0) continue;
                if (bii.pVal) *bii.pVal = 1;
            }
        }
    }

    // The cabinet test switch is not part of the player input map, so the reset
    // above clears it every frame: reapply it while the hotkey is held. Rollback
    // sessions normally own the input stream (an extra unsynchronized press would
    // desync a netplay peer), but a local session has no peer to desync: the
    // frontend allows the press there through g_testAllowLocalRollback.
    const bool deliverTest = g_testPressed.load(std::memory_order_relaxed) &&
        (g_inputCallback == nullptr || g_testAllowLocalRollback.load(std::memory_order_relaxed));
    if (deliverTest)
    {
        for (size_t t = 0; t < g_testInputTargets.size(); t++)
        {
            memset(&bii, 0, sizeof(bii));
            if (BurnDrvGetInputInfo(&bii, g_testInputTargets[t]) != 0) continue;
            if (bii.pVal) *bii.pVal = 1;
        }
    }
}

void apply_latched_input()
{
    if (!g_inputCallback)
    {
        apply_input_masks(g_localMasks);
        return;
    }
    int players = g_inputPlayers > 0 ? g_inputPlayers : g_players;
    if ((int)g_latchedInput.size() < players * g_inputSize)
        g_latchedInput.assign((size_t)players * g_inputSize, 0);
    std::vector<uint32_t> masks((size_t)players, 0);
    memset(g_latchedInput.data(), 0, g_latchedInput.size());
    if (g_inputCallback(masks.data(), g_inputSize, players) == 0)
    {
        apply_input_masks(g_localMasks);
        return;
    }
    apply_input_masks(masks);
}

// FBNeo frontends normally turn the game's DIP switch list into constant
// inputs (GameInp) and load the per-switch defaults from the entries flagged
// with nFlags == 0xFF. This runtime writes the driver's input memory directly
// and never builds that table, so without this step every DIP byte (CPS1's
// Cpi01A/Cpi01C/Cpi01E, etc.) stays at zero -- which silently disables things
// like the CPS1 "Demo Sound" switch (attract mode is mute). Replicate the
// defaults here, writing them straight into the driver input memory.
void apply_dip_defaults()
{
    struct BurnDIPInfo bdi;
    int i;
    int nDIPOffset = 0;

    // The optional DIP_OFFSET marker shifts the indices of all later DIPs.
    for (i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
    {
        if (bdi.nFlags == 0xF0)
        {
            nDIPOffset = bdi.nInput;
            break;
        }
    }

    std::vector<std::pair<UINT32, UINT8> > dips;
    for (i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
    {
        if (bdi.nFlags != 0xFF) continue;   // 0xFF marks a DIP default entry

        const UINT32 idx = (UINT32)(bdi.nInput + nDIPOffset);
        const UINT8  bit = (UINT8)(bdi.nSetting & bdi.nMask);

        bool merged = false;
        for (size_t k = 0; k < dips.size(); k++)
        {
            if (dips[k].first == idx)
            {
                dips[k].second = (UINT8)((dips[k].second & ~bdi.nMask) | bit);
                merged = true;
                break;
            }
        }
        if (!merged) dips.push_back(std::make_pair(idx, bit));
    }

    for (size_t k = 0; k < dips.size(); k++)
    {
        struct BurnInputInfo bii;
        memset(&bii, 0, sizeof(bii));
        if (BurnDrvGetInputInfo(&bii, dips[k].first) != 0) continue;
        if (bii.pVal) *bii.pVal = dips[k].second;
    }

    // Re-apply any frontend overrides on top of the driver defaults so a
    // user's DIP choices survive a driver reset. Keys are BurnInputInfo
    // indices (already offset-adjusted, matching the defaults above).
    {
        std::vector<std::pair<int, uint8_t> > overrides;
        {
            std::lock_guard<std::mutex> lock(g_dipMutex);
            overrides = g_dipOverrides;
        }
        for (size_t k = 0; k < overrides.size(); k++)
        {
            struct BurnInputInfo bii;
            memset(&bii, 0, sizeof(bii));
            if (BurnDrvGetInputInfo(&bii, (UINT32)overrides[k].first) != 0) continue;
            if (bii.pVal) *bii.pVal = overrides[k].second;
        }
    }
}

// One ordered entry for a player: a display name plus the driver input indices
// it drives when active. Mirrors what the frontend shows and what the runtime
// mask bit selects.
struct DriverInputEntry { std::string name; std::vector<UINT32> targets; };

bool asciiEqualNoCase(const char* a, const char* b)
{
    for (; *a && *b; a++, b++)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == *b;
}

// Enumerates the driver currently selected in nBurnDrvActive and returns, per
// player, the plain digital inputs followed by the FBNeo-style combination
// macros. The order is the contract between the runtime mask (bit index), the
// bindings the frontend stores by name, and the input list it displays, so it
// must stay deterministic. Mirrors FinalBurn Neo's GameInpInitMacros().
void enumerate_input_entries(int maxPlayers, std::vector<std::vector<DriverInputEntry> >& out)
{
    out.assign((size_t)maxPlayers, std::vector<DriverInputEntry>());

    // Button slots used to synthesise the combos, per player.
    struct MacroSlots
    {
        int weakPunch = -1, mediumPunch = -1, strongPunch = -1;
        int weakKick  = -1, mediumKick  = -1, strongKick  = -1;
        int buttonA   = -1, buttonB     = -1, buttonC     = -1, buttonD = -1;
    };
    std::vector<MacroSlots> slots((size_t)maxPlayers);

    struct BurnInputInfo bii;
    for (UINT32 i = 0;; i++)
    {
        memset(&bii, 0, sizeof(bii));
        if (BurnDrvGetInputInfo(&bii, i) != 0) break;
        if (bii.nType != BIT_DIGITAL || !bii.szName) continue;
        if (bii.szName[0] != 'P' || bii.szName[2] != ' ' ||
            bii.szName[1] < '1' || bii.szName[1] > '9') continue;

        const int p = bii.szName[1] - '1';
        if (p < 0 || p >= maxPlayers) continue;
        if (out[(size_t)p].size() >= 32) continue;   // runtime mask is 32 bits

        DriverInputEntry entry;
        entry.name = bii.szName;
        entry.targets.push_back(i);
        out[(size_t)p].push_back(entry);

        const char* suffix = bii.szName + 2;
        MacroSlots& s = slots[(size_t)p];
        if      (asciiEqualNoCase(suffix, " Weak Punch"))   s.weakPunch   = (int)i;
        else if (asciiEqualNoCase(suffix, " Medium Punch")) s.mediumPunch = (int)i;
        else if (asciiEqualNoCase(suffix, " Strong Punch")) s.strongPunch = (int)i;
        else if (asciiEqualNoCase(suffix, " Weak Kick"))    s.weakKick    = (int)i;
        else if (asciiEqualNoCase(suffix, " Medium Kick"))  s.mediumKick  = (int)i;
        else if (asciiEqualNoCase(suffix, " Strong Kick"))  s.strongKick  = (int)i;
        else if (asciiEqualNoCase(suffix, " Button A"))     s.buttonA     = (int)i;
        else if (asciiEqualNoCase(suffix, " Button B"))     s.buttonB     = (int)i;
        else if (asciiEqualNoCase(suffix, " Button C"))     s.buttonC     = (int)i;
        else if (asciiEqualNoCase(suffix, " Button D"))     s.buttonD     = (int)i;
    }

    for (int p = 0; p < maxPlayers; p++)
    {
        const MacroSlots& s   = slots[(size_t)p];
        const std::string pfx = "P" + std::to_string(p + 1) + " Buttons ";

        auto addMacro = [&](const std::string& label, std::initializer_list<int> idx)
        {
            if (out[(size_t)p].size() >= 32) return;
            DriverInputEntry e;
            e.name = pfx + label;
            for (int v : idx)
            {
                if (v >= 0) e.targets.push_back((UINT32)v);
            }
            if (!e.targets.empty()) out[(size_t)p].push_back(e);
        };

        const bool punch3 = s.weakPunch >= 0 && s.mediumPunch >= 0 && s.strongPunch >= 0;
        const bool kick3  = s.weakKick  >= 0 && s.mediumKick  >= 0 && s.strongKick  >= 0;

        if (punch3) addMacro("3x Punch", { s.weakPunch,   s.mediumPunch, s.strongPunch });
        if (kick3)  addMacro("3x Kick",  { s.weakKick,    s.mediumKick,  s.strongKick  });
        if (punch3 && kick3)
        {
            addMacro("Weak PK",   { s.weakPunch,   s.weakKick   });
            addMacro("Medium PK", { s.mediumPunch, s.mediumKick });
            addMacro("Strong PK", { s.strongPunch, s.strongKick });
            addMacro("SP + WK",   { s.strongPunch, s.weakKick   });
        }

        if (s.buttonA >= 0 && s.buttonB >= 0 && s.buttonC >= 0 && s.buttonD >= 0)
        {
            addMacro("AB",   { s.buttonA, s.buttonB });
            addMacro("AC",   { s.buttonA, s.buttonC });
            addMacro("AD",   { s.buttonA, s.buttonD });
            addMacro("BC",   { s.buttonB, s.buttonC });
            addMacro("BD",   { s.buttonB, s.buttonD });
            addMacro("CD",   { s.buttonC, s.buttonD });
            addMacro("ABC",  { s.buttonA, s.buttonB, s.buttonC });
            addMacro("ABD",  { s.buttonA, s.buttonB, s.buttonD });
            addMacro("ACD",  { s.buttonA, s.buttonC, s.buttonD });
            addMacro("BCD",  { s.buttonB, s.buttonC, s.buttonD });
            addMacro("ABCD", { s.buttonA, s.buttonB, s.buttonC, s.buttonD });
        }
    }
}

void build_input_map()
{
    std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
    g_playerInputMap.assign((size_t)g_players, PlayerInputMap{});

    const int players = (g_players > 0) ? g_players : 2;
    std::vector<std::vector<DriverInputEntry> > entries;
    enumerate_input_entries(players, entries);

    for (int p = 0; p < (int)g_playerInputMap.size() && p < (int)entries.size(); p++)
    {
        for (size_t e = 0; e < entries[(size_t)p].size(); e++)
        {
            g_playerInputMap[(size_t)p].names.push_back(entries[(size_t)p][e].name);
            g_playerInputMap[(size_t)p].targets.push_back(entries[(size_t)p][e].targets);
        }
    }

    if (getenv("RMGK_FBNEO_TRACE") != nullptr)
    {
        for (int p = 0; p < (int)g_playerInputMap.size(); p++)
        {
            fprintf(stderr, "[FBNEO-IN] inputmap P%d count=%d:", p + 1,
                    (int)g_playerInputMap[(size_t)p].names.size());
            for (size_t e = 0; e < g_playerInputMap[(size_t)p].names.size(); e++)
            {
                fprintf(stderr, " [%d]%s", (int)e, g_playerInputMap[(size_t)p].names[e].c_str());
            }
            fprintf(stderr, "\n");
        }
    }
}

// ---------------------------------------------------------------------------
// DIP switch helpers.
//
// The driver exposes a flat table via BurnDrvGetDIPInfo. Entries are:
//   0xF0        -> offset marker (nInput = base index shift)
//   0xFE / 0xFD -> group header (szText = group name, nSetting = #settings)
//   0xFF        -> per-byte default value
//   other (>=1) -> a setting's first line (low nibble = #lines in the setting,
//                  0x40 = propagate writes to following lines, 0x80 = "ANY")
// A setting may span several lines; its continuation lines have nFlags == 0.
// Values live in the driver's input memory (BurnInputInfo[nInput + offset].pVal),
// which is exactly what apply_dip_defaults writes, so we read/write there too.

struct DipSettingRef { int firstLine; std::string name; };
struct DipGroupRef   { std::string name; int headerIndex; std::vector<DipSettingRef> settings; };

int dip_get_offset()
{
    struct BurnDIPInfo bdi;
    for (int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
    {
        if (bdi.nFlags == 0xF0) return bdi.nInput;
    }
    return 0;
}

bool dip_enumerate(std::vector<DipGroupRef>& out)
{
    out.clear();
    if (!g_driverInited) return false;
    struct BurnDIPInfo bdi;
    int currentGroup = -1;
    for (int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
    {
        if (bdi.nFlags == 0xF0 || bdi.nFlags == 0xFF) continue; // offset / default
        if (bdi.nFlags == 0xFE || bdi.nFlags == 0xFD)
        {
            DipGroupRef g;
            g.name        = bdi.szText ? bdi.szText : "";
            g.headerIndex = i;
            out.push_back(g);
            currentGroup  = (int)out.size() - 1;
            continue;
        }
        if (bdi.nFlags == 0) continue; // continuation line (skipped by its setting)
        if (currentGroup < 0) continue;
        DipSettingRef s;
        s.firstLine = i;
        s.name      = bdi.szText ? bdi.szText : "";
        out[currentGroup].settings.push_back(s);
        int lines = bdi.nFlags & 0x0F;
        if (lines > 1) i += (lines - 1); // skip this setting's continuation lines
    }
    return !out.empty();
}

bool dip_read_byte(int byteIdx, uint8_t& out)
{
    struct BurnInputInfo bii;
    memset(&bii, 0, sizeof(bii));
    if (BurnDrvGetInputInfo(&bii, (UINT32)byteIdx) != 0) return false;
    if (!bii.pVal) return false;
    out = *bii.pVal;
    return true;
}

bool dip_write_byte(int byteIdx, uint8_t value)
{
    struct BurnInputInfo bii;
    memset(&bii, 0, sizeof(bii));
    if (BurnDrvGetInputInfo(&bii, (UINT32)byteIdx) != 0) return false;
    if (!bii.pVal) return false;
    *bii.pVal = value;
    return true;
}

// Replica of inpdipsw.cpp CheckSetting(): is the setting whose first line is at
// idx the one currently selected, given the live driver input bytes?
bool dip_check_setting(int idx, int nDIPOffset)
{
    struct BurnDIPInfo bdi;
    if (BurnDrvGetDIPInfo(&bdi, idx) != 0) return false;
    uint8_t value = 0;
    if (!dip_read_byte(bdi.nInput + nDIPOffset, value)) return false;
    if ((value & bdi.nMask) != bdi.nSetting) return false;
    unsigned char nFlags = bdi.nFlags;
    if ((nFlags & 0x0F) <= 1) return true;
    for (int j = 1; j < (nFlags & 0x0F); j++)
    {
        struct BurnDIPInfo bdi2;
        if (BurnDrvGetDIPInfo(&bdi2, idx + j) != 0) return false;
        uint8_t v2 = 0;
        if (!dip_read_byte(bdi2.nInput + nDIPOffset, v2)) return false;
        if (nFlags & 0x80)
        {
            if ((v2 & bdi2.nMask) == bdi2.nSetting) return false;
        }
        else
        {
            if ((v2 & bdi2.nMask) != bdi2.nSetting) return false;
        }
    }
    return true;
}

// Replica of inpdipsw.cpp write handler: RMW the setting's first-line byte and,
// when 0x40 is set, propagate to the following nFlags == 0 lines. Collects every
// byte written (index -> value) into *written so the caller can record overrides.
bool dip_select_setting(int idx, int nDIPOffset, std::vector<std::pair<int, uint8_t> >* written)
{
    struct BurnDIPInfo bdi;
    if (BurnDrvGetDIPInfo(&bdi, idx) != 0) return false;
    int byteIdx = bdi.nInput + nDIPOffset;
    uint8_t cur = 0;
    if (!dip_read_byte(byteIdx, cur)) return false;
    uint8_t next = (uint8_t)((cur & ~bdi.nMask) | (bdi.nSetting & bdi.nMask));
    if (!dip_write_byte(byteIdx, next)) return false;
    if (written) written->push_back(std::make_pair(byteIdx, next));
    if (bdi.nFlags & 0x40)
    {
        struct BurnDIPInfo b2;
        for (int j = idx + 1; BurnDrvGetDIPInfo(&b2, j) == 0; j++)
        {
            if (b2.nFlags != 0) break;
            int bIdx = b2.nInput + nDIPOffset;
            uint8_t c2 = 0;
            if (!dip_read_byte(bIdx, c2)) return false;
            uint8_t n2 = (uint8_t)((c2 & ~b2.nMask) | (b2.nSetting & b2.nMask));
            if (!dip_write_byte(bIdx, n2)) return false;
            if (written) written->push_back(std::make_pair(bIdx, n2));
        }
    }
    return true;
}

} // namespace

void Trace(const std::string& message)
{
    fbneo_trace("%s", message.c_str());
}

// Remember every regular file in `dir` by lower-case name (see g_dirFiles).
void add_dir_files(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (dir.empty()) return;

    int seen = 0;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end && seen < 16384; it.increment(ec))
    {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        const auto size = it->file_size(fec);
        if (fec || size == 0 || size > (64ull << 20)) continue;
        const std::string name = basename_lower(it->path().string());
        if (!name.empty()) g_dirFiles.emplace(name, it->path().string());
        seen++;
    }
}

void build_dir_file_table()
{
    g_dirFiles.clear();
    if (g_archiveFiles.empty()) return;

    const std::filesystem::path dir = std::filesystem::path(g_archiveFiles[0]).parent_path();
    add_dir_files(dir);                                 // own system folder wins
    add_dir_files(dir / "bios");                        // ...then roms/bios
    add_dir_files(dir.parent_path());                   // ...then the roms root
    add_dir_files(dir.parent_path() / "bios");          // ...and <root>/../bios (Batocera)
}

// Allocate the render buffer at the driver's native depth and point the
// presenter at the 32-bpp frame. `fw` x `fh` is the full drawable buffer and
// `vw` x `vh` the visible rectangle the UI shows (top-left of the buffer for
// horizontal drivers).
void configure_video_buffers(INT32 fw, INT32 fh, INT32 vw, INT32 vh)
{
    g_videoBuffer.assign((size_t)fw * (size_t)fh * (size_t)g_nativeBpp, 0);
    nBurnBpp   = g_nativeBpp;
    nBurnPitch = fw * g_nativeBpp;
    pBurnDraw  = g_videoBuffer.data();

    g_fullWidth  = fw;
    g_fullHeight = fh;

    if (g_nativeBpp == 2)
    {
        g_displayBuffer.assign((size_t)fw * (size_t)fh * 4, 0);
        g_videoFrame.pixels = g_displayBuffer.data();
    }
    else
    {
        g_displayBuffer.clear();
        g_videoFrame.pixels = g_videoBuffer.data();
    }

    g_videoFrame.width  = vw;
    g_videoFrame.height = vh;
    g_videoFrame.pitch  = fw * 4;   // presenter stride is always 32 bpp
    g_videoFrame.bpp    = 4;

    fbneo_trace("video %dx%d visible %dx%d bpp=%d", fw, fh, vw, vh, g_nativeBpp);
}

// Re-read the driver geometry and resize when it changed. Drivers that switch
// resolution mid-game (Mega Drive 320/256 and interlace 320x448, CPS3) call
// BurnDrvSetVisibleSize() and ReinitialiseVideo(), which nulls pBurnDraw; the
// buffer must follow or a taller mode would be written past its end.
void refresh_video_geometry(bool force)
{
    INT32 fw = 0, fh = 0;
    BurnDrvGetFullSize(&fw, &fh);
    if (fw <= 0 || fh <= 0) BurnDrvGetVisibleSize(&fw, &fh);
    if (fw <= 0 || fh <= 0) { fw = 384; fh = 224; }

    // Visible region shown to the user. For horizontal drivers it is the
    // top-left vw x vh rectangle of the full buffer (BurnDrvGetVisibleOffs is
    // 0,0); vertical drivers report the already-rotated full size as displayed.
    INT32 vw = fw, vh = fh;
    if (!(g_orientationFlags & kVideoVertical))
    {
        INT32 cvw = 0, cvh = 0;
        BurnDrvGetVisibleSize(&cvw, &cvh);
        if (cvw > 0 && cvh > 0 && cvw <= fw && cvh <= fh) { vw = cvw; vh = cvh; }
    }

    if (!force && fw == g_fullWidth && fh == g_fullHeight &&
        vw == g_videoFrame.width && vh == g_videoFrame.height)
    {
        return;
    }

    configure_video_buffers(fw, fh, vw, vh);
}

// RGB565 render buffer -> XRGB8888 frame the presenter consumes.
void convert_native_to_display()
{
    if (g_nativeBpp != 2 || g_displayBuffer.empty()) return;

    const int w = g_videoFrame.width;
    const int h = g_videoFrame.height;
    const size_t srcPitch = (size_t)nBurnPitch;
    const size_t dstPitch = (size_t)g_videoFrame.pitch;
    for (int y = 0; y < h; y++)
    {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(g_videoBuffer.data() + (size_t)y * srcPitch);
        uint8_t* dst = g_displayBuffer.data() + (size_t)y * dstPitch;
        for (int x = 0; x < w; x++)
        {
            const uint32_t c = rgb565_to_xrgb8888(src[x]);
            std::memcpy(dst + (size_t)x * 4, &c, 4);
        }
    }
}

bool Initialize(const char* gameName, const char* const* archives, int archiveCount, int players)
{
    if (g_driverInited) return true;

    g_paused.store(false, std::memory_order_relaxed);
    g_testPulseFrames.store(0, std::memory_order_relaxed);
    g_testPressed.store(false, std::memory_order_relaxed);

    if (!ensure_lib_init())
    {
        return false;
    }

    g_players = (players > 0) ? players : 2;
    {
        std::lock_guard<std::mutex> lock(g_localInputMutex);
        g_localMasks.assign((size_t)g_players, 0);
    }
    g_inputSize = 4;
    g_latchedInput.assign((size_t)g_players * g_inputSize, 0);
    g_driverName = gameName ? gameName : "";

    g_archives.clear();
    g_archiveFiles.clear();
    g_rawFiles.clear();
    if (archives) {
        for (int i = 0; i < archiveCount && archives[i]; i++)
        {
            // Skip archives that do not exist: the driver may legitimately only
            // ship part of a romset (parent zips are supplied separately).
            std::error_code ec;
            if (!std::filesystem::exists(archives[i], ec)) continue;
            const std::string path = archives[i];
            g_archiveFiles.emplace_back(path);

            if (is_archive_path(path))
            {
                g_archives.emplace_back(archive_open_name(path));
            }
            else
            {
                // Loose ROM file: identify it by content, like a zip entry.
                RawRomFile raw;
                raw.path = path;
                raw.crc  = 0;
                raw.len  = 0;
                raw.smd  = has_ext(basename_lower(path), ".smd");
                if (ec || std::filesystem::file_size(path, ec) > (64ull << 20))
                {
                    continue;
                }
                if (raw.smd)
                {
                    std::vector<uint8_t> body;
                    if (read_file_bytes(path, 64ull << 20, body))
                    {
                        smd_deinterleave(body);
                        raw.crc = crc32_update(0, body.data(), body.size());
                        raw.len = (uint32_t)body.size();
                    }
                }
                else
                {
                    uint32_t crc = 0, len = 0;
                    if (crc32_file(path, crc, len))
                    {
                        raw.crc = crc;
                        raw.len = len;
                    }
                }
                g_rawFiles.push_back(raw);
            }
        }
    }
    build_dir_file_table();

    if (!g_driverName.empty())
    {
        int idx = find_driver_index(g_driverName);
        if (idx < 0)
        {
            CoreSetError("FBNeo initialization failed: unknown driver \"" + g_driverName + "\"");
            fprintf(stderr, "[fbneo] driver not found: '%s'\n", g_driverName.c_str());
            Shutdown();
            return false;
        }
        nBurnDrvActive = (UINT32)idx;
    }

    nMaxPlayers = BurnDrvGetMaxPlayers();
    nBurnLayer  = 0xFF;
    nBurnSoundRate = 48000;
    nBurnSoundLen  = 0;
    pBurnSoundOut = nullptr;

    // A generous default framebuffer; drivers that draw during BurnDrvInit()
    // have somewhere to write. It is resized to the real geometry afterwards.
    g_videoBuffer.assign(1024ull * 1024ull * 4ull, 0);

    // Orientation and pixel depth come from the driver table, so they are known
    // before BurnDrvInit(). Drivers flagged BDF_16BIT_ONLY render RGB565 and
    // keep their palette in UINT16 LUTs; give them a 16-bpp converter and a
    // 16-bpp buffer, exactly like FBNeo's own frontends.
    g_orientationFlags = 0;
    const INT32 drvFlags = BurnDrvGetFlags();
    if (drvFlags & BDF_ORIENTATION_VERTICAL) g_orientationFlags |= kVideoVertical;
    if (drvFlags & BDF_ORIENTATION_FLIPPED)  g_orientationFlags |= kVideoFlipped;
    g_nativeBpp = (drvFlags & BDF_16BIT_ONLY) ? 2 : 4;

    nBurnBpp   = g_nativeBpp;
    nBurnPitch = 1024 * g_nativeBpp;
    pBurnDraw  = g_videoBuffer.data();

    BurnHighCol = (g_nativeBpp == 2) ? fbneo_highcol16 : fbneo_highcol32;

    BurnExtLoadRom = fbneo_load_rom;

    if (BurnDrvInit() != 0)
    {
        CoreSetError("FBNeo initialization failed: BurnDrvInit failed for \"" + g_driverName + "\" (missing or incomplete romset/BIOS files?)");
        fprintf(stderr, "[fbneo] BurnDrvInit failed for '%s' (%zu archive(s)):\n", g_driverName.c_str(), g_archives.size());
        for (const auto& a : g_archiveFiles) fprintf(stderr, "[fbneo]   archive: %s\n", a.c_str());
        BurnExtLoadRom = nullptr;
        Shutdown();
        return false;
    }

    // The lib frontend has no video plugin to trigger this, so palette entries
    // that were computed during BurnDrvInit() (and the driver's cached palette
    // LUT, e.g. CPS' CpsPal) still hold the white filler output. Signal the
    // driver to rebuild its whole palette with the converter installed above.
    BurnRecalcPal();

    // BurnDrvInit()/reset blanks the frontend-owned DIP bytes; restore the
    // driver's declared defaults before the first frame runs.
    apply_dip_defaults();

    // Full framebuffer. Drivers can render well outside the visible window: the
    // CPS1 renderer, for instance, uses a 384x1024 buffer while only the top 224
    // lines are ever on screen.
    refresh_video_geometry(true);

    if (nBurnFPS > 0)
        nBurnSoundLen = (nBurnSoundRate * 100 + (nBurnFPS >> 1)) / nBurnFPS;
    g_soundFrames = nBurnSoundLen;
    g_soundBuffer.assign((size_t)(nBurnSoundLen > 0 ? nBurnSoundLen : 1) * 2, 0);
    pBurnSoundOut = g_soundBuffer.data();

    build_input_map();
    build_test_input_targets();

    g_driverInited = true;

    // The mupen64plus core isn't used for arcade sessions, but front-end code
    // (e.g. CoreGetCurrentRomSettings) may still have latched an INVALID_STATE
    // error before we got here. The runtime is ready now, so drop it.
    CoreSetError(std::string());
    return true;
}

bool IsInitialized() { return g_driverInited; }

const char* DriverShortName() { return g_driverName.c_str(); }

bool Reset()
{
    if (!g_driverInited) return false;

    // Reset Game is an exit + re-init of the driver, which preserves the
    // selected driver and the already-resolved archives. That teardown must not
    // run underneath a live frame or the presenter, so defer it to the
    // emulation thread (applied at the top of the next RunFrame under the frame
    // lock). Requesting it from the GUI thread this way is race-free.
    g_pendingReset.store(true, std::memory_order_relaxed);
    return true;
}

// Applies a deferred reset. Caller must hold g_frameMutex.
static bool apply_pending_reset()
{
    BurnDrvExit();
    g_driverInited = false;

    if (BurnDrvInit() != 0)
    {
        CoreSetError("FBNeo reset failed: BurnDrvInit failed for \"" + g_driverName + "\"");
        return false;
    }

    BurnRecalcPal();
    apply_dip_defaults();
    build_test_input_targets();
    g_driverInited = true;
    return true;
}

// Look up a driver by short name or archive stem (e.g. "sf2ce" / "sf2ce.zip").
// Fills *outFull with the driver's full display name when requested. The cache
// is populated lazily on the first call by walking the driver table once.
static bool driver_lookup(const char* gameName, std::string* outFull)
{
    if (!gameName || !*gameName) return false;

    std::lock_guard<std::recursive_mutex> lock(g_driverNameMutex);

    if (g_driverNameCache.empty())
    {
        if (!ensure_lib_init())
        {
            return false;
        }

        const INT32 count = RmgkDriverCount();
        for (INT32 i = 0; i < count; i++)
        {
            const char* shortName = RmgkDriverShortName(i);
            const char* full      = RmgkDriverFullName(i);
            if (shortName && *shortName)
            {
                const std::string key = basename_lower(shortName);
                g_driverNameCache[key] =
                    (full && *full) ? std::string(full) : std::string(shortName);

                const char* parent = RmgkDriverParentName(i);
                const std::string parentKey =
                    (parent && *parent) ? basename_lower(parent) : std::string();
                // Parent drivers point at themselves; only real clones have one.
                g_driverParentCache[key] = (parentKey == key) ? std::string() : parentKey;
            }
        }
    }

    auto it = g_driverNameCache.find(basename_lower(gameName));
    if (it == g_driverNameCache.end()) return false;
    if (outFull) *outFull = it->second;
    return true;
}

bool IsDriver(const char* gameName)
{
    return driver_lookup(gameName, nullptr);
}

bool DriverFullName(const char* gameName, std::string& outName)
{
    return driver_lookup(gameName, &outName);
}

bool DriverRomId(const char* gameName, std::string& outHex)
{
    if (!gameName || !*gameName) return false;
    outHex = driver_rom_id(basename_lower(gameName));
    return true;
}

bool DriverParent(const char* gameName, std::string& outName)
{
    if (!gameName || !*gameName) return false;

    // Make sure the caches are populated (driver_lookup fills both).
    if (!driver_lookup(gameName, nullptr)) return false;

    std::lock_guard<std::recursive_mutex> lock(g_driverNameMutex);
    auto it = g_driverParentCache.find(basename_lower(gameName));
    if (it == g_driverParentCache.end() || it->second.empty()) return false;
    outName = it->second;
    return true;
}

// ---------------------------------------------------------------------------
// Content-based driver resolution (console systems)
//
// A No-Intro/Redump collection names every file after the game, not after the
// FBNeo driver, and the same title can sit in different system folders. Instead
// of guessing from the path, identify the driver the way FBNeo itself does: by
// the CRC32 + size of the ROM data. The whole driver table is flattened once
// into (crc, len) -> driver index; a candidate then only needs its archive
// entries (or the loose file) looked up in that table.
// ---------------------------------------------------------------------------
namespace
{
struct RomKey
{
    uint32_t crc;
    uint32_t len;
    bool operator==(const RomKey& o) const { return crc == o.crc && len == o.len; }
};

struct RomKeyHash
{
    std::size_t operator()(const RomKey& k) const
    {
        return (std::size_t)(k.crc ^ ((k.len * 2654435761u) >> 1));
    }
};

std::unordered_map<RomKey, std::vector<int>, RomKeyHash> g_romIndex;
bool         g_romIndexReady = false;

std::unordered_map<std::string, std::string> g_resolveCache; // path|size|mtime -> driver
std::mutex                                   g_resolveMutex;

std::string driver_name_at(int idx)
{
    const char* name = RmgkDriverShortName(idx);
    return (name && *name) ? std::string(name) : std::string();
}

bool driver_is_clone_at(int idx)
{
    return (RmgkDriverFlags(idx) & BDF_CLONE) != 0;
}

void build_rom_index()
{
    if (g_romIndexReady) return;

    const INT32 count = RmgkDriverCount();
    for (INT32 i = 0; i < count; i++)
    {
        for (UINT32 r = 0; ; r++)
        {
            UINT32 crc = 0, len = 0, type = 0;
            if (!RmgkDriverRomInfo(i, r, &crc, &len, &type)) break;
            if (len == 0 || crc == 0) continue;
            if (type & BRF_OPT) continue; // alternate dumps do not identify a set
            const RomKey key = { crc, len };
            std::vector<int>& v = g_romIndex[key];
            if (v.size() < 64 && std::find(v.begin(), v.end(), (int)i) == v.end())
                v.push_back((int)i);
        }
    }
    g_romIndexReady = true;
}

// Driver-name prefix of the system folder a file came from (roms/nes -> "nes_").
// This is only a hint: a strong match still wins, but it breaks ties caused by
// the same dump appearing under several drivers/systems.
std::string folder_prefix(const std::string& path)
{
    static const struct { const char* dir; const char* prefix; } kMap[] = {
        { "nes",          "nes_"  },
        { "fds",          "fds_"  },
        { "snes",         "snes_" },
        { "megadrive",    "md_"   },
        { "genesis",      "md_"   },
        { "32x",          "s32x_" },
        { "sega32x",      "s32x_" },
        { "mastersystem", "sms_"  },
        { "sms",          "sms_"  },
        { "gamegear",     "gg_"   },
        { "sg1000",       "sg1k_" },
        { "coleco",       "cv_"   },
        { "colecovision", "cv_"   },
        { "msx",          "msx_"  },
        { "msx1",         "msx_"  },
        { "msx2",         "msx_"  },
        { "pce",          "pce_"  },
        { "pcengine",     "pce_"  },
        { "tg16",         "pce_"  },
        { "ngp",          "ngp_"  },
        { "ngpc",         "ngp_"  },
        { "gba",          "gba_"  },
        { "spectrum",     "spec_" },
        { "channelf",     "chf_"  },
        { "atari2600",    "a26_"  },
        { "astrocde",     "astro_"},
        { "astrocade",    "astro_"},
    };

    const std::string dir = basename_lower(std::filesystem::path(path).parent_path().string());
    for (const auto& m : kMap)
    {
        if (dir == m.dir) return m.prefix;
    }
    return std::string();
}

// Pick the driver that explains most of the supplied (crc, size) pairs. Clones
// share their parent's data, so a non-clone wins a tie; a driver whose short
// name is the archive stem wins outright (renamed but canonical sets), and the
// system folder hint settles the rest.
bool choose_driver(const std::vector<RomKey>& entries, const std::string& stem,
                   const std::string& prefix, std::string& out)
{
    if (entries.empty()) return false;
    build_rom_index();

    std::unordered_map<int, int> matches;
    for (const RomKey& key : entries)
    {
        auto it = g_romIndex.find(key);
        if (it == g_romIndex.end()) continue;
        for (int idx : it->second) matches[idx]++;
    }
    fbneo_trace("choose entries=%u drivers=%u matches=%u (index=%u)",
                (unsigned)entries.size(), (unsigned)RmgkDriverCount(),
                (unsigned)matches.size(), (unsigned)g_romIndex.size());
    // Shared blob/BIOS data can match dozens of sets; that is not a game.
    if (matches.empty() || matches.size() > 64) return false;

    int  best      = -1;
    long bestScore = -1;
    for (const auto& kv : matches)
    {
        const int idx = kv.first;
        const std::string name = driver_name_at(idx);
        long score = (long)kv.second * 1000;
        if (!driver_is_clone_at(idx)) score += 100;
        if (!prefix.empty() && name.compare(0, prefix.size(), prefix) == 0) score += 10000;
        if (!stem.empty() && basename_lower(name) == stem) score += 100000;
        if (score > bestScore || (score == bestScore && (best < 0 || idx < best)))
        {
            best = idx;
            bestScore = score;
        }
    }
    if (best < 0) return false;
    out = driver_name_at(best);
    fbneo_trace("chosen %s (score=%ld)", out.c_str(), bestScore);
    return !out.empty();
}

bool resolve_driver_by_content(const std::string& path, std::string& out)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) { fbneo_trace("not a regular file: %s", path.c_str()); return false; }

    const std::string name = basename_lower(path);
    std::string stem = name;
    const std::string::size_type dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.erase(dot);

    const std::string prefix = folder_prefix(path);

    if (is_archive_path(path))
    {
        // Canonical arcade set: the stem *is* the driver name.
        if (!stem.empty())
        {
            const int idx = find_driver_index(stem);
            if (idx >= 0)
            {
                out = driver_name_at(idx);
                if (!out.empty()) return true;
            }
        }

        const std::string openName = archive_open_name(path);
        std::lock_guard<std::mutex> zipLock(g_zipMutex);
        if (ZipOpen(const_cast<char*>(openName.c_str())) != 0) { fbneo_trace("zip open failed: %s", openName.c_str()); return false; }

        struct ZipEntry* list = nullptr;
        INT32 count = 0;
        if (ZipGetList(&list, &count) != 0)
        {
            ZipClose();
            return false;
        }

        std::vector<RomKey> entries;
        entries.reserve((size_t)(count > 0 ? count : 0));
        for (INT32 e = 0; e < count; e++)
        {
            if (list[e].nLen == 0 || list[e].nCrc == 0) continue;
            const RomKey key = { list[e].nCrc, list[e].nLen };
            entries.push_back(key);
        }

        fbneo_trace("zip %s entries=%d prefix=%s", name.c_str(), (int)entries.size(), prefix.c_str());

        bool matched = choose_driver(entries, stem, prefix, out);

        // Packed Mega Drive sets store a large part of the library as .smd: the
        // archive CRC is of the interleaved image, so no key in the driver table
        // ever matches. Decode those entries and retry with the resulting body,
        // which is the canonical .bin the driver was built against.
        if (!matched)
        {
            std::vector<RomKey> decoded;
            for (INT32 e = 0; e < count; e++)
            {
                const std::string entryName = basename_lower(list[e].szName ? list[e].szName : "");
                if (!has_ext(entryName, ".smd")) continue;
                if (list[e].nLen == 0 || list[e].nLen > ((16u << 20) + 512)) continue;

                std::vector<uint8_t> body(list[e].nLen);
                INT32 got = 0;
                if (ZipLoadFile(body.data(), (INT32)body.size(), &got, e) != 0 ||
                    got != (INT32)body.size())
                {
                    continue;
                }
                smd_deinterleave(body);
                if (body.size() < 0x4000) continue;

                const RomKey key = { crc32_update(0, body.data(), body.size()), (uint32_t)body.size() };
                if (std::find(entries.begin(), entries.end(), key) == entries.end())
                    decoded.push_back(key);
            }

            for (INT32 e = 0; e < count; e++) free(list[e].szName);
            free(list);
            ZipClose();

            if (decoded.empty()) { fbneo_trace("smd decode: no usable body"); return false; }
            entries.insert(entries.end(), decoded.begin(), decoded.end());
            fbneo_trace("smd decode: %u body key(s) added", (unsigned)decoded.size());
            return choose_driver(entries, stem, prefix, out);
        }

        for (INT32 e = 0; e < count; e++) free(list[e].szName);
        free(list);
        ZipClose();
        return true;
    }

    // Loose ROM file: only extensions that actually hold console ROM data are
    // considered, so disc images (.bin/.cue/.chd, hundreds of MB) are never read
    // just to be rejected.
    static const char* const kRomExts[] = {
        ".nes", ".unf", ".fds", ".smc", ".sfc", ".fig", ".swc",
        ".md", ".gen", ".smd", ".32x", ".sms", ".gg", ".sg", ".sgx",
        ".col", ".rom", ".mx1", ".mx2", ".pce", ".ngp", ".ngc", ".gba",
        ".tzx", ".tap", ".z80", ".sna", ".chf", ".a26", ".bin",
    };
    bool allowed = false;
    for (const char* e : kRomExts)
    {
        const std::string ext(e);
        if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
        {
            allowed = true;
            break;
        }
    }
    if (!allowed) { fbneo_trace("extension not probeable: %s", name.c_str()); return false; }

    std::error_code sec;
    const auto fileSize = std::filesystem::file_size(path, sec);
    if (sec || fileSize == 0 || fileSize > (64ull << 20)) return false;

    uint32_t crc = 0, len = 0;
    if (!crc32_file(path, crc, len) || len == 0 || len > (64u << 20)) return false;
    fbneo_trace("raw %s crc=%08x len=%u prefix=%s", name.c_str(), crc, len, prefix.c_str());

    std::vector<RomKey> entries;
    entries.push_back({ crc, len });

    // Interleaved Mega Drive dump: identify it by the decoded body instead.
    if (has_ext(name, ".smd"))
    {
        std::vector<uint8_t> body;
        if (read_file_bytes(path, 64ull << 20, body))
        {
            smd_deinterleave(body);
            if (body.size() >= 0x4000)
            {
                const RomKey key = { crc32_update(0, body.data(), body.size()), (uint32_t)body.size() };
                if (std::find(entries.begin(), entries.end(), key) == entries.end())
                    entries.push_back(key);
                fbneo_trace("smd %s body crc=%08x len=%u", name.c_str(), key.crc, key.len);
            }
        }
    }

    // SNES copier dumps carry a 512-byte header the driver does not expect;
    // retry without it so such a dump still resolves.
    const bool snesDump = (name.size() >= 4) &&
        (name.compare(name.size() - 4, 4, ".smc") == 0 || name.compare(name.size() - 4, 4, ".sfc") == 0);
    if (snesDump && (len % 1024) == 512)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (f)
        {
            if (fseek(f, 512, SEEK_SET) == 0)
            {
                uint8_t buf[64 * 1024];
                size_t got;
                uint32_t bodyCrc = 0, bodyLen = 0;
                while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
                {
                    bodyCrc = crc32_update(bodyCrc, buf, got);
                    bodyLen += (uint32_t)got;
                }
                if (bodyLen > 0) entries.push_back({ bodyCrc, bodyLen });
            }
            fclose(f);
        }
    }

    return choose_driver(entries, stem, prefix, out);
}
} // namespace

bool ResolveDriver(const char* path, std::string& shortName, std::string& fullName)
{
    shortName.clear();
    fullName.clear();
    if (!path || !*path) return false;

    std::lock_guard<std::recursive_mutex> lock(g_driverNameMutex);

    fbneo_trace("resolve: %s", path);

    if (!ensure_lib_init()) { fbneo_trace("lib init failed"); return false; }

    // Cache key carries size + mtime so re-copying or fixing a ROM at the same
    // path is noticed without restarting the emulator.
    std::error_code ec;
    const auto size  = std::filesystem::file_size(path, ec);
    const auto mtime = std::filesystem::last_write_time(path, ec);
    std::string key = path;
    key += '|';
    key += std::to_string((unsigned long long)size);
    key += '|';
    key += std::to_string((long long)mtime.time_since_epoch().count());

    {
        std::lock_guard<std::mutex> cacheLock(g_resolveMutex);
        auto it = g_resolveCache.find(key);
        if (it != g_resolveCache.end())
        {
            fbneo_trace("resolve cache hit: %s", it->second.empty() ? "(none)" : it->second.c_str());
            if (it->second.empty()) return false;
            shortName = it->second;
            return driver_lookup(shortName.c_str(), &fullName);
        }
    }

    std::string driver;
    const bool found = resolve_driver_by_content(path, driver) && !driver.empty();
    fbneo_trace("resolve %s -> %s", path, found ? driver.c_str() : "(none)");

    {
        std::lock_guard<std::mutex> cacheLock(g_resolveMutex);
        if (g_resolveCache.size() > 200000) g_resolveCache.clear();
        g_resolveCache[key] = found ? driver : std::string();
    }

    if (!found) return false;

    shortName = driver;
    if (!driver_lookup(shortName.c_str(), &fullName)) fullName = driver;
    return true;
}

void Shutdown()
{
    if (g_driverInited)
    {
        BurnDrvExit();
        g_driverInited = false;
    }
    g_paused.store(false, std::memory_order_relaxed);
    g_testPulseFrames.store(0, std::memory_order_relaxed);
    g_testPressed.store(false, std::memory_order_relaxed);
    BurnExtLoadRom = nullptr;
    pBurnDraw      = nullptr;
    pBurnSoundOut  = nullptr;
    g_videoFrame   = {};
    g_videoBuffer.clear();
    g_displayBuffer.clear();
    g_nativeBpp  = 4;
    g_fullWidth  = 0;
    g_fullHeight = 0;
    g_soundBuffer.clear();
    {
        std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
        g_playerInputMap.clear();
        g_testInputTargets.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_localInputMutex);
        g_localMasks.clear();
    }
    g_latchedInput.clear();
    g_archives.clear();
    g_archiveFiles.clear();
    g_rawFiles.clear();
    g_dirFiles.clear();
    // BurnLibExit() is intentionally NOT called here: BurnLibInit() calls it
    // internally, so a per-session exit would double-free the driver-name tables
    // on the next game (see ensure_lib_init()). The coarse tables stay alive for
    // the process and are released once via atexit.
}

int PlayerCount()       { return g_players; }
int InputBytesPerPlayer(){ return g_inputSize; }

void SetLocalInput(int playerIndex, uint32_t mask)
{
    std::lock_guard<std::mutex> lock(g_localInputMutex);
    if (playerIndex >= 0 && playerIndex < (int)g_localMasks.size())
        g_localMasks[playerIndex] = mask;
}

uint32_t GetLocalInput(int playerIndex)
{
    std::lock_guard<std::mutex> lock(g_localInputMutex);
    if (playerIndex >= 0 && playerIndex < (int)g_localMasks.size())
        return g_localMasks[playerIndex];
    return 0;
}

int GetPlayerInputCount(int playerIndex)
{
    std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
    if (playerIndex < 0 || playerIndex >= (int)g_playerInputMap.size())
        return 0;
    return (int)g_playerInputMap[playerIndex].names.size();
}

const char* GetPlayerInputName(int playerIndex, int bit)
{
    std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
    if (playerIndex < 0 || playerIndex >= (int)g_playerInputMap.size())
        return nullptr;
    const auto& names = g_playerInputMap[playerIndex].names;
    if (bit < 0 || bit >= (int)names.size())
        return nullptr;
    return names[bit].c_str();
}

bool DriverInputNames(const char* gameName, std::vector<std::vector<std::string> >& outPerPlayer)
{
    outPerPlayer.clear();
    if (!gameName || !*gameName)
        return false;

    // Only meaningful outside a live session; the runtime's own input map is
    // authoritative while a driver is running.
    if (g_driverInited)
        return false;

    if (!ensure_lib_init())
    {
        return false;
    }

    const int idx = find_driver_index(gameName);
    if (idx < 0)
        return false;

    const int maxPlayers = 4;
    outPerPlayer.assign((size_t)maxPlayers, std::vector<std::string>());

    UINT32 oldActive = nBurnDrvActive;
    nBurnDrvActive = (UINT32)idx;

    // Same enumeration the runtime uses, so the names shown here (including the
    // combination macros) are exactly the ones the frontend can bind.
    std::vector<std::vector<DriverInputEntry> > entries;
    enumerate_input_entries(maxPlayers, entries);
    for (size_t p = 0; p < entries.size() && p < outPerPlayer.size(); p++)
    {
        for (size_t e = 0; e < entries[p].size(); e++)
        {
            outPerPlayer[p].push_back(entries[p][e].name);
        }
    }

    nBurnDrvActive = oldActive;

    return true;
}

void SetPaused(bool paused)
{
    g_paused.store(paused, std::memory_order_relaxed);
}

bool IsPaused()
{
    return g_paused.load(std::memory_order_relaxed);
}

int GetDipGroupCount()
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    std::vector<DipGroupRef> groups;
    if (!dip_enumerate(groups)) return 0;
    return (int)groups.size();
}

bool GetDipGroup(int group, DipGroupInfo& out)
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    std::vector<DipGroupRef> groups;
    if (!dip_enumerate(groups)) return false;
    if (group < 0 || group >= (int)groups.size()) return false;
    const DipGroupRef& g = groups[group];
    int nDIPOffset = dip_get_offset();
    out.name          = g.name;
    out.settingCount  = (int)g.settings.size();
    out.selectedSetting = -1;
    for (int s = 0; s < (int)g.settings.size(); s++)
    {
        if (dip_check_setting(g.settings[s].firstLine, nDIPOffset))
        {
            out.selectedSetting = s;
            break;
        }
    }
    return true;
}

bool GetDipSettingName(int group, int setting, std::string& outName)
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    std::vector<DipGroupRef> groups;
    if (!dip_enumerate(groups)) return false;
    if (group < 0 || group >= (int)groups.size()) return false;
    const DipGroupRef& g = groups[group];
    if (setting < 0 || setting >= (int)g.settings.size()) return false;
    const std::string& settingName = g.settings[setting].name;
    if (!g.name.empty())
        outName = g.name + ": " + settingName;
    else
        outName = settingName;
    return true;
}

bool SetDipSelection(int group, int setting)
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    std::vector<DipGroupRef> groups;
    if (!dip_enumerate(groups)) return false;
    if (group < 0 || group >= (int)groups.size()) return false;
    const DipGroupRef& g = groups[group];
    if (setting < 0 || setting >= (int)g.settings.size()) return false;
    int nDIPOffset = dip_get_offset();
    std::vector<std::pair<int, uint8_t> > written;
    if (!dip_select_setting(g.settings[setting].firstLine, nDIPOffset, &written))
        return false;
    // Remember the written bytes so they survive a driver reset.
    if (!written.empty())
    {
        std::lock_guard<std::mutex> dlock(g_dipMutex);
        for (size_t k = 0; k < written.size(); k++)
        {
            bool merged = false;
            for (size_t m = 0; m < g_dipOverrides.size(); m++)
            {
                if (g_dipOverrides[m].first == written[k].first)
                {
                    g_dipOverrides[m].second = written[k].second;
                    merged = true;
                    break;
                }
            }
            if (!merged) g_dipOverrides.push_back(written[k]);
        }
    }
    return true;
}

void ResetDipsToDefaults()
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    {
        std::lock_guard<std::mutex> dlock(g_dipMutex);
        g_dipOverrides.clear();
    }
    apply_dip_defaults();
}

void SetDipOverrides(const std::vector<std::pair<int, uint8_t> >& overrides)
{
    std::lock_guard<std::mutex> lock(g_dipMutex);
    g_dipOverrides = overrides;
}

void GetDipOverrides(std::vector<std::pair<int, uint8_t> >& out)
{
    std::lock_guard<std::mutex> lock(g_dipMutex);
    out = g_dipOverrides;
}

bool SaveStateInto(unsigned char* buffer, int capacity, int& outLen, int& outChecksum, int /*frame*/)
{
    if (!g_driverInited || !buffer || capacity <= 0) return false;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    UINT8* def = nullptr;
    INT32  len = 0;
    if (BurnStateCompress(&def, &len, 1) != 0 || !def || len <= 0)
    {
        if (def) free(def);
        CoreSetError("FBNeo rollback state save failed: BurnStateCompress returned no data");
        return false;
    }
    if (len > capacity)
    {
        std::ostringstream stream;
        stream << "FBNeo rollback state save failed: driver state is " << len
               << " bytes but the rollback buffer holds " << capacity;
        CoreSetError(stream.str());
        fbneo_trace("%s", stream.str().c_str());
        free(def);
        return false;
    }
    memcpy(buffer, def, (size_t)len);
    outLen      = len;
    outChecksum = (int)fnv1a(def, (size_t)len);
    free(def);
    return true;
}

// Length of the state blob for the running driver, without copying it anywhere.
// Used to size the rollback ring before the session creates it: GekkoNet
// allocates every per-frame state buffer once at gekko_start(), so an
// under-sized ring can only be detected afterwards (the first save fails).
int StateBytes()
{
    if (!g_driverInited) return 0;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    UINT8* def = nullptr;
    INT32  len = 0;
    if (BurnStateCompress(&def, &len, 1) != 0 || !def || len <= 0)
    {
        if (def) free(def);
        return 0;
    }
    free(def);
    return (int)len;
}

bool SaveStateAlloc(unsigned char*& outBuffer, int& outLen, int& outChecksum, int /*frame*/)
{
    if (!g_driverInited) return false;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    UINT8* def = nullptr;
    INT32  len = 0;
    if (BurnStateCompress(&def, &len, 1) != 0 || !def || len <= 0)
    {
        if (def) free(def);
        return false;
    }
    outBuffer  = (unsigned char*)malloc((size_t)len);
    if (!outBuffer) { free(def); return false; }
    memcpy(outBuffer, def, (size_t)len);
    free(def);
    outLen      = len;
    outChecksum = (int)fnv1a(outBuffer, (size_t)len);
    return true;
}

bool LoadState(const unsigned char* buffer, int len)
{
    if (!g_driverInited || !buffer || len <= 0) return false;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    // The state blob now carries the complete sound-chip state (CPU, Z80, YM2151
    // and MSM6295 render phase included), so a rollback load restores the audio
    // timeline together with the game state. No separate sound shadow is needed.
    return BurnStateDecompress((UINT8*)buffer, len, 1) == 0;
}

// TEMPORARY debug aid: dumps the current visible frame to a PPM (P6) when
// RMGK_FBNEO_DUMP_FRAME=<path> is set, once, a few frames in. Used to verify
// the emulated video output without a display.
static void debug_maybe_dump_frame()
{
    static int s_counter = 0;
    static int s_last = -1;
    static bool s_done = false;
    static bool s_parsed = false;
    static std::vector<int> s_at;
    if (s_done) return;
    const char* path = std::getenv("RMGK_FBNEO_DUMP_FRAME");
    if (!path || !*path) return;
    if (!s_parsed)
    {
        s_parsed = true;
        const char* list = std::getenv("RMGK_FBNEO_DUMP_AT");
        if (list && *list)
        {
            for (const char* p = list; *p; )
            {
                const int n = std::atoi(p);
                if (n > 0) s_at.push_back(n);
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
        if (s_at.empty()) { s_at.push_back(120); s_at.push_back(600); s_at.push_back(1200); }
        s_last = *std::max_element(s_at.begin(), s_at.end());
    }
    ++s_counter;
    if (s_counter >= s_last) s_done = true;
    if (std::find(s_at.begin(), s_at.end(), s_counter) == s_at.end()) return;

    char pathBuf[1024];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s.%d", path, s_counter);
    FILE* f = fopen(pathBuf, "wb");
    if (!f) return;
    const int w = g_videoFrame.width, h = g_videoFrame.height;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
    {
        const uint8_t* row = g_videoFrame.pixels + (size_t)y * g_videoFrame.pitch;
        for (int x = 0; x < w; x++)
        {
            const uint8_t b = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t r = row[x * 4 + 2];
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
    fprintf(stderr, "[FBNEO-DBG] dumped frame %dx%d to %s\n", w, h, pathBuf); fflush(stderr);
}

bool RunFrame(int flags)
{
    if (!g_driverInited) return false;
    std::lock_guard<std::mutex> lock(g_frameMutex);

    if (g_pendingReset.exchange(false, std::memory_order_relaxed))
    {
        if (!apply_pending_reset())
        {
            return false;
        }
    }

    // Paused: keep the presenter fed with the last rendered frame and silence,
    // but don't step the driver. The reset above still runs so Reset-while-
    // paused works. The rollback/pacing loop keeps calling us, so the window
    // never goes black and timing stays stable.
    if (g_paused.load(std::memory_order_relaxed))
    {
        if (flags & Output_Audio) {
            const size_t bytes = (size_t)(nBurnSoundLen > 0 ? nBurnSoundLen : 0) * 2 * sizeof(int16_t);
            memset(g_soundBuffer.data(), 0, std::min(bytes, g_soundBuffer.size() * sizeof(int16_t)));
        }
        return true;
    }

    const bool audioOutput = (flags & Output_Audio) != 0;

    apply_latched_input();

    // The previous frame may have switched resolution (Mega Drive interlace
    // 320x448, 256x224 mode, CPS3); follow the new geometry before drawing.
    refresh_video_geometry(false);

    // Video, like audio, is part of the emulated state and not just an output.
    // Several drivers derive *saved* values inside their draw routines: the
    // CPS-2 renderer, for instance, advances the sprite Z buffer and its
    // offsets (nZOffset / nMaxZValue / nMaxZMask) and recomputes the effective
    // palette from CpsDraw(), and all of that is in the CPS-2 savestate. A
    // rollback resim runs with Output_None, so leaving pBurnDraw null on
    // speculative frames made the state a peer saved for frame N depend on
    // whether *that* peer had rolled back into N: GekkoNet then reports a
    // checksum desync against the peer that ran N normally (reproduced with
    // ssf2t/CPS-2 -- a single-frame desync right after a rollback, with
    // identical audio and no visible divergence). Render every frame into the
    // framebuffer, exactly like pBurnSoundOut below; only frames with
    // Output_Video reach the presenter, and the next visible frame repaints
    // the whole buffer.
    pBurnDraw = g_videoBuffer.data();

    // Audio is part of the emulated state, not just an output: the sound DSPs
    // only advance when pBurnSoundOut is set (e.g. qs.cpp does
    // `if (pBurnSoundOut) QscUpdate(...)` in QsndEndFrame). Leaving it null on
    // speculative rollback frames makes the sound-chip state depend on each
    // peer's rollback history, which shows up as a GekkoNet checksum desync and
    // as divergent/repeated audio. Render into the scratch buffer on every
    // frame; only audible frames hand it to the presenter.
    pBurnSoundOut = g_soundBuffer.data();

    // The sound cores accumulate into pBurnSoundOut instead of overwriting it,
    // so the buffer has to be zeroed before each frame -- otherwise the samples
    // pile up frame after frame and eventually overflow. (Every other FBNeo
    // frontend does this; omitting it made replayed frames diverge.)
    {
        const size_t bytes = (size_t)(nBurnSoundLen > 0 ? nBurnSoundLen : 0) * 2 * sizeof(int16_t);
        memset(g_soundBuffer.data(), 0, std::min(bytes, g_soundBuffer.size() * sizeof(int16_t)));
    }

    auto t0 = clock_::now();
    BurnDrvFrame();

    // Drivers rendering RGB565 (Mega Drive/32X, CPS3, Cave, GBA) left their
    // native pixels in g_videoBuffer; the presenter only speaks XRGB8888.
    if (flags & Output_Video)
        convert_native_to_display();

    // Spend one frame of a frontend test-switch tap. Doing this on the emulation
    // side (instead of with a QTimer) guarantees the driver actually samples the
    // switch: a tap armed while the session was paused is still delivered, frame
    // by frame, once the session runs again.
    if (g_testPulseFrames.load(std::memory_order_relaxed) > 0)
    {
        if (g_testPulseFrames.fetch_sub(1, std::memory_order_relaxed) <= 1)
        {
            g_testPulseFrames.store(0, std::memory_order_relaxed);
            g_testPressed.store(false, std::memory_order_relaxed);
        }
    }
    // One diagnostic line per audible frame: lets a captured netplay session be
    // compared frame-by-frame against the peer's log (RMS mismatch == the audio
    // stream diverged even when the visible game state still matched).
    if (audioOutput)
    {
        const int n = (nBurnSoundLen > 0 ? nBurnSoundLen : 0) * 2;
        double sq = 0.0;
        const int16_t* pcm = g_soundBuffer.data();
        for (int i = 0; i < n; i++) { const double v = (double)pcm[i]; sq += v * v; }
        const int rms = n > 0 ? (int)(std::sqrt(sq / (double)n) + 0.5) : 0;
        rmgkSndLog("snd_f f=" + std::to_string(rmgkSndFrameIndex) + " rms=" +
                   std::to_string(rms));
        rmgkSndFrameIndex++;
    }
    g_stats = {};
    g_stats.total_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        clock_::now() - t0).count();
    return true;
}

bool Execute(const m64p_rollback_execute_callbacks& callbacks)
{
    if (!g_driverInited) return false;
    for (;;)
    {
        int r = callbacks.begin_frame ? callbacks.begin_frame(callbacks.user_data) : 0;
        if (r != 1) break;
        rmgk_gekko_trace_loop_mark(3);
        RunFrame(Output_Video | Output_Audio);
        rmgk_gekko_trace_loop_mark(4);
        debug_maybe_dump_frame();
        // Pace immediately before the present, not before the emulated frame.
        // The pacer's deadline is a presentation deadline: sleeping before
        // RunFrame makes the swap land one frame_time late, which pushes it
        // past the display vblank and costs a full refresh of latency.
        if (callbacks.pace_before_present)
            callbacks.pace_before_present(callbacks.user_data);
        if (g_presentCallback)
        {
            rmgk_gekko_trace_loop_mark(5);
            g_presentCallback(g_videoFrame.pixels, g_videoFrame.width, g_videoFrame.height,
                              g_videoFrame.pitch, g_orientationFlags,
                              g_soundBuffer.data(), g_soundFrames, g_presentUser);
            rmgk_gekko_trace_loop_mark(6);
        }
        if (callbacks.visible_frame_complete)
            callbacks.visible_frame_complete(callbacks.user_data, &g_stats);
        if (callbacks.end_frame && callbacks.end_frame(callbacks.user_data) == 0)
            return false;
    }
    return true;
}

bool SampleInput(void* values, int size, int players)
{
    if (!values || size != g_inputSize) return false;
    uint8_t* v = (uint8_t*)values;
    for (int i = 0; i < players; i++)
    {
        uint32_t m = 0;
        {
            std::lock_guard<std::mutex> lock(g_localInputMutex);
            m = (i < (int)g_localMasks.size()) ? g_localMasks[i] : 0;
        }
        memcpy(v + (size_t)i * size, &m, (size_t)size);
    }
    return true;
}

void SetInputCallback(int (*callback)(void* values, int size, int players))
{
    // A rollback/netplay session drives input through the callback: drop any
    // test switch the local hotkey was holding so the press cannot leak into
    // the synchronized stream.
    g_testPulseFrames.store(0, std::memory_order_relaxed);
    g_testPressed.store(false, std::memory_order_relaxed);
    g_testAllowLocalRollback.store(false, std::memory_order_relaxed);
    g_inputCallback = callback;
}

void SetTestPressed(bool pressed)
{
    if (pressed)
    {
        // A held press (controller hotkey) keeps the switch down on its own, so
        // it supersedes any pending frame-counted tap.
        g_testPulseFrames.store(0, std::memory_order_relaxed);
        g_testPressed.store(true, std::memory_order_relaxed);
        return;
    }

    // Don't cut a frame-counted tap short: it releases itself once its budget is
    // spent, which may be after a pause that outlasted the click.
    if (g_testPulseFrames.load(std::memory_order_relaxed) == 0)
    {
        g_testPressed.store(false, std::memory_order_relaxed);
    }
}

void SetTestDeliveryAllowed(bool allowed)
{
    g_testAllowLocalRollback.store(allowed, std::memory_order_relaxed);
}

void PulseTestButton(int frames)
{
    const bool allowLocalRollback = g_testAllowLocalRollback.load(std::memory_order_relaxed);
    // Rollback/netplay owns the input stream; an extra unsynchronized press
    // would desync the session. Local sessions also run through that layer but
    // have no peer to desync, so the frontend opts in for them.
    if (g_inputCallback != nullptr && !allowLocalRollback) return;
    if (frames < 1) frames = 1;
    if (frames > 600) frames = 600;

    // Arm the budget before the press so a frame can never observe a switch that
    // is down with no budget left to release it.
    g_testPulseFrames.store(frames, std::memory_order_relaxed);
    g_testPressed.store(true, std::memory_order_relaxed);
}

bool HasTestInput()
{
    std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
    return !g_testInputTargets.empty();
}

void SetInputPlayers(int players)     { g_inputPlayers = players; }

void SetNetgameMode(bool netgame)
{
    // Pins/clears the NeoGeo RTC + BurnRandom seed inside the core. Meaningful
    // only before Initialize(); changing it mid-session has no effect on the
    // already-seeded drivers.
    nRMGKNetgameMode = netgame ? 1 : 0;
}

void SetDeterministic(bool enabled)   { (void)enabled; }
void SetTimesyncScale(double scale)   { (void)scale; }
void SetVerboseStats(bool enabled)    { (void)enabled; }

void SetFrameCallback(FrameCallback callback, void* user)
{
    g_presentCallback = callback;
    g_presentUser     = user;
}

const VideoFrame& GetVideoFrame()     { return g_videoFrame; }

const int16_t* GetSoundBuffer(int& sampleFrames)
{
    sampleFrames = g_soundFrames;
    return g_soundBuffer.data();
}

void GetRunFrameStats(m64p_rollback_run_frame_stats& out) { out = g_stats; }

} // namespace fbneo_rt
