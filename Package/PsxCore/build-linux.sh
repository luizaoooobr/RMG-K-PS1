#!/usr/bin/env bash
#
# Build the patched swanstation libretro core for Linux x86_64.
#
# The Linux AppImage bundles this core at
# shared/lib/RMG/Core/cores/swanstation_libretro.so, so it has to be built with
# the RMG-K netplay determinism patch before Package/AppImage/Create.sh runs.
#
# Usage:
#   Package/PsxCore/build-linux.sh [output-dir]
#
# Environment:
#   SWANSTATION_SRC   checkout to reuse (default: Package/PsxCore/.swanstation)
#   SWANSTATION_REPO  git remote (default: https://github.com/libretro/swanstation.git)
#   SWANSTATION_REV   pinned revision (default: see below)
#   JOBS              parallel build jobs (default: nproc)
#
# The checkout is reset to the pinned revision and cleaned before patching, so
# point SWANSTATION_SRC at a dedicated clone, not at a working tree you edit.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
patch_file="$script_dir/swanstation-netplay-determinism.patch"
repo_url="${SWANSTATION_REPO:-https://github.com/libretro/swanstation.git}"
revision="${SWANSTATION_REV:-631f0fb31b809e0f889d7c551fe695788e39c1d3}"
src_dir="${SWANSTATION_SRC:-$script_dir/.swanstation}"
out_dir="${1:-$script_dir}"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
core_name="swanstation_libretro.so"

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

# The core bakes BUILD_DATE into the binary; take it from the pinned commit so a
# rebuild produces the same bytes instead of whatever day it was compiled.
build_date="${BUILD_DATE:-$(TZ=UTC git -C "$src_dir" show -s --format=%cd --date=format-local:'%Y-%m-%d %H:%M:%S' "$revision")}"

echo "==> building with platform=unix ($jobs jobs, BUILD_DATE=$build_date)"
make -C "$src_dir" -f Makefile.libretro platform=unix -j"$jobs" BUILD_DATE="$build_date"

mkdir -p "$out_dir"
cp -f "$src_dir/$core_name" "$out_dir/$core_name"

echo
echo "built: $out_dir/$core_name"
sha256sum "$out_dir/$core_name"
