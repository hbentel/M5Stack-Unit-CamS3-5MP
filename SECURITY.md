# Security Policy

## Supported Versions

Only the latest release and the current `main` branch are supported.
This is a single-device firmware project; older releases do not receive
backported security fixes — always use the latest release.

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

- **HTTP server** (port 80/81) — no authentication by default; trusts the LAN
- **BLE provisioning** is active only on first boot or after factory reset
- **MQTT credentials** are stored in NVS and configurable via the `/setup` page
- **OTA updates** require a shared token when `OTA Token` is set on `/setup` —
  the MQTT payload must be JSON `{"url":"...","token":"<token>"}`. Without a token
  configured, bare URL strings are accepted (legacy mode).
- **`/api/coredump`** requires an `Authorization: Bearer <token>` header when
  `Coredump Token` is set on `/setup`. Without a token, access is open.
- The device does not expose any service to the public internet by design

If you deploy this firmware in an environment where the device's HTTP port is
reachable from untrusted networks, add a reverse proxy with authentication.

## Known Limitations

- Most HTTP endpoints (`/`, `/stream`, `/health`, `/stats`, `/setup`, `/api/logs`,
  `/factory_reset`) have no authentication
- `/api/logs` exposes the full boot and runtime log, which may include IP addresses,
  MQTT broker hostnames, and other configuration details visible in log output
- `/factory_reset` can be triggered by any client on the LAN (POST required)
- Tokens configured on `/setup` are transmitted in plaintext over HTTP (port 80,
  unencrypted); use on a trusted LAN only
