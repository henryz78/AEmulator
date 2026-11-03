#!/bin/sh
# The guest's su: arm64, static, no libc. Any clang with the AArch64 target and lld builds it (the NDK's too):
#   sh build.sh            (CC=<clang> to pick one)
cd "$(dirname "$0")" || exit 1
CC=${CC:-clang}
if [ -z "${CC##*ndk*}" ] || [ -n "$NDK" ]; then
  B="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"; [ -x "$B/clang" ] || [ -x "$B/clang.exe" ] || B="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
  CC="$B/clang"
fi
OUT=../../app/src/main/assets/engines/common/aemu_su
"$CC" --target=aarch64-linux-gnu -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -nostdlib -static \
  -fuse-ld=lld -Wl,--build-id=none -Wl,-z,noexecstack -Wl,-e,_start -Wall -o "$OUT" su.c || exit 1
"${CC%clang*}llvm-strip" "$OUT" 2>/dev/null || llvm-strip "$OUT" 2>/dev/null || true
ls -la "$OUT"
