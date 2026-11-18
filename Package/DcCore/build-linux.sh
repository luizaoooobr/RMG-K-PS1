#!/usr/bin/env bash
#
# Build the patched parallel-dc libretro core for Linux x86_64.
#
# The Linux AppImage bundles this core at
# shared/lib/RMG/Core/cores/parallel_dc_libretro.so, so it has to be built with
# the same revision and patch as the Windows DLL before Package/AppImage/Create.sh
# runs.
#
# Usage (inside the RMG-K Docker builder, or any Linux x86_64 build host):
#   Package/DcCore/build-linux.sh [output-dir]
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
core_name="parallel_dc_libretro.so"

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
git -C "$src_dir" apply --check "$patch_file"
git -C "$src_dir" apply "$patch_file"

echo "==> building Linux core ($jobs jobs)"
make -C "$src_dir" clean >/dev/null 2>&1 || true
make -C "$src_dir" -j"$jobs" platform=unix \
	HAVE_VULKAN=0 HAVE_GL3=0 HAVE_GL4=0

mkdir -p "$out_dir"
cp -f "$src_dir/$core_name" "$out_dir/$core_name"

echo
echo "built: $out_dir/$core_name"
sha256sum "$out_dir/$core_name"
