#!/usr/bin/env python3
"""Kiwithon Benchmark Suite (Tools/kiwi/benchmark_suite.py).

Measures and reports key Kiwithon performance metrics:
  1. Cold start latency (`./kiwithon -c "import json, datetime, sqlite3, re"`)
  2. JSON serialization & deserialization throughput (ops/sec & MB/sec)
  3. Markdown rendering + XSS HTML sanitization latency (`_kiwi_markdown` vs fallback)
  4. Storage directory scanner latency (`_kiwi_native.scan_directory` vs `os.walk`)
  5. SIMD knowledge search & scoring latency (`_kiwi_native` vs pure Python)
  6. QR Reed-Solomon GF(256) hot loop speed (Tier-2 JIT / specialized interpreter)
  7. Resident Set Size (RSS) memory footprint & post-fork CoW stability (`sys._kiwi_immortalize_heap`)
"""

from __future__ import annotations

import argparse
import json
import os
import resource
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import _kiwi_native
except ImportError:
    _kiwi_native = None

try:
    import _kiwi_markdown
except ImportError:
    _kiwi_markdown = None


BENCH_MARKDOWN = """# KiwiSpaceONE Orbital Node Diagnostics

Welcome to **KiwiSpaceONE** running on `kiwithon` (Python 3.15a0+).

| Metric | Target | Observed | Status |
|--------|--------|----------|--------|
| Cold Boot | < 12ms | 6.8ms | PASS |
| Markdown Render | < 50us | 18.2us | PASS |
| Storage Scan | < 1.0ms | 0.25ms | PASS |

```python
def compute_checksum(payload: bytes) -> int:
    acc = 0xCAFEBABE
    for b in payload:
        acc = ((acc << 5) ^ (acc >> 27) ^ b) & 0xFFFFFFFF
    return acc
```

> Alert: <script>evil()</script> <a href="javascript:alert(1)">bad</a>
> Safe link: [Documentation](https://kiwispace.example.org/docs)
"""


def _measure_ns(fn, iterations: int, warmup: int = 5) -> dict[str, float]:
    for _ in range(warmup):
        fn()
    samples_us: list[float] = []
    t_total_start = time.perf_counter_ns()
    for _ in range(iterations):
        t0 = time.perf_counter_ns()
        fn()
        t1 = time.perf_counter_ns()
        samples_us.append((t1 - t0) / 1_000.0)
    t_total_end = time.perf_counter_ns()
    total_sec = max((t_total_end - t_total_start) / 1e9, 1e-9)
    samples_us.sort()
    return {
        "iterations": iterations,
        "mean_us": statistics.fmean(samples_us),
        "median_us": statistics.median(samples_us),
        "p95_us": samples_us[int(0.95 * (len(samples_us) - 1))],
        "min_us": samples_us[0],
        "ops_per_sec": iterations / total_sec,
    }


def bench_cold_start(executable: str, runs: int = 20) -> dict[str, float]:
    """Measure cold startup + frozen hot stdlib import latency."""
    cmd = [
        executable,
        "-c",
        "import json, datetime, zoneinfo, hashlib, hmac, urllib.parse, sqlite3, re, pathlib",
    ]
    samples_ms: list[float] = []
    for _ in range(runs):
        t0 = time.perf_counter_ns()
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        t1 = time.perf_counter_ns()
        samples_ms.append((t1 - t0) / 1_000_000.0)
    samples_ms.sort()
    return {
        "runs": runs,
        "mean_ms": statistics.fmean(samples_ms),
        "median_ms": statistics.median(samples_ms),
        "p95_ms": samples_ms[int(0.95 * (len(samples_ms) - 1))],
        "min_ms": samples_ms[0],
    }


def bench_json_throughput(iterations: int = 1000) -> dict[str, float]:
    """Measure JSON dumps + loads throughput on telemetry payloads."""
    payload = {
        "mission": "KiwiSpaceONE",
        "nodes": [
            {
                "id": f"node-{i:03d}",
                "online": (i % 2 == 0),
                "temp_c": 42.5 + (i * 0.25),
                "tags": ["aarch64", "cortex-a76", "kiwithon", "frozen"],
            }
            for i in range(40)
        ],
    }
    encoded_sample = json.dumps(payload)
    byte_len = len(encoded_sample.encode("utf-8"))

    def _step():
        s = json.dumps(payload, separators=(",", ":"))
        _ = json.loads(s)

    stats = _measure_ns(_step, iterations=iterations)
    stats["payload_bytes"] = byte_len
    stats["mb_per_sec"] = (stats["ops_per_sec"] * byte_len * 2) / (1024 * 1024)
    return stats


def bench_markdown_and_sanitize(iterations: int = 1000) -> dict[str, object]:
    """Measure _kiwi_markdown C extension latency."""
    if _kiwi_markdown is not None:
        def _native_step():
            _kiwi_markdown.render(BENCH_MARKDOWN, sanitize=True)

        res = _measure_ns(_native_step, iterations=iterations)
        res["backend"] = "_kiwi_markdown (C extension)"
        return res

    import html
    def _fallback_step():
        _ = html.escape(BENCH_MARKDOWN)

    res = _measure_ns(_fallback_step, iterations=iterations)
    res["backend"] = "html.escape (fallback)"
    return res


def bench_storage_scanner(iterations: int = 300) -> dict[str, object]:
    """Compare _kiwi_native.scan_storage_tree vs os.walk + os.stat."""
    with tempfile.TemporaryDirectory(prefix="kiwi_bench_scan_") as tmpdir:
        root = Path(tmpdir)
        for d_idx in range(6):
            sub = root / f"folder_{d_idx}"
            sub.mkdir(parents=True, exist_ok=True)
            for f_idx in range(25):
                (sub / f"file_{f_idx:02d}.dat").write_bytes(b"X" * 512)

        def _py_walk():
            files = 0
            total_bytes = 0
            for dirpath, _, filenames in os.walk(root):
                for fn in filenames:
                    st = os.stat(os.path.join(dirpath, fn))
                    files += 1
                    total_bytes += st.st_size
            return files, total_bytes

        py_stats = _measure_ns(_py_walk, iterations=iterations)

        if _kiwi_native is not None:
            def _c_scan():
                return _kiwi_native.scan_storage_tree(str(root), max_depth=3)

            c_stats = _measure_ns(_c_scan, iterations=iterations)
            speedup = py_stats["median_us"] / max(c_stats["median_us"], 0.01)
            return {
                "native_c": c_stats,
                "python_os_walk": py_stats,
                "speedup_x": round(speedup, 2),
            }
        return {"python_os_walk": py_stats, "native_c": None, "speedup_x": 1.0}


def bench_simd_knowledge_search(iterations: int = 500) -> dict[str, object]:
    """Compare _kiwi_native SIMD knowledge search vs pure Python `.lower().find()`."""
    entries = [
        (
            f"doc_{i:03d}.md",
            f"Raspberry Pi 5 Orbital Subsystem {i} Manual",
            (BENCH_MARKDOWN * 4) + f"\nSubsystem identifier: CORTEX-A76-NODE-{i}\n",
        )
        for i in range(60)
    ]
    dict_entries = [
        {"id": eid, "title": title, "content": content}
        for eid, title, content in entries
    ]
    query = "CORTEX-A76-NODE"

    def _py_search():
        terms = [t.lower() for t in query.split() if t]
        scored = []
        for eid, title, content in entries:
            tl = title.lower()
            cl = content.lower()
            score = 0
            for t in terms:
                if t in tl:
                    score += 10
                if t in cl:
                    score += 3
            if score > 0:
                scored.append((score, eid))
        scored.sort(reverse=True)
        return scored

    py_stats = _measure_ns(_py_search, iterations=iterations)

    if _kiwi_native is not None:
        def _c_search():
            return _kiwi_native.score_knowledge_entries(dict_entries, query)

        c_stats = _measure_ns(_c_search, iterations=iterations)
        speedup = py_stats["median_us"] / max(c_stats["median_us"], 0.01)
        return {
            "native_simd": c_stats,
            "python_lower": py_stats,
            "speedup_x": round(speedup, 2),
        }
    return {"python_lower": py_stats, "native_simd": None, "speedup_x": 1.0}


def bench_qr_reed_solomon(iterations: int = 500) -> dict[str, float]:
    """Measure QR Reed-Solomon GF(256) ECC encoding loop speed."""
    gf_exp = [0] * 512
    gf_log = [0] * 256
    x = 1
    for i in range(255):
        gf_exp[i] = x
        gf_log[x] = i
        x <<= 1
        if x & 0x100:
            x ^= 0x11D
    for i in range(255, 512):
        gf_exp[i] = gf_exp[i - 255]

    gen = [1, 59, 13, 104, 189, 68, 209, 30, 8, 163, 65, 41, 229, 98, 50, 36, 59]
    data = [(i * 73 + 19) & 0xFF for i in range(64)]

    def _rs_encode():
        msg = data + [0] * (len(gen) - 1)
        for i in range(len(data)):
            coef = msg[i]
            if coef != 0:
                lcoef = gf_log[coef]
                for j in range(1, len(gen)):
                    msg[i + j] ^= gf_exp[lcoef + gf_log[gen[j]]]
        return msg[len(data):]

    return _measure_ns(_rs_encode, iterations=iterations)


def bench_rss_and_immortalize() -> dict[str, object]:
    """Measure process RSS footprint and verify sys._kiwi_immortalize_heap()."""
    ru = resource.getrusage(resource.RUSAGE_SELF)
    # On Linux ru_maxrss is in kilobytes
    rss_kb = ru.ru_maxrss
    has_immortalize = hasattr(sys, "_kiwi_immortalize_heap")
    immortalized_ok = False
    if has_immortalize:
        immortalized_ok = bool(sys._kiwi_immortalize_heap())
    return {
        "max_rss_kb": rss_kb,
        "max_rss_mb": round(rss_kb / 1024.0, 2),
        "has_kiwi_immortalize_heap": has_immortalize,
        "immortalized_ok": immortalized_ok,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Kiwithon Benchmark Suite")
    parser.add_argument("--json", action="store_true", help="Output benchmark results as JSON")
    parser.add_argument("--quick", action="store_true", help="Run with reduced iteration counts")
    args = parser.parse_args(argv)

    scale = 0.25 if args.quick else 1.0

    results = {
        "runtime": sys.version,
        "executable": sys.executable,
        "cold_start": bench_cold_start(sys.executable, runs=max(5, int(20 * scale))),
        "json_throughput": bench_json_throughput(iterations=max(100, int(1000 * scale))),
        "markdown_sanitize": bench_markdown_and_sanitize(iterations=max(100, int(1000 * scale))),
        "storage_scanner": bench_storage_scanner(iterations=max(50, int(300 * scale))),
        "simd_knowledge_search": bench_simd_knowledge_search(iterations=max(50, int(500 * scale))),
        "qr_reed_solomon": bench_qr_reed_solomon(iterations=max(100, int(500 * scale))),
        "memory_rss": bench_rss_and_immortalize(),
    }

    if args.json:
        print(json.dumps(results, indent=2))
        return 0

    print("=" * 72)
    print(" Kiwithon Benchmark Suite Report")
    print("=" * 72)
    print(f" Runtime               : {sys.version.splitlines()[0]}")
    print(f" Executable            : {sys.executable}")
    cs = results["cold_start"]
    print(f" Cold Start (median)   : {cs['median_ms']:.2f} ms (min {cs['min_ms']:.2f} ms, p95 {cs['p95_ms']:.2f} ms)")
    jt = results["json_throughput"]
    print(f" JSON Roundtrip        : {jt['median_us']:.1f} us/op ({jt['ops_per_sec']:,.0f} ops/s, {jt['mb_per_sec']:.1f} MB/s)")
    md = results["markdown_sanitize"]
    print(f" Markdown + Sanitize   : {md['median_us']:.1f} us/op ({md['ops_per_sec']:,.0f} ops/s) [{md['backend']}]")
    sc = results["storage_scanner"]
    if sc["native_c"]:
        print(
            f" Storage Scanner       : {sc['native_c']['median_us']:.1f} us (C) vs "
            f"{sc['python_os_walk']['median_us']:.1f} us (os.walk) -> {sc['speedup_x']}x speedup"
        )
    else:
        print(f" Storage Scanner       : {sc['python_os_walk']['median_us']:.1f} us (os.walk)")
    ks = results["simd_knowledge_search"]
    if ks["native_simd"]:
        print(
            f" SIMD Knowledge Search : {ks['native_simd']['median_us']:.1f} us (SIMD) vs "
            f"{ks['python_lower']['median_us']:.1f} us (Python) -> {ks['speedup_x']}x speedup"
        )
    else:
        print(f" SIMD Knowledge Search : {ks['python_lower']['median_us']:.1f} us (Python)")
    qr = results["qr_reed_solomon"]
    print(f" QR Reed-Solomon Loop  : {qr['median_us']:.1f} us/op ({qr['ops_per_sec']:,.0f} ops/s)")
    mem = results["memory_rss"]
    print(f" Max RSS Footprint     : {mem['max_rss_mb']:.2f} MB ({mem['max_rss_kb']} KB) | Immortalize: {mem['immortalized_ok']}")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())
