# Pre-GitHub TODO

## Must Do Before Public Upload
- [ ] Remove and rotate exposed API key in `.env`.
- [ ] Ensure sensitive local files are not uploaded if publishing via folder upload (not Git).
- [ ] Fix `.gitignore` so required source header is not excluded (`wifi.h` is currently ignored).
- [ ] Remove backup source files: `main/main.bak`, `main/main.bak2`.

## Strongly Recommended
- [ ] Make tooling less machine-specific:
  - `idf_env.sh` currently hardcodes a local Python 3.14 path.
  - `monitor.sh` currently assumes macOS `/dev/cu.usbmodem*`.
- [ ] Replace local-network defaults with neutral defaults in `main/Kconfig.projbuild` (MQTT broker URL).
- [ ] Add minimal CI build workflow (`.github/workflows/build.yml`) for ESP-IDF build sanity.
- [ ] Add `CONTRIBUTING.md` and `SECURITY.md`.

## Nice Polish
- [ ] Update `components/esp32-camera/idf_component.yml` metadata to reflect the PY260-focused fork.
- [ ] Make factory-reset BLE name message dynamic (avoid hardcoded `PROV_unitcams3` in HTTP response text).
- [ ] Add `CHANGELOG.md` and create an initial release tag (for example `v0.1.0`).
