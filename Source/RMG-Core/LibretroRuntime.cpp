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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
libretro_rt::System g_system = libretro_rt::System::Psx;
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

// Hardware-rendering cores (parallel-dc has no software renderer) draw into an
// FBO the frontend supplies and hand the frame back as
// RETRO_HW_FRAME_BUFFER_VALID. The frontend's GL hooks resolve the functions and
// own the output context; we keep the FBO/texture here and read the pixels back
// into the ordinary g_videoBuffer so the existing software-frame presenter and
// the deterministic rollback path are unchanged.
struct HwRenderState
{
    bool                     active       = false;
    bool                     resetDone    = false;
    bool                     framePending = false;
    int                      width        = 0;
    int                      height       = 0;
    uint32_t                 framebuffer  = 0;
    uint32_t                 texture      = 0;
    retro_hw_render_callback callbacks    = {};
};
HwRenderState g_hw;

// Kept outside HwRenderState: the frontend registers them once at startup, but
// g_hw is cleared between games. Storing them here avoids losing the hooks on
// the second launch.
libretro_rt::GlGetProcFn              g_hwGetProc      = nullptr;
libretro_rt::GlGetDefaultFramebufferFn g_hwGetDefaultFb = nullptr;

// GL entry points we need for the FBO readback, resolved through the frontend
// hook (which in turn uses the current QOpenGLContext). No GL headers required.
typedef void          (*GlGenFramebuffersFn)(int, uint32_t*);
typedef void          (*GlBindFramebufferFn)(unsigned, uint32_t);
typedef void          (*GlGenTexturesFn)(int, uint32_t*);
typedef void          (*GlBindTextureFn)(unsigned, uint32_t);
typedef void          (*GlTexImage2DFn)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
typedef void          (*GlTexParameteriFn)(unsigned, unsigned, int);
typedef void          (*GlFramebufferTexture2DFn)(unsigned, unsigned, unsigned, uint32_t, int);
typedef unsigned      (*GlCheckFramebufferStatusFn)(unsigned);
typedef void          (*GlReadPixelsFn)(int, int, int, int, unsigned, unsigned, void*);
typedef void          (*GlViewportFn)(int, int, int, int);
typedef void          (*GlDeleteFramebuffersFn)(int, const uint32_t*);
typedef void          (*GlDeleteTexturesFn)(int, const uint32_t*);

static GlGenFramebuffersFn       p_glGenFramebuffers       = nullptr;
static GlBindFramebufferFn       p_glBindFramebuffer       = nullptr;
static GlGenTexturesFn           p_glGenTextures           = nullptr;
static GlBindTextureFn           p_glBindTexture           = nullptr;
static GlTexImage2DFn            p_glTexImage2D            = nullptr;
static GlTexParameteriFn         p_glTexParameteri         = nullptr;
static GlFramebufferTexture2DFn  p_glFramebufferTexture2D  = nullptr;
static GlCheckFramebufferStatusFn p_glCheckFramebufferStatus = nullptr;
static GlReadPixelsFn            p_glReadPixels            = nullptr;
static GlViewportFn              p_glViewport              = nullptr;
static GlDeleteFramebuffersFn    p_glDeleteFramebuffers    = nullptr;
static GlDeleteTexturesFn        p_glDeleteTextures        = nullptr;

static const unsigned kGlTexture2D          = 0x0DE1;
static const unsigned kGlRgba               = 0x1908;
static const unsigned kGlUnsignedByte       = 0x1401;
static const unsigned kGlTextureMinFilter   = 0x2801;
static const unsigned kGlTextureMagFilter   = 0x2800;
static const unsigned kGlLinear             = 0x2601;
static const unsigned kGlFramebuffer        = 0x8D40;
static const unsigned kGlColorAttachment0   = 0x8CE0;
static const unsigned kGlFramebufferComplete = 0x8CD5;

// Audio: cores may call audio_sample_batch several times per frame.
std::vector<int16_t> g_audio;

double g_fps = 60.0;

m64p_rollback_run_frame_stats g_stats = {};
unsigned int g_frameChecksum = 0;

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
    // Hardware-rendering cores pass a sentinel instead of pixels: the picture is
    // in the FBO we handed them. Defer the readback until the frontend calls
    // ResolveHwFrame(), which runs with the GL context current.
    if (data == RETRO_HW_FRAME_BUFFER_VALID)
    {
        g_hw.framePending = true;
        g_hw.width        = static_cast<int>(width);
        g_hw.height       = static_cast<int>(height);
        return;
    }
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

// Frontend GL hook adapters. The frontend's hook resolves through the current
// QOpenGLContext; we cast to plain function pointers because the libretro header
// is intentionally not exposed to the GUI layer.
static retro_proc_address_t cb_hw_get_proc_address(const char* name)
{
    if (g_hwGetProc == nullptr || name == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<retro_proc_address_t>(g_hwGetProc(name));
}

static uintptr_t cb_hw_get_current_framebuffer(void)
{
    return static_cast<uintptr_t>(g_hw.framebuffer);
}

static bool cb_environment(unsigned cmd, void* data)
{
    switch (cmd)
    {
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
            if (data != nullptr)
            {
                *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGL_CORE;
                return true;
            }
            return false;

        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            if (data == nullptr)
            {
                return false;
            }
            g_hw.callbacks = *static_cast<const retro_hw_render_callback*>(data);
            g_hw.active    = true;
            g_hw.callbacks.get_current_framebuffer = cb_hw_get_current_framebuffer;
            g_hw.callbacks.get_proc_address        = cb_hw_get_proc_address;
            *static_cast<retro_hw_render_callback*>(data) = g_hw.callbacks;
            return true;

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

// State checksum for the rollback path. The save callback runs once per frame
// and the core state is several MB, so the byte-wise FNV above costs more than
// the emulation itself there (a 4.3 MB swanstation state took ~3 ms). This is
// the same 64-bit FNV-1a over eight-byte words with four independent lanes, so
// the multiply latency overlaps and the loop runs at memory speed. GekkoNet
// only ever compares the value with a peer running the same build, so the
// digest does not have to match any historical value - it only has to be
// deterministic for identical bytes.
uint32_t fnv1a64_lanes(const unsigned char* data, size_t len)
{
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t h0 = 1469598103934665603ull;
    uint64_t h1 = 1469598103934665603ull;
    uint64_t h2 = 1469598103934665603ull;
    uint64_t h3 = 1469598103934665603ull;

    size_t i = 0;
    for (; i + 32 <= len; i += 32)
    {
        uint64_t w0 = 0;
        uint64_t w1 = 0;
        uint64_t w2 = 0;
        uint64_t w3 = 0;
        std::memcpy(&w0, data + i, 8);
        std::memcpy(&w1, data + i + 8, 8);
        std::memcpy(&w2, data + i + 16, 8);
        std::memcpy(&w3, data + i + 24, 8);
        h0 = (h0 ^ w0) * kPrime;
        h1 = (h1 ^ w1) * kPrime;
        h2 = (h2 ^ w2) * kPrime;
        h3 = (h3 ^ w3) * kPrime;
    }
    for (; i + 8 <= len; i += 8)
    {
        uint64_t w = 0;
        std::memcpy(&w, data + i, 8);
        h0 = (h0 ^ w) * kPrime;
    }
    for (; i < len; i++)
    {
        h0 = (h0 ^ data[i]) * kPrime;
    }

    uint64_t hash = h0;
    hash = (hash ^ h1) * kPrime;
    hash = (hash ^ h2) * kPrime;
    hash = (hash ^ h3) * kPrime;
    hash ^= hash >> 32;
    return static_cast<uint32_t>(hash);
}

// Layout of the state swanstation writes (src/core/save_state_version.h). The
// core reports an 11 MB upper bound through retro_serialize_size() but only
// writes the header, the disc display name and the machine data (~4.3 MB for
// Street Fighter EX2), leaving the rest of the buffer untouched.
struct SwanStateHeader
{
    uint32_t magic;
    uint32_t version;
    uint8_t  title[128];
    uint8_t  game_code[32];
    uint32_t media_filename_length;
    uint32_t offset_to_media_filename;
    uint32_t media_subimage_index;
    uint32_t unused_offset_to_playlist_filename;
    uint32_t screenshot_width;
    uint32_t screenshot_height;
    uint32_t screenshot_size;
    uint32_t offset_to_screenshot;
    uint32_t data_compression_type;
    uint32_t data_compressed_size;
    uint32_t data_uncompressed_size;
    uint32_t offset_to_data;
};
static_assert(sizeof(SwanStateHeader) == 216, "swanstation SAVE_STATE_HEADER layout changed");

constexpr uint32_t kSwanStateMagic       = 0x43435544u;              // "DUCC"
constexpr uint64_t kSwanDiskTrailerMagic = 0x435349444e415753ull;    // "SWANDISC"

// Number of bytes swanstation actually wrote into `buffer`: the machine data
// plus the disc trailer the libretro layer appends right after it. The state
// buffer itself is reused between saves, so its tail still holds bytes from an
// older, longer state; those bytes are not machine state and must not be
// hashed. Hashing only this prefix is equivalent to zeroing and hashing the
// whole buffer for two peers running the same core build, and it skips both the
// 11 MB memset and ~7 MB of hashing per save. Returns 0 when the buffer is not
// a swanstation state (unknown core or a layout change), in which case the
// caller falls back to the normalized whole-buffer hash.
size_t swanstation_written_bytes(const unsigned char* buffer, size_t size)
{
    if (size < sizeof(SwanStateHeader))
    {
        return 0;
    }

    SwanStateHeader header;
    std::memcpy(&header, buffer, sizeof(header));
    if (header.magic != kSwanStateMagic || header.data_compression_type != 0)
    {
        return 0;
    }

    const uint64_t data_end =
        static_cast<uint64_t>(header.offset_to_data) + static_cast<uint64_t>(header.data_uncompressed_size);
    if (data_end < sizeof(SwanStateHeader) || data_end > size)
    {
        return 0;
    }

    size_t written = static_cast<size_t>(data_end);

    // DiskStateHeader: magic(8) version(4) flags(4) index(4) path_length(4).
    constexpr size_t kDiskHeaderSize = 24;
    if (size - written >= kDiskHeaderSize)
    {
        uint64_t trailer_magic = 0;
        std::memcpy(&trailer_magic, buffer + written, sizeof(trailer_magic));
        if (trailer_magic == kSwanDiskTrailerMagic)
        {
            uint32_t path_length = 0;
            std::memcpy(&path_length, buffer + written + 20, sizeof(path_length));
            if (path_length <= size - written - kDiskHeaderSize)
            {
                written += kDiskHeaderSize + path_length;
            }
        }
    }

    return written;
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

    // Do not leak this core's nominal rate into a later FBNeo/N64 session in
    // the same process; those paths either publish their own rate or use 60.
#ifdef _WIN32
    _putenv_s("RMGK_ROLLBACK_PRESENT_HZ_EFFECTIVE", "");
#else
    unsetenv("RMGK_ROLLBACK_PRESENT_HZ_EFFECTIVE");
#endif

    g_audio.clear();
    g_videoBuffer.clear();
    g_videoFrame = libretro_rt::VideoFrame();
    if (g_hw.framebuffer != 0 && p_glDeleteFramebuffers != nullptr)
    {
        p_glDeleteFramebuffers(1, &g_hw.framebuffer);
    }
    if (g_hw.texture != 0 && p_glDeleteTextures != nullptr)
    {
        p_glDeleteTextures(1, &g_hw.texture);
    }
    g_hw = HwRenderState();
    g_localInput.clear();
    g_appliedInput.clear();
    g_coreOptions.clear();
    close_library();
}

// LRPS2 discovers BIOS images from <systemDir>/pcsx2/bios and, without an
// explicit option, uses whichever one the folder happens to enumerate first.
// A synchronized session copies that folder from each peer's regular system
// directory, so pin the option to one deterministic filename. The bytes are
// still checked by the state checksum on frame 0: two peers with the same
// filename but different BIOS contents will desync immediately instead of
// silently booting different machines.
static std::string ps2_pinned_bios_name(const char* systemDir)
{
    if (systemDir == nullptr || systemDir[0] == '\0')
    {
        return std::string();
    }

    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::path(systemDir) / "pcsx2" / "bios";
    if (!std::filesystem::is_directory(dir, ec))
    {
        return std::string();
    }

    std::string best;
    for (std::filesystem::directory_iterator it(dir, ec), end;
         !ec && it != end; it.increment(ec))
    {
        const std::filesystem::path& from = it->path();
        if (!std::filesystem::is_regular_file(from, ec))
        {
            ec.clear();
            continue;
        }

        std::string name = from.filename().string();
        std::string lower(name);
        for (char& c : lower)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        if (lower.size() < 5 ||
            (lower.compare(lower.size() - 4, 4, ".bin") != 0 &&
             lower.compare(lower.size() - 4, 4, ".rom") != 0))
        {
            continue;
        }
        if (best.empty() || name < best)
        {
            best = name;
        }
    }
    return best;
}

} // namespace

namespace libretro_rt
{

bool Initialize(const char* corePath, const char* gamePath, const char* systemDir,
                const char* saveDir, int players, System system)
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
    g_system      = system;
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
    g_hw          = HwRenderState();
    g_geometryWidth  = 0;
    g_geometryHeight = 0;
    g_stats = {};
    g_gameId = compute_game_id(gamePath);
    g_logEnabled = std::getenv("RMGK_PSX_LOG") != nullptr;

    clear_core_log();

    g_localInput.assign(static_cast<size_t>(g_players) * static_cast<size_t>(g_inputSize), 0);
    g_appliedInput.assign(static_cast<size_t>(g_players) * static_cast<size_t>(g_inputSize), 0);

    // Pin the machine-local parts of the core before it boots. Overrides are
    // ignored by cores that don't declare the key, so each family only reacts
    // to its own options.
    if (g_system == System::Psx)
    {
    // SwanStation (DuckStation) defaults to a hardware renderer, which this
    // embedding runtime never provides: the frame has to stay CPU-side so
    // rollback can snapshot and replay it. Its worker threads would also make
    // frame production depend on host timing.
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
    }
    else if (g_system == System::Ps2)
    {
        // LRPS2 (parallel-ps2) is driven through the same lockstep path as
        // parallel-dc. Pin everything that can make one host boot a different
        // machine from another; the options are ignored if a core build does
        // not declare them.
        //
        // Our libretro host only exposes an OpenGL FBO to hardware-rendering
        // cores. LRPS2 defaults to Vulkan in its own README, but its "Auto"
        // setting asks the frontend first -- forcing OpenGL here keeps the
        // picture on the CPU-visible FBO the rollback presenter reads back.
        // The OpenGL renderer calls into the GPU driver from LRPS2's own GS
        // thread, where our on-demand QOpenGLContext is not current; NVIDIA's
        // ICD then dereferences a null context and crashes. The software GS
        // renderer has no GL dependency and feeds our CPU-visible frame path
        // directly, so it is both crash-free and deterministic for lockstep.
        SetOptionOverride("pcsx2_renderer", "Software (SW)");
        SetOptionOverride("pcsx2_uncapped_framerate_hint", "disabled");
        SetOptionOverride("pcsx2_game_enhancements_hint", "disabled");
        SetOptionOverride("pcsx2_enable_cheats", "disabled");
        SetOptionOverride("pcsx2_use_external_gameindex", "disabled");
        SetOptionOverride("pcsx2_usb_camera", "disabled");
        SetOptionOverride("pcsx2_dev9_hdd", "disabled");
        SetOptionOverride("pcsx2_dev9_eth", "disabled");
        SetOptionOverride("pcsx2_mtvu", "disabled");
        SetOptionOverride("pcsx2_instant_vu1", "enabled");
        SetOptionOverride("pcsx2_ee_cpu", "Recompiler");
        SetOptionOverride("pcsx2_ee_cycle_rate", "100% (Normal Speed)");
        SetOptionOverride("pcsx2_ee_cycle_skip", "disabled");
        SetOptionOverride("pcsx2_ee_accurate_fpu", "enabled");

        if (g_memoryCardDisabled)
        {
            // A synchronized session must not open the player's PS2 memory
            // cards, HDD image or cheat files. Emulation.cpp points saveDir at
            // an isolated per-session directory; disabling the shared cards
            // removes them from the emulated machine and from the state too.
            SetOptionOverride("pcsx2_shared_memory_cards", "disabled");

            // PS2 has no HLE fallback: same as the naomi.zip problem, both
            // peers must boot an identical BIOS. Pick one filename from the
            // isolated folder deterministically; the frame-0 state checksum
            // still catches peers that supplied different bytes for it.
            const std::string bios = ps2_pinned_bios_name(g_systemDir.c_str());
            if (!bios.empty())
            {
                SetOptionOverride("pcsx2_bios", bios.c_str());
            }
        }
    }
    else
    {
        // parallel-dc: keep the machine identical on both peers. Its core
        // options are auto-detected from the host in some cases (DIV matching,
        // SH4 timing, ARM7 recompiler), so pin the deterministic choices.
        // "auto" system detection is left alone: the core identifies NAOMI and
        // Atomiswave from the romset contents, which is more reliable than the
        // host guessing from a file name.
        SetOptionOverride("reicast_gdrom_fast_loading", "disabled");
        SetOptionOverride("reicast_frame_skipping", "disabled");
        SetOptionOverride("reicast_div_matching", "disabled");
        SetOptionOverride("reicast_sh4_timing", "legacy");
        SetOptionOverride("reicast_allow_service_buttons", "enabled");
        // VMUs are part of the serialized state. A synchronized session must
        // not use the player's VMU files; Emulation.cpp points saveDir at an
        // isolated per-session directory for the same reason.
        SetOptionOverride("reicast_per_content_vmus", "disabled");
        if (g_netgameMode || g_memoryCardDisabled)
        {
            SetOptionOverride("reicast_threaded_rendering", "disabled");
            // The ARM7 interpreter is bit-identical across hosts; the
            // recompiler's emitted code can differ between CPU families.
            SetOptionOverride("reicast_arm7_recompiler", "disabled");
        }
        if (g_memoryCardDisabled)
        {
            // Network session only: offline keeps the user's normal BIOS setup.
            //
            // parallel-dc defaults to use_real_bios=enabled, and the isolated
            // system directory is seeded from each peer's regular BIOS folder.
            // A host that has dc_boot.bin/naomi.zip therefore boots a real BIOS
            // while a host without them boots HLE/reios: different machines,
            // different initial state and audio, desync on frame 0 before any
            // input. Force HLE for Dreamcast and NAOMI so both peers derive the
            // boot state from the game header, not from files that happen to
            // exist on each host.
            //
            // NAOMI also needs a fixed region: "Default" becomes 3, matching no
            // NAOMI BIOS blob, so the core used to pick one by zip order.
            // Atomiswave is left untouched: the core requires awbios.zip even
            // with real BIOS disabled, so it keeps its own BIOS setup.
            if (g_system == System::Dreamcast || g_system == System::Naomi)
            {
                SetOptionOverride("reicast_use_real_bios", "disabled");
            }
            if (g_system == System::Naomi)
            {
                SetOptionOverride("reicast_region", "Japan");
            }
        }
    }

    // The DC core declares JOYPAD subclasses (twinstick, ASCII stick). Forcing
    // the base pad keeps every widget of the 12-byte packet addressable through
    // the same RETRO_DEVICE_ID_JOYPAD_* bits the input dialog binds. PS2 keeps
    // the default analog endpoint so the DualShock axes in the same 12-byte
    // packet reach LRPS2 through RETRO_DEVICE_ANALOG.
    if (g_system == System::Dreamcast || g_system == System::Naomi ||
        g_system == System::Atomiswave)
    {
        g_portDevice         = RETRO_DEVICE_JOYPAD;
        g_portDeviceExplicit = true;
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

    // The rollback pacer defaults to 60 Hz. Publish the core's nominal rate so
    // the libretro path uses the same effective-HZ contract as Mupen; without
    // this, a 59.94 Hz PS1 title drifts against the presentation grid.
    // An explicit RMGK_ROLLBACK_PRESENT_HZ still wins in rmgk_gekko.cpp.
    char presentHz[64];
    std::snprintf(presentHz, sizeof(presentHz), "%.12f", g_fps);
#ifdef _WIN32
    _putenv_s("RMGK_ROLLBACK_PRESENT_HZ_EFFECTIVE", presentHz);
#else
    setenv("RMGK_ROLLBACK_PRESENT_HZ_EFFECTIVE", presentHz, 1);
#endif
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

// True when `hint` appears as a whole path component/word. "dc" inside a
// filename like "luckydice.chd" must not route a PS1 disc to the DC core.
static bool path_has_system_hint(const std::string& lower, const char* hint)
{
    const size_t hintLen = std::strlen(hint);
    size_t pos = 0;
    while ((pos = lower.find(hint, pos)) != std::string::npos)
    {
        const bool leftOk = pos == 0 || lower[pos - 1] == '/' || lower[pos - 1] == '\\' ||
                            lower[pos - 1] == ' ' || lower[pos - 1] == '-' || lower[pos - 1] == '_';
        const size_t end = pos + hintLen;
        const bool rightOk = end == lower.size() || lower[end] == '/' || lower[end] == '\\' ||
                             lower[end] == ' ' || lower[end] == '-' || lower[end] == '_' ||
                             lower[end] == '.';
        if (leftOk && rightOk)
        {
            return true;
        }
        pos = end;
    }
    return false;
}

bool IsMediaFile(const char* path, System* detected)
{
    if (detected != nullptr)
    {
        *detected = System::Psx;
    }
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
    const bool naomiHint = path_has_system_hint(lower, "naomi") || path_has_system_hint(lower, "naomi2");
    const bool atomHint  = path_has_system_hint(lower, "atomiswave") || path_has_system_hint(lower, "atom");
    const bool ps2Hint   = path_has_system_hint(lower, "ps2") ||
                           path_has_system_hint(lower, "psx2") ||
                           path_has_system_hint(lower, "playstation2") ||
                           path_has_system_hint(lower, "playstation 2");
    const bool dcHint    = path_has_system_hint(lower, "dreamcast") || path_has_system_hint(lower, "dc") ||
                           naomiHint || atomHint;
    const System hintedSystem = ps2Hint ? System::Ps2
                               : atomHint ? System::Atomiswave
                               : (naomiHint ? System::Naomi : System::Dreamcast);

    // Formats only parallel-dc understands. .elf is also bootable by LRPS2,
    // so an explicit PS2 hint wins over the Dreamcast default.
    if (ext == ".gdi" || ext == ".cdi")
    {
        if (detected != nullptr) *detected = System::Dreamcast;
        return true;
    }
    if (ext == ".elf")
    {
        if (detected != nullptr) *detected = ps2Hint ? System::Ps2 : System::Dreamcast;
        return true;
    }
    if (ext == ".lst" || ext == ".dat")
    {
        if (detected != nullptr) *detected = atomHint ? System::Atomiswave : System::Naomi;
        return true;
    }

    // Archives and bare NAOMI ROM binaries are shared with arcade consoles;
    // FBNeo has already had first refusal, so only claim them when the path
    // itself identifies a DC-family system.
    if (ext == ".zip" || ext == ".7z" || ext == ".rar" || (ext == ".bin" && dcHint && !naomiHint && !atomHint))
    {
        if (!dcHint)
        {
            return false;
        }
        if (detected != nullptr) *detected = hintedSystem;
        return true;
    }

    // A bare .bin is only treated as a disc when a sibling .cue names it;
    // otherwise every random binary file would route to a libretro core.
    if (ext == ".bin")
    {
        std::string cue = lower.substr(0, dot) + ".cue";
        FILE* file = std::fopen(cue.c_str(), "rb");
        if (file != nullptr)
        {
            if (detected != nullptr) *detected = (dcHint || ps2Hint) ? hintedSystem : System::Psx;
            std::fclose(file);
            return true;
        }
        return false;
    }

    if (ext == ".cue" || ext == ".chd" || ext == ".m3u" ||
        ext == ".ccd" || ext == ".mds" || ext == ".toc")
    {
        if (detected != nullptr) *detected = (dcHint || ps2Hint) ? hintedSystem : System::Psx;
        return true;
    }

    // ISO/CSO are shared by PS1 and PS2; only claim them when the path names
    // the PS2, otherwise leave them to the PS1 side (or the generic browser).
    if ((ext == ".iso" || ext == ".cso") && ps2Hint)
    {
        if (detected != nullptr) *detected = System::Ps2;
        return true;
    }

    // PS1-only disc formats.
    if (ext == ".pbp")
    {
        return true;
    }

    return false;
}

System CurrentSystem()
{
    return g_system;
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
        CoreSetError("LibretroRuntime: core state " + std::to_string(size) +
            "B does not fit the rollback buffer " + std::to_string(capacity) + "B");
        return false;
    }

    // retro_serialize_size() is an upper bound, not the length the core writes:
    // swanstation reports 11 MB and fills only the first ~4.3 MB. Unknown cores
    // can leave the tail uninitialised (SaveStateAlloc hands us malloc'd
    // memory), so those keep the old normalisation: zero the whole buffer
    // before serialising, then hash everything. SwanStation is trimmed below,
    // which is what the PS1 rollback path uses once per frame.
    const bool swanStation =
        g_coreName.find("SwanStation") != std::string::npos ||
        g_corePath.find("swanstation") != std::string::npos;
    if (!swanStation)
    {
        std::memset(buffer, 0, size);
    }

    if (!p_retro_serialize(buffer, size))
    {
        CoreSetError("LibretroRuntime: retro_serialize failed");
        return false;
    }

    // outLen stays the full size: it is the length handed back to LoadState().
    outLen = static_cast<int>(size);

    size_t hashed = size;
    if (swanStation)
    {
        const size_t written = swanstation_written_bytes(buffer, size);
        if (written >= sizeof(SwanStateHeader))
        {
            hashed = written;
        }
        else
        {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true))
            {
                libretro_log("rollback state has an unexpected swanstation layout; "
                             "hashing the whole %zu-byte buffer", size);
            }
        }
    }

    outChecksum = static_cast<int>(fnv1a64_lanes(buffer, hashed));
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

    // Hidden frames from GekkoNet re-simulation pass CoreFrameOutput_None.
    // Their audio is discarded by the next visible reset_audio(), so hashing it
    // only wastes CPU inside the 16.7 ms frame budget. The healthcheck digest
    // is consumed by visible_frame_complete(), which always follows a visible
    // RunFrame(Output_Video | Output_Audio).
    const bool visibleAudio = (flags & Output_Audio) != 0;
    if (visibleAudio)
    {
        reset_audio();
    }

    const auto begin = clock_::now();
    p_retro_run();
    g_stats = {};
    g_stats.total_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock_::now() - begin).count());
    if (visibleAudio)
    {
        g_frameChecksum = fnv1a64_lanes(
            reinterpret_cast<const unsigned char*>(g_audio.data()),
            g_audio.size() * sizeof(int16_t));
    }
    else
    {
        g_frameChecksum = 0;
    }

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

    // A synchronized session must not derive the emulated Dreamcast RTC from
    // the host clock: the local time zone/DST is written to flash, used for the
    // console ID when the flash is empty, and copied into HLE BIOS RAM. Peers in
    // different time zones then boot different machines and content that reads
    // the clock later in a match (KOF98) produces different audio. parallel-dc
    // reads this variable in GetRTC_now(); offline sessions get "0" and keep
    // the core's original RTC behavior.
#ifdef _WIN32
    _putenv_s("RMGK_DETERMINISTIC_RTC", disabled ? "1" : "0");
#else
    setenv("RMGK_DETERMINISTIC_RTC", disabled ? "1" : "0", 1);
#endif
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

void SetGlHooks(GlGetProcFn getProc, GlGetDefaultFramebufferFn getDefaultFb)
{
    g_hwGetProc      = getProc;
    g_hwGetDefaultFb = getDefaultFb;
}

static bool hw_resolve_procs()
{
    if (g_hwGetProc == nullptr)
    {
        return false;
    }
    auto resolve = [](const char* name) -> void*
    {
        return reinterpret_cast<void*>(g_hwGetProc(name));
    };
    p_glGenFramebuffers        = reinterpret_cast<GlGenFramebuffersFn>(resolve("glGenFramebuffers"));
    p_glBindFramebuffer        = reinterpret_cast<GlBindFramebufferFn>(resolve("glBindFramebuffer"));
    p_glGenTextures            = reinterpret_cast<GlGenTexturesFn>(resolve("glGenTextures"));
    p_glBindTexture            = reinterpret_cast<GlBindTextureFn>(resolve("glBindTexture"));
    p_glTexImage2D             = reinterpret_cast<GlTexImage2DFn>(resolve("glTexImage2D"));
    p_glTexParameteri          = reinterpret_cast<GlTexParameteriFn>(resolve("glTexParameteri"));
    p_glFramebufferTexture2D   = reinterpret_cast<GlFramebufferTexture2DFn>(resolve("glFramebufferTexture2D"));
    p_glCheckFramebufferStatus = reinterpret_cast<GlCheckFramebufferStatusFn>(resolve("glCheckFramebufferStatus"));
    p_glReadPixels             = reinterpret_cast<GlReadPixelsFn>(resolve("glReadPixels"));
    p_glViewport               = reinterpret_cast<GlViewportFn>(resolve("glViewport"));
    p_glDeleteFramebuffers     = reinterpret_cast<GlDeleteFramebuffersFn>(resolve("glDeleteFramebuffers"));
    p_glDeleteTextures         = reinterpret_cast<GlDeleteTexturesFn>(resolve("glDeleteTextures"));
    return p_glGenFramebuffers != nullptr && p_glBindFramebuffer != nullptr &&
           p_glGenTextures != nullptr && p_glTexImage2D != nullptr &&
           p_glFramebufferTexture2D != nullptr && p_glReadPixels != nullptr;
}

void ResolveHwFrame()
{
    if (!g_hw.active)
    {
        return;
    }

    // The core resolves its whole GL function table in context_reset(). Some
    // cores (parallel-dc) boot in 2D and only create the 3D renderer seconds
    // later, before handing us the first RETRO_HW_FRAME_BUFFER_VALID frame.
    // Initialize the context even with no frame pending, otherwise that first
    // renderer calls a null glGenBuffers.
    if (!hw_resolve_procs())
    {
        libretro_log("libretro HW frame: frontend did not supply usable GL function pointers");
        return;
    }

    const int w = g_hw.width > 0 ? g_hw.width : g_geometryWidth;
    const int h = g_hw.height > 0 ? g_hw.height : g_geometryHeight;
    if (w <= 0 || h <= 0)
    {
        return;
    }

    if (g_hw.framebuffer == 0 || g_hw.texture == 0 || w != g_videoFrame.width || h != g_videoFrame.height)
    {
        if (g_hw.framebuffer != 0)
        {
            p_glDeleteFramebuffers(1, &g_hw.framebuffer);
            g_hw.framebuffer = 0;
        }
        if (g_hw.texture != 0)
        {
            p_glDeleteTextures(1, &g_hw.texture);
            g_hw.texture = 0;
        }
        p_glGenTextures(1, &g_hw.texture);
        p_glBindTexture(kGlTexture2D, g_hw.texture);
        p_glTexParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<int>(kGlLinear));
        p_glTexParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<int>(kGlLinear));
        p_glTexImage2D(kGlTexture2D, 0, static_cast<int>(kGlRgba), w, h, 0,
                       kGlRgba, kGlUnsignedByte, nullptr);
        p_glGenFramebuffers(1, &g_hw.framebuffer);
        p_glBindFramebuffer(kGlFramebuffer, g_hw.framebuffer);
        p_glFramebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, kGlTexture2D, g_hw.texture, 0);
        p_glBindFramebuffer(kGlFramebuffer, 0);

        g_videoBuffer.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
        g_videoFrame.width  = w;
        g_videoFrame.height = h;
        g_videoFrame.pitch  = w * 4;
        g_videoFrame.bpp    = 4;
        g_videoFrame.pixels = reinterpret_cast<const uint8_t*>(g_videoBuffer.data());
    }

    if (!g_hw.resetDone && g_hw.callbacks.context_reset != nullptr)
    {
        g_hw.callbacks.context_reset();
        g_hw.resetDone = true;
    }

    if (!g_hw.framePending)
    {
        return;
    }
    g_hw.framePending = false;

    p_glBindFramebuffer(kGlFramebuffer, g_hw.framebuffer);
    if (p_glCheckFramebufferStatus != nullptr &&
        p_glCheckFramebufferStatus(kGlFramebuffer) != kGlFramebufferComplete)
    {
        libretro_log("libretro HW frame: incomplete framebuffer");
        p_glBindFramebuffer(kGlFramebuffer, 0);
        return;
    }
    p_glViewport(0, 0, w, h);
    // GL's first row is the bottom; the presenter expects top-down rows.
    std::vector<uint8_t> readback(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    p_glReadPixels(0, 0, w, h, kGlRgba, kGlUnsignedByte, readback.data());
    p_glBindFramebuffer(kGlFramebuffer, 0);

    // glReadPixels gives RGBA; g_videoBuffer holds 0x00RRGGBB so its bytes are
    // BGRA, exactly what the GL_BGRA upload in the presenter expects. Flip Y
    // and swap R/B in one pass.
    uint32_t* dst = g_videoBuffer.data();
    for (int y = 0; y < h; y++)
    {
        const uint8_t* src = readback.data() +
            static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) * 4;
        uint32_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int x = 0; x < w; x++)
        {
            const uint8_t* p = src + static_cast<size_t>(x) * 4;
            row[x] = (static_cast<uint32_t>(p[3]) << 24) |
                     (static_cast<uint32_t>(p[0]) << 16) |
                     (static_cast<uint32_t>(p[1]) << 8) |
                     static_cast<uint32_t>(p[2]);
        }
    }
    g_videoFrame.pixels = reinterpret_cast<const uint8_t*>(g_videoBuffer.data());
}

void GetRunFrameStats(m64p_rollback_run_frame_stats& out)
{
    out = g_stats;
}

unsigned int GetFrameChecksum()
{
    return g_frameChecksum;
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
