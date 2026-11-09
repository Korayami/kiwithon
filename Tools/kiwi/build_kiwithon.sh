#!/usr/bin/env bash
# =============================================================================
# Kiwithon End-to-End Build Pipeline (Tools/kiwi/build_kiwithon.sh)
#
# Supports:
#   - Raspberry Pi 5 (aarch64 ARM Cortex-A76)
#   - x86_64 Linux workstations / servers
#
# Pipeline Stages:
#   1. AOT Jinja2 Template Precompilation (if KIWITHON_APP_DIR is set)
#   2. Regenerate Frozen Modules (hot stdlib + optional application bundle)
#   3. Architecture-tuned Configure (LTO, Tail-Call Interp, Tier-2 JIT, mimalloc, PGO/BOLT)
#   4. PGO + BOLT Instrumented Build & Profiling Workload (Tools/kiwi/bolt_workload.py)
#   5. Verification of ./kiwithon binary
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

HOST_ARCH="${KIWITHON_TARGET_ARCH:-$(uname -m)}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
PYTHON_FOR_REGEN="${PYTHON_FOR_REGEN:-python3}"

echo "=================================================================="
echo " Kiwithon Build Pipeline"
echo " Repository Root : ${REPO_ROOT}"
echo " Target Arch     : ${HOST_ARCH}"
echo " Parallel Jobs   : ${JOBS}"
echo " App Bundle Dir  : ${KIWITHON_APP_DIR:-<none (stdlib-only freeze)>}"
echo "=================================================================="

# -----------------------------------------------------------------------------
# 1. AOT-compile Jinja2 templates if KIWITHON_APP_DIR is provided
# -----------------------------------------------------------------------------
if [[ -n "${KIWITHON_APP_DIR:-}" && -d "${KIWITHON_APP_DIR}" ]]; then
    echo "[1/5] Precompiling Jinja2 templates from ${KIWITHON_APP_DIR}..."
    "${PYTHON_FOR_REGEN}" "${REPO_ROOT}/Tools/kiwi/compile_jinja.py" \
        "${KIWITHON_APP_DIR}" \
        -o "${KIWITHON_APP_DIR}/build/_kiwi_precompiled_templates.py"
else
    echo "[1/5] KIWITHON_APP_DIR not set; skipping Jinja2 template AOT precompilation."
fi

# -----------------------------------------------------------------------------
# 2. Freeze hot stdlib + optional KiwiSpaceONE application modules
# -----------------------------------------------------------------------------
echo "[2/5] Running Tools/build/freeze_modules.py..."
"${PYTHON_FOR_REGEN}" "${REPO_ROOT}/Tools/build/freeze_modules.py"

# -----------------------------------------------------------------------------
# 3. Select Architecture-Specific Compiler & Linker Flags
# -----------------------------------------------------------------------------
case "${HOST_ARCH}" in
    aarch64|arm64)
        echo "[3/5] Configuring for Raspberry Pi 5 (aarch64 ARM Cortex-A76)..."
        ARCH_CFLAGS="-O3 -mcpu=cortex-a76+crypto+crc+dotprod+fp16 -mtune=cortex-a76 -falign-functions=64 -fno-semantic-interposition -fomit-frame-pointer"
        ARCH_LDFLAGS="-Wl,--no-fix-cortex-a53-843419 -Wl,-Bsymbolic-functions -Wl,--icf=safe"
        ;;
    x86_64|amd64)
        echo "[3/5] Configuring for x86_64..."
        ARCH_CFLAGS="-O3 -march=native -mtune=native -falign-functions=32 -fno-semantic-interposition -fomit-frame-pointer"
        ARCH_LDFLAGS="-Wl,-Bsymbolic-functions -Wl,--icf=safe"
        ;;
    *)
        echo "[3/5] Configuring for generic architecture (${HOST_ARCH})..."
        ARCH_CFLAGS="-O3 -fno-semantic-interposition -fomit-frame-pointer"
        ARCH_LDFLAGS="-Wl,-Bsymbolic-functions"
        ;;
esac

# Verify linker support for --icf=safe (GNU ld vs lld/gold) and fallback cleanly if unsupported
if ! ${CC:-cc} ${ARCH_LDFLAGS} -xc - -o /dev/null <<< "int main(void){return 0;}" 2>/dev/null; then
    ARCH_LDFLAGS="${ARCH_LDFLAGS//-Wl,--icf=safe/}"
fi

# Ensure Modules/Setup.local has static linkage entries for _kiwi_native and _kiwi_markdown
cat > "${REPO_ROOT}/Modules/Setup.local" << 'EOF'
# -*- makefile -*-
*static*
_kiwi_native _kiwi_nativemodule.c
_kiwi_markdown _kiwi_markdownmodule.c _kiwi_md4c/kiwi_md.c
EOF

# Detect Clang/LLVM toolchain for --with-tail-call-interp and --with-lto=full
CONFIGURE_ARGS=(
    "--enable-optimizations"
    "--with-mimalloc"
    "--without-doc-strings"
)

# Check if compiler supports --with-tail-call-interp (Clang 19+ / GCC 15+ preserve_none/musttail)
if [[ "${KIWITHON_DISABLE_TAIL_CALL:-0}" != "1" ]]; then
    if ${CC:-cc} -Werror -xc - -o /dev/null <<< "__attribute__((preserve_none)) void f(void){} int main(void){return 0;}" 2>/dev/null; then
        CONFIGURE_ARGS+=("--with-tail-call-interp")
    else
        echo "  [info] Compiler does not support preserve_none/musttail; omitting --with-tail-call-interp."
    fi
fi

# Check if LLVM/Clang toolchain is available for Tier-2 JIT and LTO=full
if [[ "${KIWITHON_DISABLE_JIT:-0}" != "1" ]]; then
    CONFIGURE_ARGS+=("--enable-experimental-jit=yes")
fi

if ${CC:-cc} --version 2>/dev/null | grep -qi clang; then
    CONFIGURE_ARGS+=("--with-lto=full")
else
    CONFIGURE_ARGS+=("--with-lto")
fi

# Graceful detection for --enable-bolt (requires llvm-bolt and perf2bolt / merge-fdata)
HAVE_BOLT=0
if command -v llvm-bolt >/dev/null 2>&1 && (command -v perf2bolt >/dev/null 2>&1 || command -v merge-fdata >/dev/null 2>&1); then
    echo "  [info] llvm-bolt detected; enabling BOLT post-link optimization (--enable-bolt)."
    CONFIGURE_ARGS+=("--enable-bolt")
    HAVE_BOLT=1
else
    echo "  [info] llvm-bolt not found on PATH; falling back to PGO+LTO without BOLT."
fi

# Custom profiling task driving PGO and BOLT via Tools/kiwi/bolt_workload.py
PROFILE_TASK="${REPO_ROOT}/Tools/kiwi/bolt_workload.py"

echo "  CFLAGS  = ${ARCH_CFLAGS}"
echo "  LDFLAGS = ${ARCH_LDFLAGS}"
echo "  ARGS    = ${CONFIGURE_ARGS[*]}"

./configure \
    CFLAGS="${ARCH_CFLAGS} ${CFLAGS:-}" \
    LDFLAGS="${ARCH_LDFLAGS} ${LDFLAGS:-}" \
    PROFILE_TASK="${PROFILE_TASK}" \
    "${CONFIGURE_ARGS[@]}"

# -----------------------------------------------------------------------------
# 4. Build ./kiwithon (with PGO + optional BOLT training)
# -----------------------------------------------------------------------------
echo "[4/5] Building ./kiwithon (${JOBS} jobs)..."
if [[ "${KIWITHON_SKIP_PGO:-0}" == "1" ]]; then
    make -j"${JOBS}" BUILDPYTHON=kiwithon PROFILE_TASK="${PROFILE_TASK}" build_all
else
    make -j"${JOBS}" BUILDPYTHON=kiwithon PROFILE_TASK="${PROFILE_TASK}" profile-opt
    if [[ "${HAVE_BOLT}" == "1" ]] && grep -q "^bolt-opt:" Makefile 2>/dev/null; then
        echo "[4b/5] Applying BOLT binary layout optimization..."
        make -j"${JOBS}" BUILDPYTHON=kiwithon PROFILE_TASK="${PROFILE_TASK}" bolt-opt || \
            echo "  [warn] BOLT step encountered non-fatal environment constraint; retaining PGO+LTO ./kiwithon binary."
    fi
fi

# Ensure ./kiwithon exists if Makefile target produced ./python
if [[ ! -x "${REPO_ROOT}/kiwithon" && -x "${REPO_ROOT}/python" ]]; then
    cp -f "${REPO_ROOT}/python" "${REPO_ROOT}/kiwithon"
fi

# -----------------------------------------------------------------------------
# 5. Smoke Test & Summary
# -----------------------------------------------------------------------------
echo "[5/5] Verifying ./kiwithon binary..."
if [[ -x "${REPO_ROOT}/kiwithon" ]]; then
    "${REPO_ROOT}/kiwithon" -VV
    "${REPO_ROOT}/kiwithon" -c "import sys, _kiwi_native, _kiwi_markdown; print('Kiwithon extensions verified:', hasattr(sys, '_kiwi_immortalize_heap'), hasattr(_kiwi_native, 'scan_storage_tree'), hasattr(_kiwi_markdown, 'render'))"
    echo "=================================================================="
    echo " Build Complete: ${REPO_ROOT}/kiwithon"
    echo "=================================================================="
else
    echo "ERROR: ./kiwithon binary was not produced." >&2
    exit 1
fi
