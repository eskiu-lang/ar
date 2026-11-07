#!/bin/sh
# Checks native/android/ar_api.h against ARCore itself, with no device: each function's
# declaration and each value against Google's arcore_c_api.h (compiled with the NDK's clang,
# warnings as errors), and each function's name against the symbols libarcore_sdk_c.so
# exports. Downloads ARCore into a temporary directory (nothing is kept).
#
#     tools/check_arcore_api.sh [VERSION]      (default 1.57.0; needs the Android NDK)
set -eu
version=${1:-1.57.0}
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk}
ndk=${ANDROID_NDK_HOME:-$(ls -d "$sdk"/ndk/* 2>/dev/null | sort -V | tail -1)}
bin=$(ls -d "$ndk"/toolchains/llvm/prebuilt/*/bin | head -1)

curl -fsSL -o "$work/arcore_c_api.h" \
  "https://raw.githubusercontent.com/google-ar/arcore-android-sdk/v$version/libraries/include/arcore_c_api.h"
curl -fsSL -o "$work/core.aar" "https://dl.google.com/android/maven2/com/google/ar/core/$version/core-$version.aar"
(cd "$work" && unzip -q core.aar 'jni/arm64-v8a/*')

{
  echo '#include "arcore_c_api.h"'
  echo "#include \"$root/native/android/ar_api.h\""
  echo '#define X(result, name, params) result (*check_##name) params = name;'
  echo 'AR_API'
  grep -o 'AR_[A-Z0-9_]*_ = ' "$root/native/android/ar_api.h" | sed 's/_ = //' | while read -r value; do
    echo "_Static_assert(${value}_ == $value, \"$value\");"
  done
} > "$work/check.c"
"$bin/clang" --target=aarch64-linux-android29 -std=c11 -Wall -Werror -Wincompatible-pointer-types \
  -I"$work" -c "$work/check.c" -o "$work/check.o"

"$bin/llvm-nm" -D --defined-only "$work/jni/arm64-v8a/libarcore_sdk_c.so" | awk '{sub(/@.*/, "", $NF); print $NF}' | sort -u > "$work/exported"
missing=0
for name in $(grep -o '^    X([^,]*, [A-Za-z_]*' "$root/native/android/ar_api.h" | sed 's/.*, //'); do
  grep -qx "$name" "$work/exported" || { echo "not exported: $name"; missing=1; }
done
[ $missing -eq 0 ] || exit 1
echo "ar_api.h matches ARCore $version ($(grep -c '    X(' "$root/native/android/ar_api.h") functions)"
