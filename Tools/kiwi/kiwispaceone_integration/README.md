# KiwiSpaceONE + Kiwithon Integration Guide

This directory (`Tools/kiwi/kiwispaceone_integration/`) provides drop-in runtime hooks and automated source/AST patchers to accelerate **KiwiSpaceONE** on **Kiwithon** (`./kiwithon`), targeting Raspberry Pi 5 (`aarch64` ARM Cortex-A76) and `x86_64`.

## Included Tooling

1. **`kiwi_runtime_hooks.py`**:
   - **Lazy `bleach` elimination & `_kiwi_markdown` C hook**: Defines static `_BASE_ALLOWED_TAGS` and `_BASE_ALLOWED_ATTRIBUTES` without importing `bleach` at startup, and routes `render_markdown_fast()` / `sanitize_html_fast()` directly to the single-pass C extension `_kiwi_markdown`.
   - **`_kiwi_native` C hooks**:
     - `scan_storage_directory_fast()`: Multi-threaded GIL-free `pthreads` + `readdir`/`lstat` directory scanner (`_kiwi_native.scan_directory`).
     - `search_knowledge_entries_fast()` & `simd_contains_ci()`: ARM64 NEON / x86_64 AVX2 SIMD case-insensitive substring and multi-term document scoring.
   - **Config & Timezone caching**: `load_config_cached()` uses `st_mtime_ns` fast checks to avoid repeated JSON disk reads; `get_app_timezone_name()` and `get_app_timezone()` cache `ZoneInfo` instances via `@lru_cache`.
   - **Static asset `st_mtime` cache**: `get_static_asset_mtime_prod()` caches static asset modification timestamps (`@lru_cache(maxsize=512)`) in production mode.
   - **SQLAlchemy `sessionmaker` cache**: `get_cached_sessionmaker()` caches `sessionmaker(bind=engine)` (`@lru_cache(maxsize=8)`).
   - **Pre-fork Heap Immortalization & AOT Jinja2 Loader**: `prepare_kiwithon_app_for_serving(app)` installs `KiwiPrecompiledLoader` from `_kiwi_precompiled_templates` and invokes `sys._kiwi_immortalize_heap()` immediately before `app.run()`.

2. **`apply_kiwispaceone_optimizations.py`**:
   - Automated source/AST patcher that installs `kiwi_runtime_hooks.py` into a KiwiSpaceONE checkout and patches Markdown rendering, storage scanning, knowledge search, config/timezone loading, static asset mtime checks, SQLAlchemy `sessionmaker`, and `main.py` boot immortalization.

## End-to-End Usage

```bash
# 1. Apply source & runtime hook optimizations to KiwiSpaceONE
python3 Tools/kiwi/kiwispaceone_integration/apply_kiwispaceone_optimizations.py /path/to/KiwiSpaceONE

# 2. Precompile Jinja2 templates, freeze KiwiSpaceONE + hot stdlib into ./kiwithon, and build with PGO/BOLT
KIWITHON_APP_DIR=/path/to/KiwiSpaceONE ./Tools/kiwi/build_kiwithon.sh

# 3. Boot KiwiSpaceONE directly from frozen C-rodata bytecode inside ./kiwithon
./kiwithon -m kiwispaceone._main_app
```
