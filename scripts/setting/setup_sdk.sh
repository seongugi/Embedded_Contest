#!/usr/bin/env bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

source "$SCRIPT_DIR/project.conf"

SDK_PATH="$PROJECT_ROOT/$SDK_DIR"

echo "========================================"
echo "      SDK Environment Setup"
echo "========================================"

#--------------------------------------------------
# Check SDK
#--------------------------------------------------
if [ ! -d "$SDK_PATH" ]; then
    echo "ERROR: SDK directory not found."
    echo ""
    echo "Run:"
    echo "    ./scripts/setting/download_sdk.sh"
    return 1
fi

#--------------------------------------------------
# Set Environment Variables
#--------------------------------------------------
echo "[1/2] Setting environment..."

export SDK_ROOT="$SDK_PATH"

# SDK 배포본에 따라 sysroot가 sdk/sysroot 또는
# sdk/<toolchain-prefix>/sysroot에 놓일 수 있음. 경로 이름을 고정하지 않고
# 실제 SQLite 개발 헤더가 들어 있는 완전한 sysroot를 먼저 선택함.
SYSROOT_CANDIDATES=(
    "$SDK_ROOT/sysroot"
    "$SDK_ROOT/${TOOLCHAIN_PREFIX}/sysroot"
)

SYSROOT=""

for CANDIDATE in "${SYSROOT_CANDIDATES[@]}"; do
    if [ -f "$CANDIDATE/usr/include/sqlite3.h" ]; then
        SYSROOT="$CANDIDATE"
        break
    fi
done

# SDK 디렉터리 배치가 다시 바뀌어도 usr/include/sqlite3.h를 기준으로 탐색함.
if [ -z "$SYSROOT" ]; then
    SQLITE_HEADER="$(
        find "$SDK_ROOT" \
            -type f \
            -path '*/usr/include/sqlite3.h' \
            -print \
            -quit
    )"

    if [ -n "$SQLITE_HEADER" ]; then
        SYSROOT="${SQLITE_HEADER%/usr/include/sqlite3.h}"
    fi
fi

if [ -z "$SYSROOT" ] || [ ! -d "$SYSROOT" ]; then
    echo "ERROR: usable SDK sysroot를 찾을 수 없습니다."
    echo "Checked:"
    printf '    %s\n' "${SYSROOT_CANDIDATES[@]}"
    echo ""
    echo "SDK를 다시 받은 뒤 setup_sdk.sh를 실행하십시오."
    return 1
fi

export SYSROOT

# Debian cross compiler가 Pi SDK의 헤더·시작 오브젝트·라이브러리를 사용하게 한다.
SDK_MULTIARCH_LIB="$SYSROOT/usr/lib/aarch64-linux-gnu"
SDK_CXX_VERSION="$(find "$SYSROOT/usr/include/c++" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort -V | tail -n 1)"

if [ -z "$SDK_CXX_VERSION" ] ||
   [ ! -f "$SYSROOT/usr/include/aarch64-linux-gnu/asm/errno.h" ] ||
   [ ! -f "$SDK_MULTIARCH_LIB/libstdc++.so" ] ||
   [ ! -f "$SDK_MULTIARCH_LIB/libgcc_s.so" ]; then
    echo "ERROR: Pi 개발 헤더·런타임 링크가 부족합니다. SDK v1.2 이상을 설치하십시오."
    return 1
fi

# SDK의 C++ 헤더와 호환되는 같은 버전의 C/C++ 컴파일러를 선택한다.
SDK_CXX_MAJOR="${SDK_CXX_VERSION%%.*}"
SDK_CC=""
SDK_CXX=""
for SDK_COMPILER_SUFFIX in "-$SDK_CXX_MAJOR" ""; do
    SDK_CC_CANDIDATE="aarch64-linux-gnu-gcc$SDK_COMPILER_SUFFIX"
    SDK_CXX_CANDIDATE="aarch64-linux-gnu-g++$SDK_COMPILER_SUFFIX"
    command -v "$SDK_CC_CANDIDATE" >/dev/null 2>&1 || continue
    command -v "$SDK_CXX_CANDIDATE" >/dev/null 2>&1 || continue
    SDK_CC_MAJOR="$($SDK_CC_CANDIDATE -dumpversion | cut -d. -f1)"
    SDK_GXX_MAJOR="$($SDK_CXX_CANDIDATE -dumpversion | cut -d. -f1)"
    if [ "$SDK_CC_MAJOR" -ge "$SDK_CXX_MAJOR" ] && [ "$SDK_CC_MAJOR" = "$SDK_GXX_MAJOR" ]; then
        SDK_CC="$SDK_CC_CANDIDATE"
        SDK_CXX="$SDK_CXX_CANDIDATE"
        break
    fi
done
if [ -z "$SDK_CC" ]; then
    echo "ERROR: AArch64 GCC/G++ $SDK_CXX_MAJOR 이상이 필요합니다. setup_host.sh를 실행하십시오."
    return 1
fi
SDK_GCC_INCLUDE="$($SDK_CC -print-file-name=include)"

SDK_COMMON_FLAGS="--sysroot=$SYSROOT -B$SDK_MULTIARCH_LIB/ -nostdinc"
SDK_C_HEADERS="-isystem $SDK_GCC_INCLUDE -isystem $SYSROOT/usr/include/aarch64-linux-gnu -isystem $SYSROOT/usr/include"
SDK_CXX_HEADERS="-isystem $SYSROOT/usr/include/c++/$SDK_CXX_VERSION -isystem $SYSROOT/usr/include/aarch64-linux-gnu/c++/$SDK_CXX_VERSION -isystem $SYSROOT/usr/include/c++/$SDK_CXX_VERSION/backward"
SDK_LINK_FLAGS="-Wl,-rpath-link,$SDK_MULTIARCH_LIB"
export CC="$SDK_CC $SDK_COMMON_FLAGS $SDK_C_HEADERS $SDK_LINK_FLAGS"
export CXX="$SDK_CXX $SDK_COMMON_FLAGS $SDK_CXX_HEADERS $SDK_C_HEADERS $SDK_LINK_FLAGS"

if [ -f "$SYSROOT/opt/onnxruntime/include/onnxruntime_cxx_api.h" ] &&
   [ -f "$SYSROOT/opt/onnxruntime/lib/libonnxruntime.so" ]; then
    export ONNXRUNTIME_ROOT="$SYSROOT/opt/onnxruntime"
fi

export AR="aarch64-linux-gnu-ar"
export AS="aarch64-linux-gnu-as"
export LD="aarch64-linux-gnu-ld"
export NM="aarch64-linux-gnu-nm"
export STRIP="aarch64-linux-gnu-strip"
export RANLIB="aarch64-linux-gnu-ranlib"
export OBJCOPY="aarch64-linux-gnu-objcopy"
export OBJDUMP="aarch64-linux-gnu-objdump"
export GDB="gdb-multiarch"

export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"

# setup_sdk.sh가 성공했는데 실제 make 단계에서 처음 SQLite 누락이 드러나는 일을
# 방지함. 헤더, 링크용 libsqlite3.so와 ARM 링크를 여기서 한 번에 검증함.
if [ ! -f "$SYSROOT/usr/include/sqlite3.h" ]; then
    echo "ERROR: selected sysroot에 sqlite3.h가 없습니다: $SYSROOT"
    return 1
fi

SQLITE_LINK_LIBRARY="$(
    find "$SYSROOT" \
        \( -type f -o -type l \) \
        -name 'libsqlite3.so' \
        -print \
        -quit
)"

if [ -z "$SQLITE_LINK_LIBRARY" ]; then
    echo "ERROR: selected sysroot에 링크용 libsqlite3.so가 없습니다: $SYSROOT"
    echo "libsqlite3.so.0만 있고 libsqlite3.so가 없으면 SDK 개발 파일이 불완전합니다."
    return 1
fi

SQLITE_CHECK_BIN="${TMPDIR:-/tmp}/ace-sdk-sqlite-check.$$"

if ! printf '%s\n' \
        '#include <sqlite3.h>' \
        'int main(void) { return sqlite3_libversion_number() > 0 ? 0 : 1; }' \
    | $CC -std=c11 -x c - -lsqlite3 -o "$SQLITE_CHECK_BIN"; then
    rm -f "$SQLITE_CHECK_BIN"
    echo "ERROR: selected sysroot로 SQLite ARM 링크 시험에 실패했습니다."
    echo "Sysroot: $SYSROOT"
    return 1
fi

rm -f "$SQLITE_CHECK_BIN"

#--------------------------------------------------
# Done
#--------------------------------------------------
echo "[2/2] Completed."

echo ""
echo "Cross Compiler : $SDK_CC / $SDK_CXX"
echo "Sysroot        : $SYSROOT"
echo "SQLite header  : $SYSROOT/usr/include/sqlite3.h"
echo "SQLite library : $SQLITE_LINK_LIBRARY"

echo ""
echo "========================================"
echo "SDK environment is ready."
echo "========================================"
