#!/usr/bin/env python3
"""Kiwithon PGO & BOLT Profiling Workload (Tools/kiwi/bolt_workload.py).

Exercises the exact hot paths targeted by Kiwithon on Raspberry Pi 5 and x86_64:
  1. `_kiwi_native` parallel storage scanner (`scan_directory`) and SIMD search
     (`simd_case_insensitive_find`, `score_knowledge_entries`).
  2. `_kiwi_markdown` C-accelerated CommonMark + GFM table/fenced-code rendering
     and single-pass HTML sanitization (`render_markdown`, `sanitize_html`).
  3. Hot stdlib `json` parsing and serialization (`json.loads`, `json.dumps`),
     `datetime`/`zoneinfo`, `hashlib`/`hmac`, `urllib.parse`, and `sqlite3`.
  4. Object attribute and descriptor access (`property`, `__slots__`, method binds)
     to train inline caches and `LOAD_ATTR` superinstructions.
  5. Tier-2 JIT hot numeric/bitwise loops (GF(256) Reed-Solomon QR polynomial
     division and matrix masking).
  6. Optional KiwiSpaceONE pytest suite execution if `KIWITHON_APP_DIR` is set.
"""

from __future__ import annotations

import datetime
import hashlib
import hmac
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import urllib.parse
from pathlib import Path

try:
    import _kiwi_native
except ImportError:
    _kiwi_native = None

try:
    import _kiwi_markdown
except ImportError:
    _kiwi_markdown = None


SAMPLE_MARKDOWN = """# KiwiSpaceONE Mission Briefing: Sector 7G

Welcome to **KiwiSpaceONE** aboard the *Raspberry Pi 5* orbital node.

## Telemetry Summary

| Subsystem | Status | Temp (C) | Power (W) | Notes |
|-----------|--------|----------|-----------|-------|
| Core CPU  | ONLINE | 52.4     | 4.8       | Cortex-A76 @ 2.4GHz |
| Comms     | NOMINAL| 41.0     | 1.2       | Encrypted Link `X-Band` |
| LifeSupp  | ACTIVE | 21.5     | 3.1       | O2 Scrubbers at 98% |

### Emergency Procedures

1. Verify `sys._kiwi_immortalize_heap()` status in telemetry logs.
2. Run diagnostic script:

```python
def verify_orbital_trajectory(delta_v: float, isp: float) -> float:
    if delta_v <= 0:
        return 0.0
    return delta_v * 9.80665 / max(isp, 1.0)
```

> **Note**: Unsafe HTML like <script>alert("xss")</script> and <img src=x onerror=alert(1)>
> or [malicious link](javascript:alert(1)) must be stripped automatically!

Visit [Mission Control](https://kiwispace.example.org/control?node=rpi5&auth=ok) for live updates.
"""


class _TelemetrySlotRecord:
    __slots__ = ("node_id", "timestamp", "cpu_temp", "memory_bytes", "status")

    def __init__(self, node_id: str, timestamp: int, cpu_temp: float, memory_bytes: int, status: str):
        self.node_id = node_id
        self.timestamp = timestamp
        self.cpu_temp = cpu_temp
        self.memory_bytes = memory_bytes
        self.status = status

    @property
    def is_healthy(self) -> bool:
        return self.status == "ONLINE" and self.cpu_temp < 80.0

    def to_dict(self) -> dict:
        return {
            "node_id": self.node_id,
            "timestamp": self.timestamp,
            "cpu_temp": self.cpu_temp,
            "memory_bytes": self.memory_bytes,
            "status": self.status,
            "healthy": self.is_healthy,
        }


def workload_native_extensions(iterations: int = 120) -> None:
    """Exercise _kiwi_native directory scanner and SIMD substring/scoring kernels."""
    with tempfile.TemporaryDirectory(prefix="kiwi_bolt_") as tmpdir:
        root = Path(tmpdir)
        for sub in ("knowledge", "logs", "telemetry", "assets"):
            d = root / sub
            d.mkdir(parents=True, exist_ok=True)
            for i in range(15):
                f = d / f"entry_{i:02d}.md"
                f.write_text(SAMPLE_MARKDOWN + f"\n<!-- item {i} -->\n", encoding="utf-8")

        haystacks = [
            (
                f"mission_guide_{i}.md",
                f"Orbital Node {i} Telemetry & Survival Guide",
                SAMPLE_MARKDOWN * 2,
            )
            for i in range(40)
        ]

        for _ in range(iterations):
            if _kiwi_native is not None:
                _kiwi_native.scan_storage_tree(str(root), max_depth=3)
                _kiwi_native.search_knowledge_files(str(root), "cortex-a76")
                for _, title, body in haystacks[:10]:
                    _kiwi_native.simd_case_insensitive_find(body, "cortex-a76")
                    _kiwi_native.simd_case_insensitive_find(title, "telemetry")
                _kiwi_native.score_knowledge_entries(
                    [{"id": eid, "title": title, "content": body} for eid, title, body in haystacks],
                    "orbital",
                )
            else:
                # Fallback stdlib path if _kiwi_native not yet built
                total = 0
                for dirpath, _, filenames in os.walk(root):
                    for fn in filenames:
                        total += os.path.getsize(os.path.join(dirpath, fn))


def workload_markdown_and_sanitize(iterations: int = 300) -> None:
    """Exercise _kiwi_markdown CommonMark/GFM parser and HTML sanitizer."""
    dirty_html = (
        '<div class="note" onclick="steal()">'
        '<h2>Alert</h2><script>evil()</script>'
        '<p>Check <a href="https://example.com/docs" target="_blank">Docs</a> '
        'and <a href="javascript:void(0)">Bad</a></p>'
        '<table><thead><tr><th>K</th><th>V</th></tr></thead>'
        '<tbody><tr><td>1</td><td>2</td></tr></tbody></table>'
        '</div>'
    )
    for _ in range(iterations):
        if _kiwi_markdown is not None:
            rendered = _kiwi_markdown.render(SAMPLE_MARKDOWN, sanitize=True)
            _kiwi_markdown.sanitize_html(dirty_html)
            if not rendered:
                raise RuntimeError("Empty markdown output")
        else:
            _ = SAMPLE_MARKDOWN.splitlines()


def workload_json_stdlib_and_sqlite(iterations: int = 250) -> None:
    """Exercise frozen stdlib hot paths: json, datetime, hmac/hashlib, urllib, sqlite3."""
    records = [
        _TelemetrySlotRecord(f"rpi5-node-{i}", 1700000000 + i * 60, 48.5 + (i % 15), 512 * 1024 * 1024, "ONLINE")
        for i in range(50)
    ]

    conn = sqlite3.connect(":memory:")
    conn.execute("CREATE TABLE telemetry (node TEXT, ts INTEGER, temp REAL, payload TEXT)")
    conn.executemany(
        "INSERT INTO telemetry VALUES (?, ?, ?, ?)",
        [(r.node_id, r.timestamp, r.cpu_temp, json.dumps(r.to_dict())) for r in records],
    )
    conn.commit()

    secret = b"kiwithon-orbital-hmac-key"
    for i in range(iterations):
        payload_list = [r.to_dict() for r in records]
        raw_json = json.dumps(payload_list, separators=(",", ":"))
        decoded = json.loads(raw_json)
        if len(decoded) != 50:
            raise AssertionError("JSON roundtrip mismatch")

        sig = hmac.new(secret, raw_json.encode("utf-8"), hashlib.sha256).hexdigest()
        _ = urllib.parse.urlencode({"node": "rpi5", "sig": sig[:16], "iter": i})
        _ = datetime.datetime.now(datetime.timezone.utc).isoformat()

        cur = conn.execute("SELECT node, temp, payload FROM telemetry WHERE temp > ? LIMIT 10", (50.0,))
        for _, _, p_txt in cur.fetchall():
            _ = json.loads(p_txt)

    conn.close()


def workload_jit_reed_solomon(iterations: int = 200) -> None:
    """Exercise Tier-2 JIT hot integer/bitwise loop: GF(256) Reed-Solomon QR encoding."""
    # Precompute GF(256) log/antilog tables with primitive polynomial 0x11D
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

    # Generator polynomial for 16 ECC codewords
    gen = [1]
    for i in range(16):
        nxt = [0] * (len(gen) + 1)
        factor = gf_exp[i]
        for j, coeff in enumerate(gen):
            nxt[j] ^= coeff
            if coeff != 0:
                nxt[j + 1] ^= gf_exp[gf_log[coeff] + gf_log[factor]]
        gen = nxt

    data = [((i * 37) ^ 0x5A) & 0xFF for i in range(64)]

    for _ in range(iterations):
        msg = data + [0] * (len(gen) - 1)
        for i in range(len(data)):
            coef = msg[i]
            if coef != 0:
                log_coef = gf_log[coef]
                for j in range(1, len(gen)):
                    g = gen[j]
                    if g != 0:
                        msg[i + j] ^= gf_exp[log_coef + gf_log[g]]
        ecc = msg[len(data):]
        if len(ecc) != 16:
            raise AssertionError("Reed-Solomon ECC length mismatch")


def workload_kiwispaceone_pytest() -> None:
    """If KIWITHON_APP_DIR is set and contains tests, run pytest to train app paths."""
    app_dir = os.environ.get("KIWITHON_APP_DIR", "").strip()
    if not app_dir or not os.path.isdir(app_dir):
        return
    tests_dir = os.path.join(app_dir, "tests")
    if not os.path.isdir(tests_dir):
        return
    print(f"[bolt_workload] Running KiwiSpaceONE test suite in {app_dir}...")
    subprocess.run(
        [sys.executable, "-m", "pytest", "-q", tests_dir],
        cwd=app_dir,
        check=False,
    )


def main() -> int:
    scale = int(os.environ.get("KIWITHON_WORKLOAD_SCALE", "1"))
    print(f"[bolt_workload] Starting Kiwithon PGO/BOLT training workload (scale={scale})...")
    workload_native_extensions(iterations=120 * scale)
    workload_markdown_and_sanitize(iterations=300 * scale)
    workload_json_stdlib_and_sqlite(iterations=250 * scale)
    workload_jit_reed_solomon(iterations=200 * scale)
    workload_kiwispaceone_pytest()
    print("[bolt_workload] Training workload completed successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
