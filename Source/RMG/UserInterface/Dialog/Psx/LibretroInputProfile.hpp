#ifndef LIBRETRO_INPUT_PROFILE_HPP
#define LIBRETRO_INPUT_PROFILE_HPP

#include <RMG-Core/Emulation.hpp>
#include <RMG-Core/Settings.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace UserInterface
{
namespace Dialog
{

// One dialog per machine family driven by the bundled libretro cores.
// NAOMI 1/2 and Atomiswave share a single arcade profile (like Flycast Dojo):
// the cabinet buttons live on the same joypad bits, so there is no reason to
// maintain two dialogs and two saved binding sets. Dreamcast stays separate.
enum class LibretroInputProfileId
{
    Psx        = 0,
    Dreamcast  = 1,
    Arcade     = 2,
    Ps2        = 4,        // dedicated DualShock 2 profile, independent from PS1
    Naomi      = Arcade,   // old call sites keep working
    Atomiswave = Arcade,
};

// A PS2 DualShock has 16 digital buttons plus two analog sticks. Joypad bits
// stay 0..15; negative codes identify analog axis rows in the dialog so the
// sticks can be configured instead of being hardcoded to the first gamepad.
enum LibretroAnalogRowBit
{
    LibretroAxisLeftX  = -1,
    LibretroAxisLeftY  = -2,
    LibretroAxisRightX = -3,
    LibretroAxisRightY = -4,
};

constexpr int LIBRETRO_MAX_PROFILE_ROWS = 20;

inline bool isLibretroAnalogRow(int bit)
{
    return bit >= LibretroAxisRightY && bit <= LibretroAxisLeftX;
}

// Returns 0=LX, 1=LY, 2=RX, 3=RY for an analog row, -1 for a digital button.
inline int libretroAnalogAxisIndex(int bit)
{
    return isLibretroAnalogRow(bit) ? (-bit - 1) : -1;
}

struct LibretroInputProfile
{
    const char* title;
    const char* bindingSection;
    const char* legacyBindingSections[2];
    const char* deviceSectionPrefix;
    const char* legacyDeviceSectionPrefixes[2];
    const char* traceFile;
    int         count;
    const char* names[LIBRETRO_MAX_PROFILE_ROWS];  // dialog legend, in display order
    int         bits[LIBRETRO_MAX_PROFILE_ROWS];   // joypad bit, or LibretroAnalogRowBit
    int         defaults[LIBRETRO_MAX_PROFILE_ROWS]; // SDL3 scancode fallback for P1
    const char* legacyNames[LIBRETRO_MAX_PROFILE_ROWS][6]; // older names of each row
};

inline const LibretroInputProfile& libretroInputProfile(LibretroInputProfileId id)
{
    static const LibretroInputProfile profiles[5] = {
        // PlayStation: keep the original 14-row table verbatim so existing
        // saved bindings and the legacy scancode fallback keep working.
        {
            "PlayStation Input",
            "RMG PS1 Input - Global", { nullptr, nullptr },
            "RMG PS1 Device - P", { nullptr, nullptr },
            "psx_input.log",
            14,
            {
                "PS1 Cross", "PS1 Circle", "PS1 Square", "PS1 Triangle",
                "PS1 L1", "PS1 R1", "PS1 L2", "PS1 R2",
                "PS1 Start", "PS1 Select",
                "PS1 D-Pad Up", "PS1 D-Pad Down", "PS1 D-Pad Left", "PS1 D-Pad Right",
            },
            { 0, 8, 1, 9, 10, 11, 12, 13, 3, 2, 4, 5, 6, 7 },
            {
                SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S,
                SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R,
                SDL_SCANCODE_RETURN, SDL_SCANCODE_RSHIFT,
                SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
            },
            {
                { "PS1 Cross", nullptr, nullptr, nullptr },
                { "PS1 Circle", nullptr, nullptr, nullptr },
                { "PS1 Square", nullptr, nullptr, nullptr },
                { "PS1 Triangle", nullptr, nullptr, nullptr },
                { "PS1 L1", nullptr, nullptr, nullptr },
                { "PS1 R1", nullptr, nullptr, nullptr },
                { "PS1 L2", nullptr, nullptr, nullptr },
                { "PS1 R2", nullptr, nullptr, nullptr },
                { "PS1 Start", nullptr, nullptr, nullptr },
                { "PS1 Select", nullptr, nullptr, nullptr },
                { "PS1 D-Pad Up", nullptr, nullptr, nullptr },
                { "PS1 D-Pad Down", nullptr, nullptr, nullptr },
                { "PS1 D-Pad Left", nullptr, nullptr, nullptr },
                { "PS1 D-Pad Right", nullptr, nullptr, nullptr },
                { nullptr, nullptr, nullptr, nullptr },
                { nullptr, nullptr, nullptr, nullptr },
            },
        },
        // Dreamcast: D-Pad, face buttons (A/B/X/Y), triggers, Start/Select,
        // then extended L2/R2/L3/R3. Bits stay tied to the hardware button.
        {
            "Dreamcast",
            "RMG DC Input - Global", { nullptr, nullptr },
            "RMG DC Device - P", { nullptr, nullptr },
            "dc_input.log",
            16,
            {
                "DC D-Pad Up", "DC D-Pad Down", "DC D-Pad Left", "DC D-Pad Right",
                "DC A - Inferior Esquerdo  (verde)", "DC B - Inferior Direito   (vermelho)",
                "DC X - Superior Esquerdo  (azul)", "DC Y - Superior Direito   (amarelo)",
                "L Trigger - Gatilho esquerdo", "R Trigger - Gatilho direito",
                "DC Start", "DC Select",
                "DC L2", "DC R2", "DC L3", "DC R3",
            },
            { 4, 5, 6, 7, 0, 8, 1, 9, 10, 11, 3, 2, 12, 13, 14, 15 },
            {
                SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S,
                SDL_SCANCODE_Q, SDL_SCANCODE_W,
                SDL_SCANCODE_RETURN, SDL_SCANCODE_RSHIFT,
                SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL,
            },
            {
                { "DC D-Pad Up", nullptr, nullptr, nullptr },
                { "DC D-Pad Down", nullptr, nullptr, nullptr },
                { "DC D-Pad Left", nullptr, nullptr, nullptr },
                { "DC D-Pad Right", nullptr, nullptr, nullptr },
                { "DC A (South, B)", nullptr, nullptr, nullptr },
                { "DC B (East, A)", nullptr, nullptr, nullptr },
                { "DC X (West, Y)", nullptr, nullptr, nullptr },
                { "DC Y (North, X)", nullptr, nullptr, nullptr },
                { "DC L (unused)", nullptr, nullptr, nullptr },
                { "DC R (unused)", nullptr, nullptr, nullptr },
                { "DC Start", nullptr, nullptr, nullptr },
                { "DC Select", "DC Select (unused)", nullptr, nullptr },
                { "DC L2 (unused)", nullptr, nullptr, nullptr },
                { "DC R2 (unused)", nullptr, nullptr, nullptr },
                { "DC L3 (unused)", nullptr, nullptr, nullptr },
                { "DC R3 (unused)", nullptr, nullptr, nullptr },
            },
        },
        // Unified NAOMI 1/2 + Atomiswave arcade pad. The two cores use the
        // same joypad bits for BTN1..BTN5, Coin, Start and TEST/SERVICE; on
        // Atomiswave bits 10/12/13 are not wired by the core, but keeping the
        // NAOMI layout here lets the same binding set drive both machines.
        {
            "Naomi 1 e 2 / Atomiswave",
            "RMG Arcade Input - Global",
            { "RMG NAOMI Input - Global", "RMG Atom Input - Global" },
            "RMG Arcade Device - P",
            { "RMG NAOMI Device - P", "RMG Atom Device - P" },
            "arcade_input.log",
            16,
            {
                "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
                "BTN1 - Soco Fraco / A", "BTN2 - Soco Medio / B",
                "BTN3 - Soco Forte / C", "BTN4 - Chute Fraco / D",
                "BTN5 - Chute Medio / E", "BTN6 - Chute Forte / F",
                "Coin", "Start",
                "BTN7 - Macro de 3 socos / Macro A+B+C",
                "BTN8 - Macros de 3 Chutes / Macro D+E+F",
                "Teste", "Service",
            },
            { 4, 5, 6, 7, 0, 8, 1, 9, 11, 10, 2, 3, 13, 12, 14, 15 },
            {
                SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S,
                SDL_SCANCODE_W, SDL_SCANCODE_Q,
                SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
                SDL_SCANCODE_R, SDL_SCANCODE_E, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL,
            },
            {
                { "D-Pad Up", nullptr, nullptr, nullptr },
                { "D-Pad Down", nullptr, nullptr, nullptr },
                { "D-Pad Left", nullptr, nullptr, nullptr },
                { "D-Pad Right", nullptr, nullptr, nullptr },
                { "BTN1", "BTN1 - Soco Fraco", "BTN1 - A (Soco Fraco)", nullptr },
                { "BTN2", "BTN2 - Soco Medio", "BTN2 - C (Soco Forte)", "BTN2 - Soco Medio / C", nullptr, nullptr },
                { "BTN3", "BTN3 - Soco Forte", "BTN3 - E (Blowback / Tag)", "BTN3 - Soco Forte / E", nullptr, nullptr },
                { "BTN4", "BTN4 - Chute Fraco", "BTN4 - B (Chute Fraco)", "BTN4 - Chute Fraco / B", nullptr, nullptr },
                { "BTN5", "BTN5 - Chute Medio", "BTN5 - D (Chute Forte)", "BTN5 - Chute Medio / D", nullptr, nullptr },
                { "BTN6", "BTN6 - Chute Forte", "BTN6 - Macro A+B", "L (unused)", "BTN6 - Chute Forte / Macro A+B", nullptr },
                { "Coin", nullptr, nullptr, nullptr },
                { "Start", nullptr, nullptr, nullptr },
                { "BTN7", "BTN7 - Macro de 3 socos", "BTN7 - Macro C+D", "R2 (unused)", "BTN7 - Macro de 3 socos / Macro C+D", nullptr },
                { "BTN8", "BTN8 - Macros de 3 Chutes", "BTN8 - Macro C+B", "L2 (unused)", "BTN8 - Macros de 3 Chutes / Macro C+B", nullptr },
                { "Test", "Teste", nullptr, nullptr },
                { "Service", nullptr, nullptr, nullptr },
            },
        },
        // Alias of the unified arcade profile. Kept so code that still asks
        // for "Atomiswave" reaches the exact same bindings and settings.
        {
            "Naomi 1 e 2 / Atomiswave",
            "RMG Arcade Input - Global",
            { "RMG NAOMI Input - Global", "RMG Atom Input - Global" },
            "RMG Arcade Device - P",
            { "RMG NAOMI Device - P", "RMG Atom Device - P" },
            "arcade_input.log",
            16,
            {
                "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
                "BTN1 - Soco Fraco / A", "BTN2 - Soco Medio / B",
                "BTN3 - Soco Forte / C", "BTN4 - Chute Fraco / D",
                "BTN5 - Chute Medio / E", "BTN6 - Chute Forte / F",
                "Coin", "Start",
                "BTN7 - Macro de 3 socos / Macro A+B+C",
                "BTN8 - Macros de 3 Chutes / Macro D+E+F",
                "Teste", "Service",
            },
            { 4, 5, 6, 7, 0, 8, 1, 9, 11, 10, 2, 3, 13, 12, 14, 15 },
            {
                SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S,
                SDL_SCANCODE_W, SDL_SCANCODE_Q,
                SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
                SDL_SCANCODE_R, SDL_SCANCODE_E, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL,
            },
            {
                { "D-Pad Up", nullptr, nullptr, nullptr },
                { "D-Pad Down", nullptr, nullptr, nullptr },
                { "D-Pad Left", nullptr, nullptr, nullptr },
                { "D-Pad Right", nullptr, nullptr, nullptr },
                { "BTN1", "BTN1 - Soco Fraco", "BTN1 - A (Soco Fraco)", nullptr },
                { "BTN2", "BTN2 - Soco Medio", "BTN2 - C (Soco Forte)", "BTN2 - Soco Medio / C", nullptr, nullptr },
                { "BTN3", "BTN3 - Soco Forte", "BTN3 - E (Blowback / Tag)", "BTN3 - Soco Forte / E", nullptr, nullptr },
                { "BTN4", "BTN4 - Chute Fraco", "BTN4 - B (Chute Fraco)", "BTN4 - Chute Fraco / B", nullptr, nullptr },
                { "BTN5", "BTN5 - Chute Medio", "BTN5 - D (Chute Forte)", "BTN5 - Chute Medio / D", nullptr, nullptr },
                { "BTN6", "BTN6 - Chute Forte", "BTN6 - Macro A+B", "L (unused)", "BTN6 - Chute Forte / Macro A+B", nullptr },
                { "Coin", nullptr, nullptr, nullptr },
                { "Start", nullptr, nullptr, nullptr },
                { "BTN7", "BTN7 - Macro de 3 socos", "BTN7 - Macro C+D", "R2 (unused)", "BTN7 - Macro de 3 socos / Macro C+D", nullptr },
                { "BTN8", "BTN8 - Macros de 3 Chutes", "BTN8 - Macro C+B", "L2 (unused)", "BTN8 - Macros de 3 Chutes / Macro C+B", nullptr },
                { "Test", "Teste", nullptr, nullptr },
                { "Service", nullptr, nullptr, nullptr },
            },
        },
        // PlayStation 2: dedicated DualShock 2 layout with its own saved
        // section, so PS1 bindings are never reused by a PS2 game. The 16
        // digital rows cover every joypad button (including L3/R3); the last
        // four rows map the analog sticks to the 12-byte packet's axes.
        {
            "PlayStation 2 Input",
            "RMG PS2 Input - Global", { nullptr, nullptr },
            "RMG PS2 Device - P", { nullptr, nullptr },
            "ps2_input.log",
            20,
            {
                "PS2 D-Pad Up", "PS2 D-Pad Down", "PS2 D-Pad Left", "PS2 D-Pad Right",
                "PS2 Cross", "PS2 Circle", "PS2 Square", "PS2 Triangle",
                "PS2 L1", "PS2 R1", "PS2 L2", "PS2 R2",
                "PS2 Select", "PS2 Start", "PS2 L3", "PS2 R3",
                "PS2 Analog Left X", "PS2 Analog Left Y",
                "PS2 Analog Right X", "PS2 Analog Right Y",
            },
            { 4, 5, 6, 7, 0, 8, 1, 9, 10, 11, 12, 13, 2, 3, 14, 15,
              LibretroAxisLeftX, LibretroAxisLeftY, LibretroAxisRightX, LibretroAxisRightY },
            {
                SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S,
                SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R,
                SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL,
                SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
            },
            {
                { "PS2 D-Pad Up", nullptr, nullptr, nullptr },
                { "PS2 D-Pad Down", nullptr, nullptr, nullptr },
                { "PS2 D-Pad Left", nullptr, nullptr, nullptr },
                { "PS2 D-Pad Right", nullptr, nullptr, nullptr },
                { "PS2 Cross", nullptr, nullptr, nullptr },
                { "PS2 Circle", nullptr, nullptr, nullptr },
                { "PS2 Square", nullptr, nullptr, nullptr },
                { "PS2 Triangle", nullptr, nullptr, nullptr },
                { "PS2 L1", nullptr, nullptr, nullptr },
                { "PS2 R1", nullptr, nullptr, nullptr },
                { "PS2 L2", nullptr, nullptr, nullptr },
                { "PS2 R2", nullptr, nullptr, nullptr },
                { "PS2 Select", nullptr, nullptr, nullptr },
                { "PS2 Start", nullptr, nullptr, nullptr },
                { "PS2 L3", nullptr, nullptr, nullptr },
                { "PS2 R3", nullptr, nullptr, nullptr },
                { "PS2 Analog Left X", nullptr, nullptr, nullptr },
                { "PS2 Analog Left Y", nullptr, nullptr, nullptr },
                { "PS2 Analog Right X", nullptr, nullptr, nullptr },
                { "PS2 Analog Right Y", nullptr, nullptr, nullptr },
            },
        },
    };
    const int index = static_cast<int>(id);
    return profiles[index >= 0 && index < 5 ? index : 0];
}

// Loads the binding lists of the current section and, for the arcade profile,
// of both legacy sections. The first occurrence of a name wins, so a binding
// written by the unified dialog always beats an older NAOMI/Atomiswave one.
inline void loadProfileBindingSettings(const LibretroInputProfile& profile,
                                       std::vector<std::string>& names,
                                       std::vector<int>& types,
                                       std::vector<int>& data,
                                       std::vector<int>& extra)
{
    names.clear();
    types.clear();
    data.clear();
    extra.clear();

    const char* sections[3] = {
        profile.bindingSection,
        profile.legacyBindingSections[0],
        profile.legacyBindingSections[1],
    };

    for (const char* section : sections)
    {
        if (section == nullptr || section[0] == '\0')
        {
            continue;
        }
        std::vector<std::string> n =
            CoreSettingsGetStringListValue(SettingsID::FBNeo_BindingNames, section);
        std::vector<int> t =
            CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingTypes, section);
        std::vector<int> d =
            CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingData, section);
        std::vector<int> e =
            CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingExtraData, section);

        const size_t count = std::min(n.size(), std::min(t.size(), std::min(d.size(), e.size())));
        for (size_t i = 0; i < count; i++)
        {
            if (n[i].empty() ||
                std::find(names.begin(), names.end(), n[i]) != names.end())
            {
                continue;
            }
            names.push_back(n[i]);
            types.push_back(t[i]);
            data.push_back(d[i]);
            extra.push_back(e[i]);
        }
    }
}

// Device selection survives the profile merge as well.
inline int loadProfileDeviceType(const LibretroInputProfile& profile, int player, int fallback)
{
    const char* prefixes[3] = {
        profile.deviceSectionPrefix,
        profile.legacyDeviceSectionPrefixes[0],
        profile.legacyDeviceSectionPrefixes[1],
    };
    int result = fallback;
    for (const char* prefix : prefixes)
    {
        if (prefix == nullptr || prefix[0] == '\0')
        {
            continue;
        }
        const std::string section = std::string(prefix) + std::to_string(player);
        const int value = CoreSettingsGetIntValue(SettingsID::FBNeo_DeviceType, section);
        if (value > 0)
        {
            return value;
        }
        result = value;
    }
    return result;
}

// Reorders the flat setting lists saved by an older build into the current
// dialog order. Every historic name of a row resolves to the same hardware
// button, so a saved key follows that button instead of its old row number.
inline void remapLibretroSavedBindings(const LibretroInputProfile& profile,
                                       const std::vector<std::string>& savedNames,
                                       const std::vector<int>& savedTypes,
                                       const std::vector<int>& savedData,
                                       const std::vector<int>& savedExtra,
                                       std::vector<std::string>& names,
                                       std::vector<int>& types,
                                       std::vector<int>& data,
                                       std::vector<int>& extra)
{
    const int count = profile.count;
    names.assign(count, "");
    types.assign(count, -1); // Source_Keyboard
    data.assign(count, 0);
    extra.assign(count, 0);

    std::unordered_map<std::string, int> savedIndex;
    for (int i = 0; i < static_cast<int>(savedNames.size()); i++)
    {
        if (!savedNames[i].empty())
        {
            savedIndex.emplace(savedNames[i], i);
        }
    }

    for (int row = 0; row < count; row++)
    {
        names[row] = profile.names[row];
        data[row]  = profile.defaults[row];

        int src = -1;
        if (profile.names[row] != nullptr)
        {
            auto it = savedIndex.find(profile.names[row]);
            if (it != savedIndex.end())
            {
                src = it->second;
            }
        }
        // Newest aliases first: a name written by the immediately previous
        // build must win over an older NAOMI/Atomiswave section with the same
        // button under a different name.
        for (int n = 5; src < 0 && n >= 0; n--)
        {
            const char* legacy = profile.legacyNames[row][n];
            if (legacy == nullptr)
            {
                continue;
            }
            auto it = savedIndex.find(legacy);
            if (it != savedIndex.end())
            {
                src = it->second;
            }
        }

        if (src >= 0 && src < static_cast<int>(savedTypes.size()) &&
            src < static_cast<int>(savedData.size()) &&
            src < static_cast<int>(savedExtra.size()))
        {
            types[row] = savedTypes[src];
            data[row]  = savedData[src];
            extra[row] = savedExtra[src];
        }
    }
}

inline LibretroInputProfileId libretroInputProfileForSystem(CoreLibretroSystem system)
{
    switch (system)
    {
        case CoreLibretroSystem::Dreamcast:  return LibretroInputProfileId::Dreamcast;
        case CoreLibretroSystem::Naomi:      return LibretroInputProfileId::Arcade;
        case CoreLibretroSystem::Atomiswave: return LibretroInputProfileId::Arcade;
        case CoreLibretroSystem::Ps2:        return LibretroInputProfileId::Ps2;
        default:                             return LibretroInputProfileId::Psx;
    }
}

} // namespace Dialog
} // namespace UserInterface

#endif // LIBRETRO_INPUT_PROFILE_HPP
