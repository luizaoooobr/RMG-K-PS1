/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_LIBRETRO_RUNTIME_HPP
#define CORE_LIBRETRO_RUNTIME_HPP

// Generic libretro embedding runtime.
//
// This is the RMG-K side glue for a third-party libretro core (Beetle PSX /
// Mednafen PSX today, any deterministic core tomorrow). It mirrors the exact
// command surface of FbneoRuntime.hpp so the rollback layer, the GekkoNet
// session and the frontend presenter do not need to know which emulator is
// underneath.
//
// The core is loaded with dlopen/LoadLibrary, never linked: libretro cores are
// separate works (Beetle PSX is GPL-2.0, RMG is GPL-3.0) and the dynamic
// boundary is the same one RetroArch itself uses.
//
// One frame == one retro_run(). The core never owns a thread, so the frontend
// owns the clock -- which is exactly what rollback requires.

#include "m64p/api/m64p_types.h"

#include <cstdint>
#include <string>

namespace libretro_rt
{
    // Frame output flags (matches CoreFrameOutputFlags semantics).
    enum FrameOutputFlags
    {
        Output_None  = 0,
        Output_Video = 1 << 0,
        Output_Audio = 1 << 1,
    };

    // Byte-exact layout of the arcade/libretro frame, so the frontend presenter
    // and the rollback layer can treat both runtimes through one struct.
    struct VideoFrame
    {
        const uint8_t* pixels = nullptr;
        int width  = 0;
        int height = 0;
        int pitch  = 0; // bytes per line
        int bpp    = 0; // bytes per pixel (always 4 -- XRGB8888)
    };

    // Input packet layout, per player, InputBytesPerPlayer() bytes:
    //   [0..1]  uint16 little endian button mask (bit == RETRO_DEVICE_ID_JOYPAD_*)
    //   [2..3]  int16  left stick X  (-32768..32767)
    //   [4..5]  int16  left stick Y
    //   [6..7]  int16  right stick X
    //   [8..9]  int16  right stick Y
    //   [10..11] reserved (must be zero)
    // Keeping the whole pad in the rollback input means both peers replay the
    // analog axes too, not just the digital buttons.
    constexpr int kInputBytesPerPlayer = 12;

    // Lifecycle.
    //
    // corePath   : full path to the libretro core shared library (.dll/.so/.dylib).
    // gamePath   : content to load (.cue/.chd/.pbp/.m3u/.bin/...).
    // systemDir  : directory the core uses for BIOS/system files.
    // saveDir    : directory the core uses for its own save files (memory cards).
    // players    : number of seats whose input packets are maintained.
    bool Initialize(const char* corePath, const char* gamePath, const char* systemDir,
                    const char* saveDir, int players);
    bool IsInitialized();
    void Shutdown();

    // retro_reset(): soft reset of the loaded content.
    bool Reset();

    // True when `path` looks like a disc image this runtime can load.
    bool IsMediaFile(const char* path);

    // Human readable core name ("Sony - PlayStation (Beetle PSX)"...) and the
    // peer-shared content identifier (<= 64 chars, hex). Empty when idle.
    const char* CoreName();
    const char* GameId();

    // Boot-time core log lines (BIOS/region/disc image as the core saw them),
    // captured so a rollback session can record which machine each peer booted.
    std::string RecentCoreLog();

    // Same content id as above for a path that isn't loaded yet (the rom
    // browser tags entries with it so the lobby can match rooms before the
    // disc boots). Empty when the file can't be read.
    std::string ContentIdForPath(const char* path);

    int PlayerCount();
    int InputBytesPerPlayer(); // always kInputBytesPerPlayer

    // Controller port device announced to the core (defaults to analog, i.e.
    // DualShock). Exposed so tools/tests can compare against a digital pad.
    void SetPortDeviceAnalog(bool analog);
    void SetPortDevice(unsigned device);
    void GetInputQueryStats(int& joypad, int& joypadHits, int& analog, int& analogHits);
    void ResetInputQueryStats();

    // Per-player physical input packet (see layout above). Index is 0-based.
    void SetLocalInput(int playerIndex, const uint8_t* input);
    void GetLocalInput(int playerIndex, uint8_t* out);

    // Pause control: keeps the last frame on screen and outputs silence without
    // stepping the core.
    void SetPaused(bool paused);
    bool IsPaused();

    // Round-trip the complete core state through retro_serialize/unserialize.
    bool SaveStateInto(unsigned char* buffer, int capacity, int& outLen, int& outChecksum, int frame);
    bool SaveStateAlloc(unsigned char*& outBuffer, int& outLen, int& outChecksum, int frame);
    bool LoadState(const unsigned char* buffer, int len);

    // Byte size the core reports for one serialized state. Queried right after
    // Initialize() so the GekkoNet session can size its rollback ring.
    int SerializeSize();

    // Run a single emulated frame (applies latched inputs).
    bool RunFrame(int flags);

    // Drive the rollback frame loop, mirroring M64CMD_ROLLBACK_EXECUTE.
    bool Execute(const m64p_rollback_execute_callbacks& callbacks);

    // Sample the current physical input packets into values/players.
    bool SampleInput(void* values, int size, int players);

    // Input callback bookkeeping (set by the rollback layer).
    void SetInputCallback(int (*callback)(void* values, int size, int players));
    void SetInputPlayers(int players);

    // Pin the machine-local values a core may seed (RTC, random) so two peers
    // boot bit-identical. Must be called before Initialize().
    void SetNetgameMode(bool netgame);
    // Drop the core's memory card from the emulated machine. Set this for
    // sessions with remote peers only: the card image is part of the serialized
    // state, and each PC has a different card file, so the two peers would
    // diverge from frame 0. Must be called before Initialize().
    void SetMemoryCardDisabled(bool disabled);

    // Last rendered frame / audio (valid after RunFrame(Output_Video/Output_Audio)).
    const VideoFrame& GetVideoFrame();
    const int16_t* GetSoundBuffer(int& sampleFrames);

    enum VideoOrientationFlags
    {
        kVideoVertical = 1 << 0,
        kVideoFlipped  = 1 << 1,
    };

    // Per-visible-frame presentation callback (same shape as FBNeo's).
    typedef void (*FrameCallback)(
        const void* video, int width, int height, int pitch, int orientationFlags,
        const int16_t* audio, int audioFrames, void* user);

    void SetFrameCallback(FrameCallback callback, void* user);

    void GetRunFrameStats(m64p_rollback_run_frame_stats& out);

    // Core option override, "key=value". Only applied when the loaded core
    // actually declares `key`; unknown keys are ignored. Must be called before
    // Initialize(). Core defaults are deterministic across peers, so this is
    // only needed to force a specific behaviour on both ends.
    void SetOptionOverride(const char* key, const char* value);

    // NR (NTSC) / PAL frame rate reported by the loaded core.
    double FrameRate();
}

#endif // CORE_LIBRETRO_RUNTIME_HPP
