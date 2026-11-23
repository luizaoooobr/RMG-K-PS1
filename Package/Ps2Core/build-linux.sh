#!/usr/bin/env bash
#
# Build the patched parallel-ps2 (LRPS2) libretro core for Linux x86_64.
#
# The Linux AppImage bundles this core at
# shared/lib/RMG/Core/cores/pcsx2_libretro.so, so it has to be built with the
# same revision and patch as the Windows DLL before Package/AppImage/Create.sh
# runs. RMG-K's libretro host only hands hardware-rendering cores an OpenGL
# FBO, so Vulkan is disabled and OpenGL is kept.
#
# Usage (inside the RMG-K Docker builder, or any Linux x86_64 build host):
#   Package/Ps2Core/build-linux.sh [output-dir]
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
core_name="pcsx2_libretro.so"

for tool in git make g++ sha256sum
do
	if ! command -v "$tool" >/dev/null 2>&1
	then
		echo "missing build tool: $tool" >&2
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

echo "==> building Linux core ($jobs jobs)"
make -C "$src_dir" clean >/dev/null 2>&1 || true
make -C "$src_dir" -j"$jobs" platform=unix \
	HAVE_VULKAN=0 HAVE_OPENGL=1

mkdir -p "$out_dir"
cp -f "$src_dir/$core_name" "$out_dir/$core_name"

echo
echo "built: $out_dir/$core_name"
sha256sum "$out_dir/$core_name"
