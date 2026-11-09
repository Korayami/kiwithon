"""Kiwithon self-update engine.

Provides functionality for:
  - kiwithon --update: fetches the latest prebuilt binary from GitHub Releases
    and installs it in the system.
  - kiwithon --raw-update: pulls the latest source repository, rebuilds kiwithon,
    and installs the freshly built binary.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import sysconfig
import tarfile
import tempfile
import urllib.error
import urllib.request
from typing import Sequence

DEFAULT_REPO = "Korayami/kiwithon"
DEFAULT_GIT_URL = f"https://github.com/{DEFAULT_REPO}.git"
DEFAULT_RELEASE_API = f"https://api.github.com/repos/{DEFAULT_REPO}/releases/latest"


def _log(msg: str) -> None:
    print(f"[kiwithon-update] {msg}", flush=True)


def _err(msg: str) -> None:
    print(f"[kiwithon-update:error] {msg}", file=sys.stderr, flush=True)


def get_platform_info() -> tuple[str, str]:
    """Return normalized (os_name, arch_name)."""
    system = platform.system().lower()
    if system == "darwin":
        os_name = "macos"
    elif system == "linux":
        os_name = "linux"
    elif system == "windows":
        os_name = "windows"
    else:
        os_name = system

    machine = platform.machine().lower()
    if machine in ("x86_64", "amd64"):
        arch_name = "x86_64"
    elif machine in ("aarch64", "arm64"):
        arch_name = "arm64" if os_name == "macos" else "aarch64"
    else:
        arch_name = machine

    return os_name, arch_name


def can_write_dir(path: str) -> bool:
    """Check if the directory is writable, creating parent if necessary."""
    cur = os.path.abspath(path)
    while cur and not os.path.exists(cur):
        parent = os.path.dirname(cur)
        if parent == cur:
            break
        cur = parent
    return os.access(cur, os.W_OK)


def run_cmd(
    cmd: list[str],
    cwd: str | None = None,
    env: dict[str, str] | None = None,
    need_sudo: bool = False,
    dry_run: bool = False,
) -> int:
    """Run a command with optional sudo and logging."""
    full_cmd = list(cmd)
    if need_sudo and os.geteuid() != 0:
        if shutil.which("sudo"):
            full_cmd = ["sudo"] + full_cmd
        else:
            _err("Root privileges required but 'sudo' was not found.")
            return 1

    cmd_str = " ".join(full_cmd)
    _log(f"Running: {cmd_str}")
    if dry_run:
        _log(f"(dry-run) would run: {cmd_str}")
        return 0

    res = subprocess.run(full_cmd, cwd=cwd, env=env)
    return res.returncode


def get_default_prefix() -> str:
    """Determine the default installation prefix."""
    env_prefix = os.environ.get("KIWITHON_PREFIX")
    if env_prefix:
        return os.path.abspath(env_prefix)

    prefix = sys.exec_prefix
    # If currently running from a build directory (e.g. contains Makefile and python binary),
    # default to /usr/local or ~/.local instead of overwriting repo tree
    if os.path.isfile(os.path.join(prefix, "Makefile.pre.in")):
        if os.geteuid() == 0 or can_write_dir("/usr/local/bin"):
            return "/usr/local"
        return os.path.expanduser("~/.local")

    return prefix


def get_default_repo_dir() -> str:
    """Determine default directory for source checkout in raw-update."""
    env_repo = os.environ.get("KIWITHON_REPO_DIR")
    if env_repo:
        return os.path.abspath(env_repo)

    # Check if currently running binary knows its build srcdir
    srcdir = sysconfig.get_config_var("srcdir") or sysconfig.get_config_var("abs_srcdir")
    if srcdir and os.path.isdir(os.path.join(srcdir, ".git")):
        return os.path.abspath(srcdir)

    return os.path.expanduser("~/.kiwithon/src")


def download_file(url: str, dest_path: str) -> None:
    """Download a file with user-agent headers to dest_path."""
    headers = {"User-Agent": "Kiwithon-Updater"}
    gh_token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if gh_token:
        headers["Authorization"] = f"token {gh_token}"

    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req) as resp, open(dest_path, "wb") as out_file:
        shutil.copyfileobj(resp, out_file)


def fetch_release_info(repo: str) -> dict:
    """Fetch the latest release information for repo."""
    custom_url = os.environ.get("KIWITHON_RELEASE_URL")
    url = custom_url or f"https://api.github.com/repos/{repo}/releases/latest"

    headers = {
        "Accept": "application/vnd.github.v3+json",
        "User-Agent": "Kiwithon-Updater",
    }
    gh_token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if gh_token:
        headers["Authorization"] = f"token {gh_token}"

    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req) as resp:
        data = resp.read().decode("utf-8")
        return json.loads(data)


def compute_sha256(filepath: str) -> str:
    """Compute the SHA256 hex digest of a file."""
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def update_binary(args: argparse.Namespace) -> int:
    """Handle kiwithon --update: download and install prebuilt binary."""
    _log("Starting Kiwithon binary update...")
    os_name, arch_name = get_platform_info()
    _log(f"Detected platform: {os_name}-{arch_name}")

    prefix = os.path.abspath(args.prefix or get_default_prefix())
    _log(f"Target install prefix: {prefix}")

    need_sudo = not can_write_dir(prefix)
    if need_sudo and os.geteuid() != 0 and not shutil.which("sudo"):
        _err(f"Cannot write to {prefix} and 'sudo' is not available.")
        return 1

    repo = args.repo or os.environ.get("KIWITHON_REPO", DEFAULT_REPO)
    try:
        release_info = fetch_release_info(repo)
    except Exception as e:
        _err(f"Failed to query latest release from GitHub ({repo}): {e}")
        _log("Falling back to standard release asset URL format...")
        release_info = {
            "tag_name": "latest",
            "assets": [],
        }

    tag_name = release_info.get("tag_name", "latest")
    _log(f"Latest release: {tag_name}")

    if args.check:
        _log(f"Update check finished. Latest release is {tag_name}.")
        return 0

    # Look for matching asset in release_info
    assets = release_info.get("assets", [])
    expected_asset_names = [
        f"kiwithon-{os_name}-{arch_name}.tar.gz",
        f"kiwithon-{tag_name}-{os_name}-{arch_name}.tar.gz",
        f"kiwithon-{os_name}-{arch_name}",
    ]

    target_asset = None
    sha256_asset = None
    for asset in assets:
        name = asset.get("name", "")
        if name in expected_asset_names:
            target_asset = asset
        elif name in ("SHA256SUMS", "SHA256SUMS.txt", "checksums.txt"):
            sha256_asset = asset

    with tempfile.TemporaryDirectory(prefix="kiwithon-update-") as tmpdir:
        archive_path = os.path.join(tmpdir, "archive.tar.gz")
        if target_asset and target_asset.get("browser_download_url"):
            download_url = target_asset["browser_download_url"]
        else:
            download_url = f"https://github.com/{repo}/releases/latest/download/kiwithon-{os_name}-{arch_name}.tar.gz"

        _log(f"Downloading {download_url}...")
        if args.dry_run:
            _log(f"(dry-run) would download {download_url} to {archive_path}")
            _log(f"(dry-run) would unpack to {prefix}")
            return 0

        try:
            download_file(download_url, archive_path)
        except urllib.error.HTTPError as e:
            _err(f"HTTP error downloading release binary: {e}")
            _err("If prebuilt binaries are not available for this release, use 'kiwithon --raw-update' to build from source.")
            return 1
        except Exception as e:
            _err(f"Download failed: {e}")
            return 1

        # Check sha256 if available
        if sha256_asset and sha256_asset.get("browser_download_url"):
            sha_file = os.path.join(tmpdir, "SHA256SUMS")
            try:
                download_file(sha256_asset["browser_download_url"], sha_file)
                computed = compute_sha256(archive_path)
                with open(sha_file, "r") as sf:
                    content = sf.read()
                if computed not in content:
                    _err(f"SHA-256 verification failed! Digest: {computed}")
                    return 1
                _log("SHA-256 verified successfully.")
            except Exception as e:
                _log(f"Warning: checksum check skipped: {e}")

        # Extract archive
        _log(f"Installing archive to {prefix}...")
        try:
            if tarfile.is_tarfile(archive_path):
                # Unpack to temporary staging folder first
                staging = os.path.join(tmpdir, "staged")
                os.makedirs(staging, exist_ok=True)
                with tarfile.open(archive_path, "r:*") as tar:
                    tar.extractall(path=staging)

                # Determine if archive contents are wrapped in a top-level dir or direct
                staged_contents = os.listdir(staging)
                if len(staged_contents) == 1 and os.path.isdir(os.path.join(staging, staged_contents[0])):
                    source_dir = os.path.join(staging, staged_contents[0])
                else:
                    source_dir = staging

                # Copy files into prefix
                cp_cmd = ["cp", "-a", f"{source_dir}/.", prefix]
                res = run_cmd(cp_cmd, need_sudo=need_sudo)
                if res != 0:
                    _err("Failed to copy files into prefix.")
                    return res
            else:
                # Standalone binary
                bin_dir = os.path.join(prefix, "bin")
                os.makedirs(bin_dir, exist_ok=True)
                dest_binary = os.path.join(bin_dir, "kiwithon")
                cp_cmd = ["cp", archive_path, dest_binary]
                res = run_cmd(cp_cmd, need_sudo=need_sudo)
                if res != 0:
                    return res
                chmod_cmd = ["chmod", "755", dest_binary]
                run_cmd(chmod_cmd, need_sudo=need_sudo)

        except Exception as e:
            _err(f"Failed to install binary: {e}")
            return 1

    _log("Kiwithon update completed successfully!")
    installed_bin = os.path.join(prefix, "bin", "kiwithon")
    if os.path.isfile(installed_bin):
        run_cmd([installed_bin, "--version"])
    return 0


def update_raw(args: argparse.Namespace) -> int:
    """Handle kiwithon --raw-update: git pull, rebuild, and install."""
    _log("Starting Kiwithon raw update (rebuild from source)...")

    repo_url = args.repo or os.environ.get("KIWITHON_REPO", DEFAULT_GIT_URL)
    if not repo_url.startswith("http") and not repo_url.startswith("git@"):
        repo_url = f"https://github.com/{repo_url}.git"

    repo_dir = os.path.abspath(args.repo_dir or get_default_repo_dir())
    prefix = os.path.abspath(args.prefix or get_default_prefix())
    branch = args.branch
    jobs = args.jobs or str(os.cpu_count() or 4)

    _log(f"Repository directory: {repo_dir}")
    _log(f"Target install prefix: {prefix}")
    _log(f"Build parallelism: -j{jobs}")

    need_sudo = not can_write_dir(prefix)
    if need_sudo and os.geteuid() != 0 and not shutil.which("sudo"):
        _err(f"Cannot write to {prefix} and 'sudo' is not available.")
        return 1

    if args.dry_run:
        _log(f"(dry-run) would pull {repo_url} (branch: {branch or 'current'}) at {repo_dir}")
        _log(f"(dry-run) would configure with --prefix={prefix}")
        _log(f"(dry-run) would run make -j{jobs} && make install")
        return 0

    # Ensure git is available
    if not shutil.which("git"):
        _err("'git' command not found. Please install git.")
        return 1

    # 1. Clone or pull
    if not os.path.isdir(os.path.join(repo_dir, ".git")):
        _log(f"Cloning {repo_url} into {repo_dir}...")
        os.makedirs(os.path.dirname(repo_dir), exist_ok=True)
        clone_cmd = ["git", "clone", repo_url, repo_dir]
        if branch:
            clone_cmd.extend(["--branch", branch])
        if run_cmd(clone_cmd) != 0:
            _err(f"Failed to clone repository from {repo_url}")
            return 1
    else:
        _log(f"Fetching updates in {repo_dir}...")
        if run_cmd(["git", "fetch", "--all", "--prune"], cwd=repo_dir) != 0:
            _err("Failed to fetch git remotes.")
            return 1

        if branch:
            if run_cmd(["git", "checkout", branch], cwd=repo_dir) != 0:
                _err(f"Failed to checkout branch '{branch}'.")
                return 1

        if run_cmd(["git", "pull", "--ff-only"], cwd=repo_dir) != 0:
            _err("Git pull failed. Local modifications may need to be stashed.")
            return 1

    # 2. Configure
    configure_script = os.path.join(repo_dir, "configure")
    if not os.path.isfile(configure_script):
        _err(f"Configure script not found at {configure_script}.")
        return 1

    config_cmd = ["./configure", f"--prefix={prefix}"]
    if args.enable_optimizations:
        config_cmd.append("--enable-optimizations")
    if args.with_lto:
        config_cmd.append("--with-lto")

    _log(f"Configuring Kiwithon in {repo_dir}...")
    if run_cmd(config_cmd, cwd=repo_dir) != 0:
        _err("Configuration failed.")
        return 1

    # 3. Build
    if args.clean:
        _log("Cleaning previous build artifacts...")
        run_cmd(["make", "clean"], cwd=repo_dir)

    _log(f"Compiling Kiwithon (jobs={jobs})...")
    if run_cmd(["make", f"-j{jobs}"], cwd=repo_dir) != 0:
        _err("Build failed.")
        return 1

    # 4. Install
    _log(f"Installing Kiwithon into {prefix}...")
    install_target = "altinstall" if args.altinstall else "install"
    install_cmd = ["make", install_target]
    if run_cmd(install_cmd, cwd=repo_dir, need_sudo=need_sudo) != 0:
        _err("Installation step failed.")
        return 1

    _log("Kiwithon raw update completed successfully!")
    installed_bin = os.path.join(prefix, "bin", "kiwithon")
    if os.path.isfile(installed_bin):
        run_cmd([installed_bin, "-VV"])
    return 0


def main(raw: bool = False, argv: Sequence[str] | None = None) -> int:
    """CLI entry point for kiwithon update."""
    if argv is None:
        argv = sys.argv[1:]

    prog_name = "kiwithon --raw-update" if raw else "kiwithon --update"
    parser = argparse.ArgumentParser(
        prog=prog_name,
        description="Kiwithon self-updater: update prebuilt binary or rebuild from source.",
    )

    parser.add_argument(
        "--prefix",
        type=str,
        default=None,
        help="Installation prefix directory (default: current prefix or /usr/local).",
    )
    parser.add_argument(
        "--repo",
        type=str,
        default=None,
        help="GitHub repo (e.g. Korayami/kiwithon) or git URL.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Simulate the update process without modifying the system.",
    )
    parser.add_argument(
        "-y",
        "--yes",
        action="store_true",
        help="Assume yes to all prompts.",
    )

    if not raw:
        parser.add_argument(
            "--check",
            action="store_true",
            help="Check for updates without downloading or installing.",
        )
    else:
        parser.add_argument(
            "--repo-dir",
            type=str,
            default=None,
            help="Local directory for git clone (default: ~/.kiwithon/src).",
        )
        parser.add_argument(
            "--branch",
            type=str,
            default=None,
            help="Git branch to build from (default: current branch or main).",
        )
        parser.add_argument(
            "-j",
            "--jobs",
            type=str,
            default=None,
            help="Number of parallel compilation jobs for make.",
        )
        parser.add_argument(
            "--clean",
            action="store_true",
            help="Run 'make clean' before compiling.",
        )
        parser.add_argument(
            "--enable-optimizations",
            action="store_true",
            help="Pass --enable-optimizations to ./configure.",
        )
        parser.add_argument(
            "--with-lto",
            action="store_true",
            help="Pass --with-lto to ./configure.",
        )
        parser.add_argument(
            "--altinstall",
            action="store_true",
            help="Use 'make altinstall' instead of 'make install'.",
        )

    args = parser.parse_args(argv)

    if raw:
        return update_raw(args)
    return update_binary(args)


if __name__ == "__main__":
    sys.exit(main(raw=False))
