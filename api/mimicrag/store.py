from __future__ import annotations

from abc import ABC, abstractmethod
import json
import threading
from typing import Iterable

from .models import Chunk, Document, DocumentVersion, Publication


DOCUMENT_FIELDS = {"document_id": "string", "tenant_id": "string", "source_uri": "string", "title": "string", "metadata_json": "string"}
VERSION_FIELDS = {"version_id": "string", "document_id": "string", "generation": "int64", "content_hash": "string", "created_at_ms": "int64", "chunk_count": "int32"}
CHUNK_FIELDS = {"chunk_id": "string", "document_id": "string", "version_id": "string", "tenant_id": "string", "ordinal": "int32", "text": "string", "start_char": "int64", "end_char": "int64", "token_estimate": "int32", "metadata_json": "string"}
PUBLICATION_FIELDS = {"document_id": "string", "version_id": "string", "generation": "int64", "published_at_ms": "int64"}


class RagStore(ABC):
    @abstractmethod
    def next_generation(self, document_id: str) -> int: ...
    @abstractmethod
    def stage(self, document: Document, version: DocumentVersion, chunks: list[Chunk]) -> None: ...
    @abstractmethod
    def publish(self, publication: Publication) -> None: ...
    @abstractmethod
    def current_version(self, document_id: str) -> DocumentVersion | None: ...
    @abstractmethod
    def current_chunks(self, document_id: str) -> list[Chunk]: ...


class InMemoryRagStore(RagStore):
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self.documents: list[Document] = []
        self.versions: list[DocumentVersion] = []
        self.chunks: list[Chunk] = []
        self.publications: list[Publication] = []

    def next_generation(self, document_id: str) -> int:
        with self._lock:
            return max((p.generation for p in self.publications if p.document_id == document_id), default=0) + 1

    def stage(self, document: Document, version: DocumentVersion, chunks: list[Chunk]) -> None:
        with self._lock:
            self.documents.append(document)
            self.versions.append(version)
            self.chunks.extend(chunks)

    def publish(self, publication: Publication) -> None:
        with self._lock:
            if not any(v.version_id == publication.version_id and v.document_id == publication.document_id for v in self.versions):
                raise ValueError("cannot publish an unstaged version")
            self.publications.append(publication)

    def _publication(self, document_id: str) -> Publication | None:
        values = [p for p in self.publications if p.document_id == document_id]
        return max(values, key=lambda p: p.generation) if values else None

    def current_version(self, document_id: str) -> DocumentVersion | None:
        with self._lock:
            publication = self._publication(document_id)
            return next((v for v in reversed(self.versions) if publication and v.version_id == publication.version_id), None)

    def current_chunks(self, document_id: str) -> list[Chunk]:
        with self._lock:
            publication = self._publication(document_id)
            return sorted((c for c in self.chunks if publication and c.version_id == publication.version_id), key=lambda c: c.ordinal)


class MimicDBRagStore(RagStore):
    """Append-only RAG catalog backed by four MimicDB datasets."""

    def __init__(self, *, host: str | None = None, port: int | None = None, database: str = "mimicrag", use_cpp: bool = True, identity_key_path: str | None = None) -> None:
        from mimicapi import Dataset
        options = {"host": host, "port": port, "database": database, "use_cpp": use_cpp, "identity_key_path": identity_key_path}
        self.documents = Dataset("rag_documents", DOCUMENT_FIELDS, **options)
        self.versions = Dataset("rag_versions", VERSION_FIELDS, **options)
        self.chunks = Dataset("rag_chunks", CHUNK_FIELDS, **options)
        self.publications = Dataset("rag_publications", PUBLICATION_FIELDS, **options)
        self._lock = threading.RLock()

    def _rows(self, dataset) -> list[dict]:
        return dataset.scan(limit=0)

    def next_generation(self, document_id: str) -> int:
        with self._lock:
            return max((int(row["generation"]) for row in self._rows(self.publications) if row["document_id"] == document_id), default=0) + 1

    def stage(self, document: Document, version: DocumentVersion, chunks: list[Chunk]) -> None:
        with self._lock:
            self.documents.append(document_id=document.document_id, tenant_id=document.tenant_id, source_uri=document.source_uri, title=document.title, metadata_json=json.dumps(document.metadata, separators=(",", ":")))
            self.versions.append(**version.__dict__)
            if chunks:
                keys = list(CHUNK_FIELDS)
                columns = {key: [] for key in keys}
                for chunk in chunks:
                    row = {**chunk.__dict__, "metadata_json": json.dumps(chunk.metadata, separators=(",", ":"))}
                    row.pop("metadata")
                    for key in keys:
                        columns[key].append(row[key])
                self.chunks.append_batch(columns)

    def publish(self, publication: Publication) -> None:
        with self._lock:
            if not any(row["version_id"] == publication.version_id for row in self._rows(self.versions)):
                raise ValueError("cannot publish an unstaged version")
            self.publications.append(**publication.__dict__)

    def _current_id(self, document_id: str) -> str | None:
        rows = [row for row in self._rows(self.publications) if row["document_id"] == document_id]
        return str(max(rows, key=lambda row: int(row["generation"]))["version_id"]) if rows else None

    def current_version(self, document_id: str) -> DocumentVersion | None:
        with self._lock:
            version_id = self._current_id(document_id)
            row = next((r for r in reversed(self._rows(self.versions)) if r["version_id"] == version_id), None)
            return DocumentVersion(**row) if row else None

    def current_chunks(self, document_id: str) -> list[Chunk]:
        with self._lock:
            version_id = self._current_id(document_id)
            result = []
            for row in self._rows(self.chunks):
                if row["version_id"] != version_id:
                    continue
                values = dict(row)
                values["metadata"] = json.loads(values.pop("metadata_json"))
                result.append(Chunk(**values))
            return sorted(result, key=lambda value: value.ordinal)
