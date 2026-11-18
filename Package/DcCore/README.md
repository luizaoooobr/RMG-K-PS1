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

No emulator logic is patched, so the core keeps upstream state compatibility
for the pinned revision.

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

Network sessions use throw-away directories under
`CoreGetUserDataDirectory()/netplay/<system>/{system,save}`; offline sessions
continue to use the normal VMU/save directories.

## Controls

All libretro families share the same 12-byte per-player packet (16-bit button
mask plus four 16-bit axes). The input dialogs keep separate settings sections
so a Dreamcast binding never overwrites a PlayStation one:

| Menu / toolbar | Profile | Bits used |
| --- | --- | --- |
| `inp psx` | PlayStation, 14 rows | legacy PS1 names/bits |
| `inp dc` | Dreamcast, 16 rows | JOYPAD A/B/X/Y/START/SELECT/D-Pad/L/R/L3/R3 |
| `inp naomi` | NAOMI, 16 rows | BTN1..BTN8, COIN, START, D-Pad, TEST, SERVICE |
| `inp atom` | Atomiswave, 16 rows | BTN1..BTN5, COIN, START, D-Pad, TEST, SERVICE |

TEST and SERVICE are exposed on bits 14 and 15, matching the upstream NAOMI and
Atomiswave joymaps.

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
