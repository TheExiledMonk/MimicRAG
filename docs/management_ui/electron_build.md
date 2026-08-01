# MimicDB Management UI Electron Build

## Overview

Electron bundles the UI and ships a packaged desktop app. The app starts a
bundled UI service binary (PyInstaller) and loads `http://127.0.0.1:8000/ui`
inside the Electron window.

This is the most reliable cross-platform option (Linux/macOS/Windows).

## Build steps (per platform)

Electron is not cross-compile friendly. Build on each target OS.

1) Build the UI assets:
- `cd management_ui/ui`
- `npm install`
- `npm run build`

2) Build the UI service binary:
- `pip install pyinstaller uvicorn fastapi`
- `make ui-service-bin`

3) Package the Electron app:
- `cd management_ui/electron`
- `npm install`
- `npm run dist`

The packaged app output is created under `management_ui/electron/dist/`.

## Environment overrides

You can override the service binary path for development:
- `MIMICDB_UI_SERVICE_BIN=/path/to/mimicdb-ui-service`

The Electron app passes these to the service:
- `MIMICDB_UI_BIND_HOST` (default `127.0.0.1`)
- `MIMICDB_UI_BIND_PORT` (default `8000`)
- `MIMICDB_UI_DEFAULT_DB`
- `MIMICDB_UI_STORAGE_ROOT`
- `MIMICDB_UI_IDENTITY_KEY`

## Notes

- The service binary is bundled via `extraResources` into the Electron app.
- If the service fails to start within 15s, the app exits with an error.
