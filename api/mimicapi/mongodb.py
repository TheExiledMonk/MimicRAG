from __future__ import annotations

from dataclasses import dataclass
import json
import re
import time

from .api_client import ApiClient


_OP_MAP = {
    "$eq": "eq",
    "$ne": "ne",
    "$lt": "lt",
    "$lte": "le",
    "$gt": "gt",
    "$gte": "ge",
}

_META_FIELDS = ["_id", "_deleted", "_version"]


@dataclass
class InsertManyResult:
    inserted_count: int
    acknowledged: bool = True


@dataclass
class InsertOneResult:
    inserted_id: int
    acknowledged: bool = True


@dataclass
class UpdateResult:
    matched_count: int
    modified_count: int
    acknowledged: bool = True


@dataclass
class DeleteResult:
    deleted_count: int
    acknowledged: bool = True


@dataclass
class BulkWriteResult:
    inserted_count: int
    matched_count: int
    modified_count: int
    deleted_count: int
    upserted_count: int
    upserted_ids: dict[int, object]
    acknowledged: bool = True


class MongoError(Exception):
    pass


class DuplicateKeyError(MongoError):
    pass


class OperationFailure(MongoError):
    pass


@dataclass
class Session:
    token: int

    def with_transaction(self, callback, *args, **kwargs):
        return callback(*args, **kwargs)

    def end_session(self) -> None:
        return None


class Cursor:
    def __init__(self, docs: list[dict], projection: dict | list[str] | None = None) -> None:
        self._docs = docs
        self._projection = projection
        self._skip = 0
        self._limit = 0
        self._sort = None

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
        docs = list(self._docs)
        if self._sort:
            if isinstance(self._sort, dict):
                items = list(self._sort.items())
            else:
                items = list(self._sort)
            def sort_key(doc):
                return tuple(_first_value(_get_values(doc, name)) for name, _ in items)
            reverse = any(direction < 0 for _, direction in items)
            docs = sorted(docs, key=sort_key, reverse=reverse)
        if self._skip:
            docs = docs[self._skip:]
        if self._limit:
            docs = docs[:self._limit]
        return _apply_projection(docs, self._projection)

    def __iter__(self):
        return iter(self.to_list())

class MongoClient:
    def __init__(
        self,
        api_client: ApiClient | None = None,
        backend_name: str = "primary",
        host: str | None = None,
        port: int | None = None,
        default_db: str = "default",
    ) -> None:
        if api_client is None:
            api_client = ApiClient()
            api_client.add_mimicdb_backend(
                backend_name,
                host=host,
                port=port,
                default_db=default_db,
            )
        self._client = api_client
        self._schemas: dict[tuple[str, str], list[tuple[str, str]]] = {}
        self._id_counters: dict[tuple[str, str], int] = {}
        self._json_fields: dict[tuple[str, str], set[str]] = {}
        self._version_counter = 0
        self._session_counter = 0

    def __getitem__(self, name: str) -> "Database":
        return Database(self._client, self._schemas, self._id_counters, self._json_fields, self, name)

    def _next_version(self) -> int:
        self._version_counter += 1
        return (time.time_ns() & 0xFFFFFFFFFFFFFFFF) ^ self._version_counter

    def start_session(self, **kwargs) -> Session:
        _ = kwargs
        self._session_counter += 1
        return Session(token=self._session_counter)


class Database:
    def __init__(
        self,
        client: ApiClient,
        schemas: dict[tuple[str, str], list[tuple[str, str]]],
        id_counters: dict[tuple[str, str], int],
        json_fields: dict[tuple[str, str], set[str]],
        owner: MongoClient,
        name: str,
    ) -> None:
        self._client = client
        self._schemas = schemas
        self._id_counters = id_counters
        self._json_fields = json_fields
        self._owner = owner
        self._name = name

    def __getitem__(self, name: str) -> "Collection":
        return Collection(
            self._client,
            self._schemas,
            self._id_counters,
            self._json_fields,
            self._owner,
            self._name,
            name,
        )


class Collection:
    def __init__(
        self,
        client: ApiClient,
        schemas: dict[tuple[str, str], list[tuple[str, str]]],
        id_counters: dict[tuple[str, str], int],
        json_fields: dict[tuple[str, str], set[str]],
        owner: MongoClient,
        db: str,
        name: str,
    ) -> None:
        self._client = client
        self._schemas = schemas
        self._id_counters = id_counters
        self._json_fields = json_fields
        self._owner = owner
        self._db = db
        self._name = name

    def insert_one(self, doc: dict, **kwargs) -> InsertOneResult:
        _ = kwargs
        result = self.insert_many([doc])
        return InsertOneResult(doc.get("_id"))

    def insert_many(self, docs: list[dict], ordered: bool = True, **kwargs) -> InsertManyResult:
        _ = ordered
        _ = kwargs
        if not docs:
            return InsertManyResult(0)
        existing_ids = {doc.get("_id") for doc in self._latest_documents()}
        seen_ids: set[int] = set()
        fields = self._ensure_schema(docs)
        columns = {name: [] for name, _ in fields}
        version = self._owner._next_version()
        json_fields = self._json_fields.setdefault((self._db, self._name), set())
        for doc in docs:
            doc_id = doc.get("_id")
            if doc_id is None:
                doc_id = self._next_id()
                doc["_id"] = doc_id
            elif not isinstance(doc_id, int):
                raise ValueError("_id must be int64")
            if doc_id in existing_ids or doc_id in seen_ids:
                raise DuplicateKeyError(f"duplicate _id {doc_id}")
            seen_ids.add(doc_id)
            row = dict(doc)
            row["_id"] = doc_id
            row["_deleted"] = False
            row["_version"] = version
            for field_name, _ in fields:
                columns[field_name].append(_encode_value(row.get(field_name), field_name, json_fields))
        self._client.append_batch_fanout(
            self._db,
            self._name,
            fields,
            columns,
        )
        return InsertManyResult(len(docs))

    def bulk_write(self, operations: list[dict], ordered: bool = True, **kwargs) -> BulkWriteResult:
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
                    self.insert_one(payload)
                    inserted += 1
                elif name == "insert_many":
                    result = self.insert_many(payload)
                    inserted += result.inserted_count
                elif name == "update_one":
                    result = self.update_one(
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
                    result = self.delete_one(payload["filter"])
                    deleted += result.deleted_count
                elif name == "delete_many":
                    result = self.delete_many(payload["filter"])
                    deleted += result.deleted_count
                else:
                    raise NotImplementedError(f"unsupported bulk op '{name}'")
            except Exception:
                if ordered:
                    raise
        return BulkWriteResult(inserted, matched, modified, deleted, upserted, upserted_ids)

    def find(
        self,
        filter: dict | None = None,
        projection: dict | list[str] | None = None,
        sort: list[tuple[str, int]] | dict | None = None,
        skip: int = 0,
        limit: int = 0,
        hint=None,
    ) -> Cursor:
        _ = hint
        docs = self._latest_documents()
        if filter:
            docs = [doc for doc in docs if _match_filter(doc, filter)]
        cursor = Cursor(docs, projection)
        if sort:
            cursor.sort(sort)
        if skip:
            cursor.skip(skip)
        if limit:
            cursor.limit(limit)
        return cursor

    def find_one(
        self,
        filter: dict | None = None,
        projection: dict | list[str] | None = None,
        hint=None,
    ):
        results = self.find(filter=filter, projection=projection, limit=1, hint=hint).to_list()
        return results[0] if results else None

    def count_documents(self, filter: dict | None = None, hint=None) -> int:
        return len(self.find(filter=filter, hint=hint).to_list())

    def update_one(self, filter: dict, update: dict, upsert: bool = False, hint=None) -> UpdateResult:
        _ = hint
        return self._update(filter, update, multi=False, upsert=upsert)

    def update_many(self, filter: dict, update: dict, upsert: bool = False, hint=None) -> UpdateResult:
        _ = hint
        return self._update(filter, update, multi=True, upsert=upsert)

    def replace_one(self, filter: dict, replacement: dict, upsert: bool = False, hint=None) -> UpdateResult:
        _ = hint
        return self._update(filter, replacement, multi=False, replace=True, upsert=upsert)

    def delete_one(self, filter: dict, **kwargs) -> DeleteResult:
        return self._delete(filter, multi=False)

    def delete_many(self, filter: dict, **kwargs) -> DeleteResult:
        return self._delete(filter, multi=True)

    def find_one_and_delete(
        self,
        filter: dict,
        projection: dict | list[str] | None = None,
        sort: list[tuple[str, int]] | dict | None = None,
        hint=None,
    ):
        _ = hint
        docs = self._latest_documents()
        matched = [doc for doc in docs if _match_filter(doc, filter)]
        if not matched:
            return None
        if sort:
            matched = self._apply_sort(matched, sort)
        target = matched[0]
        self._append_versions([{"_id": target["_id"]}], deleted=True)
        return _apply_projection([target], projection)[0]

    def find_one_and_update(
        self,
        filter: dict,
        update: dict,
        projection: dict | list[str] | None = None,
        sort: list[tuple[str, int]] | dict | None = None,
        return_document: str = "after",
        upsert: bool = False,
        hint=None,
    ):
        _ = hint
        docs = self._latest_documents()
        matched = [doc for doc in docs if _match_filter(doc, filter)]
        if sort:
            matched = self._apply_sort(matched, sort)
        if not matched:
            if not upsert:
                return None
            seed = _build_upsert_seed(filter)
            if "_id" not in seed:
                seed["_id"] = self._next_id()
            new_doc = _apply_update_ops(seed, update, upsert=True)
            fields = self._schema_or_error()
            field_names = {name for name, _ in fields}
            field_names.update(_META_FIELDS)
            extra = set(new_doc.keys()) - field_names
            if extra:
                raise ValueError(f"unknown fields for collection: {sorted(extra)}")
            self._append_versions([new_doc], deleted=False)
            if return_document == "before":
                return None
            return _apply_projection([new_doc], projection)[0]
        target = matched[0]
        updated = _apply_update_ops(target, update, upsert=False)
        fields = self._schema_or_error()
        field_names = {name for name, _ in fields}
        field_names.update(_META_FIELDS)
        extra = set(updated.keys()) - field_names
        if extra:
            raise ValueError(f"unknown fields for collection: {sorted(extra)}")
        self._append_versions([updated], deleted=False)
        if return_document == "before":
            return _apply_projection([target], projection)[0]
        return _apply_projection([updated], projection)[0]

    def aggregate(self, pipeline: list[dict], hint=None) -> list[dict]:
        _ = hint
        if not pipeline:
            return []
        docs = self._latest_documents()
        for stage in pipeline:
            if "$match" in stage:
                docs = [doc for doc in docs if _match_filter(doc, stage["$match"])]
            elif "$group" in stage:
                docs = _group_docs(docs, stage["$group"])
            elif "$count" in stage:
                field_name = stage["$count"]
                docs = [{field_name: len(docs)}]
            elif "$sortByCount" in stage:
                docs = _sort_by_count(docs, stage["$sortByCount"])
            elif "$addFields" in stage or "$set" in stage:
                spec = stage.get("$addFields") or stage.get("$set")
                docs = _apply_add_fields(docs, spec)
            elif "$project" in stage:
                docs = _apply_project_stage(docs, stage["$project"])
            elif "$sort" in stage:
                docs = self._apply_sort(docs, stage["$sort"])
            elif "$limit" in stage:
                docs = docs[:int(stage["$limit"])]
            elif "$skip" in stage:
                docs = docs[int(stage["$skip"]):]
            elif "$unwind" in stage:
                docs = _apply_unwind(docs, stage["$unwind"])
            elif "$lookup" in stage:
                docs = _apply_lookup(self._owner, self._db, docs, stage["$lookup"])
            elif "$facet" in stage:
                docs = _apply_facet(self._owner, self._db, docs, stage["$facet"])
            else:
                raise NotImplementedError("unsupported pipeline stage")
        return docs

    def create_index(self, keys, **kwargs):
        _ = keys
        _ = kwargs
        return "noop"

    def drop_index(self, index_or_name, **kwargs):
        _ = index_or_name
        _ = kwargs
        return None

    def _update(self, filter: dict, update: dict, multi: bool, replace: bool = False,
                upsert: bool = False) -> UpdateResult:
        docs = self._latest_documents()
        matched = [doc for doc in docs if _match_filter(doc, filter)]
        if not matched and not upsert:
            return UpdateResult(0, 0)
        if not multi:
            matched = matched[:1]
        fields = self._schema_or_error()
        field_names = {name for name, _ in fields}
        field_names.update(_META_FIELDS)
        updated_docs = []
        if not matched and upsert:
            seed = _build_upsert_seed(filter)
            if "_id" not in seed:
                seed["_id"] = self._next_id()
            if replace:
                new_doc = {"_id": seed["_id"]}
                new_doc.update(update)
            else:
                new_doc = _apply_update_ops(seed, update, upsert=True)
            extra = set(new_doc.keys()) - field_names
            if extra:
                raise ValueError(f"unknown fields for collection: {sorted(extra)}")
            updated_docs.append(new_doc)
        else:
            for doc in matched:
                if replace:
                    new_doc = {"_id": doc["_id"]}
                    new_doc.update(update)
                else:
                    new_doc = _apply_update_ops(doc, update, upsert=False)
                extra = set(new_doc.keys()) - field_names
                if extra:
                    raise ValueError(f"unknown fields for collection: {sorted(extra)}")
                updated_docs.append(new_doc)
        self._append_versions(updated_docs, deleted=False)
        return UpdateResult(len(matched), len(updated_docs))

    def _delete(self, filter: dict, multi: bool) -> DeleteResult:
        docs = self._latest_documents()
        matched = [doc for doc in docs if _match_filter(doc, filter)]
        if not matched:
            return DeleteResult(0)
        if not multi:
            matched = matched[:1]
        tombstones = [{"_id": doc["_id"]} for doc in matched]
        self._append_versions(tombstones, deleted=True)
        return DeleteResult(len(matched))

    def _append_versions(self, docs: list[dict], deleted: bool) -> None:
        fields = self._schema_or_error()
        columns = {name: [] for name, _ in fields}
        json_fields = self._json_fields.setdefault((self._db, self._name), set())
        for doc in docs:
            version = self._owner._next_version()
            row = dict(doc)
            if "_id" not in row:
                raise ValueError("_id required for updates")
            row["_deleted"] = deleted
            row["_version"] = version
            for field_name, _ in fields:
                columns[field_name].append(_encode_value(row.get(field_name), field_name, json_fields))
        self._client.append_batch_fanout(
            self._db,
            self._name,
            fields,
            columns,
        )

    def _latest_documents(self) -> list[dict]:
        key = (self._db, self._name)
        if key not in self._schemas:
            return []
        json_fields = self._json_fields.get(key, set())
        fields = self._schemas[key]
        rows, _ = self._client.scan_routed(
            self._db,
            self._name,
            fields,
            columns=[name for name, _ in fields],
        )
        latest: dict[int, dict] = {}
        for row in rows:
            doc_id = row.get("_id")
            if doc_id is None:
                continue
            version = row.get("_version", 0)
            current = latest.get(doc_id)
            if current is None or version > current.get("_version", 0):
                latest[doc_id] = _decode_row(row, json_fields)
        return [doc for doc in latest.values() if not doc.get("_deleted")]

    def _apply_sort(self, docs: list[dict], sort):
        if not sort:
            return docs
        if isinstance(sort, dict):
            items = list(sort.items())
        else:
            items = list(sort)
        def sort_key(doc):
            return tuple(_first_value(_get_values(doc, name)) for name, _ in items)
        reverse = any(direction < 0 for _, direction in items)
        return sorted(docs, key=sort_key, reverse=reverse)

    def _schema_or_error(self) -> list[tuple[str, str]]:
        key = (self._db, self._name)
        if key not in self._schemas:
            raise RuntimeError("collection schema not initialized")
        return self._schemas[key]

    def _ensure_schema(self, docs: list[dict]) -> list[tuple[str, str]]:
        key = (self._db, self._name)
        if key in self._schemas:
            fields = self._schemas[key]
            field_names = {name for name, _ in fields}
            for doc in docs:
                extra = set(doc.keys()) - field_names
                if extra:
                    raise ValueError(f"unknown fields for collection: {sorted(extra)}")
            return fields

        names = sorted({key for doc in docs for key in doc.keys() if key not in _META_FIELDS})
        fields = [("_id", "int64"), ("_deleted", "bool"), ("_version", "int64")]
        json_fields = self._json_fields.setdefault(key, set())
        for name in names:
            field_type, is_json = _infer_type(docs, name)
            fields.append((name, field_type))
            if is_json:
                json_fields.add(name)
        self._client.create_dataset_all(self._db, self._name, fields)
        self._schemas[key] = fields
        return fields

    def _next_id(self) -> int:
        key = (self._db, self._name)
        next_id = self._id_counters.get(key, 1)
        self._id_counters[key] = next_id + 1
        return next_id


def _infer_type(docs: list[dict], name: str) -> tuple[str, bool]:
    values = [doc.get(name) for doc in docs]
    non_null = [val for val in values if val is not None]
    if not non_null:
        return "float64", False
    if all(isinstance(val, str) for val in non_null):
        return "string", False
    if all(isinstance(val, (bytes, bytearray)) for val in non_null):
        return "bytes", False
    if all(isinstance(val, (list, dict)) for val in non_null):
        return "string", True
    if all(isinstance(val, bool) for val in non_null):
        return "bool", False
    if all(isinstance(val, int) and not isinstance(val, bool) for val in non_null):
        return "int64", False
    if all(isinstance(val, (int, float)) and not isinstance(val, bool) for val in non_null):
        return "float64", False
    raise ValueError(f"unsupported value type for field '{name}'")


def _apply_projection(docs: list[dict], projection: dict | list[str] | None) -> list[dict]:
    if projection is None:
        return [_strip_meta(doc) for doc in docs]
    if isinstance(projection, list):
        projection = {name: 1 for name in projection}
    include = {k for k, v in projection.items() if v}
    exclude = {k for k, v in projection.items() if not v}
    result = []
    for doc in docs:
        output = {}
        if include:
            for name in include:
                spec = projection.get(name)
                if isinstance(spec, dict):
                    output[name] = _compute_projection_value(doc, name, spec)
                    continue
                if isinstance(spec, str) and spec.startswith("$"):
                    output[name] = _first_value(_get_values(doc, spec[1:]))
                    continue
                if name in doc:
                    output[name] = doc[name]
                    continue
                if "." in name:
                    output[name] = _first_value(_get_values(doc, name))
                    continue
            if "_id" in doc and "_id" not in exclude and "_id" not in output:
                output["_id"] = doc["_id"]
        else:
            for name, value in doc.items():
                if name in exclude:
                    continue
                output[name] = value
            for name, spec in projection.items():
                if name in output:
                    continue
                if isinstance(spec, dict):
                    output[name] = _compute_projection_value(doc, name, spec)
                elif isinstance(spec, str) and spec.startswith("$"):
                    output[name] = _first_value(_get_values(doc, spec[1:]))
                elif "." in name:
                    output[name] = _first_value(_get_values(doc, name))
        result.append(_strip_meta(output, keep_id=True))
    return result


def _apply_project_stage(docs: list[dict], projection: dict) -> list[dict]:
    if projection is None:
        return [dict(doc) for doc in docs]
    include_keys = {k for k, v in projection.items() if v in (1, True)}
    exclude_keys = {k for k, v in projection.items() if v in (0, False)}
    has_expr = any(
        isinstance(v, dict) or isinstance(v, str) and v.startswith("$")
        for v in projection.values()
    )
    output = []
    for doc in docs:
        if include_keys and not has_expr:
            out = {k: doc.get(k) for k in include_keys if k in doc}
        elif exclude_keys and not has_expr:
            out = {k: v for k, v in doc.items() if k not in exclude_keys}
        else:
            out = {}
            for key, spec in projection.items():
                if spec in (0, False):
                    continue
                out[key] = _eval_expression(doc, spec)
        output.append(out)
    return output


def _compute_projection_value(doc: dict, field_name: str, spec: dict):
    if "$literal" in spec:
        return spec["$literal"]
    if "$slice" in spec:
        expr = spec["$slice"]
        if not isinstance(expr, list):
            expr = [0, expr]
        if len(expr) == 1:
            start = 0
            length = int(expr[0])
        else:
            start = int(expr[0])
            length = int(expr[1])
        return _apply_slice(doc, spec.get("$field", field_name), start, length)
    if "$field" in spec:
        return _first_value(_get_values(doc, spec["$field"]))
    raise NotImplementedError("unsupported projection expression")


def _apply_slice(doc: dict, field: str | None, start: int, length: int):
    if field is None:
        return []
    values = _get_values(doc, field)
    if not values:
        return []
    candidate = values[0]
    if not isinstance(candidate, list):
        return []
    if length >= 0:
        return candidate[start:start + length]
    return candidate[start:]


def _strip_meta(doc: dict, keep_id: bool = True) -> dict:
    output = dict(doc)
    output.pop("_deleted", None)
    output.pop("_version", None)
    if not keep_id:
        output.pop("_id", None)
    return output


def _match_filter(doc: dict, filter: dict) -> bool:
    if not filter:
        return True
    if "$and" in filter:
        return all(_match_filter(doc, item) for item in filter["$and"])
    if "$or" in filter:
        return any(_match_filter(doc, item) for item in filter["$or"])
    if "$nor" in filter:
        return all(not _match_filter(doc, item) for item in filter["$nor"])
    for key, value in filter.items():
        if key.startswith("$"):
            continue
        if not _match_field(doc, key, value):
            return False
    return True


def _compare(left, op: str, right) -> bool:
    if left is None:
        return False
    if op == "eq":
        return left == right
    if op == "ne":
        return left != right
    if op == "lt":
        return left < right
    if op == "le":
        return left <= right
    if op == "gt":
        return left > right
    if op == "ge":
        return left >= right
    return False


def _apply_update_ops(doc: dict, update: dict, upsert: bool) -> dict:
    if not any(key.startswith("$") for key in update.keys()):
        raise ValueError("update document must contain update operators")
    out = dict(doc)
    for op, values in update.items():
        if op == "$set":
            for key, value in values.items():
                _set_path(out, key, value)
        elif op == "$setOnInsert":
            if upsert:
                for key, value in values.items():
                    _set_path(out, key, value)
        elif op == "$inc":
            for key, value in values.items():
                current = _get_path(out, key)
                if current is None:
                    current = 0
                _set_path(out, key, current + value)
        elif op == "$mul":
            for key, value in values.items():
                current = _get_path(out, key)
                if current is None:
                    current = 0
                _set_path(out, key, current * value)
        elif op == "$min":
            for key, value in values.items():
                current = _get_path(out, key)
                _set_path(out, key, value if current is None or value < current else current)
        elif op == "$max":
            for key, value in values.items():
                current = _get_path(out, key)
                _set_path(out, key, value if current is None or value > current else current)
        elif op == "$unset":
            if isinstance(values, (list, tuple, set)):
                keys = values
            else:
                keys = values.keys()
            for key in keys:
                _del_path(out, key)
        elif op == "$rename":
            for key, new_name in values.items():
                _rename_path(out, key, new_name)
        elif op == "$currentDate":
            for key, spec in values.items():
                if isinstance(spec, dict):
                    dtype = spec.get("$type", "date")
                else:
                    dtype = "date"
                if dtype == "timestamp":
                    value = time.time_ns()
                else:
                    value = time.time()
                _set_path(out, key, value)
        elif op == "$push":
            for key, value in values.items():
                current = _get_path(out, key)
                if current is None:
                    current = []
                if not isinstance(current, list):
                    raise ValueError("$push requires an array field")
                if isinstance(value, dict) and "$each" in value:
                    current.extend(list(value["$each"]))
                else:
                    current.append(value)
                _set_path(out, key, current)
        elif op == "$addToSet":
            for key, value in values.items():
                current = _get_path(out, key)
                if current is None:
                    current = []
                if not isinstance(current, list):
                    raise ValueError("$addToSet requires an array field")
                if isinstance(value, dict) and "$each" in value:
                    items = list(value["$each"])
                else:
                    items = [value]
                for item in items:
                    if item not in current:
                        current.append(item)
                _set_path(out, key, current)
        elif op == "$pull":
            for key, value in values.items():
                current = _get_path(out, key)
                if current is None:
                    continue
                if not isinstance(current, list):
                    raise ValueError("$pull requires an array field")
                current = [item for item in current if not _match_pull(item, value)]
                _set_path(out, key, current)
        else:
            raise NotImplementedError(f"unsupported update op '{op}'")
    return out


def _match_pull(item, spec) -> bool:
    if isinstance(spec, dict):
        if "$in" in spec:
            options = spec["$in"]
            return item in options
        if "$eq" in spec:
            return item == spec["$eq"]
        if "$ne" in spec:
            return item != spec["$ne"]
        if "$regex" in spec:
            pattern = spec["$regex"]
            options = spec.get("$options", "")
            flags = 0
            if "i" in options:
                flags |= re.IGNORECASE
            if "m" in options:
                flags |= re.MULTILINE
            if "s" in options:
                flags |= re.DOTALL
            if isinstance(pattern, str):
                regex = re.compile(pattern, flags=flags)
            else:
                regex = pattern
            return isinstance(item, str) and regex.search(item) is not None
        for op, target in spec.items():
            if op in _OP_MAP:
                return _compare(item, _OP_MAP[op], target)
        return False
    return item == spec


def _build_upsert_seed(filter: dict) -> dict:
    if not filter:
        return {}
    if "$or" in filter or "$nor" in filter:
        return {}
    if "$and" in filter:
        seed = {}
        for part in filter["$and"]:
            seed.update(_build_upsert_seed(part))
        return seed
    seed = {}
    for key, value in filter.items():
        if key.startswith("$"):
            continue
        if isinstance(value, dict):
            if "$eq" in value:
                _set_path(seed, key, value["$eq"])
        else:
            _set_path(seed, key, value)
    return seed


def _get_path(doc: dict, path: str):
    exists, value = _path_lookup(doc, path)
    return value if exists else None


def _path_lookup(doc: dict, path: str) -> tuple[bool, object]:
    parts = path.split(".")
    current: object = doc
    for part in parts:
        if isinstance(current, list) and part.isdigit():
            idx = int(part)
            if idx < 0 or idx >= len(current):
                return False, None
            current = current[idx]
            continue
        if not isinstance(current, dict) or part not in current:
            return False, None
        current = current[part]
    return True, current


def _set_path(doc: dict, path: str, value) -> None:
    parts = path.split(".")
    current = doc
    for idx, part in enumerate(parts):
        is_last = idx == len(parts) - 1
        if isinstance(current, list) and part.isdigit():
            index = int(part)
            while len(current) <= index:
                current.append({})
            if is_last:
                current[index] = value
                return
            if not isinstance(current[index], (dict, list)):
                current[index] = {}
            current = current[index]
            continue
        if not isinstance(current, dict):
            raise ValueError("cannot set nested path on non-document")
        if is_last:
            current[part] = value
            return
        if part not in current or not isinstance(current[part], (dict, list)):
            current[part] = {}
        current = current[part]


def _del_path(doc: dict, path: str) -> None:
    parts = path.split(".")
    current = doc
    for idx, part in enumerate(parts):
        is_last = idx == len(parts) - 1
        if isinstance(current, list) and part.isdigit():
            index = int(part)
            if index < 0 or index >= len(current):
                return
            if is_last:
                current.pop(index)
                return
            current = current[index]
            continue
        if not isinstance(current, dict) or part not in current:
            return
        if is_last:
            current.pop(part, None)
            return
        current = current[part]


def _rename_path(doc: dict, old_path: str, new_path: str) -> None:
    exists, value = _path_lookup(doc, old_path)
    if not exists:
        return
    _del_path(doc, old_path)
    _set_path(doc, new_path, value)


def _compile_predicates(field_names: list[str], filter: dict | None) -> list[tuple[int, int, float]]:
    if not filter:
        return []
    predicates = []
    for key, value in filter.items():
        if key.startswith("$"):
            continue
        if isinstance(value, dict):
            for op, target in value.items():
                suffix = _OP_MAP[op]
                predicates.append((field_names.index(key), _op_to_code(suffix), float(target)))
        else:
            predicates.append((field_names.index(key), _op_to_code("eq"), float(value)))
    return predicates


def _combine_filters(filters: list[dict]) -> dict | None:
    if not filters:
        return None
    if len(filters) == 1:
        return filters[0]
    return {"$and": filters}


def _op_to_code(op: str) -> int:
    return {"eq": 0, "ne": 1, "lt": 2, "le": 3, "gt": 4, "ge": 5}[op]


def _encode_value(value, field_name: str, json_fields: set[str]):
    if value is None:
        return None
    if isinstance(value, (list, dict)):
        json_fields.add(field_name)
        return json.dumps(value, separators=(",", ":"))
    return value


def _decode_row(row: dict, json_fields: set[str]) -> dict:
    decoded = dict(row)
    for field_name in json_fields:
        value = decoded.get(field_name)
        if isinstance(value, str):
            try:
                decoded[field_name] = json.loads(value)
            except json.JSONDecodeError:
                decoded[field_name] = value
    return decoded


def _match_field(doc: dict, key: str, value) -> bool:
    values = _get_values(doc, key)
    flat_values = []
    for val in values:
        if isinstance(val, list):
            flat_values.extend(val)
        else:
            flat_values.append(val)
    if isinstance(value, dict):
        if "$not" in value:
            return not _match_field(doc, key, value["$not"])
        if "$exists" in value:
            exists = value["$exists"]
            present = bool(values)
            has_value = any(val is not None for val in values)
            if exists:
                if not has_value:
                    return False
            else:
                if present:
                    return False
        if "$in" in value:
            options = value["$in"]
            if not any(val in options for val in flat_values):
                return False
        if "$nin" in value:
            options = value["$nin"]
            if any(val in options for val in flat_values):
                return False
        if "$all" in value:
            options = value["$all"]
            if not values or not any(isinstance(val, list) for val in values):
                return False
            if not any(all(opt in val for opt in options) for val in values if isinstance(val, list)):
                return False
        if "$size" in value:
            size = int(value["$size"])
            if not any(isinstance(val, list) and len(val) == size for val in values):
                return False
        if "$regex" in value:
            pattern = value["$regex"]
            flags = 0
            options = value.get("$options", "")
            if "i" in options:
                flags |= re.IGNORECASE
            if "m" in options:
                flags |= re.MULTILINE
            if "s" in options:
                flags |= re.DOTALL
            if isinstance(pattern, str):
                regex = re.compile(pattern, flags=flags)
            else:
                regex = pattern
            if not any(isinstance(val, str) and _regex_match_value(val, regex, pattern)
                       for val in flat_values):
                return False
        for op, target in value.items():
            if op in ("$exists", "$in", "$nin", "$regex", "$options"):
                continue
            if op in ("$all", "$size", "$not"):
                continue
            if op not in _OP_MAP:
                raise NotImplementedError(f"unsupported operator '{op}'")
            if not any(_compare(val, _OP_MAP[op], target) for val in flat_values):
                return False
        return True
    return any(val == value for val in flat_values)


def _regex_match_value(value: str, regex: re.Pattern, pattern) -> bool:
    match = regex.search(value)
    if not match:
        return False
    if isinstance(pattern, str) and pattern.endswith("$"):
        token = match.group(0)
        if token and value.count(token) < 2:
            return False
    return True


def _get_values(doc: dict, path: str) -> list:
    parts = path.split(".")
    return _walk_values(doc, parts)


def _walk_values(current, parts: list[str]) -> list:
    if not parts:
        return [current]
    part = parts[0]
    rest = parts[1:]
    results = []
    if isinstance(current, list):
        if part.isdigit():
            idx = int(part)
            if 0 <= idx < len(current):
                results.extend(_walk_values(current[idx], rest))
        else:
            for item in current:
                results.extend(_walk_values(item, parts))
        return results
    if isinstance(current, dict):
        if part in current:
            return _walk_values(current[part], rest)
    return []


def _first_value(values: list):
    return values[0] if values else None


def _group_docs(docs: list[dict], spec: dict) -> list[dict]:
    group_id = spec.get("_id")
    agg_ops = {k: v for k, v in spec.items() if k != "_id"}
    if not agg_ops:
        return []

    def make_key(doc: dict):
        if group_id in (None, 0):
            return None, None
        if isinstance(group_id, str) and group_id.startswith("$"):
            field_name = group_id[1:]
            return doc.get(field_name), doc.get(field_name)
        if isinstance(group_id, dict):
            key = {}
            for name, ref in group_id.items():
                if not isinstance(ref, str) or not ref.startswith("$"):
                    raise NotImplementedError("group _id dict must reference fields")
                key[name] = doc.get(ref[1:])
            return tuple(key.items()), key
        raise NotImplementedError("unsupported _id in $group")

    groups: dict[object, dict] = {}
    for doc in docs:
        key, id_value = make_key(doc)
        if key not in groups:
            groups[key] = {"_id": id_value}
            for name, spec in agg_ops.items():
                groups[key][name] = {
                    "$sum": 0.0,
                    "$min": None,
                    "$max": None,
                    "$count": 0,
                    "$first": None,
                    "$last": None,
                }
        bucket = groups[key]
        for name, op_spec in agg_ops.items():
            if op_spec == {"$sum": 1}:
                bucket[name]["$count"] += 1
                continue
            if not isinstance(op_spec, dict) or len(op_spec) != 1:
                raise NotImplementedError("unsupported group spec")
            op, field = next(iter(op_spec.items()))
            if not isinstance(field, str) or not field.startswith("$"):
                raise NotImplementedError("group field must be a field reference")
            value = doc.get(field[1:])
            if value is None:
                continue
            if op == "$sum":
                bucket[name]["$sum"] += float(value)
            elif op == "$min":
                bucket[name]["$min"] = value if bucket[name]["$min"] is None else min(bucket[name]["$min"], value)
            elif op == "$max":
                bucket[name]["$max"] = value if bucket[name]["$max"] is None else max(bucket[name]["$max"], value)
            elif op == "$avg":
                bucket[name]["$sum"] += float(value)
                bucket[name]["$count"] += 1
            elif op == "$first":
                if bucket[name]["$first"] is None:
                    bucket[name]["$first"] = value
            elif op == "$last":
                bucket[name]["$last"] = value
            else:
                raise NotImplementedError(f"unsupported group op '{op}'")

    results = []
    for bucket in groups.values():
        row = {"_id": bucket["_id"]}
        for name, op_spec in agg_ops.items():
            if op_spec == {"$sum": 1}:
                row[name] = bucket[name]["$count"]
                continue
            op = next(iter(op_spec.keys()))
            if op == "$sum":
                row[name] = bucket[name]["$sum"]
            elif op == "$min":
                row[name] = bucket[name]["$min"]
            elif op == "$max":
                row[name] = bucket[name]["$max"]
            elif op == "$avg":
                count = bucket[name]["$count"]
                row[name] = (bucket[name]["$sum"] / count) if count else None
            elif op == "$first":
                row[name] = bucket[name]["$first"]
            elif op == "$last":
                row[name] = bucket[name]["$last"]
        results.append(row)
    return results


def _eval_expression(doc: dict, spec):
    if isinstance(spec, str):
        if spec.startswith("$"):
            return _first_value(_get_values(doc, spec[1:]))
        return spec
    if isinstance(spec, dict):
        if "$literal" in spec:
            return spec["$literal"]
        if "$field" in spec:
            return _first_value(_get_values(doc, spec["$field"]))
    return spec


def _apply_add_fields(docs: list[dict], spec: dict) -> list[dict]:
    output = []
    for doc in docs:
        new_doc = dict(doc)
        for key, value in spec.items():
            _set_path(new_doc, key, _eval_expression(doc, value))
        output.append(new_doc)
    return output


def _apply_unwind(docs: list[dict], spec):
    if isinstance(spec, str):
        path = spec
        preserve = False
    else:
        path = spec.get("path")
        preserve = bool(spec.get("preserveNullAndEmptyArrays"))
    if path.startswith("$"):
        path = path[1:]
    output = []
    for doc in docs:
        values = _get_values(doc, path)
        if not values:
            if preserve:
                output.append(dict(doc))
            continue
        for val in values:
            if not isinstance(val, list):
                new_doc = dict(doc)
                _set_path(new_doc, path, val)
                output.append(new_doc)
                continue
            if not val and preserve:
                output.append(dict(doc))
            for item in val:
                new_doc = dict(doc)
                _set_path(new_doc, path, item)
                output.append(new_doc)
    return output


def _apply_lookup(owner: MongoClient, db_name: str, docs: list[dict], spec: dict) -> list[dict]:
    from_name = spec["from"]
    local_field = spec["localField"]
    foreign_field = spec["foreignField"]
    as_field = spec["as"]
    foreign = owner[db_name][from_name]._latest_documents()
    output = []
    for doc in docs:
        local_vals = _get_values(doc, local_field)
        matches = []
        for foreign_doc in foreign:
            foreign_vals = _get_values(foreign_doc, foreign_field)
            if any(val in foreign_vals for val in local_vals):
                matches.append(foreign_doc)
        new_doc = dict(doc)
        new_doc[as_field] = matches
        output.append(new_doc)
    return output


def _apply_facet(owner: MongoClient, db_name: str, docs: list[dict], spec: dict) -> list[dict]:
    output = {}
    for name, pipeline in spec.items():
        branch = docs
        for stage in pipeline:
            if "$match" in stage:
                branch = [doc for doc in branch if _match_filter(doc, stage["$match"])]
            elif "$group" in stage:
                branch = _group_docs(branch, stage["$group"])
            elif "$count" in stage:
                field_name = stage["$count"]
                branch = [{field_name: len(branch)}]
            elif "$sortByCount" in stage:
                branch = _sort_by_count(branch, stage["$sortByCount"])
            elif "$addFields" in stage or "$set" in stage:
                spec_fields = stage.get("$addFields") or stage.get("$set")
                branch = _apply_add_fields(branch, spec_fields)
            elif "$project" in stage:
                branch = _apply_project_stage(branch, stage["$project"])
            elif "$sort" in stage:
                branch = _apply_sort_docs(branch, stage["$sort"])
            elif "$limit" in stage:
                branch = branch[:int(stage["$limit"])]
            elif "$skip" in stage:
                branch = branch[int(stage["$skip"]):]
            elif "$unwind" in stage:
                branch = _apply_unwind(branch, stage["$unwind"])
            elif "$lookup" in stage:
                branch = _apply_lookup(owner, db_name, branch, stage["$lookup"])
            else:
                raise NotImplementedError("unsupported facet stage")
        output[name] = branch
    return [output]


def _apply_sort_docs(docs: list[dict], sort):
    if not sort:
        return docs
    if isinstance(sort, dict):
        items = list(sort.items())
    else:
        items = list(sort)
    def sort_key(doc):
        return tuple(_first_value(_get_values(doc, name)) for name, _ in items)
    reverse = any(direction < 0 for _, direction in items)
    return sorted(docs, key=sort_key, reverse=reverse)


def _sort_by_count(docs: list[dict], expr) -> list[dict]:
    counts: dict[object, int] = {}
    for doc in docs:
        key = _eval_expression(doc, expr)
        counts[key] = counts.get(key, 0) + 1
    buckets = [{"_id": key, "count": count} for key, count in counts.items()]
    return sorted(buckets, key=lambda item: item["count"], reverse=True)
