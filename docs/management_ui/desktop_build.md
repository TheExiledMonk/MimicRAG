# MimicDB Management UI Desktop Build

## Overview

The desktop app uses pywebview to embed the web UI and starts the FastAPI
service locally. The service serves the UI from `management_ui/ui/dist` when
available.

If pywebview is unreliable on your platform, prefer the Electron build in
`docs/management_ui/electron_build.md`.

## Build steps (per platform)

1. Build the web UI assets:
   - `cd management_ui/ui`
   - `npm install`
   - `npm run build`

2. Package the desktop launcher with PyInstaller:
   - `pip install pyinstaller pywebview uvicorn fastapi`
   - `make ui-binary`

Notes:
- The output binary is placed under `dist/` by PyInstaller.
- The build embeds `management_ui/ui/dist` into the binary.
- Set `MIMICDB_UI_DIST` to override the UI asset path if needed.
- For GUI-only builds, set `console=False` in `management_ui/desktop/mimicdb_ui.spec`.
- Linux runtime requires GTK system packages (PyGObject): `python3-gi`, `gir1.2-gtk-3.0`.
 - Build on each target OS (PyInstaller is not cross-compile).

## Runtime configuration

These environment variables override defaults:
- `MIMICDB_UI_BIND_HOST` (default `127.0.0.1`)
- `MIMICDB_UI_BIND_PORT` (default `8000`)
- `MIMICDB_UI_DEFAULT_DB` (default `default`)
- `MIMICDB_UI_STORAGE_ROOT` (default `./data`)

## Smoke test checklist

- Launch the desktop binary.
- Connect to a MimicDB server.
- Browse databases and datasets.
- Load a schema.
- Run a preview scan and an aggregate.
