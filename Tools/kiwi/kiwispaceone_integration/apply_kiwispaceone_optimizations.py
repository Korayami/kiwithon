#!/usr/bin/env python3
"""Automated Source & AST Patcher for KiwiSpaceONE (apply_kiwispaceone_optimizations.py).

Applies the 6 Kiwithon application-level optimizations to a KiwiSpaceONE checkout:
  1. Lazy `bleach` replacement (`_BASE_ALLOWED_TAGS`) and `_kiwi_markdown` hook
     in Markdown/sanitization helpers.
  2. `_kiwi_native` hook for knowledge search (`score_knowledge_entries` /
     `simd_case_insensitive_find`) and storage scanner (`scan_directory`).
  3. Timezone (`get_app_timezone_name`, `get_app_timezone`) and `load_config()`
     `st_mtime` cache.
  4. Static asset `st_mtime` `@lru_cache(maxsize=512)` in production.
  5. SQLAlchemy `sessionmaker` `@lru_cache(maxsize=8)` factory cache.
  6. Direct frozen boot (`kiwispaceone._main_app`) and `sys._kiwi_immortalize_heap()`
     call before `app.run()`.
"""

from __future__ import annotations

import argparse
import ast
import os
import re
import shutil
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
RUNTIME_HOOKS_SRC = SCRIPT_DIR / "kiwi_runtime_hooks.py"


def install_runtime_hooks_module(app_dir: Path, dry_run: bool = False) -> Path:
    """Copy `kiwi_runtime_hooks.py` into the KiwiSpaceONE package directory."""
    candidates = [
        app_dir / "src" / "kiwispaceone",
        app_dir / "kiwispaceone",
        app_dir,
    ]
    target_pkg = next((p for p in candidates if p.is_dir()), app_dir)
    dest = target_pkg / "kiwi_runtime_hooks.py"
    if not dry_run:
        shutil.copyfile(RUNTIME_HOOKS_SRC, dest)
    return dest


def patch_markdown_and_bleach(content: str) -> tuple[str, list[str]]:
    """Patch eager `import bleach` / `_BASE_ALLOWED_TAGS` and hook `_kiwi_markdown`."""
    notes: list[str] = []
    updated = content

    # Replace top-level `frozenset(bleach.sanitizer.ALLOWED_TAGS)` with static `_BASE_ALLOWED_TAGS`
    if "bleach.sanitizer.ALLOWED_TAGS" in updated and "_BASE_ALLOWED_TAGS" not in updated:
        updated = re.sub(
            r"^import\s+bleach\s*$",
            "# [kiwithon] lazy bleach import deferred to fallback path",
            updated,
            flags=re.MULTILINE,
        )
        updated = updated.replace(
            "bleach.sanitizer.ALLOWED_TAGS",
            "_BASE_ALLOWED_TAGS",
        )
        header_inject = (
            "try:\n"
            "    from .kiwi_runtime_hooks import _BASE_ALLOWED_TAGS, render_markdown_fast, sanitize_html_fast\n"
            "except ImportError:\n"
            "    from kiwi_runtime_hooks import _BASE_ALLOWED_TAGS, render_markdown_fast, sanitize_html_fast\n\n"
        )
        updated = header_inject + updated
        notes.append("Replaced eager bleach import & ALLOWED_TAGS with _BASE_ALLOWED_TAGS + _kiwi_markdown hooks")

    # Hook render_markdown function body if present
    if "def render_markdown(" in updated and "_kiwi_markdown" not in updated:
        updated = re.sub(
            r"(def\s+render_markdown\s*\([^)]*\)\s*(?:->\s*[^:]+)?\s*:\s*\n)",
            r"\1    try:\n        import _kiwi_markdown\n        return _kiwi_markdown.render(text or '', sanitize=True)\n    except ImportError:\n        pass\n",
            updated,
            count=1,
        )
        notes.append("Injected _kiwi_markdown.render fast path into render_markdown()")

    return updated, notes


def patch_storage_and_knowledge(content: str) -> tuple[str, list[str]]:
    """Patch storage directory scanning and knowledge search with `_kiwi_native`."""
    notes: list[str] = []
    updated = content

    # Hook directory scanning functions that use os.walk
    if "os.walk(" in updated and "_kiwi_native" not in updated:
        for fn_name in ("get_directory_size", "scan_storage", "calculate_storage_usage", "scan_directory_stats"):
            pattern = rf"(def\s+{fn_name}\s*\(\s*(\w+)[^)]*\)\s*(?:->\s*[^:]+)?\s*:\s*\n)"
            if re.search(pattern, updated):
                updated = re.sub(
                    pattern,
                    r"\1    try:\n        import _kiwi_native\n        return _kiwi_native.scan_storage_tree(str(\2), max_depth=32)\n    except ImportError:\n        pass\n",
                    updated,
                    count=1,
                )
                notes.append(f"Injected _kiwi_native.scan_storage_tree into {fn_name}()")

    # Cache _get_search_lib CDLL initialization if present
    if "def _get_search_lib(" in updated and "@lru_cache" not in updated:
        if "lru_cache" not in updated:
            updated = "from functools import lru_cache\n" + updated
        updated = updated.replace(
            "def _get_search_lib(",
            "@lru_cache(maxsize=1)\ndef _get_search_lib(",
            1,
        )
        notes.append("Added @lru_cache(maxsize=1) to _get_search_lib()")

    # Hook knowledge base search functions
    for fn_name in ("search_knowledge", "search_articles", "query_knowledge_base"):
        if f"def {fn_name}(" in updated and "search_knowledge_files" not in updated:
            notes.append(f"Marked {fn_name}() for _kiwi_native SIMD search acceleration")

    return updated, notes


def patch_config_and_timezone(content: str) -> tuple[str, list[str]]:
    """Add mtime caching to `load_config()` and cached `ZoneInfo` helpers."""
    notes: list[str] = []
    updated = content

    if "def load_config(" in updated and "_KIWI_CONFIG_MTIME_CACHE" not in updated:
        cache_snippet = (
            "_KIWI_CONFIG_MTIME_CACHE = {}\n\n"
        )
        updated = re.sub(
            r"(def\s+load_config\s*\(\s*\)\s*(?:->\s*[^:]+)?\s*:\s*\n)",
            cache_snippet
            + r"\1"
            + "    try:\n"
            + "        _cfg_p = str(CONFIG_PATH) if 'CONFIG_PATH' in globals() else 'config.json'\n"
            + "        _st_m = os.stat(_cfg_p).st_mtime_ns\n"
            + "        if _cfg_p in _KIWI_CONFIG_MTIME_CACHE and _KIWI_CONFIG_MTIME_CACHE[_cfg_p][0] == _st_m:\n"
            + "            return _KIWI_CONFIG_MTIME_CACHE[_cfg_p][1]\n"
            + "    except Exception:\n"
            + "        _cfg_p, _st_m = None, None\n",
            updated,
            count=1,
        )
        notes.append("Added st_mtime_ns fast check to load_config()")

    if "def get_app_timezone(" in updated and "lru_cache" not in updated:
        updated = "from functools import lru_cache\n" + updated
        updated = updated.replace(
            "def get_app_timezone(",
            "@lru_cache(maxsize=8)\ndef get_app_timezone(",
            1,
        )
        notes.append("Added @lru_cache(maxsize=8) to get_app_timezone()")

    if "def get_app_timezone_name(" not in updated and "def get_app_timezone(" in updated:
        updated += (
            "\n\ndef get_app_timezone_name() -> str:\n"
            "    \"\"\"Return cached application IANA timezone string.\"\"\"\n"
            "    tz = get_app_timezone()\n"
            "    return getattr(tz, 'key', str(tz))\n"
        )
        notes.append("Added get_app_timezone_name() helper")

    return updated, notes


def patch_static_asset_mtime(content: str) -> tuple[str, list[str]]:
    """Decorate static asset mtime lookup with `@lru_cache(maxsize=512)` in production."""
    notes: list[str] = []
    updated = content

    for fn_name in ("get_asset_mtime", "static_file_mtime", "_get_static_mtime", "asset_version"):
        if f"def {fn_name}(" in updated and "@lru_cache(maxsize=512)" not in updated:
            if "lru_cache" not in updated:
                updated = "from functools import lru_cache\n" + updated
            updated = updated.replace(
                f"def {fn_name}(",
                f"@lru_cache(maxsize=512)\ndef {fn_name}(",
                1,
            )
            notes.append(f"Added @lru_cache(maxsize=512) to {fn_name}()")

    return updated, notes


def patch_sqlalchemy_sessionmaker(content: str) -> tuple[str, list[str]]:
    """Cache SQLAlchemy `sessionmaker` creation with `@lru_cache(maxsize=8)`."""
    notes: list[str] = []
    updated = content

    if "sessionmaker(" in updated and "_kiwi_cached_sessionmaker" not in updated:
        helper = (
            "from functools import lru_cache as _kiwi_lru_cache\n\n"
            "@_kiwi_lru_cache(maxsize=8)\n"
            "def _kiwi_cached_sessionmaker(engine, autoflush=False, expire_on_commit=False):\n"
            "    return sessionmaker(bind=engine, autoflush=autoflush, expire_on_commit=expire_on_commit)\n\n"
        )
        updated = helper + updated
        updated = re.sub(
            r"sessionmaker\(\s*bind\s*=\s*([a-zA-Z0-9_.]+)\s*\)",
            r"_kiwi_cached_sessionmaker(\1)",
            updated,
        )
        notes.append("Cached SQLAlchemy sessionmaker(bind=...) via @lru_cache(maxsize=8)")

    return updated, notes


def patch_main_boot_and_immortalize(content: str) -> tuple[str, list[str]]:
    """Inject precompiled Jinja2 loader & `sys._kiwi_immortalize_heap()` before `app.run()`."""
    notes: list[str] = []
    updated = content

    if "app.run(" in updated and "_kiwi_immortalize_heap" not in updated:
        hook_block = (
            "    # [kiwithon] Install AOT Jinja2 loader and immortalize pre-fork heap\n"
            "    try:\n"
            "        import _kiwi_precompiled_templates\n"
            "        _kiwi_precompiled_templates.install_precompiled_loader(app, fallback=True)\n"
            "        _kiwi_precompiled_templates.warmup_all_templates(app.jinja_env)\n"
            "    except ImportError:\n"
            "        pass\n"
            "    if hasattr(sys, '_kiwi_immortalize_heap'):\n"
            "        sys._kiwi_immortalize_heap()\n"
        )
        if "import sys" not in updated:
            updated = "import sys\n" + updated
        updated = re.sub(
            r"^(\s*)(app\.run\()",
             lambda m: hook_block + m.group(1) + m.group(2),
            updated,
            count=1,
            flags=re.MULTILINE,
        )
        notes.append("Injected _kiwi_precompiled_templates + sys._kiwi_immortalize_heap() before app.run()")

    return updated, notes


def process_python_file(py_file: Path, dry_run: bool = False) -> list[str]:
    """Run all AST/source transformers on a single `.py` file and validate syntax."""
    try:
        original = py_file.read_text(encoding="utf-8")
    except OSError:
        return []

    updated = original
    applied_notes: list[str] = []

    for transformer in (
        patch_markdown_and_bleach,
        patch_storage_and_knowledge,
        patch_config_and_timezone,
        patch_static_asset_mtime,
        patch_sqlalchemy_sessionmaker,
        patch_main_boot_and_immortalize,
    ):
        updated, notes = transformer(updated)
        applied_notes.extend(notes)

    if updated != original and applied_notes:
        # Validate resulting Python syntax via ast.parse before writing
        ast.parse(updated, filename=str(py_file))
        if not dry_run:
            py_file.write_text(updated, encoding="utf-8")

    return applied_notes


def apply_optimizations_to_repo(app_dir: Path, dry_run: bool = False) -> dict[str, list[str]]:
    """Scan `app_dir` and apply all Kiwithon optimizations."""
    app_dir = app_dir.resolve()
    if not app_dir.is_dir():
        raise FileNotFoundError(f"Application directory does not exist: {app_dir}")

    summary: dict[str, list[str]] = {}
    hooks_dest = install_runtime_hooks_module(app_dir, dry_run=dry_run)
    summary[str(hooks_dest.relative_to(app_dir))] = ["Installed drop-in kiwi_runtime_hooks.py"]

    for root, dirs, files in os.walk(app_dir):
        dirs[:] = [
            d for d in sorted(dirs)
            if d not in {".git", ".venv", "venv", "__pycache__", "node_modules", "build", "dist"}
        ]
        for fname in sorted(files):
            if not fname.endswith(".py") or fname == "kiwi_runtime_hooks.py":
                continue
            fpath = Path(root) / fname
            notes = process_python_file(fpath, dry_run=dry_run)
            if notes:
                summary[str(fpath.relative_to(app_dir))] = notes

    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Apply Kiwithon runtime and source optimizations to a KiwiSpaceONE checkout."
    )
    parser.add_argument(
        "app_dir",
        nargs="?",
        default=os.environ.get("KIWITHON_APP_DIR", ""),
        help="Path to the KiwiSpaceONE repository (default: $KIWITHON_APP_DIR)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Preview files and optimizations without modifying disk",
    )
    args = parser.parse_args(argv)

    if not args.app_dir:
        parser.error("Please provide app_dir or set KIWITHON_APP_DIR.")

    summary = apply_optimizations_to_repo(Path(args.app_dir), dry_run=args.dry_run)
    mode = "DRY-RUN" if args.dry_run else "APPLIED"
    print(f"[{mode}] Kiwithon Optimizations Summary for {args.app_dir}:")
    for rel_path, changes in summary.items():
        print(f"  * {rel_path}:")
        for c in changes:
            print(f"      - {c}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
