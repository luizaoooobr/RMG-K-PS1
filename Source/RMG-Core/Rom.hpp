/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_ROM_HPP
#define CORE_ROM_HPP

#include <filesystem>

enum class CoreRomType
{
    Cartridge  = 0,
    Disk       = 1,
    Arcade     = 2,
    PlayStation = 3,
    Dreamcast   = 4,
    Naomi       = 5,
    Atomiswave  = 6,
    PlayStation2 = 7
};

// True for every machine driven by the bundled libretro runtime. They all use
// the same 12-byte input packet and cannot be launched with another ROM/disk.
inline bool CoreIsLibretroRomType(CoreRomType type)
{
    return type == CoreRomType::PlayStation || type == CoreRomType::PlayStation2 ||
           type == CoreRomType::Dreamcast || type == CoreRomType::Naomi ||
           type == CoreRomType::Atomiswave;
}

// opens the given file as ROM
bool CoreOpenRom(std::filesystem::path file);

// returns whether core has a ROM opened
bool CoreHasRomOpen(void);

// attempts to retrieve the ROM type
bool CoreGetRomType(CoreRomType& type);

// attempts to retrieve ROM path
bool CoreGetRomPath(std::filesystem::path& path);

// closes ROM
bool CoreCloseRom(void);

#endif // CORE_ROM_HPP