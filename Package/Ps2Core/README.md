# parallel-ps2 (LRPS2) core for RMG-K

PlayStation 2 sessions in RMG-K are driven by the libretro core
[`libretro/parallel-ps2`](https://github.com/libretro/parallel-ps2), built from
one pinned revision and shipped inside both release packages. The core inherits
PCSX2's GPL/LGPL licensing.

## Pinned source

| Item | Value |
| --- | --- |
| Upstream | `https://github.com/libretro/parallel-ps2.git` |
| Revision | `6fbf88296c0fef637a2c50064ecc8deebd577d9a` |
| Frontend library | `pcsx2_libretro.so` / `pcsx2_libretro.dll` |
| Renderer | OpenGL (RMG-K hands hardware-rendering cores an OpenGL FBO) |
| Licensing | GPL/LGPL, inherited from PCSX2 |

`build-linux.sh` and `build-windows.sh` reset their checkout to this revision,
clean it, apply `parallel-ps2-libretro-fixes.patch`, and build with
`HAVE_VULKAN=0 HAVE_OPENGL=1`. Both platforms use the same source and options;
CI runs these exact scripts and embeds their output in the AppImage/portable as
well as publishing it as standalone release assets.

## Patch: deterministic RTC

RMG-K exports `RMGK_DETERMINISTIC_RTC=1` when it starts a synchronized session
(the same mechanism used by parallel-dc). The patch makes the core's CDVD block
seed the PS2 RTC with a fixed epoch (2000-01-01 00:00:00 UTC, then the usual
GMT+9 shift) instead of `time(NULL)` when that variable is set. Games that read
the console clock therefore see the same date/time on both peers. Offline
sessions leave the variable at `0` and keep the host clock.

## Netplay setup in RMG-K

PS2 uses the **same lockstep path as Dreamcast**, not per-frame rollback:

- A serialized state is around 47-50 MB. RMG-K only keeps the frame-0 anchor
  and relies on the audio/input healthcheck for per-frame desync detection.
- `pcsx2_renderer=OpenGL`, `pcsx2_mtvu=disabled`,
  `pcsx2_uncapped_framerate_hint=disabled`, `pcsx2_enable_cheats=disabled` and
  the other timing/accuracy overrides are pinned by the host at startup.
- `pcsx2_shared_memory_cards=disabled` in synchronized sessions: the emulator
  never touches the player's memory cards, HDD image or cheat files. Saves and
  savestates stay in isolated per-session directories.
- A PS2 BIOS is mandatory. Put the same `.bin`/`.rom` file in
  `<user data>/ps2/system/pcsx2/bios/` on each peer. A synchronized session
  copies only that BIOS folder into its isolated system tree and pins
  `pcsx2_bios` to one filename; the frame-0 state checksum catches peers that
  supplied different bytes under the same name.
- PS2 uses the DualShock2/analog endpoint and shares the PS1 analog input
  profile and saved bindings.

PS2 content is detected from paths containing `ps2`, `psx2`, `playstation2` or
`playstation 2`. `.elf` files need that hint (otherwise they are treated as
Dreamcast); `.iso`/`.cso` files are only claimed with the hint, so PS1 disc
images are left alone.

## Building

From an MSYS2 UCRT64 shell for Windows:

```bash
bash Package/Ps2Core/build-windows.sh            # writes pcsx2_libretro.dll here
bash Package/Ps2Core/build-windows.sh /some/dir  # or to an explicit output dir
```

On a Linux x86_64 build host:

```bash
bash Package/Ps2Core/build-linux.sh              # writes pcsx2_libretro.so here
```

Useful overrides: `PS2_SRC` (reuse a dedicated checkout), `PS2_REPO`, `PS2_REV`
and `JOBS`. The checkout is hard-reset and cleaned before the patch is applied,
so never point `PS2_SRC` at a tree with changes you want to keep.

## Reference builds

The release assets are named `pcsx2_libretro-<version>-linux64.so` and
`pcsx2_libretro-<version>-win64.dll`. Inside AppImage/portable packages they are
installed as `pcsx2_libretro.so` / `pcsx2_libretro.dll`; that is the filename
the RMG-K runtime looks for.

### Windows (validated locally, MSYS2 UCRT64)

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `pcsx2_libretro.dll` | 8,945,296 | `aadffb638da204898cb8ed0e7ce98609896dca7de8fd5e99d1e065b605a06356` |

### Linux

Filled in from the first successful CI build of this integration (same pinned
revision and patch as the Windows reference above).
