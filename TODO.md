# Pre-GitHub TODO

## Must Do Before Public Upload
- [ ] Remove and rotate exposed API key in `.env`.
- [x] Ensure sensitive local files are not uploaded — `.gitignore` is comprehensive.
- [x] Fix `.gitignore` re `wifi.h` — not needed, project uses BLE provisioning.
- [x] Remove backup source files — `*.bak` added to `.gitignore`, bak files removed.

## Strongly Recommended
- [x] Make tooling less machine-specific:
  - `idf_env.sh` Python 3.14 path — macOS-specific but documented; CI uses Docker so unaffected.
  - `monitor.sh` / `flash.sh` / `flashmon.sh` — now auto-detect macOS and Linux ports.
- [x] Replace local-network defaults in `main/Kconfig.projbuild` (MQTT broker URL) — done.
- [x] Add minimal CI build workflow (`.github/workflows/build.yml`) — done.
- [x] Add `CONTRIBUTING.md` and `SECURITY.md`.

## Nice Polish
- [x] Update `components/esp32-camera/idf_component.yml` metadata to reflect the PY260-focused fork.
- [x] Make factory-reset BLE name message dynamic (avoid hardcoded `PROV_unitcams3` in HTTP response text).
- [x] Add `CHANGELOG.md` and create an initial release tag (for example `v0.1.0`).
