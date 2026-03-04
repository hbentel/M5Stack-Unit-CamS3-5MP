# Contributing

This is a focused, hardware-specific firmware project for the M5Stack Unit CamS3-5MP.
Contributions that improve reliability, fix bugs, or extend functionality for that hardware
are welcome.

## Before You Start

- Read `CLAUDE.md` for architecture constraints and hardware limits that must not be changed
  (XCLK frequency, ISR core pinning, PSRAM speed, flash-write safety rules)
- Search existing issues before opening a new one

## Reporting Bugs

Open a GitHub issue and include:

- What you observed vs. what you expected
- Serial monitor output (capture with `bash monitor.sh > monitor.log`)
- ESP-IDF version (`idf.py --version`) and build script used (`build.sh` or `build_v6.sh`)
- Whether the problem is reproducible or intermittent

For camera stability issues (NO-SOI floods, frame truncation), include:
- `curl http://<device-ip>/stats` output
- `curl http://<device-ip>/health` output

## Building

### Prerequisites

- ESP-IDF v5.3.2 — see `setup_idf.sh` for automated setup on Linux/macOS
- Python 3.9+ (3.14 recommended — bypasses an importlib.metadata bug in some IDF tools)

### Build commands

```bash
bash build.sh              # Build firmware
bash build.sh clean        # Full clean + rebuild
bash flash.sh              # Flash to connected device (auto-detects port)
bash monitor.sh            # Open serial monitor (Ctrl+] to exit)
```

IDF v6 builds use `bash build_v6.sh` — requires a separate v6 installation.

### Testing

```bash
curl http://192.168.50.44/health   # JSON: uptime, heap, SHA-256 of running firmware
curl http://192.168.50.44/stats    # FPS, drop counters, Wi-Fi RSSI
curl -o /tmp/snap.jpg http://192.168.50.44/   # Single JPEG snapshot
```

Call `/stats` twice 5–10 s apart — the FPS field is a delta counter.

## Pull Requests

- One logical change per PR
- Keep changes small and focused — large refactors are hard to review for
  hardware-specific correctness
- Do not change XCLK frequency, ISR core assignment, PSRAM speed, or the
  flash-write safety architecture without a detailed explanation and test results

## Code Style

- Match the style of the surrounding code
- C99, ESP-IDF conventions
- No trailing whitespace; Unix line endings
