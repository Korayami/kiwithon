<p align="center">
  <img src="assets/icon.svg" width="96" height="96" alt="Kiwithon Icon" />
</p>

<h1 align="center">🥝 Kiwithon</h1>

<p align="center">
  <strong>High-Performance CPython 3.16 Optimization Fork for Raspberry Pi 5 &amp; KiwiSpaceONE</strong>
</p>

<p align="center">
  <a href="https://github.com/Korayami/kiwithon/releases"><img src="https://img.shields.io/badge/version-3.16.0a1%2Bdev%20%7C%203.15%20Optimized-brightgreen.svg?style=flat-square" alt="Version 3.16.0a1+dev | 3.15 Optimized"/></a>
  <a href="https://github.com/Korayami/kiwithon"><img src="https://img.shields.io/badge/architecture-ARM64%20(Cortex--A76)%20%7C%20x86__64-blue.svg?style=flat-square" alt="ARM64 & x86_64"/></a>
  <a href="https://github.com/Korayami/kiwithon"><img src="https://img.shields.io/badge/acceleration-Tier--2%20JIT%20%7C%20C11%20SIMD%20%7C%20Immortal%20Heap-orange.svg?style=flat-square" alt="Tier-2 JIT & C11 SIMD"/></a>
  <a href="https://github.com/Varesine/KiwiSpaceONE"><img src="https://img.shields.io/badge/ecosystem-KiwiSpaceONE%20Integrated-success.svg?style=flat-square" alt="KiwiSpaceONE Integrated"/></a>
  <a href="https://pypi.org/"><img src="https://img.shields.io/badge/compatibility-100%25%20CPython%20%26%20pip%20ABI-informational.svg?style=flat-square" alt="100% CPython & pip ABI"/></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-PSF--2.0-blueviolet.svg?style=flat-square" alt="PSF-2.0"/></a>
</p>

<p align="center">
  <img src="assets/kiwi_logo_render.png" alt="Kiwithon 3D Logo" width="820" />
</p>

---

## 📑 Inhaltsverzeichnis / Table of Contents

- [Über Kiwithon / About Kiwithon](#-über-kiwithon--about-kiwithon)
- [Kernoptimierungen / Core Optimizations](#-kernoptimierungen--core-optimizations)
  - [1. Tier-2 JIT Warmup & Backoff Tuning](#1-tier-2-jit-warmup--backoff-tuning)
  - [2. Immortalized Heap (`sys._kiwi_immortalize_heap`)](#2-immortalized-heap-sys_kiwi_immortalize_heap)
  - [3. C-Level Lazy Import Exclusions](#3-c-level-lazy-import-exclusions)
  - [4. C11 Native SIMD Module (`_kiwi_native`)](#4-c11-native-simd-module-_kiwi_native)
  - [5. C Markdown Engine mit KaTeX-Schutz (`_kiwi_markdown`)](#5-c-markdown-engine-mit-katex-schutz-_kiwi_markdown)
  - [6. SIMD-beschleunigtes JSON](#6-simd-beschleunigtes-json)
  - [7. AOT Jinja2 Precompilation & Deepfreeze](#7-aot-jinja2-precompilation--deepfreeze)
  - [8. Raspberry Pi 5 & x86_64 PGO + LTO + BOLT Pipeline](#8-raspberry-pi-5--x86_64-pgo--lto--bolt-pipeline)
- [Schnellinstallation / Quick Installation](#-schnellinstallation--quick-installation)
- [Aktualisierung / Updating Kiwithon](#-aktualisierung--updating-kiwithon)
- [Kompilierung aus Quellcode / Build from Source](#-kompilierung-aus-quellcode--build-from-source)
- [KiwiSpaceONE Integration](#-kiwispaceone-integration)
- [Benchmark & Telemetrie](#-benchmark--telemetrie)
- [Kompatibilität & Upstream-Richtlinien](#-kompatibilität--upstream-richtlinien)
- [Lizenz](#-lizenz)

---

## 🥝 Über Kiwithon / About Kiwithon

**Kiwithon** ist ein spezialisierter High-Performance-Fork von **CPython 3.16/3.15**, der speziell für das **KiwiSpaceONE**-Ökosystem und leistungskritische Embedded-, Edge- und Server-Umgebungen wie den **Raspberry Pi 5 (ARM Cortex-A76)** sowie moderne **x86_64**-Serverarchitekturen entwickelt wurde.

### Warum Kiwithon?
Standard-CPython bietet hervorragende Portabilität, stößt jedoch bei memory-intensiven Web-Workloads und I/O-lastigen Aufgaben auf ressourcenbeschränkter Hardware an physikalische Grenzen. **Kiwithon** löst diese Engpässe durch tiefgreifende C-Level-Erweiterungen im Interpreter-Kern, ohne die Python-Spezifikation zu verletzen:

- **100% CPython ABI- & pip-Kompatibilität:** `sys.implementation.name` bleibt `cpython`, sodass alle PyPI-Packages, Binär-Wheels (NumPy, Cryptography, SQLAlchemy etc.) und Standard-Tools ohne Modifikation nahtlos funktionieren.
- **Duale Binär- und Environment-Identität:** Erzeugt parallel `kiwithon` / `kiwithon3` und abwärtskompatible `python` / `python3` Symlinks; unterstützt `KIWITHON*` Environment-Variablen mit transparentem Fallback auf `PYTHON*`.
- **Dramatisch reduzierte Startup-Latenz:** Bis zu **4.8× schnellerer Kaltstart** durch AOT-Jinja2-Precompilation und Deepfreezing heißer Module direkt ins C-Datensegment.
- **Massive RAM-Einsparung bei Server-Prozessen:** Durch Heap-Immortalization vor dem Worker-Fork entfällt der GC-Tracking-Overhead für statische Datenstrukturen vollständig.

---

## ⚡ Kernoptimierungen / Core Optimizations

### 1. Tier-2 JIT Warmup & Backoff Tuning
- Maßgeschneiderte Primzahl-minus-Eins-Warmup-Schwellenwerte (`126` / `508` / `250`) in `pystate.c` und `Include/internal/pycore_backoff.h`.
- Dynamische Laufzeitsteuerung über `KIWITHON_JIT_HOT_THRESHOLD`, `KIWITHON_JIT_WARM_THRESHOLD` und `KIWITHON_JIT_BACKOFF`.
- Verhindert vorzeitiges Deoptimieren bei wiederkehrenden Micro-Loops und stabilisiert die JIT-Trace-Generierung auf ARM64.

### 2. Immortalized Heap (`sys._kiwi_immortalize_heap`)
- Ermöglicht das dauerhafte Markieren aller bis zum Server-Start alloziierten Python-Objekte als unsterblich (`_PyGC_ImmortalizeAllTrackedObjects`).
- **Vorteil:** Eliminiert GC-Cycles über unveränderliche Konfigurationen, ORM-Metadaten, Routing-Tabellen und Templates vor dem Forken von Gunicorn/Uvicorn-Workern.

### 3. C-Level Lazy Import Exclusions
- Kernel-Level Lazy-Import-Steuerung (`sys.set_lazy_imports_exclude()`, `KIWITHON_LAZY_IMPORTS_EXCLUDE`).
- Ermöglicht das gezielte Ausschließen kritischer Core-Module (`builtins`, `sys`, `encodings`, `kiwispaceone.timezones`) aus lazy evaluation Ketten für deterministische Latenz.

### 4. C11 Native SIMD Module (`_kiwi_native`)
Statisch in den Kiwithon-Kern gelinkt (`Modules/Setup.local`), vollautomatisch beschleunigt:
- **`simd_contains_ci()` & `search_knowledge_entries_fast()`**: ARM64 NEON- und x86_64 AVX2-vektorisierte, case-insensitive Multiterm-Volltextsuche.
- **O(1) 64-Bit Packed Extension Classifier**: Klassifiziert Dateiendungen ohne String-Allokationen in Nanosekunden.
- **GIL-Free Directory Tree Crawler**: Paralleler `statx`- und `readdir`-Dateibaum-Scan mit Release des Global Interpreter Locks.
- **Constant-Time Auth Verifier**: Timing-sichere kryptographische String- und Token-Vergleiche auf C-Ebene.

### 5. C Markdown Engine mit KaTeX-Schutz (`_kiwi_markdown`)
- Hochperformanter Single-Pass CommonMark/GFM-Parser auf Basis von optimiertem `md4c`.
- **KaTeX-Mathematikschutz:** Schützt Inline- (`$...$`) und Display-Math (`$$...$$`) automatisch vor störendem HTML-Escaping.
- **Zero-Allocation HTML-Sanitizer:** Ersetzt das rechenintensive Python-`bleach` durch eine ultra-schnelle C-Allowlist-Prüfung.

### 6. SIMD-beschleunigtes JSON
- Vektorisierte ASCII-Escaping- und String-Scanning-Routinen in `Modules/_json.c` (`kiwi_simd_json.h`).
- Reduziert JSON-Serialisierungs- und Deserialisierungszeiten bei großen API-Payloads um bis zu **42%**.

### 7. AOT Jinja2 Precompilation & Deepfreeze
- **`Tools/kiwi/compile_jinja.py`**: Kompiliert Jinja2-Templates Ahead-of-Time in statischen Python-Bytecode.
- **`Tools/build/freeze_modules.py`**: Unterstützt `KIWITHON_APP_DIR`, um gesamte Web-Applikationen zusammen mit der Python-Standardbibliothek tief in das Binär-Image einzubetten (`.rodata`).

### 8. Raspberry Pi 5 & x86_64 PGO + LTO + BOLT Pipeline
- Automatisierter 3-Stufen-Buildprozess (`Tools/kiwi/build_kiwithon.sh`):
  1. Profile-Guided Optimization (PGO) mit realitätsnahen Profiling-Workloads (`Tools/kiwi/bolt_workload.py`).
  2. Link-Time Optimization (LTO) über Modulgrenzen hinweg.
  3. Binary Optimization and Layout Tool (LLVM BOLT) für minimale Branch-Misses und perfekte iTLB-Cache-Lokalität auf Cortex-A76 Cores.

---

## 📦 Schnellinstallation / Quick Installation

Installiere die aktuellste Version von Kiwithon mit einem einzigen Befehl:

```bash
curl -fsSL https://raw.githubusercontent.com/Korayami/kiwithon/main/install.sh | sh
```

Um direkt aus dem Git-Quellcode lokal auf deinem System zu kompilieren:

```bash
curl -fsSL https://raw.githubusercontent.com/Korayami/kiwithon/main/install.sh | sh -s -- --raw
```

### Installationsoptionen
```bash
# Benutzerdefiniertes Präfix (z. B. ~/.local oder /opt/kiwithon)
curl -fsSL https://raw.githubusercontent.com/Korayami/kiwithon/main/install.sh | sh -s -- --prefix /opt/kiwithon

# Parallele Build-Jobs festlegen
curl -fsSL https://raw.githubusercontent.com/Korayami/kiwithon/main/install.sh | sh -s -- --raw -j 4
```

---

## 🔄 Aktualisierung / Updating Kiwithon

Nach der Erstinstallation hält sich Kiwithon direkt über die Kommandozeile aktuell:

```bash
# Vorkompiliertes Binär-Update beziehen
kiwithon --update

# Quellcode aktualisieren, frisch kompilieren und installieren
kiwithon --raw-update
```

---

## 🛠️ Kompilierung aus Quellcode / Build from Source

### Standard-Build
```bash
./configure --enable-optimizations --with-lto
make -j$(nproc)
make test
sudo make install
```

### Vollständig optimierter PGO + LTO + BOLT Build
```bash
# Für x86_64 oder Raspberry Pi 5 (aarch64)
./Tools/kiwi/build_kiwithon.sh
```

---

## 🤝 KiwiSpaceONE Integration

Kiwithon enthält fertige Integrations-Hooks und einen AST-Patcher für KiwiSpaceONE in `Tools/kiwi/kiwispaceone_integration/`:

```bash
# 1. Quellcode- und Laufzeithooks auf KiwiSpaceONE anwenden
python3 Tools/kiwi/kiwispaceone_integration/apply_kiwispaceone_optimizations.py /path/to/KiwiSpaceONE

# 2. KiwiSpaceONE inklusive vorkompilierter Jinja2-Templates in Kiwithon deepfreezen
KIWITHON_APP_DIR=/path/to/KiwiSpaceONE ./Tools/kiwi/build_kiwithon.sh

# 3. KiwiSpaceONE mit voller SIMD- und Immortal-Heap-Beschleunigung starten
./kiwithon -m kiwispaceone._main_app
```

---

## 📊 Benchmark & Telemetrie

Gemessen mit `Tools/kiwi/benchmark_suite.py` auf **Raspberry Pi 5 (8 GB, Broadcom BCM2712 Cortex-A76 @ 2.4 GHz)** im Vergleich zu Standard CPython:

| Benchmark / Operation | CPython 3.16 Upstream | Kiwithon (PGO+LTO+BOLT) | Beschleunigung |
| :--- | :---: | :---: | :---: |
| **Cold Startup (`-c "pass"`)** | 28.4 ms | **12.1 ms** | **2.35× schneller** |
| **Markdown Parsing + KaTeX Math** | 142.0 ms | **14.8 ms** | **9.59× schneller** |
| **Case-Insensitive Substring Search** | 89.2 ms | **9.1 ms** | **9.80× schneller (NEON)** |
| **Directory Scan (`100k files`)** | 412.0 ms | **88.4 ms** | **4.66× schneller (statx)** |
| **JSON API Response Serialization** | 34.6 ms | **20.1 ms** | **1.72× schneller (SIMD)** |
| **Worker Fork RSS Memory** | 68.2 MB | **38.9 MB** | **-42.9% RAM-Einsparung** |

---

## 🛡️ Kompatibilität & Upstream-Richtlinien

Kiwithon befolgt strikt die [Richtlinien der Python Software Foundation](https://devguide.python.org/getting-started/ai-tools/):
- Alle Standardbibliothekstests (`make test`) bleiben grün.
- Volle Rückwärtskompatibilität zu Python 3.14, 3.15 und 3.16.
- C-Erweiterungen nutzen isolierte Namespaces (`_kiwi_*`), um Konflikte mit zukünftigen Upstream-Modulen auszuschließen.

---

## 📜 Lizenz

Kiwithon basiert auf CPython und steht unter der **Python Software Foundation License Version 2.0 (PSF-2.0)**.
Weitere Details finden sich in der Datei [`LICENSE`](LICENSE).

Die Kiwi-spezifischen Optimierungsmodule und Integrationstools sind Teil des **KiwiSpaceONE**-Ökosystems. Alle Marken und Handelsnamen sind Eigentum der jeweiligen Inhaber.
