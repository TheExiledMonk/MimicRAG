from __future__ import annotations

from dataclasses import dataclass, field
from concurrent.futures import ThreadPoolExecutor, as_completed
import time
from collections import deque
from dataclasses import dataclass
import json
from pathlib import Path

from .policy import ApiPolicy
from .adapter import BackendAdapter
from .mimicdb_adapter import MimicDBAdapter

@dataclass
class ServerInfo:
    name: str
    host: str | None = None
    port: int | None = None
    local: bool = False
    healthy: bool = True
    last_failure: float | None = None
    blacklist_until: float | None = None
    latency_ms: float | None = None


class ApiClient:
    def __init__(self, policy: ApiPolicy | None = None) -> None:
        self._servers: dict[str, ServerInfo] = {}
        self._transports: dict[str, BackendAdapter] = {}
        self._batch_counter = 0
        self._executor = ThreadPoolExecutor(max_workers=8)
        self._policy = policy or ApiPolicy()
        self._blacklist_ttl_sec = self._policy.failure_policy.blacklist_ttl_sec
        self._max_retries = self._policy.failure_policy.max_retries
        self._replay_log = deque(maxlen=1024)
        self._replay_log_path: Path | None = None
        self._replay_log_file = None
        self._debug = False
        self._cancel_requested = False

    def add_server(self, name: str, host: str | None = None, port: int | None = None,
                   local: bool = False) -> None:
        if local and (host is not None or port is not None):
            raise ValueError("local server cannot specify host/port")
        if not local and (host is None or port is None):
            raise ValueError("network server requires host and port")
        self._servers[name] = ServerInfo(name=name, host=host, port=port, local=local)

    def add_mimicdb_backend(
        self,
        name: str,
        host: str | None = None,
        port: int | None = None,
        default_db: str = "default",
    ) -> None:
        local = host is None and port is None
        self.add_server(name, host=host, port=port, local=local)
        self.register_transport(
            name,
            MimicDBAdapter(host=host, port=port, default_db=default_db),
        )

    def register_transport(self, name: str, transport: BackendAdapter) -> None:
        if name not in self._servers:
            raise KeyError(f"unknown server '{name}'")
        self._transports[name] = transport

    def transport_for(self, name: str):
        return self._transports.get(name)

    def remove_server(self, name: str) -> None:
        self._servers.pop(name, None)

    def servers(self) -> list[ServerInfo]:
        return list(self._servers.values())

    def mark_failure(self, name: str) -> None:
        info = self._servers.get(name)
        if info is None:
            return
        info.healthy = False
        info.last_failure = time.time()
        info.blacklist_until = info.last_failure + self._blacklist_ttl_sec

    def mark_success(self, name: str, latency_ms: float | None = None) -> None:
        info = self._servers.get(name)
        if info is None:
            return
        info.healthy = True
        info.blacklist_until = None
        if latency_ms is not None:
            info.latency_ms = latency_ms

    def update_latency(self, name: str, latency_ms: float) -> None:
        info = self._servers.get(name)
        if info is None:
            return
        info.latency_ms = latency_ms

    def select_fastest(self) -> ServerInfo | None:
        healthy = [s for s in self._servers.values() if self._is_healthy(s)]
        if not healthy:
            return None
        return min(
            healthy,
            key=lambda s: s.latency_ms if s.latency_ms is not None else float("inf"),
        )

    def set_blacklist_ttl(self, seconds: float) -> None:
        if seconds < 0:
            raise ValueError("blacklist TTL must be non-negative")
        self._blacklist_ttl_sec = seconds
        self._policy.failure_policy.blacklist_ttl_sec = seconds

    def set_max_retries(self, count: int) -> None:
        if count < 0:
            raise ValueError("max_retries must be non-negative")
        self._max_retries = count
        self._policy.failure_policy.max_retries = count

    def set_debug(self, enabled: bool) -> None:
        self._debug = enabled

    def cancel(self) -> None:
        self._cancel_requested = True

    def reset_cancel(self) -> None:
        self._cancel_requested = False

    def _is_healthy(self, server: ServerInfo) -> bool:
        if not server.healthy:
            if server.blacklist_until is None:
                return False
            if time.time() < server.blacklist_until:
                return False
            server.healthy = True
        return True

    def select_transport(self):
        fastest = self.select_fastest()
        if fastest is None:
            return None
        return self._transports.get(fastest.name)

    def query_agg_routed(
        self,
        db: str,
        dataset: str,
        field_index: int,
        predicates: list[tuple[int, int, float]] | None = None,
        consistency: str | None = None,
        verify: bool | None = None,
    ) -> tuple[dict[str, float], dict[str, float]]:
        servers = [s for s in self._servers.values() if self._is_healthy(s)]
        if not servers:
            raise RuntimeError("no healthy servers available")
        if consistency is None:
            consistency = self._policy.read_policy.consistency
        if verify is None:
            verify = self._policy.read_policy.verify
        if consistency not in ("any", "quorum"):
            raise ValueError("consistency must be 'any' or 'quorum'")

        def query_one(name: str) -> tuple[str, dict[str, float], float]:
            transport = self._transports.get(name)
            if transport is None:
                raise RuntimeError(f"missing transport for '{name}'")
            start = time.time()
            result = transport.query_agg(db, dataset, field_index, predicates)
            latency = (time.time() - start) * 1000.0
            self.mark_success(name, latency)
            return name, result, latency

        if consistency == "any":
            fastest = self.select_fastest()
            if fastest is None:
                fastest = servers[0]
            name, result, latency = query_one(fastest.name)
            stats = {
                "server": name,
                "latency_ms": latency,
                "servers_contacted": 1,
                "servers_succeeded": 1,
                "servers_failed": 0,
            }
            if verify and len(servers) > 1:
                for secondary in servers:
                    if secondary.name == name:
                        continue
                    try:
                        _, secondary_result, secondary_latency = query_one(secondary.name)
                        stats["servers_contacted"] += 1
                        stats["servers_succeeded"] += 1
                        if self._debug:
                            stats.setdefault("verification_latency_ms", []).append(secondary_latency)
                        if secondary_result.get("count") != result.get("count"):
                            break
                    except Exception:
                        self.mark_failure(secondary.name)
                        stats["servers_contacted"] += 1
                        stats["servers_failed"] += 1
            if "rows_scanned" in result:
                stats["rows_scanned"] = result["rows_scanned"]
            return result, stats

        needed = len(servers) // 2 + 1
        results: dict[str, dict[str, float]] = {}
        stats = {
            "servers_contacted": 0,
            "servers_succeeded": 0,
            "servers_failed": 0,
        }
        for server in servers:
            try:
                name, res, latency = query_one(server.name)
                results[name] = res
                stats["servers_contacted"] += 1
                stats["servers_succeeded"] += 1
                if self._debug:
                    stats.setdefault("latency_ms", {})[name] = latency
                if len(results) >= needed:
                    break
            except Exception:
                self.mark_failure(server.name)
                stats["servers_contacted"] += 1
                stats["servers_failed"] += 1
        if len(results) < needed:
            raise RuntimeError("quorum read not reached")
        first_key = next(iter(results))
        if "rows_scanned" in results[first_key]:
            stats["rows_scanned"] = results[first_key]["rows_scanned"]
        return results[first_key], stats

    def next_batch_id(self) -> int:
        self._batch_counter += 1
        return ((time.time_ns() & 0xFFFFFFFFFFFFFFFF) ^ self._batch_counter) & 0xFFFFFFFFFFFFFFFF

    def append_batch_fanout(
        self,
        db: str,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: dict[str, list],
        batch_id: int | None = None,
        quorum: int | None = None,
    ) -> dict[str, bool]:
        if batch_id is None:
            batch_id = self.next_batch_id()
        entry = {
            "db": db,
            "dataset": dataset,
            "fields": fields,
            "columns": columns,
            "batch_id": batch_id,
        }
        self._replay_log.append(entry)
        self._append_replay_entry(entry)
        servers = [s.name for s in self._servers.values() if self._is_healthy(s)]
        if not servers:
            raise RuntimeError("no servers registered")
        if self._cancel_requested:
            raise RuntimeError("operation cancelled")
        if quorum is None:
            quorum = self._policy.write_policy.quorum
        needed = quorum if quorum is not None else (len(servers) // 2 + 1)
        results: dict[str, bool] = {}
        futures = {}
        start_times: dict[str, float] = {}
        for name in servers:
            transport = self._transports.get(name)
            if transport is None:
                results[name] = False
                continue
            start_times[name] = time.time()
            futures[self._executor.submit(
                transport.append_batch, db, dataset, fields, columns, batch_id
            )] = name

        successes = 0
        pending = []
        for future in as_completed(futures):
            name = futures[future]
            try:
                future.result()
                results[name] = True
                successes += 1
                self.mark_success(
                    name,
                    latency_ms=(time.time() - start_times.get(name, time.time())) * 1000.0,
                )
            except Exception:
                results[name] = False
                self.mark_failure(name)
            if successes >= needed:
                pending = [f for f in futures if not f.done()]
                break
            if self._cancel_requested:
                raise RuntimeError("operation cancelled")

        if successes < needed:
            retries = 0
            while retries < self._max_retries and successes < needed:
                retries += 1
                for name in list(results.keys()):
                    if results.get(name):
                        continue
                    transport = self._transports.get(name)
                    if transport is None or not self._is_healthy(self._servers[name]):
                        continue
                    try:
                        transport.append_batch(db, dataset, fields, columns, batch_id)
                        results[name] = True
                        successes += 1
                        self.mark_success(name)
                        if successes >= needed:
                            break
                    except Exception:
                        self.mark_failure(name)
                    if self._cancel_requested:
                        raise RuntimeError("operation cancelled")
            if successes < needed:
                raise RuntimeError("quorum not reached")

        if pending:
            def finish_pending(pend):
                for f in pend:
                    name = futures[f]
                    try:
                        f.result()
                        results[name] = True
                        self.mark_success(
                            name,
                            latency_ms=(time.time() - start_times.get(name, time.time())) * 1000.0,
                        )
                    except Exception:
                        results[name] = False
                        self.mark_failure(name)

            self._executor.submit(finish_pending, pending)

        stats = {
            "servers_contacted": len(servers),
            "servers_succeeded": sum(1 for v in results.values() if v),
            "servers_failed": sum(1 for v in results.values() if not v),
        }
        return {"results": results, "stats": stats}

    def create_database_all(self, name: str) -> dict[str, int]:
        stats = {"servers_contacted": 0, "servers_succeeded": 0, "servers_failed": 0}
        for server_name, transport in self._transports.items():
            if server_name not in self._servers or not self._is_healthy(self._servers[server_name]):
                continue
            stats["servers_contacted"] += 1
            try:
                transport.create_database(name)
                self.mark_success(server_name)
                stats["servers_succeeded"] += 1
            except Exception:
                self.mark_failure(server_name)
                stats["servers_failed"] += 1
        return stats

    def list_databases_all(self) -> list[str]:
        databases: set[str] = set()
        for server_name, transport in self._transports.items():
            if server_name not in self._servers or not self._is_healthy(self._servers[server_name]):
                continue
            try:
                databases.update(transport.list_databases())
                self.mark_success(server_name)
            except Exception:
                self.mark_failure(server_name)
        return sorted(databases)

    def drop_database_all(self, name: str) -> dict[str, int]:
        stats = {"servers_contacted": 0, "servers_succeeded": 0, "servers_failed": 0}
        for server_name, transport in self._transports.items():
            if server_name not in self._servers or not self._is_healthy(self._servers[server_name]):
                continue
            stats["servers_contacted"] += 1
            try:
                transport.drop_database(name)
                self.mark_success(server_name)
                stats["servers_succeeded"] += 1
            except Exception:
                self.mark_failure(server_name)
                stats["servers_failed"] += 1
        return stats

    def create_dataset_all(
        self,
        db: str,
        name: str,
        fields: list[tuple[str, str]],
    ) -> dict[str, int]:
        self.create_database_all(db)
        stats = {"servers_contacted": 0, "servers_succeeded": 0, "servers_failed": 0}
        for server_name, transport in self._transports.items():
            if server_name not in self._servers or not self._is_healthy(self._servers[server_name]):
                continue
            stats["servers_contacted"] += 1
            try:
                transport.create_dataset(db, name, fields)
                self.mark_success(server_name)
                stats["servers_succeeded"] += 1
            except Exception:
                self.mark_failure(server_name)
                stats["servers_failed"] += 1
        return stats

    def drop_dataset_all(self, db: str, name: str) -> dict[str, int]:
        stats = {"servers_contacted": 0, "servers_succeeded": 0, "servers_failed": 0}
        for server_name, transport in self._transports.items():
            if server_name not in self._servers or not self._is_healthy(self._servers[server_name]):
                continue
            stats["servers_contacted"] += 1
            try:
                transport.drop_dataset(db, name)
                self.mark_success(server_name)
                stats["servers_succeeded"] += 1
            except Exception:
                self.mark_failure(server_name)
                stats["servers_failed"] += 1
        return stats

    def scan_routed(
        self,
        db: str,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: list[str] | None = None,
        predicates: list[tuple[int, int, float]] | None = None,
        limit: int = 0,
        offset: int = 0,
    ) -> tuple[list[dict], dict[str, float]]:
        servers = [s for s in self._servers.values() if self._is_healthy(s)]
        if not servers:
            raise RuntimeError("no healthy servers available")
        fastest = self.select_fastest() or servers[0]
        transport = self._transports.get(fastest.name)
        if transport is None:
            raise RuntimeError(f"missing transport for '{fastest.name}'")
        start = time.time()
        rows = transport.scan(db, dataset, fields, columns, predicates, limit, offset)
        latency = (time.time() - start) * 1000.0
        self.mark_success(fastest.name, latency)
        stats = {
            "server": fastest.name,
            "latency_ms": latency,
            "servers_contacted": 1,
            "servers_succeeded": 1,
            "servers_failed": 0,
            "rows_returned": len(rows),
        }
        return rows, stats

    def policy(self) -> ApiPolicy:
        return self._policy

    def replay_for(self, name: str) -> None:
        transport = self._transports.get(name)
        if transport is None:
            raise RuntimeError(f"missing transport for '{name}'")
        if name not in self._servers:
            raise RuntimeError(f"unknown server '{name}'")

        def replay():
            for entry in list(self._replay_log):
                try:
                    transport.append_batch(
                        entry["db"],
                        entry["dataset"],
                        entry["fields"],
                        entry["columns"],
                        entry["batch_id"],
                    )
                    self.mark_success(name)
                except Exception:
                    self.mark_failure(name)
                    break

        self._executor.submit(replay)

    def enable_disk_replay_log(self, path: str, max_entries: int = 1024) -> None:
        log_path = Path(path)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self._replay_log_path = log_path
        self._replay_log = deque(maxlen=max_entries)
        if log_path.exists():
            with log_path.open("r", encoding="utf-8") as handle:
                for line in handle:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        entry = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if self._valid_replay_entry(entry):
                        self._replay_log.append(entry)
        self._replay_log_file = log_path.open("a", encoding="utf-8")

    def disable_disk_replay_log(self) -> None:
        if self._replay_log_file is not None:
            self._replay_log_file.close()
            self._replay_log_file = None
        self._replay_log_path = None

    def _append_replay_entry(self, entry: dict) -> None:
        if self._replay_log_file is None:
            return
        self._replay_log_file.write(json.dumps(entry) + "\n")
        self._replay_log_file.flush()

    def _valid_replay_entry(self, entry: dict) -> bool:
        return all(key in entry for key in ("db", "dataset", "fields", "columns", "batch_id"))

    def fanout_query_agg(
        self,
        db: str,
        dataset: str,
        field_index: int,
        predicates: list[tuple[int, int, float]] | None = None,
    ) -> dict[str, float]:
        results = []
        stats = {
            "servers_contacted": 0,
            "servers_succeeded": 0,
            "servers_failed": 0,
        }
        for name, transport in self._transports.items():
            if name not in self._servers or not self._is_healthy(self._servers[name]):
                continue
            try:
                results.append(transport.query_agg(db, dataset, field_index, predicates))
                self.mark_success(name)
                stats["servers_contacted"] += 1
                stats["servers_succeeded"] += 1
            except Exception:
                self.mark_failure(name)
                stats["servers_contacted"] += 1
                stats["servers_failed"] += 1
        if not results:
            raise RuntimeError("no successful query results")
        merged = {
            "count": sum(r.get("count", 0) for r in results),
            "sum": sum(r.get("sum", 0.0) for r in results),
        }
        mins = [r.get("min") for r in results if r.get("min") is not None]
        maxs = [r.get("max") for r in results if r.get("max") is not None]
        merged["min"] = min(mins) if mins else None
        merged["max"] = max(maxs) if maxs else None
        rows = [r.get("rows_scanned") for r in results if r.get("rows_scanned") is not None]
        if rows:
            stats["rows_scanned"] = sum(rows)
        return {"result": merged, "stats": stats}

    def resolve_schema(self, field_names: list[str], target: list[str]) -> list[int]:
        return [field_names.index(name) for name in target]

    def shape_result(self, rows: list[dict], fields: list[str]) -> list[dict]:
        return [{key: row.get(key) for key in fields} for row in rows]

    def sort_rows(self, rows: list[dict], key: str) -> list[dict]:
        return sorted(rows, key=lambda item: item.get(key))
