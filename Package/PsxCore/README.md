# PsxCore (swanstation)

Build recipe for the PlayStation libretro core used by the RMG-K PS1 builds.

RMG-K loads `swanstation_libretro` for PS1 sessions. Upstream swanstation has a
few pieces of controller and disc-path state that are not deterministic across
machines, which makes PS1 netplay (rollback/state comparison) desync between two
PCs — including the same setup on Windows and Linux. The patch in this directory
fixes that; both the Windows portable build and the Linux AppImage must ship a
core built from it.

- Upstream: `https://github.com/libretro/swanstation.git`
- Pinned revision: `631f0fb31b809e0f889d7c551fe695788e39c1d3`
- Patch: `swanstation-netplay-determinism.patch`

## What the patch changes

- `src/core/analog_controller.*`, `src/core/negcon_rumble.*`: serialize the
  analog-mode lock flag and the analog-button latch, and make the toggle
  edge-triggered. The frontend no longer keeps its own press latch, so a
  rollback replay sees the same toggle edges as the original run.
- `src/libretro/libretro_host_interface.cpp`: pass the raw combo level to the
  controller every frame instead of latching it in the frontend.
- `src/core/system.cpp`, `src/libretro/libretro_host_interface.cpp`: write only
  the disc display name (not the absolute path) into the state header and the
  `SWANDISC` trailer. Two PCs with the same disc in different folders then
  produce the same state bytes; load still resolves the disc by display name.
- `src/core/save_state_version.h`: `SAVE_STATE_VERSION` 57 → 59 (the two new
  controller fields).

States are not compatible with an unpatched core, and vice versa.

## Linux

`build-linux.sh` resets a checkout to the pinned revision, applies the patch and
builds `swanstation_libretro.so` with `Makefile.libretro platform=unix`.

```bash
./Package/PsxCore/build-linux.sh
# or pick the output directory
./Package/PsxCore/build-linux.sh /tmp/psx-core
```

The RMG-K Linux builds are done in the `rmg-builder` Docker container. Run the
script inside that container (the WSL filesystem, mounted into the container) so
the resulting `.so` matches the glibc used by the AppImage.

`Package/AppImage/Create.sh` bundles the core at
`shared/lib/RMG/Core/cores/swanstation_libretro.so`. It reads the core from
`$RMGK_PSX_CORE_SO` when set, otherwise from
`Package/PsxCore/swanstation_libretro.so` (the default output of this script).
Set `VERSION` to pin the AppImage file name when the tag is not on `HEAD`, e.g.
`VERSION=vdev-9999 ./Package/AppImage/Create.sh`.

## Windows (MSYS2 UCRT64)

`build-windows.sh` does the same with `Makefile.libretro platform=win` and
produces `swanstation_libretro.dll`.

```bash
./Package/PsxCore/build-windows.sh
```

Copy the DLL to `Bin/Release/Core/cores/swanstation_libretro.dll`. The portable
build already ships `libwinpthread-1.dll` at the frontend root; keep it there.

## Reproducibility

Both scripts reset the checkout to the pinned revision, apply the patch and pass
`BUILD_DATE` taken from that commit (UTC) instead of the compile date, so a
rebuild with the same toolchain produces the same bytes. Set `BUILD_DATE` (or
`JOBS`, `SWANSTATION_SRC`, `SWANSTATION_REV`) to override any of it.

## Verification

The core must produce identical libretro state checksums on every machine and
OS. A quick check is to boot the same disc and serialize after the first
`retro_run` with a libretro probe: the reference values for the pinned revision
and patch are

- state after the first `retro_run`: `1979947366`
- state after the second `retro_run`: `89706466`

Both must match between the Windows `.dll` and the Linux `.so`. A mismatch means
the patch is missing, a different revision was built, or one side is still using
the path-based header.

Reference builds published with `vdev-9999`:

| Artifact | Size | SHA-256 |
| --- | --- | --- |
| `swanstation_libretro-vdev-9999-linux64.so` | 7,087,920 | `3d80b326df8465188ac371472d1f428e5002ae34acc944880d361a7b27e83088` |
| Windows `swanstation_libretro.dll` (inside the zip) | 22,378,502 | `4e15febfe36b41242f6230633cc45f519dc5b683571a0518ae196a65a598d8ec` |
