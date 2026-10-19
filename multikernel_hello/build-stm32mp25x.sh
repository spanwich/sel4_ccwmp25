#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VENV="${SEL4_VENV:-$WORKSPACE/sel4-dev-env}"
if [ ! -f "$VENV/bin/activate" ] &&
   [ -f "$WORKSPACE/../../sel4-dev-env/bin/activate" ]; then
    VENV="$(cd "$WORKSPACE/../.." && pwd)/sel4-dev-env"
fi

K1_BUILD="$WORKSPACE/build-ccwmp25-mk-hello-k1"
K0_BUILD="$WORKSPACE/build-ccwmp25-mk-hello-k0"
ROOTSERVER_BUILD_ID="ccwmp25-mk-n4c-v16"
CROSS_PREFIX="aarch64-linux-gnu-"
NM="${CROSS_PREFIX}nm"
MAX_TRAMPOLINE_PASSES=3

if [ "${1:-build}" = "clean" ]; then
    rm -rf "$K1_BUILD" "$K0_BUILD"
    echo "Removed $K1_BUILD and $K0_BUILD"
    exit 0
fi

if [ ! -f "$VENV/bin/activate" ]; then
    echo "ERROR: missing venv at $VENV" >&2
    exit 2
fi

if ! command -v "$NM" >/dev/null 2>&1; then
    echo "ERROR: missing symbol reader: $NM" >&2
    exit 2
fi

# shellcheck disable=SC1091
source "$VENV/bin/activate"

COMMON_ARGS=(
    -G Ninja
    -DCMAKE_TOOLCHAIN_FILE="$WORKSPACE/kernel/gcc.cmake"
    -DCROSS_COMPILER_PREFIX="$CROSS_PREFIX"
    -DROOTSERVER_BUILD_ID="$ROOTSERVER_BUILD_ID"
    -DSEL4_CACHE_DIR="$WORKSPACE/.sel4_cache"
)

configure_kernel() {
    local build_dir="$1"
    local kid="$2"
    local dts="$3"
    shift 3

    mkdir -p "$build_dir"
    (
        cd "$build_dir"
        cmake "${COMMON_ARGS[@]}" \
            -DMK_KERNEL_ID="$kid" \
            -DKernelCustomDTS="$dts" \
            "$@" \
            "$SCRIPT_DIR"
    )
}

verify_prepared_secondary_config() {
    local build_dir="$1"
    local expected="$2"
    local config="$build_dir/kernel/gen_config/kernel/gen_config.h"

    [ -f "$config" ] || { echo "ERROR: missing kernel config: $config" >&2; exit 4; }
    if [ "$expected" = "enabled" ]; then
        grep -q '^#define CONFIG_ARM_PREPARED_SECONDARY_BOOT  1$' "$config" || {
            echo "ERROR: expected CONFIG_ARM_PREPARED_SECONDARY_BOOT=1 in $config" >&2
            exit 4
        }
    else
        if grep -q '^#define CONFIG_ARM_PREPARED_SECONDARY_BOOT  1$' "$config"; then
            echo "ERROR: K0 unexpectedly has CONFIG_ARM_PREPARED_SECONDARY_BOOT=1 in $config" >&2
            exit 4
        fi
    fi
}

verify_rootserver_build_id() {
    local artifact="$1"

    grep -aFq "$ROOTSERVER_BUILD_ID" "$artifact" || {
        echo "ERROR: missing rootserver build id $ROOTSERVER_BUILD_ID in $artifact" >&2
        exit 5
    }
}

find_trampoline_paddr() {
    local elf="$1"

    "$NM" -n "$elf" |
        awk '$3 == "mk_core1_trampoline" && !found {
                 value = "0x" $1;
                 found = 1
             }
             END {
                 if (!found) {
                     exit 1
                 }
                 print value
             }'
}

echo "==> Configure/build K1 hello rootserver"
configure_kernel "$K1_BUILD" 1 "$SCRIPT_DIR/dts/stm32mp25x-k1.dts" \
    -DKernelArmPreparedSecondaryBoot=ON
( cd "$K1_BUILD" && ninja )
verify_prepared_secondary_config "$K1_BUILD" enabled

K1_KERNEL_ELF="$K1_BUILD/kernel/kernel.elf"
K1_ROOTSERVER="$K1_BUILD/rootserver/rootserver_hello"
K1_DTB="$K1_BUILD/kernel/kernel.dtb"

for artifact in "$K1_KERNEL_ELF" "$K1_ROOTSERVER" "$K1_DTB"; do
    [ -f "$artifact" ] || { echo "ERROR: missing K1 artifact: $artifact" >&2; exit 3; }
done
verify_rootserver_build_id "$K1_ROOTSERVER"

echo "==> Configure/build K0 bundle and derive its linked K1 trampoline address"
K0_CONFIG_ARGS=(
    -DCCWMP25HelloMultikernel=ON
    -DCCWMP25HelloMultikernelCount=2
    -DCCWMP25HelloMultikernelDispatch=OFF
    -DCCWMP25HelloMultikernelDispatchAfterMmu=OFF
    -DMULTIKERNEL_K1_KERNEL_ELF="$K1_KERNEL_ELF"
    -DMULTIKERNEL_K1_ROOTSERVER="$K1_ROOTSERVER"
    -DMULTIKERNEL_K1_DTB="$K1_DTB"
)

trampoline_paddr=0x0
trampoline_converged=0
for pass in $(seq 1 "$MAX_TRAMPOLINE_PASSES"); do
    echo "    trampoline link pass $pass: rootserver expects $trampoline_paddr"
    configure_kernel "$K0_BUILD" 0 "$SCRIPT_DIR/dts/stm32mp25x-k0.dts" \
        -DMK_CORE1_TRAMPOLINE_PADDR="$trampoline_paddr" \
        "${K0_CONFIG_ARGS[@]}"
    ( cd "$K0_BUILD" && ninja )

    K0_ELFLOADER="$K0_BUILD/elfloader-tool/elfloader"
    [ -f "$K0_ELFLOADER" ] || {
        echo "ERROR: missing K0 elfloader: $K0_ELFLOADER" >&2
        exit 3
    }
    linked_trampoline_paddr="$(find_trampoline_paddr "$K0_ELFLOADER")"
    echo "    linked mk_core1_trampoline=$linked_trampoline_paddr"

    if [ "$trampoline_paddr" != "0x0" ] &&
       [ "$linked_trampoline_paddr" = "$trampoline_paddr" ]; then
        trampoline_converged=1
        break
    fi
    trampoline_paddr="$linked_trampoline_paddr"
done

if [ "$trampoline_converged" != "1" ]; then
    echo "ERROR: trampoline/rootserver link contract did not converge after $MAX_TRAMPOLINE_PASSES passes" >&2
    exit 6
fi

configured_trampoline_paddr="$(
    sed -n 's/^MK_CORE1_TRAMPOLINE_PADDR:[^=]*=//p' "$K0_BUILD/CMakeCache.txt"
)"
if [ "$configured_trampoline_paddr" != "$linked_trampoline_paddr" ]; then
    echo "ERROR: K0 rootserver expects $configured_trampoline_paddr but elfloader linked $linked_trampoline_paddr" >&2
    exit 6
fi

verify_prepared_secondary_config "$K0_BUILD" disabled
verify_rootserver_build_id "$K0_BUILD/rootserver/rootserver_hello"

set -- "$K0_BUILD"/images/rootserver_hello-image-arm-stm32mp*
if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
    echo "ERROR: expected exactly one final STM32MP2 image" >&2
    exit 7
fi
FINAL_IMAGE="$1"

echo
echo "Final image: $FINAL_IMAGE"
echo "K1 trampoline handoff: $linked_trampoline_paddr (derived and verified)"
ls -lh "$FINAL_IMAGE"
