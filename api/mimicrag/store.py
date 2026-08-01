from __future__ import annotations

from abc import ABC, abstractmethod
import json
import threading
import hashlib

from .models import Chunk, Document, DocumentVersion, EmbeddingRecord, Publication


DOCUMENT_FIELDS = {"document_id": "string", "tenant_id": "string", "source_uri": "string", "title": "string", "metadata_json": "string"}
VERSION_FIELDS = {"version_id": "string", "document_id": "string", "generation": "int64", "content_hash": "string", "created_at_ms": "int64", "chunk_count": "int32"}
CHUNK_FIELDS = {"chunk_id": "string", "document_id": "string", "version_id": "string", "tenant_id": "string", "ordinal": "int32", "text": "string", "start_char": "int64", "end_char": "int64", "token_estimate": "int32", "metadata_json": "string"}
PUBLICATION_FIELDS = {"document_id": "string", "version_id": "string", "generation": "int64", "published_at_ms": "int64"}
EMBEDDING_FIELDS = {"chunk_id": "string", "version_id": "string", "tenant_tag": "int64", "access_tag": "int64", "model_key": "string", "model_tag": "int64", "created_at_ms": "int64", "embedding": "vector_float32"}


def tenant_tag(tenant_id: str) -> int:
    # The wire predicate value is a float64, so keep the stable tag exactly representable.
    return int.from_bytes(hashlib.blake2b(tenant_id.encode("utf-8"), digest_size=8).digest(), "little") & ((1 << 53) - 1)


def identity_tag(value: str) -> int:
    return int.from_bytes(hashlib.blake2b(value.encode("utf-8"), digest_size=8).digest(), "little") & ((1 << 53) - 1)


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
    @abstractmethod
    def visible_chunks(self, tenant_id: str, access_scope: str | None = "public") -> list[Chunk]: ...
    @abstractmethod
    def document(self, document_id: str) -> Document | None: ...
    @abstractmethod
    def put_embeddings(self, records: list[EmbeddingRecord]) -> int: ...
    @abstractmethod
    def embedded_chunk_ids(self, model_key: str) -> set[str]: ...
    @abstractmethod
    def vector_candidates(self, query: list[float], tenant_id: str, access_scope: str, model_key: str, top_k: int, approximate: bool, probes: int = 0) -> list[tuple[str, float]]: ...


class InMemoryRagStore(RagStore):
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self.documents: list[Document] = []
        self.versions: list[DocumentVersion] = []
        self.chunks: list[Chunk] = []
        self.publications: list[Publication] = []
        self.embeddings: list[EmbeddingRecord] = []
        self._published: dict[str, Publication] = {}

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
            current = self._published.get(publication.document_id)
            if current is None or publication.generation >= current.generation:
                self._published[publication.document_id] = publication

    def _publication(self, document_id: str) -> Publication | None:
        return self._published.get(document_id)

    def current_version(self, document_id: str) -> DocumentVersion | None:
        with self._lock:
            publication = self._publication(document_id)
            return next((v for v in reversed(self.versions) if publication and v.version_id == publication.version_id), None)

    def current_chunks(self, document_id: str) -> list[Chunk]:
        with self._lock:
            publication = self._publication(document_id)
            return sorted((c for c in self.chunks if publication and c.version_id == publication.version_id), key=lambda c: c.ordinal)

    def visible_chunks(self, tenant_id: str, access_scope: str | None = "public") -> list[Chunk]:
        with self._lock:
            current = {p.version_id for p in self._published.values()}
            scopes = None if access_scope is None else {"public", access_scope}
            return [c for c in self.chunks if c.tenant_id == tenant_id and c.version_id in current and (scopes is None or str(c.metadata.get("access_scope", "public")) in scopes)]

    def document(self, document_id: str) -> Document | None:
        with self._lock:
            return next((d for d in reversed(self.documents) if d.document_id == document_id), None)

    def put_embeddings(self, records: list[EmbeddingRecord]) -> int:
        with self._lock:
            existing = {(r.chunk_id, r.model_key) for r in self.embeddings}
            fresh = [r for r in records if (r.chunk_id, r.model_key) not in existing]
            self.embeddings.extend(fresh)
            return len(fresh)

    def embedded_chunk_ids(self, model_key: str) -> set[str]:
        with self._lock:
            return {r.chunk_id for r in self.embeddings if r.model_key == model_key}

    def vector_candidates(self, query: list[float], tenant_id: str, access_scope: str, model_key: str, top_k: int, approximate: bool, probes: int = 0) -> list[tuple[str, float]]:
        del approximate, probes
        import math
        with self._lock:
            allowed = {c.chunk_id for c in self.visible_chunks(tenant_id, access_scope)}
            qnorm = math.sqrt(sum(v * v for v in query))
            ranked = []
            for record in self.embeddings:
                if record.model_key != model_key or record.tenant_id != tenant_id or record.chunk_id not in allowed or len(record.vector) != len(query):
                    continue
                norm = math.sqrt(sum(v * v for v in record.vector))
                score = sum(a * b for a, b in zip(query, record.vector)) / (qnorm * norm) if qnorm and norm else 0.0
                ranked.append((record.chunk_id, score))
            return sorted(ranked, key=lambda item: (-item[1], item[0]))[:top_k]


class MimicDBRagStore(RagStore):
    """Append-only RAG catalog backed by MimicDB datasets."""

    def __init__(self, *, host: str | None = None, port: int | None = None, database: str = "mimicrag", use_cpp: bool = True, identity_key_path: str | None = None) -> None:
        from mimicapi import Dataset
        if host is not None or port is not None:
            self._ensure_network_schema(host or "127.0.0.1", port or 9000, database, identity_key_path)
        options = {"host": host, "port": port, "database": database, "use_cpp": use_cpp, "identity_key_path": identity_key_path, "create": not (host is not None or port is not None)}
        self.documents = Dataset("rag_documents", DOCUMENT_FIELDS, **options)
        self.versions = Dataset("rag_versions", VERSION_FIELDS, **options)
        self.chunks = Dataset("rag_chunks", CHUNK_FIELDS, **options)
        self.publications = Dataset("rag_publications", PUBLICATION_FIELDS, **options)
        self.embeddings = Dataset("rag_embeddings", EMBEDDING_FIELDS, **options)
        self._lock = threading.RLock()

    @staticmethod
    def _ensure_network_schema(host: str, port: int, database: str, identity_key_path: str | None) -> None:
        from client.mimicdb_client import MimicDBClient, ProtocolError
        client = MimicDBClient(host=host, port=port, default_db=database, identity_key_path=identity_key_path)
        try:
            if database not in client.list_databases():
                client.create_database(database)
            for name, fields in (("rag_documents", DOCUMENT_FIELDS), ("rag_versions", VERSION_FIELDS), ("rag_chunks", CHUNK_FIELDS), ("rag_publications", PUBLICATION_FIELDS), ("rag_embeddings", EMBEDDING_FIELDS)):
                try:
                    client.create_dataset(name, list(fields.items()), database=database)
                except ProtocolError:
                    # The protocol has no list-datasets operation; duplicate creation is
                    # the expected restart path and the existing schema is used below.
                    pass
        finally:
            client.close()

    def _rows(self, dataset) -> list[dict]:
        return dataset.scan(limit=0)

    def _embedding_rows(self) -> list[dict]:
        return self.embeddings.scan(columns=[name for name in EMBEDDING_FIELDS if name != "embedding"], limit=0)

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

    def visible_chunks(self, tenant_id: str, access_scope: str | None = "public") -> list[Chunk]:
        with self._lock:
            publications: dict[str, dict] = {}
            for row in self._rows(self.publications):
                old = publications.get(row["document_id"])
                if old is None or int(row["generation"]) > int(old["generation"]):
                    publications[row["document_id"]] = row
            version_ids = {str(row["version_id"]) for row in publications.values()}
            result = []
            for row in self._rows(self.chunks):
                if row["tenant_id"] != tenant_id or row["version_id"] not in version_ids:
                    continue
                values = dict(row)
                values["metadata"] = json.loads(values.pop("metadata_json"))
                chunk = Chunk(**values)
                if access_scope is None or str(chunk.metadata.get("access_scope", "public")) in {"public", access_scope}:
                    result.append(chunk)
            return result

    def document(self, document_id: str) -> Document | None:
        with self._lock:
            row = next((r for r in reversed(self._rows(self.documents)) if r["document_id"] == document_id), None)
            if not row:
                return None
            values = dict(row)
            values["metadata"] = json.loads(values.pop("metadata_json"))
            return Document(**values)

    def put_embeddings(self, records: list[EmbeddingRecord]) -> int:
        with self._lock:
            existing = {(row["chunk_id"], row["model_key"]) for row in self._embedding_rows()}
            fresh = [record for record in records if (record.chunk_id, record.model_key) not in existing]
            if fresh:
                self.embeddings.append_batch({
                    "chunk_id": [r.chunk_id for r in fresh],
                    "version_id": [r.version_id for r in fresh],
                    "tenant_tag": [tenant_tag(r.tenant_id) for r in fresh],
                    "access_tag": [identity_tag(r.access_scope) for r in fresh],
                    "model_key": [r.model_key for r in fresh],
                    "model_tag": [identity_tag(r.model_key) for r in fresh],
                    "created_at_ms": [r.created_at_ms for r in fresh],
                    "embedding": [list(r.vector) for r in fresh],
                })
            return len(fresh)

    def embedded_chunk_ids(self, model_key: str) -> set[str]:
        with self._lock:
            return {row["chunk_id"] for row in self._embedding_rows() if row["model_key"] == model_key}

    def vector_candidates(self, query: list[float], tenant_id: str, access_scope: str, model_key: str, top_k: int, approximate: bool, probes: int = 0) -> list[tuple[str, float]]:
        with self._lock:
            fields = list(EMBEDDING_FIELDS)
            scopes = {"public", access_scope}
            hits = []
            for scope in scopes:
                predicates = [
                    (fields.index("tenant_tag"), 0, float(tenant_tag(tenant_id))),
                    (fields.index("access_tag"), 0, float(identity_tag(scope))),
                    (fields.index("model_tag"), 0, float(identity_tag(model_key))),
                ]
                hits.extend(self.embeddings.vector_search("embedding", query, top_k=top_k, metric="cosine", predicates=predicates, approximate=approximate, probes=probes))
            rows = self._embedding_rows()
            allowed = {c.chunk_id for c in self.visible_chunks(tenant_id, access_scope)}
            result = []
            for hit in hits:
                row_id = int(hit["row_id"])
                if row_id >= len(rows):
                    continue
                row = rows[row_id]
                if row["model_key"] == model_key and row["chunk_id"] in allowed:
                    # MimicDB returns ascending distance; expose a larger-is-better score.
                    result.append((str(row["chunk_id"]), -float(hit["distance"])))
            return sorted(result, key=lambda item: (-item[1], item[0]))[:top_k]
