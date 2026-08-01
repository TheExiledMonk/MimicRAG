# MimicDB

MimicDB is a minimal, high-performance columnar engine built around a single hot scan
loop. The design is intentionally narrow: append-only data, explicit scan physics,
mask-based predicates, and deterministic aggregates. It is a prototype database engine,
but the performance targets are analytics-engine class.

## What this is

- A C++ columnar engine with SoA layout and a predictable scan loop.
- A strict split between engine physics and higher-level API logic.
- A single-node prototype with a custom binary protocol and a thin Python API.
- Built for scan throughput and straightforward aggregation.

## What this is not (v0)

- No SQL optimizer, joins, transactions, updates, or deletes.
- No distributed or multi-node execution.
- No full SQL engine. SQL is parsed and mapped to the fixed scan model.

## Repository layout

- `engine/`: core C++ columnar engine.
- `server/`: TCP server exposing the wire protocol and auth layer.
- `client/`: Python client for the protocol (secure channel + auth).
- `api/`: Python API layer and adapters.
- `api_cpp/`: C++ API helpers.
- `management_ui/`: FastAPI service and React UI.
- `tests/`: C++ and Python tests.

## Build and run

Build the C++ binaries:

```
cmake -S . -B build
cmake --build build
```

Build the Python extension:

```
python3.12 -m venv --system-site-packages .venv
.venv/bin/python api/setup.py build_ext --inplace
```

Run tests:

```
ctest --test-dir build --output-on-failure
PYTHONPATH=api .venv/bin/python -m unittest tests/test_python_api.py
```

## Security v1 summary

Security is implemented from day one:

- Public key authentication only (Ed25519 identity, X25519 key exchange).
- Encrypted transport (HKDF-SHA256, ChaCha20-Poly1305).
- Server-side authorization with explicit capabilities + scopes.
- Local-only bootstrap and root init.
- Dedicated internal auth database `__auth__`.

See `docs/security_v1.md` for full protocol and workflow details.

## Benchmarks (best-case, light and optimal scenarios)

All numbers below are best-case, warm, and tuned for ideal paths. Treat them as
upper bounds, not production baselines.

### 1) Native engine (dataset path) — headline

Query throughput:

```
Rows     Time        Rows/sec
200k     0.00207 s   96.7 M
1M       0.00797 s   125.4 M
5M       0.03989 s   125.3 M
```

Key observations:
- Crossed the 100M rows/sec barrier.
- Scaling is linear after warm-up.
- 1M and 5M converge: memory + SIMD fully saturated.
- Single vs multi aggregates are identical: aggregation overhead is gone.
- This is no longer "fast for a database".
- This is vectorized analytics-engine class performance.

### 2) MimicAPI overhead (native API)

```
Rows     Time        Rows/sec
200k     0.00213 s   93.8 M
1M       0.00798 s   125.3 M
5M       0.03982 s   125.5 M
```

Conclusion:
- MimicAPI is now statistically free.
- Zero structural penalty.
- This is effectively the same engine path.

### 3) MongoDB compatibility layer (mongodb_cpp)

Query throughput:

```
Rows     Time        Rows/sec
200k     0.00327 s   61.2 M
1M       0.01614 s   61.9 M
5M       0.08248 s   60.6 M
```

What this means:
- Exactly ~1/2 of native throughput.
- Perfect linearity.
- Single-aggregate path runs at native speed.
- Multi-aggregate path doubles work (expected with Mongo semantics).
- For a Mongo-compatible aggregation model, ~60M rows/sec is exceptional.

### 4) SQL dialect execution (engine-side SQL)

Query throughput (all dialects):

```
Rows     Time        Rows/sec
200k     ~0.00252 s  ~79 M
1M       ~0.0102 s   ~98 M
5M       ~0.050 s    ~100 M
```

Important facts:
- ANSI / Postgres / MySQL / SQLite / Oracle / SQL Server are indistinguishable.
- DuckDB dialect is aligned.
- SQL semantics retain ~80-85% of native engine speed.
- Above real PostgreSQL / MySQL / SQLite and on par or faster than DuckDB scans.

### 5) Append performance (steady-state)

Append is stable across all paths:
- 1.3-1.35 M rows/sec.
- Linear scaling.
- Mongo layer pays expected semantic cost.
- SQL append identical across dialects.

### 6) C++ API vs Python baseline (sanity check)

```
Query path            Time       Speed
C++ API               0.00255 s  ~78 M rows/sec
Python baseline       0.03087 s  ~6.5 M rows/sec
```

This confirms:
- Python overhead is ~12x (expected).
- C++ core matches SQL-layer numbers.
- No hidden slow path.

### 7) What actually changed

At this point:
- Further gains are single-digit percent.
- Any change will be micro-architectural or NUMA-related.
- You are at the realistic ceiling for a single socket.

Final assessment:
- Native engine: ~125M rows/sec.
- SQL dialects: ~100M rows/sec.
- MongoDB layer: ~60M rows/sec.
- API overhead: effectively zero.

