/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "LibretroRuntime.hpp"
#include "Error.hpp"

#include "libretro.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
using clock_ = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Dynamic core loading
// ---------------------------------------------------------------------------

#ifdef _WIN32
typedef HMODULE lib_handle_t;
static lib_handle_t lib_open(const char* path)         { return LoadLibraryA(path); }
static void*        lib_symbol(lib_handle_t h, const char* name) { return (void*)GetProcAddress(h, name); }
static void         lib_close(lib_handle_t h)          { if (h != nullptr) FreeLibrary(h); }
#else
typedef void* lib_handle_t;
static lib_handle_t lib_open(const char* path)         { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void*        lib_symbol(lib_handle_t h, const char* name) { return dlsym(h, name); }
static void         lib_close(lib_handle_t h)          { if (h != nullptr) dlclose(h); }
#endif

// The entry points a core must export. Anything missing means the file is not
// a libretro core (or is a partial build), and we refuse to load it.
#define LIBRETRO_SYMBOLS(X)                                                   \
    X(retro_set_environment)                                                  \
    X(retro_set_video_refresh)                                                \
    X(retro_set_audio_sample)                                                 \
    X(retro_set_audio_sample_batch)                                           \
    X(retro_set_input_poll)                                                   \
    X(retro_set_input_state)                                                  \
    X(retro_init)                                                             \
    X(retro_deinit)                                                           \
    X(retro_api_version)                                                      \
    X(retro_get_system_info)                                                  \
    X(retro_get_system_av_info)                                               \
    X(retro_set_controller_port_device)                                       \
    X(retro_reset)                                                            \
    X(retro_run)                                                              \
    X(retro_serialize_size)                                                   \
    X(retro_serialize)                                                        \
    X(retro_unserialize)                                                      \
    X(retro_load_game)                                                        \
    X(retro_unload_game)                                                      \
    X(retro_get_region)

#define DECLARE_SYMBOL(name) decltype(&::name) p_##name = nullptr;
LIBRETRO_SYMBOLS(DECLARE_SYMBOL)
#undef DECLARE_SYMBOL

lib_handle_t g_lib = nullptr;
std::string  g_corePath;
std::string  g_coreName;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

std::mutex g_frameMutex;
std::mutex g_inputMutex;

bool        g_initialized    = false;
bool        g_gameLoaded     = false;
bool        g_shutdownFlag   = false;
std::atomic<bool> g_paused{false};
int         g_players        = 2;
int         g_inputSize      = libretro_rt::kInputBytesPerPlayer;
int         g_inputPlayers   = 0;
bool        g_netgameMode    = false;
// Only a session with remote peers needs the memory card removed: on one machine
// both seats share the same card, so keeping it cannot cause a checksum split.
bool        g_memoryCardDisabled = false;
// Controller device announced to the core. JOYPAD is the only base device every
// libretro core understands; Initialize() upgrades it to the best device the
// loaded core actually declares (SwanStation exposes the DualShock as
// RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0) and reports "None" for the bare
// RETRO_DEVICE_ANALOG value).
unsigned    g_portDevice = RETRO_DEVICE_JOYPAD;
bool        g_portDeviceExplicit = false;
std::vector<unsigned> g_declaredControllerDevices;

std::vector<uint8_t> g_localInput;   // g_players * g_inputSize
std::vector<uint8_t> g_appliedInput; // g_players * g_inputSize (this frame)

int (*g_inputCallback)(void*, int, int) = nullptr;

libretro_rt::FrameCallback g_presentCallback = nullptr;
void*                      g_presentUser     = nullptr;

std::string  g_systemDir;
std::string  g_saveDir;
std::string  g_gamePath;
std::string  g_gameId;

enum retro_pixel_format g_pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;

// Video: always converted to XRGB8888 so the arcade presenter can blit it with
// a single GL_BGRA upload (same path FBNeo uses).
libretro_rt::VideoFrame g_videoFrame;
std::vector<uint32_t>     g_videoBuffer;
int g_geometryWidth  = 0;
int g_geometryHeight = 0;

// Audio: cores may call audio_sample_batch several times per frame.
std::vector<int16_t> g_audio;

double g_fps = 60.0;

m64p_rollback_run_frame_stats g_stats = {};

// Core options: defaults captured from the core's declarations plus any
// explicit overrides. A missing key answers false, letting the core use its own
// default -- identical on both peers, which is what rollback needs.
struct CoreOptionValue
{
    std::string value;
    std::string defaultValue;
};
std::map<std::string, CoreOptionValue> g_coreOptions;
std::map<std::string, std::string>     g_optionOverrides;

bool g_logEnabled = false;

// Boot-time core log lines (bounded, oldest kept). Recorded even with the
// verbose log off: a rollback session dumps them at start so the two peers can
// be compared on the things that decide whether they are emulating the same
// machine at all (BIOS region and file, disc image, memory card, core build).
// The buffer keeps the *first* lines and stops growing, because the interesting
// ones (BIOS picked, disc opened) are logged while the content loads -- a ring
// that drops the oldest would throw exactly those away as the session goes on.
std::mutex               g_coreLogMutex;
std::vector<std::string> g_coreLogRing;
constexpr std::size_t    kCoreLogRingMax = 256;

void append_core_log_line(const char* line)
{
    if (line == nullptr || line[0] == '\0')
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_coreLogMutex);
    if (g_coreLogRing.size() >= kCoreLogRingMax)
    {
        return;
    }
    g_coreLogRing.emplace_back(line);
}

void clear_core_log()
{
    std::lock_guard<std::mutex> lock(g_coreLogMutex);
    g_coreLogRing.clear();
}

void libretro_log(const char* fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    append_core_log_line(buffer);

    if (!g_logEnabled)
    {
        return;
    }

    std::fprintf(stderr, "[PSX] %s\n", buffer);
    std::fflush(stderr);
}

void store_core_option(const char* key, const char* value)
{
    if (key == nullptr)
    {
        return;
    }

    CoreOptionValue entry;
    if (value != nullptr)
    {
        entry.defaultValue = value;
        entry.value        = value;
    }

    const auto overrideIt = g_optionOverrides.find(key);
    if (overrideIt != g_optionOverrides.end())
    {
        entry.value = overrideIt->second;
    }

    g_coreOptions[key] = entry;
}

// ---------------------------------------------------------------------------
// libretro callbacks
// ---------------------------------------------------------------------------

static void cb_video_refresh(const void* data, unsigned width, unsigned height, size_t pitch)
{
    if (data == nullptr)
    {
        // Duplicate frame: keep the previous buffer.
        return;
    }
    if (width == 0 || height == 0)
    {
        return;
    }

    if (static_cast<int>(width) != g_videoFrame.width ||
        static_cast<int>(height) != g_videoFrame.height)
    {
        g_videoBuffer.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
        g_videoFrame.width  = static_cast<int>(width);
        g_videoFrame.height = static_cast<int>(height);
        g_videoFrame.pitch  = static_cast<int>(width) * 4;
        g_videoFrame.pixels = reinterpret_cast<const uint8_t*>(g_videoBuffer.data());
    }
    else if (g_videoBuffer.size() != static_cast<size_t>(width) * static_cast<size_t>(height))
    {
        g_videoBuffer.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
        g_videoFrame.pixels = reinterpret_cast<const uint8_t*>(g_videoBuffer.data());
    }

    const size_t rowBytes = static_cast<size_t>(width) * 4;

    switch (g_pixelFormat)
    {
        case RETRO_PIXEL_FORMAT_XRGB8888:
        {
            for (unsigned y = 0; y < height; y++)
            {
                const uint8_t* src = static_cast<const uint8_t*>(data) + y * pitch;
                uint8_t* dst = reinterpret_cast<uint8_t*>(g_videoBuffer.data()) + y * rowBytes;
                std::memcpy(dst, src, rowBytes);
            }
            break;
        }
        case RETRO_PIXEL_FORMAT_RGB565:
        {
            for (unsigned y = 0; y < height; y++)
            {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(
                    static_cast<const uint8_t*>(data) + y * pitch);
                uint32_t* dst = g_videoBuffer.data() + static_cast<size_t>(y) * width;
                for (unsigned x = 0; x < width; x++)
                {
                    const uint16_t c = src[x];
                    const uint32_t r = static_cast<uint32_t>((c >> 11) & 0x1F) * 255u / 31u;
                    const uint32_t g = static_cast<uint32_t>((c >> 5) & 0x3F) * 255u / 63u;
                    const uint32_t b = static_cast<uint32_t>(c & 0x1F) * 255u / 31u;
                    dst[x] = (r << 16) | (g << 8) | b;
                }
            }
            break;
        }
        case RETRO_PIXEL_FORMAT_0RGB1555:
        default:
        {
            for (unsigned y = 0; y < height; y++)
            {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(
                    static_cast<const uint8_t*>(data) + y * pitch);
                uint32_t* dst = g_videoBuffer.data() + static_cast<size_t>(y) * width;
                for (unsigned x = 0; x < width; x++)
                {
                    const uint16_t c = src[x];
                    const uint32_t r = static_cast<uint32_t>((c >> 10) & 0x1F) * 255u / 31u;
                    const uint32_t g = static_cast<uint32_t>((c >> 5) & 0x1F) * 255u / 31u;
                    const uint32_t b = static_cast<uint32_t>(c & 0x1F) * 255u / 31u;
                    dst[x] = (r << 16) | (g << 8) | b;
                }
            }
            break;
        }
    }

    g_videoFrame.pixels = reinterpret_cast<const uint8_t*>(g_videoBuffer.data());
    g_videoFrame.bpp    = 4;
}

static size_t cb_audio_sample_batch(const int16_t* data, size_t frames)
{
    if (data != nullptr && frames > 0)
    {
        g_audio.insert(g_audio.end(), data, data + frames * 2);
    }
    return frames;
}

static void cb_audio_sample(int16_t left, int16_t right)
{
    g_audio.push_back(left);
    g_audio.push_back(right);
}

static void cb_input_poll(void)
{
}

static int16_t read_axis(const uint8_t* packet, int offset)
{
    int16_t value = 0;
    std::memcpy(&value, packet + offset, sizeof(value));
    return value;
}

// Diagnostics: how the core actually queries input (used by the input harness
// to tell a wrong port device from a wrong packet layout).
std::atomic<uint32_t> g_queryJoypad{0};
std::atomic<uint32_t> g_queryJoypadHit{0};
std::atomic<uint32_t> g_queryAnalog{0};
std::atomic<uint32_t> g_queryAnalogHit{0};

static int16_t cb_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port >= static_cast<unsigned>(g_players))
    {
        return 0;
    }

    const uint8_t* packet =
        g_appliedInput.data() + static_cast<size_t>(port) * static_cast<size_t>(g_inputSize);

    uint16_t mask = 0;
    std::memcpy(&mask, packet, sizeof(mask));

    // Cores poll with the base device id, but accept subclass ids too so a
    // DualShock-style endpoint (subclass of ANALOG) never falls into default.
    switch (device & 0xFFu)
    {
        case RETRO_DEVICE_JOYPAD:
        {
            g_queryJoypad.fetch_add(1, std::memory_order_relaxed);
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            {
                if (mask != 0) g_queryJoypadHit.fetch_add(1, std::memory_order_relaxed);
                return static_cast<int16_t>(mask);
            }
            if (id > RETRO_DEVICE_ID_JOYPAD_R3)
            {
                return 0;
            }
            const int16_t value = ((mask >> id) & 1u) != 0 ? 1 : 0;
            if (value != 0) g_queryJoypadHit.fetch_add(1, std::memory_order_relaxed);
            return value;
        }
        case RETRO_DEVICE_ANALOG:
        {
            g_queryAnalog.fetch_add(1, std::memory_order_relaxed);
            const bool right = (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT);
            if (id == RETRO_DEVICE_ID_ANALOG_X)
            {
                const int16_t value = read_axis(packet, right ? 6 : 2);
                if (value != 0) g_queryAnalogHit.fetch_add(1, std::memory_order_relaxed);
                return value;
            }
            if (id == RETRO_DEVICE_ID_ANALOG_Y)
            {
                const int16_t value = read_axis(packet, right ? 8 : 4);
                if (value != 0) g_queryAnalogHit.fetch_add(1, std::memory_order_relaxed);
                return value;
            }
            return 0;
        }
        default:
            return 0;
    }
}

static void cb_log(enum retro_log_level level, const char* fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    append_core_log_line(buffer);

    if (!g_logEnabled && level > RETRO_LOG_WARN)
    {
        return;
    }

    std::fprintf(stderr, "[PSX] %s\n", buffer);
    std::fflush(stderr);
}

static bool cb_environment(unsigned cmd, void* data)
{
    switch (cmd)
    {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            if (data != nullptr)
            {
                *static_cast<bool*>(data) = true;
            }
            return true;

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            if (data == nullptr)
            {
                return false;
            }
            g_pixelFormat = *static_cast<enum retro_pixel_format*>(data);
            return true;

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            if (data == nullptr)
            {
                return false;
            }
            *static_cast<const char**>(data) = g_systemDir.c_str();
            return true;

        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            if (data == nullptr)
            {
                return false;
            }
            *static_cast<const char**>(data) = g_saveDir.c_str();
            return true;

        case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
            if (data == nullptr)
            {
                return false;
            }
            *static_cast<const char**>(data) = nullptr;
            return false;

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            if (data == nullptr)
            {
                return false;
            }
            static_cast<struct retro_log_callback*>(data)->log = cb_log;
            return true;

        case RETRO_ENVIRONMENT_GET_VARIABLE:
        {
            if (data == nullptr)
            {
                return false;
            }
            struct retro_variable* var = static_cast<struct retro_variable*>(data);
            if (var->key == nullptr)
            {
                return false;
            }
            const auto it = g_coreOptions.find(var->key);
            if (g_logEnabled && (std::strstr(var->key, "Renderer") != nullptr ||
                                 std::strstr(var->key, "CDROM_") != nullptr ||
                                 std::strstr(var->key, "MemoryCards_") != nullptr ||
                                 std::strstr(var->key, "Runahead") != nullptr))
            {
                cb_log(RETRO_LOG_INFO, "GetVariable %s -> %s", var->key,
                    it == g_coreOptions.end() ? "<missing>" : it->second.value.c_str());
            }
            if (it == g_coreOptions.end() || it->second.value.empty())
            {
                return false;
            }
            var->value = it->second.value.c_str();
            return true;
        }

        case RETRO_ENVIRONMENT_SET_VARIABLES:
        {
            const struct retro_variable* var = static_cast<const struct retro_variable*>(data);
            for (; var != nullptr && var->key != nullptr; var++)
            {
                // "Description; value1|value2|..." -> first entry is the default.
                const char* value = var->value;
                std::string def;
                if (value != nullptr)
                {
                    const char* semi = std::strchr(value, ';');
                    if (semi != nullptr)
                    {
                        def = semi + 1;
                        while (!def.empty() && def.front() == ' ')
                        {
                            def.erase(def.begin());
                        }
                        const size_t bar = def.find('|');
                        if (bar != std::string::npos)
                        {
                            def.resize(bar);
                        }
                    }
                }
                store_core_option(var->key, def.c_str());
            }
            return true;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            if (data != nullptr)
            {
                *static_cast<bool*>(data) = false;
            }
            return true;

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        {
            const struct retro_core_option_definition* def =
                static_cast<const struct retro_core_option_definition*>(data);
            for (; def != nullptr && def->key != nullptr; def++)
            {
                store_core_option(def->key, def->default_value);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        {
            const struct retro_core_options_intl* intl =
                static_cast<const struct retro_core_options_intl*>(data);
            if (intl != nullptr && intl->us != nullptr)
            {
                const struct retro_core_option_definition* def = intl->us;
                for (; def->key != nullptr; def++)
                {
                    store_core_option(def->key, def->default_value);
                }
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        {
            const struct retro_core_options_v2* opts =
                static_cast<const struct retro_core_options_v2*>(data);
            if (opts != nullptr && opts->definitions != nullptr)
            {
                const struct retro_core_option_v2_definition* def = opts->definitions;
                for (; def->key != nullptr; def++)
                {
                    store_core_option(def->key, def->default_value);
                }
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        {
            const struct retro_core_options_v2_intl* intl =
                static_cast<const struct retro_core_options_v2_intl*>(data);
            if (intl != nullptr && intl->us != nullptr && intl->us->definitions != nullptr)
            {
                const struct retro_core_option_v2_definition* def = intl->us->definitions;
                for (; def->key != nullptr; def++)
                {
                    store_core_option(def->key, def->default_value);
                }
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_VARIABLE:
        {
            const struct retro_variable* var = static_cast<const struct retro_variable*>(data);
            if (var != nullptr && var->key != nullptr)
            {
                store_core_option(var->key, var->value);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            if (data != nullptr)
            {
                *static_cast<unsigned*>(data) = 2;
            }
            return true;

        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;

        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            if (data != nullptr)
            {
                *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
            }
            return true;

        case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
            if (data != nullptr)
            {
                *static_cast<float*>(data) = 60.0f;
            }
            return true;

        case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
            if (data != nullptr)
            {
                *static_cast<unsigned*>(data) = static_cast<unsigned>(g_players);
            }
            return true;

        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        {
            const struct retro_system_av_info* av =
                static_cast<const struct retro_system_av_info*>(data);
            if (av != nullptr && av->timing.fps > 1.0)
            {
                g_fps = av->timing.fps;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_GEOMETRY:
        {
            const struct retro_game_geometry* geo =
                static_cast<const struct retro_game_geometry*>(data);
            if (geo != nullptr && geo->base_width > 0 && geo->base_height > 0)
            {
                g_geometryWidth  = static_cast<int>(geo->base_width);
                g_geometryHeight = static_cast<int>(geo->base_height);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SHUTDOWN:
            g_shutdownFlag = true;
            return true;

        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        {
            // Remember what the first port offers so Initialize() can announce
            // the richest device the core implements instead of guessing.
            g_declaredControllerDevices.clear();
            const struct retro_controller_info* info =
                static_cast<const struct retro_controller_info*>(data);
            for (unsigned port = 0; info != nullptr && port < 8; port++, info++)
            {
                if (info->types == nullptr || info->num_types == 0)
                {
                    break;
                }
                if (port != 0)
                {
                    continue;
                }
                for (unsigned i = 0; i < info->num_types && i < 32; i++)
                {
                    if (info->types[i].desc == nullptr)
                    {
                        break;
                    }
                    g_declaredControllerDevices.push_back(info->types[i].id);
                }
            }
            return true;
        }

        // Accepted without side effects: the core only needs a "yes" so it can
        // continue booting.
        case RETRO_ENVIRONMENT_SET_ROTATION:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_MESSAGE:
        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
        case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
            return true;

        default:
            // Includes SET_HW_RENDER: refusing it makes every core fall back to
            // its software renderer, which is the only mode we can present and
            // the only one that is deterministic across peers.
            return false;
    }
}

// ---------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------

uint32_t fnv1a(const unsigned char* data, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++)
    {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

void reset_audio()
{
    g_audio.clear();
}

void apply_latched_input()
{
    const size_t total = static_cast<size_t>(g_players) * static_cast<size_t>(g_inputSize);
    g_appliedInput.assign(total, 0);

    if (g_inputCallback == nullptr)
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (g_localInput.size() == total)
        {
            g_appliedInput = g_localInput;
        }
        return;
    }

    const int players = g_inputPlayers > 0 ? g_inputPlayers : g_players;
    if (g_inputCallback(g_appliedInput.data(), g_inputSize, players) != 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_inputMutex);
    if (g_localInput.size() == total)
    {
        g_appliedInput = g_localInput;
    }
}

// Cheap, peer-stable content identity: a hash of the descriptor file (the .cue
// text or the first 256 KB of the image) plus its size. Used for logging and by
// the lobby to tell two different discs apart without reading gigabytes.
std::string compute_game_id(const std::string& path)
{
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        return std::string();
    }

    std::vector<unsigned char> head(256u * 1024u);
    const size_t read = std::fread(head.data(), 1, head.size(), file);
    std::fseek(file, 0, SEEK_END);
    const long long size = static_cast<long long>(std::ftell(file));
    std::fclose(file);
    head.resize(read);

    uint32_t hash = fnv1a(head.data(), head.size());
    for (int i = 0; i < 8; i++)
    {
        hash ^= static_cast<uint32_t>((static_cast<unsigned long long>(size) >> (i * 8)) & 0xFFu);
        hash *= 16777619u;
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%08x%08x", hash, static_cast<unsigned>(head.size()));
    return std::string(buffer);
}

void unload_symbols()
{
#define CLEAR_SYMBOL(name) p_##name = nullptr;
    LIBRETRO_SYMBOLS(CLEAR_SYMBOL)
#undef CLEAR_SYMBOL
}

void close_library()
{
    if (g_lib != nullptr)
    {
        lib_close(g_lib);
        g_lib = nullptr;
    }
    unload_symbols();
    g_coreName.clear();
}

// Teardown without taking g_frameMutex, for callers that already hold it
// (Initialize uses it to replace a previous session; std::mutex is not
// recursive).
void shutdown_unlocked()
{
    if (g_initialized && p_retro_unload_game != nullptr && g_gameLoaded)
    {
        p_retro_unload_game();
    }
    if (g_initialized && p_retro_deinit != nullptr)
    {
        p_retro_deinit();
    }

    g_initialized  = false;
    g_gameLoaded   = false;
    g_shutdownFlag = false;
    g_audio.clear();
    g_videoBuffer.clear();
    g_videoFrame = libretro_rt::VideoFrame();
    g_localInput.clear();
    g_appliedInput.clear();
    g_coreOptions.clear();
    close_library();
}

} // namespace

namespace libretro_rt
{

bool Initialize(const char* corePath, const char* gamePath, const char* systemDir,
                const char* saveDir, int players)
{
    std::lock_guard<std::mutex> lock(g_frameMutex);

    if (g_initialized)
    {
        shutdown_unlocked();
    }

    if (corePath == nullptr || corePath[0] == '\0' || gamePath == nullptr || gamePath[0] == '\0')
    {
        CoreSetError("LibretroRuntime: missing core or content path");
        return false;
    }

    g_corePath    = corePath;
    g_gamePath    = gamePath;
    g_systemDir   = systemDir != nullptr ? systemDir : std::string();
    g_saveDir     = saveDir != nullptr ? saveDir : g_systemDir;
    g_players     = std::max(players, 1);
    g_inputPlayers = 0;
    g_inputSize   = kInputBytesPerPlayer;
    g_shutdownFlag = false;
    g_paused.store(false, std::memory_order_relaxed);
    g_fps         = 60.0;
    g_pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
    g_videoFrame  = VideoFrame();
    g_videoBuffer.clear();
    g_geometryWidth  = 0;
    g_geometryHeight = 0;
    g_stats = {};
    g_gameId = compute_game_id(gamePath);
    g_logEnabled = std::getenv("RMGK_PSX_LOG") != nullptr;

    clear_core_log();

    g_localInput.assign(static_cast<size_t>(g_players) * static_cast<size_t>(g_inputSize), 0);
    g_appliedInput.assign(static_cast<size_t>(g_players) * static_cast<size_t>(g_inputSize), 0);

    // Pin the machine-local parts of the core before it boots. SwanStation
    // (DuckStation) defaults to a hardware renderer, which this embedding
    // runtime never provides: the frame has to stay CPU-side so rollback can
    // snapshot and replay it. Its worker threads would also make frame
    // production depend on host timing. Overrides are ignored by cores that
    // don't declare the key, so other libretro cores are unaffected.
    SetOptionOverride("swanstation_GPU_Renderer", "Software");
    if (g_netgameMode)
    {
        // Values below are the ones the core actually declares, and a value it
        // does not know is silently dropped in favour of the default (measured:
        // an unknown option value leaves the core on its default path). The
        // booleans are "true"/"false" and the readahead is a sector count
        // (0..32), NOT the "Disabled (Synchronous)" spelling Beetle PSX uses --
        // the previous strings were invalid, so the async reader and readahead 8
        // stayed in effect during netplay.
        SetOptionOverride("swanstation_GPU_UseThread", "false");
        // The async CD reader delivers sectors on a host thread's schedule, so a
        // seek/read that the game starts on frame N can complete at a different
        // emulated frame on each PC. That difference lands in the CD state, i.e.
        // in the savestate checksum, and shows up as a desync around the first
        // stage load. Pin reads to the emulation thread and drop the readahead
        // buffer so sector delivery is purely cycle-driven and identical on both
        // peers.
        SetOptionOverride("swanstation_CDROM_ReadThread", "false");
        SetOptionOverride("swanstation_CDROM_ReadaheadSectors", "0");
        // GekkoNet drives rollback and run-ahead itself. A second, hidden timeline
        // inside the core would be serialized as part of the state and would make
        // a frame's result depend on when the core last ran ahead.
        SetOptionOverride("swanstation_Main_RunaheadFrameCount", "0");
    }

    if (g_memoryCardDisabled)
    {
        // The memory card image is part of the core's serialized state (measured:
        // flipping one byte of the card changes the savestate, and the 128 KB card
        // is ~131 KB of the blob). With two peers, each PC has its own card file,
        // with its own saves, or no card at all -- so the two machines differ from
        // frame 0 and the very first checksum comparison fails even with identical
        // inputs. Rollback cannot repair that: the divergence is in the starting
        // state, not in a replayed frame.
        //
        // A network session therefore runs with both slots set to "None", which
        // removes the card (and its bytes) from the emulated machine and from the
        // state on both peers. Offline and local play keep the normal card.
        SetOptionOverride("swanstation_MemoryCards_Card1Type", "None");
        SetOptionOverride("swanstation_MemoryCards_Card2Type", "None");
    }

    g_lib = lib_open(corePath);
    if (g_lib == nullptr)
    {
        CoreSetError(std::string("LibretroRuntime: failed to load core: ") + corePath);
        close_library();
        return false;
    }

    bool missing = false;
    std::string missingName;
#define LOAD_SYMBOL(name)                                                      \
    p_##name = reinterpret_cast<decltype(p_##name)>(lib_symbol(g_lib, #name));  \
    if (p_##name == nullptr) { missing = true; missingName = #name; }
    LIBRETRO_SYMBOLS(LOAD_SYMBOL)
#undef LOAD_SYMBOL

    if (missing)
    {
        CoreSetError(std::string("LibretroRuntime: not a libretro core (missing ") + missingName + "): " + corePath);
        close_library();
        return false;
    }

    if (p_retro_api_version() != RETRO_API_VERSION)
    {
        if (g_logEnabled)
        {
            libretro_log("core API version %u differs from vendored header %u",
                p_retro_api_version(), RETRO_API_VERSION);
        }
    }

    struct retro_system_info info = {};
    p_retro_get_system_info(&info);
    g_coreName = info.library_name != nullptr ? info.library_name : "libretro";
    if (info.library_version != nullptr)
    {
        g_coreName += " ";
        g_coreName += info.library_version;
    }

    p_retro_set_environment(cb_environment);

    // Core options are answered from the core's own declared defaults unless a
    // caller pinned one through SetOptionOverride(). Defaults are identical on
    // both peers (same DLL, same content, no per-user config), which is exactly
    // what rollback determinism needs -- so nothing is forced here.
    p_retro_init();
    g_initialized = true;

    struct retro_game_info gameInfo = {};
    gameInfo.path = g_gamePath.c_str();
    gameInfo.data = nullptr;
    gameInfo.size = 0;
    gameInfo.meta = nullptr;

    if (!p_retro_load_game(&gameInfo))
    {
        CoreSetError(std::string("LibretroRuntime: core refused the content: ") + g_gamePath);
        shutdown_unlocked();
        return false;
    }
    g_gameLoaded = true;

    p_retro_set_video_refresh(cb_video_refresh);
    p_retro_set_audio_sample(cb_audio_sample);
    p_retro_set_audio_sample_batch(cb_audio_sample_batch);
    p_retro_set_input_poll(cb_input_poll);
    p_retro_set_input_state(cb_input_state);

    // Pick the endpoint device once, after the core declared its controller
    // types (SwanStation sends SET_CONTROLLER_INFO while loading the content).
    unsigned portDevice = g_portDevice;
    if (!g_portDeviceExplicit)
    {
        const unsigned dualShock = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0);
        bool hasAnalog = false;
        bool hasJoypad = false;
        for (unsigned declared : g_declaredControllerDevices)
        {
            if (declared == dualShock)
            {
                portDevice = dualShock;
                break;
            }
            if (declared == RETRO_DEVICE_ANALOG)
            {
                hasAnalog = true;
            }
            if (declared == RETRO_DEVICE_JOYPAD)
            {
                hasJoypad = true;
            }
        }
        if (portDevice != dualShock)
        {
            if (hasAnalog)
            {
                portDevice = RETRO_DEVICE_ANALOG;
            }
            else if (hasJoypad)
            {
                portDevice = RETRO_DEVICE_JOYPAD;
            }
        }
    }

    for (int port = 0; port < g_players; port++)
    {
        p_retro_set_controller_port_device(static_cast<unsigned>(port), portDevice);
    }

    struct retro_system_av_info av = {};
    p_retro_get_system_av_info(&av);
    if (av.timing.fps > 1.0)
    {
        g_fps = av.timing.fps;
    }
    if (av.geometry.base_width > 0 && av.geometry.base_height > 0 &&
        g_videoFrame.width == 0)
    {
        g_geometryWidth  = static_cast<int>(av.geometry.base_width);
        g_geometryHeight = static_cast<int>(av.geometry.base_height);
        g_videoBuffer.assign(static_cast<size_t>(g_geometryWidth) *
                             static_cast<size_t>(g_geometryHeight), 0u);
        g_videoFrame.width  = g_geometryWidth;
        g_videoFrame.height = g_geometryHeight;
        g_videoFrame.pitch  = g_geometryWidth * 4;
        g_videoFrame.bpp    = 4;
        g_videoFrame.pixels = reinterpret_cast<const uint8_t*>(g_videoBuffer.data());
    }

    if (g_logEnabled)
    {
        libretro_log("core='%s' content='%s' fps=%.3f size=%ux%u id=%s",
            g_coreName.c_str(), g_gamePath.c_str(), g_fps,
            av.geometry.base_width, av.geometry.base_height, g_gameId.c_str());
    }

    return true;
}

bool IsInitialized()
{
    return g_initialized && g_gameLoaded;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    shutdown_unlocked();
}

bool Reset()
{
    std::lock_guard<std::mutex> lock(g_frameMutex);

    if (!IsInitialized() || p_retro_reset == nullptr)
    {
        return false;
    }

    p_retro_reset();
    return true;
}

bool IsMediaFile(const char* path)
{
    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    std::string lower(path);
    for (char& c : lower)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    const size_t dot = lower.find_last_of('.');
    if (dot == std::string::npos)
    {
        return false;
    }

    const std::string ext = lower.substr(dot);
    if (ext == ".cue" || ext == ".chd" || ext == ".pbp" || ext == ".m3u" ||
        ext == ".ccd" || ext == ".mds" || ext == ".toc")
    {
        return true;
    }

    // A bare .bin is only treated as a disc when a sibling .cue names it;
    // otherwise every random binary file would route to the PS1 core.
    if (ext == ".bin")
    {
        std::string cue = lower.substr(0, dot) + ".cue";
        FILE* file = std::fopen(cue.c_str(), "rb");
        if (file != nullptr)
        {
            std::fclose(file);
            return true;
        }
    }

    return false;
}

const char* CoreName()
{
    return g_coreName.c_str();
}

const char* GameId()
{
    return g_gameId.c_str();
}

std::string ContentIdForPath(const char* path)
{
    if (path == nullptr || path[0] == '\0')
    {
        return std::string();
    }

    std::lock_guard<std::mutex> lock(g_frameMutex);
    return compute_game_id(path);
}

int PlayerCount()
{
    return g_players;
}

int InputBytesPerPlayer()
{
    return g_inputSize;
}

void GetInputQueryStats(int& joypad, int& joypadHits, int& analog, int& analogHits)
{
    joypad      = static_cast<int>(g_queryJoypad.load(std::memory_order_relaxed));
    joypadHits  = static_cast<int>(g_queryJoypadHit.load(std::memory_order_relaxed));
    analog      = static_cast<int>(g_queryAnalog.load(std::memory_order_relaxed));
    analogHits  = static_cast<int>(g_queryAnalogHit.load(std::memory_order_relaxed));
}

void ResetInputQueryStats()
{
    g_queryJoypad = 0;
    g_queryJoypadHit = 0;
    g_queryAnalog = 0;
    g_queryAnalogHit = 0;
}

void SetPortDeviceAnalog(bool analog)
{
    g_portDevice = analog ? RETRO_DEVICE_ANALOG : RETRO_DEVICE_JOYPAD;
    g_portDeviceExplicit = true;
}

void SetPortDevice(unsigned device)
{
    g_portDevice = device;
    g_portDeviceExplicit = true;
}

void SetLocalInput(int playerIndex, const uint8_t* input)
{
    if (playerIndex < 0 || playerIndex >= g_players || input == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_inputMutex);
    if (g_localInput.size() <
        (static_cast<size_t>(playerIndex) + 1) * static_cast<size_t>(g_inputSize))
    {
        return;
    }
    std::memcpy(g_localInput.data() + static_cast<size_t>(playerIndex) * static_cast<size_t>(g_inputSize),
        input, static_cast<size_t>(g_inputSize));
}

void GetLocalInput(int playerIndex, uint8_t* out)
{
    if (playerIndex < 0 || playerIndex >= g_players || out == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_inputMutex);
    if (g_localInput.size() <
        (static_cast<size_t>(playerIndex) + 1) * static_cast<size_t>(g_inputSize))
    {
        return;
    }
    std::memcpy(out,
        g_localInput.data() + static_cast<size_t>(playerIndex) * static_cast<size_t>(g_inputSize),
        static_cast<size_t>(g_inputSize));
}

void SetPaused(bool paused)
{
    // Pausing is honoured by RunFrame; the core itself keeps its state.
    g_paused.store(paused, std::memory_order_relaxed);
}

bool IsPaused()
{
    return g_paused.load(std::memory_order_relaxed);
}

bool SaveStateInto(unsigned char* buffer, int capacity, int& outLen, int& outChecksum, int frame)
{
    (void)frame;
    std::lock_guard<std::mutex> lock(g_frameMutex);

    if (!IsInitialized() || buffer == nullptr || p_retro_serialize == nullptr)
    {
        return false;
    }

    const size_t size = p_retro_serialize_size();
    if (size == 0 || size > static_cast<size_t>(capacity))
    {
        CoreSetError("LibretroRuntime: core state does not fit the rollback buffer");
        return false;
    }

    // retro_serialize_size() is an upper bound, not the length the core writes:
    // swanstation reports 11.5 MB and fills only the first ~4.3 MB, and the
    // filled length moves by a few bytes from frame to frame. Nothing promises
    // the rest of the buffer is initialised (SaveStateAlloc hands us malloc'd
    // memory), so hashing the declared size would mix in bytes the emulator
    // never produced -- the same machine state could then checksum differently.
    // Zero the buffer so the unwritten tail is always the same bytes, and hash
    // only the prefix the core filled; skipping the zero run also cuts the
    // per-frame checksum cost by ~60% (the save callback runs every frame).
    std::memset(buffer, 0, size);

    if (!p_retro_serialize(buffer, size))
    {
        CoreSetError("LibretroRuntime: retro_serialize failed");
        return false;
    }

    size_t written = size;
    while (written > 0 && buffer[written - 1] == 0)
    {
        --written;
    }

    // outLen stays the full size: it is the length handed back to LoadState().
    outLen      = static_cast<int>(size);
    outChecksum = static_cast<int>(fnv1a(buffer, written));
    return true;
}

bool SaveStateAlloc(unsigned char*& outBuffer, int& outLen, int& outChecksum, int frame)
{
    outBuffer = nullptr;

    if (!IsInitialized() || p_retro_serialize_size == nullptr)
    {
        return false;
    }

    const size_t size = p_retro_serialize_size();
    if (size == 0)
    {
        return false;
    }

    unsigned char* buffer = static_cast<unsigned char*>(std::malloc(size));
    if (buffer == nullptr)
    {
        return false;
    }

    if (!SaveStateInto(buffer, static_cast<int>(size), outLen, outChecksum, frame))
    {
        std::free(buffer);
        return false;
    }

    outBuffer = buffer;
    return true;
}

bool LoadState(const unsigned char* buffer, int len)
{
    std::lock_guard<std::mutex> lock(g_frameMutex);

    if (!IsInitialized() || buffer == nullptr || len <= 0 || p_retro_unserialize == nullptr)
    {
        return false;
    }

    if (!p_retro_unserialize(buffer, static_cast<size_t>(len)))
    {
        CoreSetError("LibretroRuntime: retro_unserialize failed");
        return false;
    }

    return true;
}

int SerializeSize()
{
    std::lock_guard<std::mutex> lock(g_frameMutex);

    if (!IsInitialized() || p_retro_serialize_size == nullptr)
    {
        return 0;
    }

    return static_cast<int>(p_retro_serialize_size());
}

bool RunFrame(int flags)
{
    std::lock_guard<std::mutex> lock(g_frameMutex);

    if (!IsInitialized() || p_retro_run == nullptr)
    {
        return false;
    }

    // Paused: keep the last frame and output silence without stepping the core.
    if (g_paused.load(std::memory_order_relaxed))
    {
        if ((flags & Output_Audio) != 0)
        {
            g_audio.assign(static_cast<size_t>(735) * 2, 0);
        }
        return true;
    }

    apply_latched_input();
    reset_audio();

    const auto begin = clock_::now();
    p_retro_run();
    g_stats = {};
    g_stats.total_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock_::now() - begin).count());

    if (g_shutdownFlag)
    {
        return false;
    }

    return true;
}

bool Execute(const m64p_rollback_execute_callbacks& callbacks)
{
    if (!IsInitialized())
    {
        return false;
    }

    for (;;)
    {
        const int r = callbacks.begin_frame ? callbacks.begin_frame(callbacks.user_data) : 0;
        if (r != 1)
        {
            break;
        }

        RunFrame(Output_Video | Output_Audio);

        // Pace immediately before the present: the pacer's deadline targets
        // the swap, so waiting before RunFrame would make the present land one
        // frame_time late and miss the display vblank.
        if (callbacks.pace_before_present)
        {
            callbacks.pace_before_present(callbacks.user_data);
        }

        if (g_presentCallback != nullptr)
        {
            g_presentCallback(g_videoFrame.pixels, g_videoFrame.width, g_videoFrame.height,
                g_videoFrame.pitch, 0, g_audio.data(),
                static_cast<int>(g_audio.size() / 2), g_presentUser);
        }

        if (callbacks.visible_frame_complete)
        {
            callbacks.visible_frame_complete(callbacks.user_data, &g_stats);
        }
        if (callbacks.end_frame && callbacks.end_frame(callbacks.user_data) == 0)
        {
            return false;
        }
    }

    return true;
}

bool SampleInput(void* values, int size, int players)
{
    if (values == nullptr || size != g_inputSize)
    {
        return false;
    }

    uint8_t* out = static_cast<uint8_t*>(values);
    std::lock_guard<std::mutex> lock(g_inputMutex);
    for (int i = 0; i < players; i++)
    {
        const uint8_t* src = g_localInput.data() + static_cast<size_t>(i) * static_cast<size_t>(g_inputSize);
        std::memcpy(out + static_cast<size_t>(i) * static_cast<size_t>(size), src, static_cast<size_t>(size));
    }
    return true;
}

void SetInputCallback(int (*callback)(void* values, int size, int players))
{
    g_inputCallback = callback;
}

void SetInputPlayers(int players)
{
    g_inputPlayers = players;
}

void SetNetgameMode(bool netgame)
{
    g_netgameMode = netgame;
}

void SetMemoryCardDisabled(bool disabled)
{
    g_memoryCardDisabled = disabled;
}

const VideoFrame& GetVideoFrame()
{
    return g_videoFrame;
}

const int16_t* GetSoundBuffer(int& sampleFrames)
{
    sampleFrames = static_cast<int>(g_audio.size() / 2);
    return g_audio.data();
}

void SetFrameCallback(FrameCallback callback, void* user)
{
    g_presentCallback = callback;
    g_presentUser     = user;
}

void GetRunFrameStats(m64p_rollback_run_frame_stats& out)
{
    out = g_stats;
}

void SetOptionOverride(const char* key, const char* value)
{
    if (key == nullptr)
    {
        return;
    }
    if (value == nullptr)
    {
        g_optionOverrides.erase(key);
        return;
    }

    g_optionOverrides[key] = value;

    const auto it = g_coreOptions.find(key);
    if (it != g_coreOptions.end())
    {
        it->second.value = value;
    }
}

double FrameRate()
{
    return g_fps;
}

std::string RecentCoreLog()
{
    std::lock_guard<std::mutex> lock(g_coreLogMutex);
    std::string out;
    out.reserve(g_coreLogRing.size() * 64);
    for (const std::string& line : g_coreLogRing)
    {
        out += line;
        out += '\n';
    }
    return out;
}

} // namespace libretro_rt
