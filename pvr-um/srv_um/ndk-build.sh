#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Standalone NDK build of libsrv_um_cr.so,
# an export-set check against exports.txt, and a clang-tidy pass
# over the library sources.
#
# Usage: ndk-build.sh [all|build|tidy]   (default: all)
#
# Environment:
#   ANDROID_NDK_ROOT     NDK r30 root (default /opt/android-ndk)
#   LIBHARDWARE_DIR      directory holding the platform libhardware.so to
#                        link against (default $ANDROID_PRODUCT_OUT/system/lib)
#   OUT                  output directory (default <script dir>/out)
set -eu

mode=${1:-all}
case $mode in
	all|build|tidy) ;;
	*) echo "usage: $0 [all|build|tidy]" >&2; exit 2 ;;
esac

src=$(cd "$(dirname "$0")" && pwd)
pvr=$(cd "$src/../../pvr-source" && pwd)
ndk=${ANDROID_NDK_ROOT:-/opt/android-ndk}
bin=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
api=30
target=armv7a-linux-androideabi$api
cc=$bin/$target-clang
out=${OUT:-$src/out}

lib_srcs="srv_um_abi.c srv_um_apphint.c srv_um_bridge.c srv_um_connection.c
srv_um_debug.c srv_um_devmem.c srv_um_misc.c srv_um_sync.c srv_um_utils.c"

includes="-I$src -I$pvr/include4 -I$pvr/services4/include -I$pvr/services4/system/omap"
cflags="-std=c11 -mcpu=cortex-a9 -mthumb -O2 -flto=thin -fPIC
-fvisibility=hidden -Wall -Wextra -Werror"

build() {
	libhw=${LIBHARDWARE_DIR:-${ANDROID_PRODUCT_OUT:+$ANDROID_PRODUCT_OUT/system/lib}}
	if [ -z "$libhw" ] || [ ! -f "$libhw/libhardware.so" ]; then
		echo "$0: set LIBHARDWARE_DIR (or ANDROID_PRODUCT_OUT) to a directory holding libhardware.so" >&2
		exit 1
	fi

	mkdir -p "$out/obj"
	objs=""
	for f in $lib_srcs; do
		o=$out/obj/${f%.c}.o
		# shellcheck disable=SC2086 # flag lists split on purpose
		"$cc" $cflags $includes -c "$src/$f" -o "$o"
		objs="$objs $o"
	done

	# shellcheck disable=SC2086
	"$cc" $cflags -shared -fuse-ld=lld -Wl,-soname,libsrv_um_cr.so \
		-Wl,--no-undefined -Wl,-z,defs \
		-o "$out/libsrv_um_cr.so" $objs \
		-L"$libhw" -Wl,-rpath-link,"$libhw" -lhardware -llog -ldl


	# The exported dynamic symbols equal exports.txt exactly.
	"$bin/llvm-nm" -D --defined-only "$out/libsrv_um_cr.so" |
		awk '$2 == "T" { print $3 }' | LC_ALL=C sort > "$out/exports.actual"
	LC_ALL=C sort "$src/exports.txt" > "$out/exports.expected"
	if ! cmp -s "$out/exports.expected" "$out/exports.actual"; then
		echo "$0: exported symbols differ from exports.txt:" >&2
		diff "$out/exports.expected" "$out/exports.actual" >&2 || :
		exit 1
	fi
	echo "built $out/libsrv_um_cr.so ($(wc -l < "$out/exports.actual") exports)"
}

tidy() {
	sysroot=$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot
	files=""
	for f in $lib_srcs; do
		files="$files $src/$f"
	done
	# The check set and HeaderFilterRegex live in .clang-tidy beside the
	# sources; pvr-source headers fall outside the filter.
	# shellcheck disable=SC2086
	"$bin/clang-tidy" --quiet $files -- --target=$target --sysroot="$sysroot" \
		-std=c11 -mcpu=cortex-a9 -mthumb -fvisibility=hidden $includes
	echo "clang-tidy clean"
}

case $mode in
	build) build ;;
	tidy) tidy ;;
	all) build; tidy ;;
esac
