from __future__ import annotations

import json
import os
import time
import urllib.request
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, Protocol

from .adapters import AdapterRegistry, NormalizedDocument
from .sources import SourceObject


@dataclass
class ModelRoute:
    language: str
    embedding_model: str = ""
    analysis_model: str = ""


@dataclass
class ManifestEntry:
    source_uri: str
    status: str
    document_id: str = ""
    content_hash: str = ""
    etag: str = ""
    modified: str = ""
    language: str = "und"
    warnings: list[str] = field(default_factory=list)
    error: str = ""
    previous_uri: str = ""


@dataclass
class Manifest:
    version: int = 1
    started_at_ms: int = 0
    finished_at_ms: int = 0
    entries: list[ManifestEntry] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        entries = [asdict(entry) for entry in self.entries]
        return {"format": "mimicrag-ingestion-manifest", "version": self.version,
                "started_at_ms": self.started_at_ms, "finished_at_ms": self.finished_at_ms,
                "summary": {status: sum(e.status == status for e in self.entries) for status in
                            ("created", "changed", "unchanged", "renamed", "deleted", "duplicate", "rejected")},
                "entries": entries}

    def write(self, path: str | Path) -> None:
        target = Path(path); target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(target.suffix + ".tmp")
        temporary.write_text(json.dumps(self.to_dict(), indent=2, ensure_ascii=False) + "\n")
        os.replace(temporary, target)


class Sink(Protocol):
    def ingest(self, document: NormalizedDocument, tenant_id: str, route: ModelRoute) -> dict[str, Any]: ...
    def delete(self, document_id: str, tenant_id: str) -> dict[str, Any]: ...


class MimicRagSink:
    def __init__(self, base_url: str, api_key: str = ""):
        self.base_url = base_url.rstrip("/"); self.api_key = api_key

    def _request(self, method: str, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        headers = {"Content-Type": "application/json"}
        if self.api_key: headers["Authorization"] = "Bearer " + self.api_key
        request = urllib.request.Request(self.base_url + path, data=None if payload is None else json.dumps(payload).encode(), headers=headers, method=method)
        with urllib.request.urlopen(request, timeout=120) as response: return json.load(response)

    def ingest(self, document: NormalizedDocument, tenant_id: str, route: ModelRoute) -> dict[str, Any]:
        metadata = document.ingestion_metadata()
        metadata["language"] = route.language
        metadata["model_route"] = {"embedding": route.embedding_model, "analysis": route.analysis_model}
        wire_format = document.format if document.format in ("markdown", "html", "text") else "text"
        return self._request("POST", "/v1/documents", {"tenant_id": tenant_id, "source_uri": document.source_uri,
            "title": document.title, "format": wire_format, "mode": "structured", "text": document.text, "metadata": metadata})

    def delete(self, document_id: str, tenant_id: str) -> dict[str, Any]:
        return self._request("DELETE", "/v1/documents/" + document_id, {"tenant_id": tenant_id})


def detect_language(text: str) -> str:
    sample = text[:10000]
    scripts = {"zh": len(__import__("re").findall(r"[\u4e00-\u9fff]", sample)),
               "ja": len(__import__("re").findall(r"[\u3040-\u30ff]", sample)),
               "ko": len(__import__("re").findall(r"[\uac00-\ud7af]", sample)),
               "ar": len(__import__("re").findall(r"[\u0600-\u06ff]", sample)),
               "ru": len(__import__("re").findall(r"[\u0400-\u04ff]", sample))}
    language, count = max(scripts.items(), key=lambda item: item[1])
    if count >= max(3, len(sample) // 20): return language
    words = set(__import__("re").findall(r"[a-zà-ÿ]+", sample.lower()))
    markers = {"en": {"the", "and", "this", "with"}, "es": {"el", "la", "de", "que"},
               "fr": {"le", "la", "de", "et"}, "de": {"der", "die", "das", "und"},
               "pt": {"o", "a", "de", "que"}}
    scores = {code: len(words & tokens) for code, tokens in markers.items()}
    selected, score = max(scores.items(), key=lambda item: item[1])
    return selected if score else "und"


class IngestionPipeline:
    def __init__(self, sink: Sink, *, adapters: AdapterRegistry | None = None,
                 routes: dict[str, ModelRoute] | None = None, default_route: ModelRoute | None = None):
        self.sink = sink; self.adapters = adapters or AdapterRegistry(); self.routes = routes or {}
        self.default_route = default_route or ModelRoute("multilingual")

    @staticmethod
    def _load_state(path: Path) -> dict[str, dict[str, Any]]:
        if not path.exists(): return {}
        value = json.loads(path.read_text())
        return {entry["source_uri"]: entry for entry in value.get("entries", []) if entry.get("status") != "deleted" and entry.get("document_id")}

    def sync(self, objects: Iterable[SourceObject], *, state_path: str | Path, tenant_id: str = "default") -> Manifest:
        path = Path(state_path); previous = self._load_state(path); started = int(time.time() * 1000)
        manifest = Manifest(started_at_ms=started); current: set[str] = set(); hashes: dict[str, ManifestEntry] = {}
        for old in previous.values():
            if old.get("content_hash"): hashes[old["content_hash"]] = ManifestEntry(**{k: v for k, v in old.items() if k in ManifestEntry.__dataclass_fields__})
        # Resolve known URIs before new ones so a new identical file is a duplicate,
        # while a genuinely missing old URI can still be recognized as a rename.
        source_objects = list(objects)
        source_objects.sort(key=lambda source: (source.uri not in previous, source.uri))
        for source in source_objects:
            current.add(source.uri)
            try:
                document = self.adapters.parse(source.data, source.uri, media_type=source.media_type,
                    title=str((source.metadata or {}).get("name", "")))
                digest = document.content_hash; old = previous.get(source.uri)
                if old and old.get("content_hash") == digest:
                    manifest.entries.append(ManifestEntry(source.uri, "unchanged", old.get("document_id", ""), digest, source.etag, source.modified, old.get("language", "und"), document.warnings)); continue
                duplicate = hashes.get(digest)
                if duplicate and duplicate.source_uri != source.uri and duplicate.source_uri in current:
                    manifest.entries.append(ManifestEntry(source.uri, "duplicate", "", digest, source.etag, source.modified, duplicate.language, document.warnings)); continue
                language = detect_language(document.text); configured = self.routes.get(language, self.default_route)
                route = ModelRoute(language, configured.embedding_model, configured.analysis_model)
                result = self.sink.ingest(document, tenant_id, route)
                status = "changed" if old else "created"; previous_uri = ""
                if not old and duplicate and duplicate.source_uri not in current:
                    status = "renamed"; previous_uri = duplicate.source_uri
                    if duplicate.document_id != result.get("document_id", ""): self.sink.delete(duplicate.document_id, tenant_id)
                entry = ManifestEntry(source.uri, status, result.get("document_id", ""), digest, source.etag, source.modified, language, document.warnings, previous_uri=previous_uri)
                manifest.entries.append(entry); hashes[digest] = entry
            except Exception as exc:
                manifest.entries.append(ManifestEntry(source.uri, "rejected", etag=source.etag, modified=source.modified, error=str(exc)))
        for uri, old in previous.items():
            if uri in current or any(entry.previous_uri == uri for entry in manifest.entries): continue
            try:
                self.sink.delete(old["document_id"], tenant_id)
                manifest.entries.append(ManifestEntry(uri, "deleted", old["document_id"], old.get("content_hash", ""), language=old.get("language", "und")))
            except Exception as exc:
                manifest.entries.append(ManifestEntry(uri, "rejected", old.get("document_id", ""), error="delete failed: " + str(exc)))
        manifest.finished_at_ms = int(time.time() * 1000); manifest.write(path); return manifest

    def watch(self, source: Any, *, state_path: str | Path, tenant_id: str = "default", interval_seconds: float = 2.0,
              stop: Callable[[], bool] | None = None) -> None:
        stop = stop or (lambda: False)
        while not stop():
            self.sync(source.objects(), state_path=state_path, tenant_id=tenant_id)
            time.sleep(interval_seconds)
