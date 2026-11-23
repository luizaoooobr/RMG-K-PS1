#!/usr/bin/env bash
#
# Build the patched parallel-ps2 (LRPS2) libretro core for Windows x86_64
# (MSYS2 UCRT64).
#
# The portable Windows build ships this core at Core/cores/pcsx2_libretro.dll.
# RMG-K's libretro host only hands hardware-rendering cores an OpenGL FBO, so
# Vulkan is disabled and OpenGL is kept. RMGK_DETERMINISTIC_RTC is read at run
# time by the patch in this directory; netplay sessions pin the PS2 clock on
# both peers while offline sessions keep the host clock.
#
# Usage (from an MSYS2 UCRT64 shell):
#   Package/Ps2Core/build-windows.sh [output-dir]
#
# Environment:
#   PS2_SRC     checkout to reuse (default: Package/Ps2Core/.parallel-ps2)
#   PS2_REPO    git remote (default: https://github.com/libretro/parallel-ps2.git)
#   PS2_REV     pinned revision (default: see below)
#   JOBS        parallel build jobs (default: nproc)
#
# The checkout is reset to the pinned revision and cleaned before patching, so
# point PS2_SRC at a dedicated clone, not at a working tree you edit.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
patch_file="$script_dir/parallel-ps2-libretro-fixes.patch"
repo_url="${PS2_REPO:-https://github.com/libretro/parallel-ps2.git}"
revision="${PS2_REV:-6fbf88296c0fef637a2c50064ecc8deebd577d9a}"
src_dir="${PS2_SRC:-$script_dir/.parallel-ps2}"
out_dir="${1:-$script_dir}"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
core_name="pcsx2_libretro.dll"

# PE headers and __DATE__/__TIME__ would otherwise change the hash on every
# build even when the sources are identical. Pin both so the standalone asset,
# the portable build and the README reference stay reproducible.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-946684800}"

if [[ -d /ucrt64/bin ]]
then
	export MSYSTEM=UCRT64
	export PATH="/ucrt64/bin:$PATH"
fi

linker="$(command -v g++) -Wl,--no-insert-timestamp"

for tool in git make g++ sha256sum
do
	if ! command -v "$tool" >/dev/null 2>&1
	then
		echo "missing build tool: $tool" >&2
		echo "install the MSYS2 UCRT64 toolchain (mingw-w64-ucrt-x86_64-gcc, make, git)" >&2
		exit 1
	fi
done

if [[ ! -f "$patch_file" ]]
then
	echo "patch not found: $patch_file" >&2
	exit 1
fi

if [[ ! -d "$src_dir/.git" ]]
then
	echo "==> cloning $repo_url"
	mkdir -p "$(dirname "$src_dir")"
	git clone "$repo_url" "$src_dir"
fi

echo "==> checking out $revision"
git -C "$src_dir" fetch --tags --force origin
git -C "$src_dir" reset --hard "$revision"
git -C "$src_dir" clean -fdx

echo "==> applying $patch_file"
git -C "$src_dir" apply --check --ignore-whitespace "$patch_file"
git -C "$src_dir" apply --ignore-whitespace "$patch_file"

echo "==> building Windows core ($jobs jobs)"
make -C "$src_dir" clean >/dev/null 2>&1 || true
make -C "$src_dir" -j"$jobs" platform=win64 \
	HAVE_VULKAN=0 HAVE_OPENGL=1 \
	LD="$linker"

mkdir -p "$out_dir"
cp -f "$src_dir/$core_name" "$out_dir/$core_name"

echo
echo "built: $out_dir/$core_name"
sha256sum "$out_dir/$core_name"
