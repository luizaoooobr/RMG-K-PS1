# DcCore (parallel-dc)

Build recipe for the libretro core used by RMG-K Dreamcast / NAOMI / NAOMI 2 /
Atomiswave sessions.

RMG-K loads `parallel_dc_libretro` for every libretro session that is not a
PlayStation disc. The frontend selects the core from the content path and from a
per-system `CoreLibretroSystem`, so a `.gdi/.cdi/.elf` boots as Dreamcast,
`.lst/.dat` (or a system-named archive directory) boots as NAOMI/Atomiswave, and
PlayStation descriptors keep using swanstation.

- Upstream: `https://github.com/libretro/parallel-dc.git`
- Pinned revision: `47eba939a7e5723171c8b4f72e483aeff2ebfd30`
- Patch: `parallel-dc-libretro-fixes.patch`

## What the patch changes

Upstream parallel-dc is already deterministic enough for rollback: it compiles
with `-ffp-contract=off -fno-fast-math`, advances one frame per `retro_run`, and
has a versioned serializer. The only build fix needed in this embedding is in
`core/libretro/libretro.cpp`:

- include `serialize.h` unconditionally. The HDR-renderer-only include hid a
  C-linkage declaration needed by `serialize.c`; without it the Windows link
  failed with `undefined reference to lr_serialize_v23`,
  `lr_state_before_v23` and `lr_unserialize_v23`.
- include `oslib/oslib.h` explicitly. The file uses the OS abstraction layer
  even when optional renderers are disabled, and some configurations reached it
  only through transitive includes.

The patch also makes the emulated Dreamcast RTC deterministic in
`core/hw/aica/aica_if.c`, but only when the frontend sets
`RMGK_DETERMINISTIC_RTC=1` (synchronized sessions). Upstream seeds
`RealTimeClock` from the host's local time (including its time zone and DST);
that value is written to the flash user settings, used to generate the console ID
when the flash is empty, and copied into HLE BIOS RAM. Two peers in different
time zones therefore boot different machines, and content that reads the clock
later in a match produces different audio. In netplay, RMG-K now returns the
fixed DC epoch (2000-01-01 00:00:00) instead. Offline sessions leave the
variable unset/`0` and keep the original host-clock behavior; savestate layout
is unchanged.

## Determinism settings pinned by RMG-K

The frontend forces these options before the core boots:

- `reicast_gdrom_fast_loading=disabled`
- `reicast_frame_skipping=disabled`
- `reicast_div_matching=disabled`
- `reicast_sh4_timing=legacy`
- `reicast_allow_service_buttons=enabled`
- `reicast_per_content_vmus=disabled`

In synchronized netplay sessions it also disables threaded rendering and the
ARM7 recompiler:

- `reicast_threaded_rendering=disabled`
- `reicast_arm7_recompiler=disabled`

Dreamcast and NAOMI additionally boot from the built-in HLE BIOS so a peer with
a real BIOS in its system directory cannot diverge from one without it:

- `reicast_use_real_bios=disabled` (Dreamcast, NAOMI)

NAOMI also pins `reicast_region=Japan`. Atomiswave keeps its own BIOS setup
because the core requires `awbios.zip` regardless.

Network sessions use throw-away directories under
`CoreGetUserDataDirectory()/netplay/<system>/{system,save}`; offline sessions
continue to use the normal VMU/save directories.

## Controls

All libretro families share the same 12-byte per-player packet (16-bit button
mask plus four 16-bit axes). The input dialogs keep separate settings sections
so a Dreamcast binding never overwrites a PlayStation one. NAOMI 1/2 and
Atomiswave, however, share **one** arcade profile and dialog (like Flycast
Dojo's unified arcade controls):

| Menu / toolbar | Settings section | Profile |
| --- | --- | --- |
| `inp psx` | `RMG PS1 Input - Global` | PlayStation, 14 rows |
| `inp dc` | `RMG DC Input - Global` | Dreamcast, 16 rows |
| `inp naomi+atom` | `RMG Arcade Input - Global` | NAOMI 1/2 + Atomiswave, 16 rows |

The old `RMG NAOMI Input - Global` / `RMG Atom Input - Global` sections and
their `RMG NAOMI Device - P` / `RMG Atom Device - P` counterparts are migrated
on load by button name, so an existing key keeps following the same cabinet
button. The dialog uses combined legends that follow the cabinet order for
CvS2: `BTN1 - Soco Fraco / A`, `BTN2 - Soco Medio / B`,
`BTN3 - Soco Forte / C`, `BTN4 - Chute Fraco / D`, `BTN5 - Chute Medio / E`,
`BTN6 - Chute Forte / F`, `BTN7 - Macro de 3 socos / Macro A+B+C` and
`BTN8 - Macros de 3 Chutes / Macro D+E+F`. The old legends remain as aliases,
so saved bindings keep following the same cabinet button. Clicking a row twice
starts capture just like the `Bind` button.

The arcade dialog order and the core's joymaps are:

| Dialog row | RETRO joypad bit | `nao_joymap` | `aw_joymap` |
| --- | --- | --- | --- |
| D-Pad Up/Down/Left/Right | 4/5/6/7 | D-Pad | D-Pad |
| BTN1 / BTN3 | 0 / 1 | BTN1 / BTN3 | BTN1 / BTN3 |
| Coin / Start | 2 / 3 | Coin / Start | Coin / Start |
| BTN2 / BTN4 | 8 / 9 | BTN2 / BTN4 | BTN2 / BTN4 |
| BTN5 | 11 | BTN5 | BTN5 |
| BTN6 / BTN8 / BTN7 | 10 / 12 / 13 | BTN6 / BTN8 / BTN7 | `0` (not wired) |
| Teste / Service | 14 / 15 | Test / Service | Test / Service |

Atomiswave's `aw_joymap` explicitly maps bits 10, 12 and 13 to `0`, so BTN6,
BTN7 and BTN8 cannot be triggered there without patching the core; the unified
profile keeps the NAOMI mapping because the other buttons and axes are shared.
Remapping follows Flycast: an assigned control owns both its cabinet bit and
its physical source. The frontend's hardcoded gamepad layout skips any bit that
was configured in the dialog and any gamepad button/axis that was assigned to a
different row. With `BTN1 = WEST` and `BTN4 = SOUTH`, pressing `SOUTH` now fires
only `BTN4` and pressing `WEST` only `BTN1` — previously both also triggered the
unrelated default buttons. Saved keyboard bindings are evaluated even with no
gamepad connected; rows never configured keep the default keyboard keys.

## Linux

`build-linux.sh` resets a checkout to the pinned revision, applies the patch and
builds `parallel_dc_libretro.so` with `platform=unix` and the optional GPU
renderers disabled.

The RMG-K Linux builds happen in the `rmg-builder:latest` Docker image. From an
MSYS2 shell on Windows, mount the checkout and run the script inside it:

```bash
export MSYS2_ARG_CONV_EXCL='*'
docker run --rm \
  -v 'C:\msys64\tmp:/host' \
  rmg-builder:latest \
  bash /host/rmgk-package-dc-linux.sh
```

where `rmgk-package-dc-linux.sh` calls this repository's `build-linux.sh` with
`DC_SRC=/host/parallel-dc-linux` and an output directory inside the mount.
Running the script directly inside the container with a bind-mounted checkout
works as well.

`Package/AppImage/Create.sh` bundles the core at
`shared/lib/RMG/Core/cores/parallel_dc_libretro.so`. It reads the core from
`$RMGK_DC_CORE_SO` when set, otherwise from
`Package/DcCore/parallel_dc_libretro.so` (the default output of this script).

## Windows (MSYS2 UCRT64)

`build-windows.sh` does the same with `platform=win64` and produces
`parallel_dc_libretro.dll`.

```bash
./Package/DcCore/build-windows.sh
```

Copy the DLL to `Bin/Release/Core/cores/parallel_dc_libretro.dll`. The portable
build already ships `libwinpthread-1.dll` at the frontend root; keep it there.

## Reproducibility

Both scripts reset the checkout to the pinned revision and apply the same patch.
Set `JOBS`, `DC_SRC`, `DC_REPO` or `DC_REV` to override the defaults. Never build
the Windows and Linux variants in the same source directory: the generated
object files are not compatible.

## Verification

Pinned reference builds published with `vdev-9999`:

| Artifact | Size | SHA-256 |
| --- | --- | --- |
| `parallel_dc_libretro-vdev-9999-linux64.so` | 33,894,256 | `d2853b54774e240c1599ad1a33d0404f3c806335e1eb6a4e45c6541a2c8236c5` |
| Windows `parallel_dc_libretro.dll` (inside the zip) | 14,433,780 | `f7e2e50859eb45fbb3171425485d866b58ea8a89406b74f69297e70e8369eb50` |

A ROM-backed determinism check still has to be done with a real Dreamcast,
NAOMI or Atomiswave dump inside RMG-K: boot the same content on Windows and
Linux, serialize the first frames through the rollback layer and compare the
checksums. A mismatch means the two sides used different core revisions, one of
the pinned options was overridden, or a non-isolated save directory leaked into
a synchronized session.
