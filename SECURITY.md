# Security Policy

## Supported Versions

Only the latest commit on the `main` branch is supported.
This is a single-device firmware project; there are no versioned releases with
long-term security maintenance.

## Reporting a Vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Report privately via GitHub's built-in private vulnerability reporting:
- Go to the repository page → **Security** tab → **Report a vulnerability**

Include:
- A description of the vulnerability and its potential impact
- Steps to reproduce or a proof-of-concept (if possible)
- Whether you believe it is exploitable remotely or only with physical access

You can expect an acknowledgement within a few days. If the issue is confirmed,
a fix will be developed and released, and you will be credited in the commit
message unless you prefer otherwise.

## Threat Model

This firmware is designed for a private home network camera (LAN only):

- **No authentication** on the HTTP server (port 80/81) — the device trusts its LAN
- **BLE provisioning** is active only on first boot or after factory reset
- **MQTT credentials** are stored in NVS and configurable via the `/setup` page
- **OTA updates** are triggered by publishing a firmware URL to an MQTT topic —
  only the authenticated MQTT broker can initiate OTA
- The device does not expose any service to the public internet by design

If you deploy this firmware in an environment where the device's HTTP port is
reachable from untrusted networks, add a reverse proxy with authentication.

## Known Limitations

- HTTP endpoints (`/`, `/stream`, `/health`, `/stats`, `/setup`, `/api/coredump`,
  `/factory_reset`) have no authentication
- `/api/coredump` may expose stack frames and memory contents from the last crash
- `/factory_reset` can be triggered by any client on the LAN
