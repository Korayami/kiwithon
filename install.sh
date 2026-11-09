#!/bin/sh
# Kiwithon Installer Script
# One-liner: curl -fsSL https://raw.githubusercontent.com/Korayami/kiwithon/main/install.sh | sh
# Source build: curl -fsSL https://raw.githubusercontent.com/Korayami/kiwithon/main/install.sh | sh -s -- --raw

set -e

REPO_DEFAULT="Korayami/kiwithon"
REPO_URL_DEFAULT="https://github.com/${REPO_DEFAULT}.git"
RELEASE_URL_DEFAULT="https://api.github.com/repos/${REPO_DEFAULT}/releases/latest"

info() {
    printf "\033[1;34m[kiwithon-installer]\033[0m %s\n" "$*"
}

warn() {
    printf "\033[1;33m[kiwithon-installer:warn]\033[0m %s\n" "$*" >&2
}

error() {
    printf "\033[1;31m[kiwithon-installer:error]\033[0m %s\n" "$*" >&2
}

die() {
    error "$*"
    exit 1
}

# Determine default prefix
default_prefix() {
    if [ -n "$KIWITHON_PREFIX" ]; then
        echo "$KIWITHON_PREFIX"
    elif [ "$(id -u)" -eq 0 ]; then
        echo "/usr/local"
    elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
        echo "/usr/local"
    else
        echo "$HOME/.local"
    fi
}

# Default values
PREFIX="$(default_prefix)"
RAW_BUILD="${KIWITHON_RAW:-0}"
REPO="${KIWITHON_REPO:-$REPO_URL_DEFAULT}"
REPO_DIR="${KIWITHON_REPO_DIR:-$HOME/.kiwithon/src}"
BRANCH=""
JOBS=""
CLEAN_BUILD=0

# Detect CPU count
detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu 2>/dev/null || echo 4
    else
        echo 4
    fi
}

usage() {
    cat <<EOF
Kiwithon Installer

Usage:
  install.sh [options]
  curl -fsSL https://raw.githubusercontent.com/Korayami/kiwithon/main/install.sh | sh
  curl -fsSL https://raw.githubusercontent.com/Korayami/kiwithon/main/install.sh | sh -s -- [options]

Options:
  --prefix <dir>         Install prefix directory (default: /usr/local or ~/.local)
  --raw, --from-source   Compile from source repository instead of fetching prebuilt binary
  --repo <url>           Git repository URL or owner/repo (default: ${REPO_DEFAULT})
  --repo-dir <dir>       Source directory for git checkout (default: ~/.kiwithon/src)
  --branch <name>        Git branch to checkout when building from source
  -j, --jobs <N>         Number of parallel build jobs for make (default: auto)
  --clean                Run 'make clean' before compiling when building from source
  -h, --help             Display this help message

Environment variables:
  KIWITHON_PREFIX        Installation prefix directory
  KIWITHON_RAW           Set to 1 to build from source
  KIWITHON_REPO          Repository URL
  KIWITHON_REPO_DIR      Source clone directory
EOF
}

# Parse options
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            [ -n "$2" ] || die "--prefix requires an argument"
            PREFIX="$2"
            shift 2
            ;;
        --raw|--from-source)
            RAW_BUILD=1
            shift
            ;;
        --repo)
            [ -n "$2" ] || die "--repo requires an argument"
            REPO="$2"
            shift 2
            ;;
        --repo-dir)
            [ -n "$2" ] || die "--repo-dir requires an argument"
            REPO_DIR="$2"
            shift 2
            ;;
        --branch)
            [ -n "$2" ] || die "--branch requires an argument"
            BRANCH="$2"
            shift 2
            ;;
        -j|--jobs)
            [ -n "$2" ] || die "$1 requires an argument"
            JOBS="$2"
            shift 2
            ;;
        --clean)
            CLEAN_BUILD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "Unknown option: $1 (run with -h for help)"
            ;;
    esac
done

[ -n "$JOBS" ] || JOBS="$(detect_jobs)"

# Check write permissions for target prefix
check_sudo() {
    target_dir="$1"
    mkdir -p "$target_dir" 2>/dev/null || true
    if [ -w "$target_dir" ]; then
        echo ""
    elif command -v sudo >/dev/null 2>&1; then
        echo "sudo"
    else
        die "Directory $target_dir is not writable and 'sudo' is not installed."
    fi
}

SUDO_CMD="$(check_sudo "$PREFIX")"

# Download helper
fetch() {
    url="$1"
    dest="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$dest"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$dest" "$url"
    else
        die "Neither curl nor wget was found. Please install curl or wget."
    fi
}

# Detect OS and architecture
detect_platform() {
    os_raw="$(uname -s)"
    case "$os_raw" in
        Linux)  OS="linux" ;;
        Darwin) OS="macos" ;;
        *)      OS="$(echo "$os_raw" | tr '[:upper:]' '[:lower:]')" ;;
    esac

    arch_raw="$(uname -m)"
    case "$arch_raw" in
        x86_64|amd64)
            ARCH="x86_64"
            ;;
        aarch64|arm64)
            if [ "$OS" = "macos" ]; then
                ARCH="arm64"
            else
                ARCH="aarch64"
            fi
            ;;
        *)
            ARCH="$arch_raw"
            ;;
    esac
}

install_from_source() {
    info "Building Kiwithon from source..."
    if ! command -v git >/dev/null 2>&1; then
        die "'git' is required to build from source. Please install git."
    fi
    if ! command -v make >/dev/null 2>&1; then
        die "'make' is required to build from source. Please install make."
    fi
    if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1; then
        die "A C compiler (cc, gcc, or clang) is required to build from source."
    fi

    # Normalize git repo URL if given as owner/repo
    case "$REPO" in
        http://*|https://*|git@*|/*)
            REPO_SRC="$REPO"
            ;;
        *)
            REPO_SRC="https://github.com/${REPO}.git"
            ;;
    esac

    if [ -d "$REPO_DIR/.git" ]; then
        info "Updating existing checkout at $REPO_DIR..."
        (cd "$REPO_DIR" && git fetch --all --prune)
        if [ -n "$BRANCH" ]; then
            (cd "$REPO_DIR" && git checkout "$BRANCH")
        fi
        (cd "$REPO_DIR" && git pull --ff-only)
    else
        info "Cloning $REPO_SRC into $REPO_DIR..."
        mkdir -p "$(dirname "$REPO_DIR")"
        if [ -n "$BRANCH" ]; then
            git clone --branch "$BRANCH" "$REPO_SRC" "$REPO_DIR"
        else
            git clone "$REPO_SRC" "$REPO_DIR"
        fi
    fi

    info "Configuring build (prefix: $PREFIX)..."
    (
        cd "$REPO_DIR"
        if [ ! -f "configure" ]; then
            die "configure script not found in $REPO_DIR"
        fi
        ./configure --prefix="$PREFIX"
        if [ "$CLEAN_BUILD" -eq 1 ]; then
            make clean
        fi
        info "Compiling with $JOBS jobs..."
        make -j"$JOBS"
        info "Installing Kiwithon into $PREFIX..."
        if [ -n "$SUDO_CMD" ]; then
            $SUDO_CMD make install
        else
            make install
        fi
    )
}

install_prebuilt_binary() {
    detect_platform
    info "Target platform: $OS-$ARCH"
    info "Target prefix: $PREFIX"

    TMP_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t 'kiwithon-install')"
    trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

    TARBALL="kiwithon-${OS}-${ARCH}.tar.gz"
    DOWNLOAD_URL="https://github.com/${REPO_DEFAULT}/releases/latest/download/${TARBALL}"

    info "Attempting to download prebuilt binary: $DOWNLOAD_URL..."
    if fetch "$DOWNLOAD_URL" "$TMP_DIR/$TARBALL" 2>/dev/null; then
        info "Extracting $TARBALL to $PREFIX..."
        mkdir -p "$PREFIX"
        if [ -n "$SUDO_CMD" ]; then
            $SUDO_CMD tar -xzf "$TMP_DIR/$TARBALL" -C "$PREFIX"
        else
            tar -xzf "$TMP_DIR/$TARBALL" -C "$PREFIX"
        fi
    else
        warn "Prebuilt binary not found or download failed. Falling back to source build..."
        install_from_source
    fi
}

main() {
    info "Kiwithon Installation"
    if [ "$RAW_BUILD" -eq 1 ]; then
        install_from_source
    else
        install_prebuilt_binary
    fi

    BIN="$PREFIX/bin/kiwithon"
    if [ -x "$BIN" ]; then
        info "Installation complete! Verifying:"
        "$BIN" --version

        # Check if $PREFIX/bin is in PATH
        case ":$PATH:" in
            *:"$PREFIX/bin":*) ;;
            *)
                printf "\n"
                warn "NOTE: %s/bin is not currently in your PATH." "$PREFIX"
                warn "To use kiwithon from any terminal, add this to your shell config (~/.bashrc, ~/.zshrc):"
                printf "    export PATH=\"%s/bin:\$PATH\"\n\n" "$PREFIX"
                ;;
        esac
    else
        warn "Kiwithon executable was not found at $BIN."
    fi
}

main
