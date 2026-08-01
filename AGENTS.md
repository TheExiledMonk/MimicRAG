# MimicDB Agent Notes

This repo mixes C++ engine/server code with Python APIs and a React UI. Keep changes consistent across the C++ core, Python wrappers, and the wire protocol.

## Layout overview
- `engine/`: core columnar engine in C++ (`engine/include/mimicdb`, `engine/src`).
- `server/`: single-file C++ TCP server (`server/mimicdb_server.cpp`) exposing a custom binary protocol.
- `client/`: Python reference client for the wire protocol (`client/mimicdb_client.py`).
- `api/`: Python API layer and C++ bindings (`api/mimicapi`, `api/setup.py`, `api/pyproject.toml`).
- `api_cpp/`: C++ API helpers and adapters (`api_cpp/include`, `api_cpp/src`).
- `management_ui/`: FastAPI service + React UI + pywebview desktop launcher.
- `tests/`: C++ unit tests (wired into CMake) + Python tests (unittest style).
- `mimicdb.conf`: example server config (bind, storage, compression, limits).

## Wire protocol (server/client)
- Defined in `server/mimicdb_server.cpp` and mirrored by `client/mimicdb_client.py`.
- Header fields: magic/version/opcode/status/payload size/request id (`MessageHeader`).
- Operations are numeric opcodes (`OpCode` enum). Any change must update both sides.
- Payload formats are manual byte packing; validate lengths and field types carefully.

## Build and test
- Build C++: `cmake -S . -B build && cmake --build build` (or `make build`).
- Run C++ tests: `ctest --test-dir build --output-on-failure` (or `make test`).
- Build Python extension: `api/setup.py build_ext --inplace` (expects `.venv` in repo root).
- Run Python tests: `PYTHONPATH=api python -m unittest tests/test_python_api.py` (and others).
- Network tests require `MIMICDB_SERVER_BIN` pointing to the built server binary.

## Management UI
- Service: `management_ui/service/app.py` (FastAPI) proxies to `client/mimicdb_client.py`.
- Desktop launcher: `management_ui/desktop/launcher.py` (uvicorn + pywebview).
- UI: `management_ui/ui` (React + Vite). Use `npm run dev/build/test` from that dir.

## Data/layout expectations
- On-disk dataset layout lives under `data/<db>/<dataset>/` with `schema.bin` and segment files.
- The server enforces payload size limits and uses `mimicdb.conf` for runtime config.

## Change coordination
- If you change engine types or wire formats, update:
  - C++ server (`server/mimicdb_server.cpp`)
  - Python client (`client/mimicdb_client.py`)
  - Python API transport (`api/mimicapi/transport.py`) if it mirrors structures
  - Tests that assert payload sizes or opcodes
- Keep opcode values stable; add new ones only after updating all consumers.
