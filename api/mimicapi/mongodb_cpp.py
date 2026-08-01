from __future__ import annotations

from .mongodb import (
    InsertManyResult,
    InsertOneResult,
    BulkWriteResult,
    UpdateResult,
    DeleteResult,
    DuplicateKeyError,
    OperationFailure,
    _apply_projection,
    _get_values,
    _match_filter,
)


class Session:
    def __init__(self, token: int) -> None:
        self.token = token

    def with_transaction(self, callback, *args, **kwargs):
        return callback(*args, **kwargs)

    def end_session(self) -> None:
        return None

try:
    from . import _mimicapi_mongo
except ImportError as exc:  # pragma: no cover
    _mimicapi_mongo = None
    _mimicapi_mongo_error = str(exc)
else:
    _mimicapi_mongo_error = None


class MongoClientCpp:
    def __init__(self) -> None:
        if _mimicapi_mongo is None:
            raise RuntimeError("mimicapi mongo core extension is not available")
        self._core = _mimicapi_mongo.MongoClientCore()
        self._session_token = 0

    def start_session(self, **kwargs) -> Session:
        _ = kwargs
        self._session_token += 1
        return Session(self._session_token)

    def insert_one(self, db: str, collection: str, doc: dict, **kwargs) -> InsertOneResult:
        _ = kwargs
        result = self.insert_many(db, collection, [doc])
        return InsertOneResult(doc.get("_id"), acknowledged=result.acknowledged)

    def insert_many(self, db: str, collection: str, docs: list[dict],
                    ordered: bool = True, **kwargs) -> InsertManyResult:
        _ = ordered
        _ = kwargs
        if not docs:
            return InsertManyResult(0)
        seen_ids: set[int] = set()
        batch_ids: list[int] = []
        for doc in docs:
            doc_id = doc.get("_id")
            if doc_id is None:
                continue
            if not isinstance(doc_id, int):
                raise OperationFailure("_id must be int64")
            if doc_id in seen_ids:
                raise DuplicateKeyError(f"duplicate _id {doc_id}")
            seen_ids.add(doc_id)
            batch_ids.append(doc_id)
        if batch_ids:
            try:
                existing = self._core.find(
                    db,
                    collection,
                    {"_id": {"$in": batch_ids}},
                    {"_id": 1},
                    None,
                    0,
                    0,
                )
            except RuntimeError as exc:
                message = str(exc)
                if "unknown collection" not in message:
                    raise OperationFailure(message) from exc
            else:
                existing_ids = {doc.get("_id") for doc in existing}
                dupes = seen_ids & existing_ids
                if dupes:
                    raise DuplicateKeyError(f"duplicate _id {sorted(dupes)}")
        try:
            self._core.insert_many(db, collection, docs)
        except RuntimeError as exc:
            message = str(exc)
            if "duplicate" in message:
                raise DuplicateKeyError(message) from exc
            raise OperationFailure(message) from exc
        return InsertManyResult(len(docs))

    def bulk_write(self, db: str, collection: str, operations: list[dict],
                   ordered: bool = True, **kwargs) -> BulkWriteResult:
        _ = kwargs
        inserted = 0
        matched = 0
        modified = 0
        deleted = 0
        upserted = 0
        upserted_ids: dict[int, object] = {}
        for index, op in enumerate(operations):
            try:
                if not isinstance(op, dict) or len(op) != 1:
                    raise ValueError("bulk_write operations must be single-key dicts")
                name, payload = next(iter(op.items()))
                if name == "insert_one":
                    self.insert_one(db, collection, payload)
                    inserted += 1
                elif name == "insert_many":
                    result = self.insert_many(db, collection, payload)
                    inserted += result.inserted_count
                elif name == "update_one":
                    result = self.update_one(
                        db,
                        collection,
                        payload["filter"],
                        payload["update"],
                        upsert=payload.get("upsert", False),
                    )
                    matched += result.matched_count
                    modified += result.modified_count
                    if payload.get("upsert", False) and result.matched_count == 0 and result.modified_count > 0:
                        upserted += 1
                        upserted_ids[index] = None
                elif name == "update_many":
                    result = self.update_many(
                        db,
                        collection,
                        payload["filter"],
                        payload["update"],
                        upsert=payload.get("upsert", False),
                    )
                    matched += result.matched_count
                    modified += result.modified_count
                    if payload.get("upsert", False) and result.matched_count == 0 and result.modified_count > 0:
                        upserted += 1
                        upserted_ids[index] = None
                elif name == "replace_one":
                    result = self.replace_one(
                        db,
                        collection,
                        payload["filter"],
                        payload["replacement"],
                        upsert=payload.get("upsert", False),
                    )
                    matched += result.matched_count
                    modified += result.modified_count
                    if payload.get("upsert", False) and result.matched_count == 0 and result.modified_count > 0:
                        upserted += 1
                        upserted_ids[index] = None
                elif name == "delete_one":
                    result = self.delete_one(db, collection, payload["filter"])
                    deleted += result.deleted_count
                elif name == "delete_many":
                    result = self.delete_many(db, collection, payload["filter"])
                    deleted += result.deleted_count
                else:
                    raise NotImplementedError(f"unsupported bulk op '{name}'")
            except Exception:
                if ordered:
                    raise
        return BulkWriteResult(inserted, matched, modified, deleted, upserted, upserted_ids)

    def find(self, db: str, collection: str, filter: dict | None = None,
             projection: dict | list[str] | None = None,
             sort: list[tuple[str, int]] | dict | None = None,
             skip: int = 0,
             limit: int = 0,
             hint=None) -> "Cursor":
        _ = hint
        return Cursor(self._core, db, collection, filter, projection, sort, skip, limit)

    def update(self, db: str, collection: str, filter: dict | None, update: dict,
               multi: bool = False, upsert: bool = False, replace: bool = False) -> int:
        return self._core.update(db, collection, filter, update, multi, upsert, replace)

    def update_one(self, db: str, collection: str, filter: dict | None, update: dict,
                   upsert: bool = False) -> UpdateResult:
        matched = self.update(db, collection, filter, update, multi=False, upsert=upsert)
        modified = matched if matched > 0 else (1 if upsert else 0)
        return UpdateResult(matched, modified)

    def update_many(self, db: str, collection: str, filter: dict | None, update: dict,
                    upsert: bool = False) -> UpdateResult:
        matched = self.update(db, collection, filter, update, multi=True, upsert=upsert)
        modified = matched if matched > 0 else (1 if upsert else 0)
        return UpdateResult(matched, modified)

    def replace_one(self, db: str, collection: str, filter: dict | None,
                    replacement: dict, upsert: bool = False) -> UpdateResult:
        matched = self.update(db, collection, filter, replacement,
                              multi=False, upsert=upsert, replace=True)
        modified = matched if matched > 0 else (1 if upsert else 0)
        return UpdateResult(matched, modified)

    def delete(self, db: str, collection: str, filter: dict | None,
               multi: bool = False) -> int:
        return self._core.delete(db, collection, filter, multi)

    def delete_one(self, db: str, collection: str, filter: dict | None, **kwargs) -> DeleteResult:
        _ = kwargs
        deleted = self.delete(db, collection, filter, multi=False)
        return DeleteResult(deleted)

    def delete_many(self, db: str, collection: str, filter: dict | None, **kwargs) -> DeleteResult:
        _ = kwargs
        deleted = self.delete(db, collection, filter, multi=True)
        return DeleteResult(deleted)

    def find_one_and_delete(
        self,
        db: str,
        collection: str,
        filter: dict | None,
        projection: dict | list[str] | None = None,
        sort: list[tuple[str, int]] | dict | None = None,
        **kwargs,
    ):
        _ = kwargs
        docs = self._core.find(db, collection, filter, projection, sort, 0, 1)
        if not docs:
            return None
        doc = docs[0]
        if "_id" in doc:
            doc_id = doc["_id"]
        else:
            id_docs = self._core.find(db, collection, filter, {"_id": 1}, sort, 0, 1)
            if not id_docs or "_id" not in id_docs[0]:
                return None
            doc_id = id_docs[0]["_id"]
        self._core.delete(db, collection, {"_id": doc_id}, False)
        return doc

    def find_one_and_update(
        self,
        db: str,
        collection: str,
        filter: dict | None,
        update: dict,
        projection: dict | list[str] | None = None,
        sort: list[tuple[str, int]] | dict | None = None,
        return_document: str = "after",
        upsert: bool = False,
        **kwargs,
    ):
        _ = kwargs
        docs = self._core.find(db, collection, filter, projection, sort, 0, 1)
        if not docs:
            if not upsert:
                return None
            result = self.update_one(db, collection, filter, update, upsert=True)
            if result.modified_count > 0:
                result = self._core.find(db, collection, filter, projection, sort, 0, 1)
                return result[0] if result else None
            return None
        original = docs[0]
        if return_document == "before":
            result_doc = original
        else:
            result_doc = None
        if "_id" in original:
            doc_id = original["_id"]
        else:
            id_docs = self._core.find(db, collection, filter, {"_id": 1}, sort, 0, 1)
            if not id_docs or "_id" not in id_docs[0]:
                return result_doc
            doc_id = id_docs[0]["_id"]
        self._core.update(db, collection, {"_id": doc_id}, update, False, False, False)
        if return_document == "after":
            updated_docs = self._core.find(db, collection, {"_id": doc_id}, projection, None, 0, 1)
            return updated_docs[0] if updated_docs else None
        return result_doc

    def create_index(self, db: str, collection: str, keys, **kwargs):
        _ = db
        _ = collection
        _ = keys
        _ = kwargs
        return "noop"

    def drop_index(self, db: str, collection: str, index_or_name, **kwargs):
        _ = db
        _ = collection
        _ = index_or_name
        _ = kwargs
        return None

    def aggregate(self, db: str, collection: str, pipeline: list[dict]) -> list[dict]:
        return self._core.aggregate(db, collection, pipeline)

    def stats(self, db: str, collection: str) -> dict:
        return self._core.stats(db, collection)


class Cursor:
    def __init__(
        self,
        core,
        db: str,
        collection: str,
        filter: dict | None,
        projection: dict | list[str] | None,
        sort: list[tuple[str, int]] | dict | None,
        skip: int,
        limit: int,
    ) -> None:
        self._core = core
        self._db = db
        self._collection = collection
        self._filter = filter
        self._projection = projection
        self._sort = sort
        self._skip = max(0, int(skip))
        self._limit = max(0, int(limit))

    def sort(self, sort: list[tuple[str, int]] | dict) -> "Cursor":
        self._sort = sort
        return self

    def skip(self, count: int) -> "Cursor":
        self._skip = max(0, int(count))
        return self

    def limit(self, count: int) -> "Cursor":
        self._limit = max(0, int(count))
        return self

    def to_list(self) -> list[dict]:
        if self._filter and _has_compound_filter(self._filter):
            docs = self._core.find(
                self._db,
                self._collection,
                None,
                None,
                None,
                0,
                0,
            )
            docs = [doc for doc in docs if _match_filter(doc, self._filter)]
            if self._sort:
                docs = _apply_sort(docs, self._sort)
            if self._projection is not None:
                docs = _apply_projection(docs, self._projection)
            if self._skip:
                docs = docs[self._skip:]
            if self._limit:
                docs = docs[:self._limit]
            return docs
        return self._core.find(
            self._db,
            self._collection,
            self._filter,
            self._projection,
            self._sort,
            self._skip,
            self._limit,
        )

    def __iter__(self):
        return iter(self.to_list())


def _has_compound_filter(filter: dict | None) -> bool:
    if not isinstance(filter, dict):
        return False
    return any(key in ("$or", "$and", "$nor") for key in filter.keys())


def _apply_sort(docs: list[dict], sort: list[tuple[str, int]] | dict) -> list[dict]:
    if isinstance(sort, dict):
        items = list(sort.items())
    else:
        items = list(sort)
    def sort_key(doc):
        return tuple(_get_values(doc, name)[0] if _get_values(doc, name) else None
                     for name, _ in items)
    reverse = any(direction < 0 for _, direction in items)
    return sorted(docs, key=sort_key, reverse=reverse)
