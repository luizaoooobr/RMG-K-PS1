#ifndef LIBRETRO_INPUT_PROFILE_HPP
#define LIBRETRO_INPUT_PROFILE_HPP

#include <RMG-Core/Emulation.hpp>

#include <SDL3/SDL.h>

namespace UserInterface
{
namespace Dialog
{

// One input profile per machine family driven by the bundled libretro cores.
// The dialog and the live input handler share this table so a binding saved in
// "DC Input" is read back with the same name and the same libretro joypad bit.
enum class LibretroInputProfileId
{
    Psx        = 0,
    Dreamcast  = 1,
    Naomi      = 2,
    Atomiswave = 3,
};

struct LibretroInputProfile
{
    const char* title;
    const char* bindingSection;
    const char* deviceSectionPrefix;
    const char* traceFile;
    int         count;
    const char* names[16];
    int         bits[16];     // RETRO_DEVICE_ID_JOYPAD_* bit for each row
    int         defaults[16]; // SDL3 scancode fallback for P1
};

inline const LibretroInputProfile& libretroInputProfile(LibretroInputProfileId id)
{
    static const LibretroInputProfile profiles[4] = {
        // PlayStation: keep the original 14-row table verbatim so existing
        // saved bindings and the legacy scancode fallback keep working.
        {
            "PlayStation Input",
            "RMG PS1 Input - Global",
            "RMG PS1 Device - P",
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
        },
        // parallel-dc maps JOYPAD_B/Y/A/X to the Dreamcast face buttons; bits
        // 10..15 are not wired by the core but are kept so one 16-row dialog
        // layout covers all three arcade families.
        {
            "Dreamcast Input",
            "RMG DC Input - Global",
            "RMG DC Device - P",
            "dc_input.log",
            16,
            {
                "DC A (South, B)", "DC X (West, Y)", "DC Select (unused)", "DC Start",
                "DC D-Pad Up", "DC D-Pad Down", "DC D-Pad Left", "DC D-Pad Right",
                "DC B (East, A)", "DC Y (North, X)", "DC L (unused)", "DC R (unused)",
                "DC L2 (unused)", "DC R2 (unused)", "DC L3 (unused)", "DC R3 (unused)",
            },
            { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
            {
                SDL_SCANCODE_Z, SDL_SCANCODE_A, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
                SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                SDL_SCANCODE_X, SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_W,
                SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL,
            },
        },
        // NAOMI joymap: BTN1/BTN3/COIN/START, D-pad, BTN2/BTN4/BTN6/BTN5/
        // BTN8/BTN7, TEST and SERVICE on L3/R3.
        {
            "NAOMI Input",
            "RMG NAOMI Input - Global",
            "RMG NAOMI Device - P",
            "naomi_input.log",
            16,
            {
                "BTN1", "BTN3", "Coin", "Start",
                "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
                "BTN2", "BTN4", "BTN6", "BTN5",
                "BTN8", "BTN7", "Test", "Service",
            },
            { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
            {
                SDL_SCANCODE_Z, SDL_SCANCODE_A, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
                SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                SDL_SCANCODE_X, SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_W,
                SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL,
            },
        },
        // Atomiswave joymap: BTN1/BTN3/COIN/START, D-pad, BTN2/BTN4/BTN5,
        // TEST and SERVICE. L/L2/R2 are not wired.
        {
            "Atomiswave Input",
            "RMG Atom Input - Global",
            "RMG Atom Device - P",
            "atom_input.log",
            16,
            {
                "BTN1", "BTN3", "Coin", "Start",
                "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
                "BTN2", "BTN4", "L (unused)", "BTN5",
                "L2 (unused)", "R2 (unused)", "Test", "Service",
            },
            { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
            {
                SDL_SCANCODE_Z, SDL_SCANCODE_A, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
                SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                SDL_SCANCODE_X, SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_W,
                SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL,
            },
        },
    };
    const int index = static_cast<int>(id);
    return profiles[index >= 0 && index < 4 ? index : 0];
}

inline LibretroInputProfileId libretroInputProfileForSystem(CoreLibretroSystem system)
{
    switch (system)
    {
        case CoreLibretroSystem::Dreamcast:  return LibretroInputProfileId::Dreamcast;
        case CoreLibretroSystem::Naomi:      return LibretroInputProfileId::Naomi;
        case CoreLibretroSystem::Atomiswave: return LibretroInputProfileId::Atomiswave;
        default:                             return LibretroInputProfileId::Psx;
    }
}

} // namespace Dialog
} // namespace UserInterface

#endif // LIBRETRO_INPUT_PROFILE_HPP
