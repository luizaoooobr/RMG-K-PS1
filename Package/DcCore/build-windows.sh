#!/usr/bin/env bash
#
# Build the patched parallel-dc libretro core for Windows x86_64 (MSYS2 UCRT64).
#
# The portable Windows build ships this core at Core/cores/parallel_dc_libretro.dll.
# It covers Dreamcast, NAOMI, NAOMI 2 and Atomiswave content in RMG-K.
#
# Usage (from an MSYS2 UCRT64 shell):
#   Package/DcCore/build-windows.sh [output-dir]
#
# Environment:
#   DC_SRC     checkout to reuse (default: Package/DcCore/.parallel-dc)
#   DC_REPO    git remote (default: https://github.com/libretro/parallel-dc.git)
#   DC_REV     pinned revision (default: see below)
#   JOBS       parallel build jobs (default: nproc)
#
# The checkout is reset to the pinned revision and cleaned before patching, so
# point DC_SRC at a dedicated clone, not at a working tree you edit.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
patch_file="$script_dir/parallel-dc-libretro-fixes.patch"
repo_url="${DC_REPO:-https://github.com/libretro/parallel-dc.git}"
revision="${DC_REV:-47eba939a7e5723171c8b4f72e483aeff2ebfd30}"
src_dir="${DC_SRC:-$script_dir/.parallel-dc}"
out_dir="${1:-$script_dir}"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
core_name="parallel_dc_libretro.dll"

if [[ -d /ucrt64/bin ]]
then
	export MSYSTEM=UCRT64
	export PATH="/ucrt64/bin:$PATH"
fi

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
	HAVE_VULKAN=0 HAVE_D3D11=0 HAVE_D3D12=0 HAVE_D3D9=0

mkdir -p "$out_dir"
cp -f "$src_dir/$core_name" "$out_dir/$core_name"

echo
echo "built: $out_dir/$core_name"
sha256sum "$out_dir/$core_name"
