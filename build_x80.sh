#!/usr/bin/env bash
# Build the restored X80 source with the exact Android clang12 toolchain.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
: "${CLANG_DIR:?Set CLANG_DIR to Android clang-r416183b}"
JOBS=${JOBS:-8}
LTO_JOBS=${LTO_JOBS:-1}
[[ $JOBS =~ ^[1-9][0-9]*$ && $LTO_JOBS =~ ^[1-9][0-9]*$ ]] || exit 2
(( JOBS <= 10 && LTO_JOBS <= 10 )) || { echo "Maximum parallelism is 10" >&2; exit 2; }
export PATH="$CLANG_DIR/bin:$PATH"
cd "$ROOT"
clang --version
args=(ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu-
 CC=clang "LD=ld.lld --threads=$LTO_JOBS --thinlto-jobs=$LTO_JOBS"
 AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump
 READELF=llvm-readelf OBJSIZE=llvm-size STRIP=llvm-strip)
if [[ ! -f .config ]]; then
 make "${args[@]}" vivo_x80_os5_defconfig
fi
make "${args[@]}" olddefconfig
grep -q "^# CONFIG_VIVO_X80_COMPAT is not set$" .config || {
 echo "Refusing obsolete compatibility-stub configuration" >&2; exit 2; }
for feature in CFI_CLANG LTO_CLANG_THIN SECURITY_SELINUX; do
 grep -q "^CONFIG_${feature}=y$" .config || exit 2
done
make "${args[@]}" -j"$JOBS" Image.gz
sha256sum arch/arm64/boot/Image arch/arm64/boot/Image.gz
