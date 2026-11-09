#!/usr/bin/env python3
"""Drop-in Kiwithon Runtime Optimization Hooks for KiwiSpaceONE.

Provides high-performance C-extension bridges and zero-overhead caches for
KiwiSpaceONE when running under `./kiwithon` (with automatic pure-Python
fallbacks if imported under standard CPython):

  1. Lazy `bleach` elimination (`_BASE_ALLOWED_TAGS`) and `_kiwi_markdown`
     single-pass CommonMark/GFM + HTML sanitizer bridge.
  2. `_kiwi_native` parallel `pthreads` directory scanner (`scan_directory`)
     and NEON/AVX2 SIMD case-insensitive substring & knowledge search.
  3. Mtime-cached `load_config()` and cached `ZoneInfo` timezone helpers
     (`get_app_timezone_name`, `get_app_timezone`).
  4. Production `@lru_cache(maxsize=512)` static asset `st_mtime` cache.
  5. `@lru_cache(maxsize=8)` SQLAlchemy `sessionmaker` factory cache.
  6. Precompiled Jinja2 loader installer and pre-fork `sys._kiwi_immortalize_heap()`
     invocation helper before `app.run()`.
"""

from __future__ import annotations

import functools
import json
import os
import sys
from pathlib import Path
from typing import Any, Callable
from zoneinfo import ZoneInfo

try:
    import _kiwi_native
except ImportError:
    _kiwi_native = None

try:
    import _kiwi_markdown
except ImportError:
    _kiwi_markdown = None

try:
    import _kiwi_precompiled_templates
except ImportError:
    _kiwi_precompiled_templates = None


# -----------------------------------------------------------------------------
# 1. Lazy Bleach Replacement & _kiwi_markdown Hook
# -----------------------------------------------------------------------------

# Constant frozen set replacing eager `import bleach` at module import time
_BASE_ALLOWED_TAGS: frozenset[str] = frozenset({
    "a", "abbr", "acronym", "b", "blockquote", "br", "code", "dd", "del",
    "details", "div", "dl", "dt", "em", "h1", "h2", "h3", "h4", "h5", "h6",
    "hr", "i", "img", "ins", "kbd", "li", "mark", "ol", "p", "pre", "q",
    "rp", "rt", "ruby", "s", "samp", "small", "span", "strong", "sub",
    "summary", "sup", "table", "tbody", "td", "tfoot", "th", "thead", "tr",
    "tt", "u", "ul", "var",
})

_BASE_ALLOWED_ATTRIBUTES: dict[str, tuple[str, ...]] = {
    "a": ("href", "title", "target", "rel"),
    "img": ("src", "alt", "title", "width", "height", "loading"),
    "th": ("align", "colspan", "rowspan"),
    "td": ("align", "colspan", "rowspan"),
    "code": ("class",),
    "pre": ("class",),
    "span": ("class",),
    "div": ("class",),
}


def render_markdown_fast(text: str | None, sanitize: bool = True) -> str:
    """Render Markdown to sanitized HTML using C-accelerated `_kiwi_markdown`."""
    if not text:
        return ""
    if _kiwi_markdown is not None:
        return _kiwi_markdown.render(text, sanitize=sanitize)

    # Lazy fallback to Python markdown + bleach only if _kiwi_markdown is absent
    import markdown as py_markdown
    raw_html = py_markdown.markdown(
        text,
        extensions=["tables", "fenced_code", "nl2br", "sane_lists"],
    )
    if not sanitize:
        return raw_html
    return sanitize_html_fast(raw_html)


def sanitize_html_fast(html_text: str | None) -> str:
    """Sanitize HTML using C-accelerated `_kiwi_markdown.sanitize_html`."""
    if not html_text:
        return ""
    if _kiwi_markdown is not None:
        return _kiwi_markdown.sanitize_html(html_text)

    # Lazy import bleach only on fallback path
    import bleach
    return bleach.clean(
        html_text,
        tags=_BASE_ALLOWED_TAGS,
        attributes=_BASE_ALLOWED_ATTRIBUTES,
        strip=True,
    )


# -----------------------------------------------------------------------------
# 2. _kiwi_native Storage Scanner & SIMD Knowledge Search Hooks
# -----------------------------------------------------------------------------

def scan_storage_directory_fast(path: str | os.PathLike, max_depth: int = 32) -> dict[str, Any]:
    """Scan a directory tree using `_kiwi_native.scan_storage_tree` (GIL-free C11 statx)."""
    path_str = os.fspath(path)
    if not os.path.exists(path_str):
        return {"files": 0, "dirs": 0, "total_bytes": 0}

    if _kiwi_native is not None:
        return _kiwi_native.scan_storage_tree(path_str, max_depth=max_depth)

    files = 0
    dirs = 0
    total_bytes = 0
    for root, dirnames, filenames in os.walk(path_str):
        dirs += len(dirnames)
        for fname in filenames:
            try:
                st = os.stat(os.path.join(root, fname), follow_symlinks=False)
                files += 1
                total_bytes += st.st_size
            except OSError:
                pass
    return {"files": files, "dirs": dirs, "total_bytes": total_bytes}


def search_knowledge_files_fast(
    root_dir: str | os.PathLike,
    query: str,
    max_results: int = 50,
    snippet_radius: int = 80,
) -> list[dict[str, Any]]:
    """Search knowledge base files using `_kiwi_native.search_knowledge_files` (NEON/AVX2 SIMD)."""
    if not query or not query.strip():
        return []
    root_str = os.fspath(root_dir)
    if _kiwi_native is not None and os.path.isdir(root_str):
        return _kiwi_native.search_knowledge_files(
            root_str, query, max_results=max_results, snippet_radius=snippet_radius
        )
    return []


def search_knowledge_entries_fast(
    entries: list[Any],
    query: str,
) -> list[Any]:
    """Score and rank entries using NEON/AVX2 SIMD search."""
    if not query or not query.strip() or not entries:
        return []
    if _kiwi_native is not None:
        return _kiwi_native.score_knowledge_entries(entries, query)

    terms = [t.lower() for t in query.split() if t]
    results: list[tuple[Any, int]] = []
    for entry_id, title, content in entries:
        tl = (title or "").lower()
        cl = (content or "").lower()
        score = 0
        for term in terms:
            if term in tl:
                score += 10
            if term in cl:
                score += 3
        if score > 0:
            results.append((entry_id, score))
    results.sort(key=lambda item: item[1], reverse=True)
    return results


def simd_contains_ci(haystack: str, needle: str) -> bool:
    """Case-insensitive substring check using `_kiwi_native.simd_case_insensitive_find`."""
    if not needle:
        return True
    if not haystack:
        return False
    if _kiwi_native is not None:
        return _kiwi_native.simd_case_insensitive_find(haystack, needle) >= 0
    return needle.lower() in haystack.lower()


# -----------------------------------------------------------------------------
# 3. Mtime-Cached Config Loader & Timezone Helpers
# -----------------------------------------------------------------------------

_CONFIG_CACHE: dict[str, tuple[int, dict[str, Any]]] = {}


def load_config_cached(
    config_path: str | os.PathLike,
    loader_fn: Callable[[str], dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Load application config JSON/TOML with O(1) `st_mtime_ns` invalidation."""
    path_str = os.path.abspath(os.fspath(config_path))
    try:
        mtime_ns = os.stat(path_str).st_mtime_ns
    except OSError:
        return {}

    cached = _CONFIG_CACHE.get(path_str)
    if cached is not None and cached[0] == mtime_ns:
        return cached[1]

    if loader_fn is not None:
        data = loader_fn(path_str)
    else:
        with open(path_str, "r", encoding="utf-8") as f:
            data = json.load(f)

    _CONFIG_CACHE[path_str] = (mtime_ns, data)
    return data


@functools.lru_cache(maxsize=32)
def _cached_zoneinfo(tz_name: str) -> ZoneInfo:
    try:
        return ZoneInfo(tz_name)
    except Exception:
        return ZoneInfo("UTC")


def get_app_timezone_name(config_getter: Callable[[], dict[str, Any]] | None = None) -> str:
    """Return configured IANA timezone name without re-parsing config files."""
    if config_getter is not None:
        cfg = config_getter() or {}
        tz = cfg.get("timezone") or cfg.get("TIMEZONE")
        if tz and isinstance(tz, str):
            return tz
    return os.environ.get("KIWISPACE_TIMEZONE", "UTC")


def get_app_timezone(config_getter: Callable[[], dict[str, Any]] | None = None) -> ZoneInfo:
    """Return cached `ZoneInfo` instance for the active application timezone."""
    return _cached_zoneinfo(get_app_timezone_name(config_getter))


# -----------------------------------------------------------------------------
# 4. Production Static Asset `st_mtime` LRU Cache
# -----------------------------------------------------------------------------

@functools.lru_cache(maxsize=512)
def get_static_asset_mtime_prod(filepath: str) -> int:
    """Cached static asset mtime for cache-busting query strings in production."""
    try:
        return int(os.stat(filepath).st_mtime)
    except OSError:
        return 0


def get_static_asset_mtime(filepath: str | os.PathLike, debug: bool = False) -> int:
    """Return static asset mtime (cached in production, live stat in debug mode)."""
    path_str = os.fspath(filepath)
    if debug:
        try:
            return int(os.stat(path_str).st_mtime)
        except OSError:
            return 0
    return get_static_asset_mtime_prod(path_str)


# -----------------------------------------------------------------------------
# 5. Cached SQLAlchemy `sessionmaker` Factory
# -----------------------------------------------------------------------------

@functools.lru_cache(maxsize=8)
def get_cached_sessionmaker(engine: Any, autoflush: bool = False, expire_on_commit: bool = False) -> Any:
    """Cache `sqlalchemy.orm.sessionmaker` per Engine to avoid per-request factory allocation."""
    from sqlalchemy.orm import sessionmaker
    return sessionmaker(bind=engine, autoflush=autoflush, expire_on_commit=expire_on_commit)


# -----------------------------------------------------------------------------
# 6. Precompiled Jinja2 Loader + Heap Immortalization Boot Hook
# -----------------------------------------------------------------------------

def prepare_kiwithon_app_for_serving(app: Any, warmup_templates: bool = True) -> dict[str, Any]:
    """Attach precompiled Jinja2 templates and immortalize the Python heap before `app.run()`.

    Should be called after all blueprints, routes, and SQLAlchemy models are registered,
    immediately before starting the web server or forking Gunicorn/Hypercorn workers.
    """
    status: dict[str, Any] = {
        "precompiled_templates": 0,
        "heap_immortalized": False,
        "native_extension": _kiwi_native is not None,
        "markdown_extension": _kiwi_markdown is not None,
    }

    if _kiwi_precompiled_templates is not None and hasattr(app, "jinja_env"):
        _kiwi_precompiled_templates.install_precompiled_loader(app, fallback=True)
        if warmup_templates and hasattr(_kiwi_precompiled_templates, "warmup_all_templates"):
            status["precompiled_templates"] = _kiwi_precompiled_templates.warmup_all_templates(
                app.jinja_env
            )

    if hasattr(sys, "_kiwi_immortalize_heap"):
        status["heap_immortalized"] = bool(sys._kiwi_immortalize_heap())

    return status
