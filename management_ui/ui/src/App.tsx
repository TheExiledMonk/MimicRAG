import { useEffect, useMemo, useState } from "react";

function makeId() {
  if (typeof crypto !== "undefined" && "randomUUID" in crypto) {
    return crypto.randomUUID();
  }
  return `id_${Math.random().toString(16).slice(2)}${Date.now().toString(16)}`;
}

type DatasetField = {
  name: string;
  type: string;
  nullable: boolean;
  encoding: string;
};

type Predicate = {
  field: string;
  op: string;
  value: string;
};

type ScanResponse = {
  columns: string[];
  rows: Array<Array<string | number | boolean | null>>;
  cursor: string | null;
  has_more: boolean;
};

type HealthResponse = {
  ok: boolean;
  version: string;
};

type DbResponse = {
  databases: string[];
};

type DatasetResponse = {
  datasets: string[];
};

type SchemaResponse = {
  fields: DatasetField[];
};

const TABS = [
  "Schema",
  "Data Preview",
  "Aggregates",
  "Ingest",
  "Memory",
  "Admin",
  "Settings",
] as const;
type QueryStyle = "native" | "mongo" | "sql";

type AggregateResponse = {
  count: number;
  sum: number;
  min: number | null;
  max: number | null;
  has_value: boolean;
};

type MemoryReviewItem = { memory_id: string; subject: string; status: string; namespace: string; sensitivity: string };

type HealthConfig = {
  bind_host?: string;
  bind_port?: number;
  default_database?: string;
  storage_root?: string;
  identity_key_path?: string;
};

type ParsedQuery = {
  columns: string[] | null;
  predicates: Predicate[];
  limit?: number;
  dataset?: string;
  aggregateField?: string;
};

function inferValue(value: string, fieldType: string): number | boolean | string {
  if (fieldType === "bool") {
    return value === "true";
  }
  if (fieldType === "int32" || fieldType === "int64" || fieldType === "dict_int32") {
    return Number.parseInt(value, 10);
  }
  if (fieldType === "float64") {
    return Number.parseFloat(value);
  }
  return value;
}

function normalizePredicateValue(value: unknown): string {
  if (typeof value === "boolean") {
    return value ? "true" : "false";
  }
  if (typeof value === "number") {
    return String(value);
  }
  if (typeof value === "string") {
    return value;
  }
  return JSON.stringify(value);
}

export function parseMongoPredicates(input: string): Predicate[] {
  const obj = JSON.parse(input);
  const predicates: Predicate[] = [];

  function walk(node: Record<string, unknown>) {
    if (node.$and) {
      if (!Array.isArray(node.$and)) {
        throw new Error("$and must be an array");
      }
      node.$and.forEach((item) => {
        if (!item || typeof item !== "object" || Array.isArray(item)) {
          throw new Error("each $and entry must be an object");
        }
        walk(item as Record<string, unknown>);
      });
    }
    Object.entries(node).forEach(([field, value]) => {
      if (field === "$and") {
        return;
      }
      if (value && typeof value === "object" && !Array.isArray(value)) {
        Object.entries(value).forEach(([opKey, opValue]) => {
          const opMap: Record<string, string> = {
            $eq: "eq",
            $ne: "ne",
            $lt: "lt",
            $lte: "le",
            $gt: "gt",
            $gte: "ge",
          };
          const op = opMap[opKey];
          if (!op) {
            throw new Error(`unsupported operator ${opKey}`);
          }
          predicates.push({
            field,
            op,
            value: normalizePredicateValue(opValue),
          });
        });
      } else {
        predicates.push({
          field,
          op: "eq",
          value: normalizePredicateValue(value),
        });
      }
    });
  }

  if (!obj || typeof obj !== "object" || Array.isArray(obj)) {
    throw new Error("Mongo query must be an object");
  }
  walk(obj as Record<string, unknown>);
  return predicates;
}

export function parseSqlQuery(input: string): ParsedQuery {
  const match = input.match(
    /^\s*select\s+(.+?)\s+from\s+([a-zA-Z0-9_]+)(?:\s+where\s+(.+?))?(?:\s+limit\s+(\d+))?\s*;?\s*$/i
  );
  if (!match) {
    throw new Error("invalid SQL query");
  }

  const selectPart = match[1].trim();
  const dataset = match[2].trim();
  const wherePart = match[3]?.trim();
  const limitPart = match[4]?.trim();

  let columns: string[] | null = null;
  let aggregateField: string | undefined;

  const aggMatch = selectPart.match(
    /^(count|sum|min|max)\s*\(\s*([a-zA-Z0-9_*]+)\s*\)$/i
  );
  if (aggMatch) {
    const field = aggMatch[2];
    if (field === "*") {
      throw new Error("aggregate field required");
    }
    aggregateField = field;
  } else if (selectPart !== "*") {
    columns = selectPart.split(",").map((col) => col.trim()).filter(Boolean);
  }

  const predicates: Predicate[] = [];
  if (wherePart) {
    const clauses = wherePart.split(/\s+and\s+/i);
    clauses.forEach((clause) => {
      const clauseMatch = clause.match(
        /^\s*([a-zA-Z0-9_]+)\s*(=|!=|<=|>=|<|>)\s*(.+?)\s*$/
      );
      if (!clauseMatch) {
        throw new Error(`invalid predicate: ${clause}`);
      }
      const [, field, op, rawValue] = clauseMatch;
      const opMap: Record<string, string> = {
        "=": "eq",
        "!=": "ne",
        "<": "lt",
        "<=": "le",
        ">": "gt",
        ">=": "ge",
      };
      let value = rawValue.trim();
      if (
        (value.startsWith("'") && value.endsWith("'")) ||
        (value.startsWith("\"") && value.endsWith("\""))
      ) {
        value = value.slice(1, -1);
      }
      predicates.push({ field, op: opMap[op], value });
    });
  }

  const limit = limitPart ? Number.parseInt(limitPart, 10) : undefined;
  return { columns, predicates, limit, dataset, aggregateField };
}

export default function App() {
  const [health, setHealth] = useState<HealthResponse | null>(null);
  const [healthConfig, setHealthConfig] = useState<HealthConfig | null>(null);
  const [connectStatus, setConnectStatus] = useState<string>("disconnected");
  const [connectError, setConnectError] = useState<string | null>(null);
  const [host, setHost] = useState("127.0.0.1");
  const [port, setPort] = useState("9000");
  const [database, setDatabase] = useState("default");
  const [identityKeyPath, setIdentityKeyPath] = useState("");
  const [selectedDatabase, setSelectedDatabase] = useState("");
  const [showConnect, setShowConnect] = useState(false);

  const [databases, setDatabases] = useState<string[]>([]);
  const [datasetsByDb, setDatasetsByDb] = useState<Record<string, string[]>>({});
  const [expandedDbs, setExpandedDbs] = useState<Record<string, boolean>>({});
  const [selectedDataset, setSelectedDataset] = useState<string | null>(null);
  const [schema, setSchema] = useState<DatasetField[]>([]);
  const [schemaCache, setSchemaCache] = useState<Record<string, DatasetField[]>>({});
  const [activeTab, setActiveTab] = useState<(typeof TABS)[number]>("Schema");

  const [selectedColumns, setSelectedColumns] = useState<string[]>([]);
  const [predicates, setPredicates] = useState<Predicate[]>([]);
  const [queryStyle, setQueryStyle] = useState<QueryStyle>("native");
  const [queryInput, setQueryInput] = useState("");
  const [queryError, setQueryError] = useState<string | null>(null);
  const [scanRows, setScanRows] = useState<ScanResponse | null>(null);
  const [scanCursor, setScanCursor] = useState<string | null>(null);
  const [scanError, setScanError] = useState<string | null>(null);
  const [scanBusy, setScanBusy] = useState(false);
  const [limit, setLimit] = useState("100");
  const [maxLimit, setMaxLimit] = useState("10000");
  const [ackFullScan, setAckFullScan] = useState(false);
  const [showDictIds, setShowDictIds] = useState(false);
  const [csvIncludeHeaders, setCsvIncludeHeaders] = useState(true);
  const [csvNullMode, setCsvNullMode] = useState<"NULL" | "empty">("NULL");
  const [csvWarning, setCsvWarning] = useState<string | null>(null);
  const [contextMenu, setContextMenu] = useState<{
    x: number;
    y: number;
    type: "db" | "dataset";
    db: string;
    dataset?: string;
  } | null>(null);
  const [confirmDelete, setConfirmDelete] = useState<{
    type: "db" | "dataset";
    db: string;
    dataset?: string;
  } | null>(null);
  const [confirmInput, setConfirmInput] = useState("");
  const [confirmChecked, setConfirmChecked] = useState(false);

  const [aggregateField, setAggregateField] = useState<string>("");
  const [aggregateResult, setAggregateResult] = useState<AggregateResponse | null>(null);
  const [aggregateError, setAggregateError] = useState<string | null>(null);
  const [memoryTenant, setMemoryTenant] = useState("default");
  const [memoryStatus, setMemoryStatus] = useState("");
  const [memoryItems, setMemoryItems] = useState<MemoryReviewItem[]>([]);
  const [memoryError, setMemoryError] = useState<string | null>(null);
  const [aggregateBusy, setAggregateBusy] = useState(false);

  const [ingestValues, setIngestValues] = useState<Record<string, string>>({});
  const [ingestNulls, setIngestNulls] = useState<Record<string, boolean>>({});
  const [ingestStatus, setIngestStatus] = useState<string | null>(null);
  const [ingestError, setIngestError] = useState<string | null>(null);

  const [newDatabase, setNewDatabase] = useState("");
  const [newDataset, setNewDataset] = useState("");
  const [fieldDraft, setFieldDraft] = useState<
    Array<{ id: string; name: string; type: string }>
  >([{ id: makeId(), name: "", type: "int64" }]);
  const [adminStatus, setAdminStatus] = useState<string | null>(null);
  const [adminError, setAdminError] = useState<string | null>(null);

  useEffect(() => {
    fetch("/api/health")
      .then((res) => res.json())
      .then((data) => {
        setHealth(data);
        setHealthConfig(data.config ?? null);
      })
      .catch(() => setHealth(null));
  }, []);

  useEffect(() => {
    const stored = window.localStorage.getItem("mimicdb.ui.identity_key_path");
    if (stored) {
      setIdentityKeyPath(stored);
    }
  }, []);

  useEffect(() => {
    if (!healthConfig?.identity_key_path || identityKeyPath) {
      return;
    }
    setIdentityKeyPath(healthConfig.identity_key_path);
  }, [healthConfig, identityKeyPath]);

  useEffect(() => {
    function handleClick() {
      setContextMenu(null);
    }
    if (contextMenu) {
      window.addEventListener("click", handleClick);
    }
    return () => {
      window.removeEventListener("click", handleClick);
    };
  }, [contextMenu]);

  const fieldMap = useMemo(() => {
    const map = new Map<string, DatasetField>();
    schema.forEach((field) => map.set(field.name, field));
    return map;
  }, [schema]);

  function connect() {
    setConnectError(null);
    setConnectStatus("connecting");
    fetch("/api/connect", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        host,
        port: Number(port),
        database,
        identity_key_path: identityKeyPath || undefined,
      }),
    })
      .then((res) => {
        if (!res.ok) {
          return res.json().then((data) => {
            throw new Error(data.detail || "connect failed");
          });
        }
        return res.json();
      })
      .then(() => {
        setConnectStatus("connected");
        window.localStorage.setItem(
          "mimicdb.ui.identity_key_path",
          identityKeyPath,
        );
        refreshDatabases();
        setShowConnect(false);
      })
      .catch((err) => {
        setConnectStatus("error");
        setConnectError(err.message);
      });
  }

  const identityReady = identityKeyPath.trim().length > 0;

  function refreshDatabases() {
    fetch("/api/databases")
      .then((res) => res.json())
      .then((data: DbResponse) => {
        setDatabases(data.databases);
      })
      .catch(() => setDatabases([]));
  }

  function refreshDatasets(dbName: string) {
    fetch(`/api/datasets?database=${encodeURIComponent(dbName)}`)
      .then((res) => res.json())
      .then((data: DatasetResponse) => {
        setDatasetsByDb((prev) => ({ ...prev, [dbName]: data.datasets }));
      })
      .catch(() => setDatasetsByDb((prev) => ({ ...prev, [dbName]: [] })));
  }

  function loadSchema(dbName: string, datasetName: string) {
    const key = `${dbName}.${datasetName}`;
    const cached = schemaCache[key];
    if (cached) {
      setSchema(cached);
      setSelectedColumns(cached.map((field) => field.name));
      setAggregateField(cached[0]?.name ?? "");
      const valueMap: Record<string, string> = {};
      const nullMap: Record<string, boolean> = {};
      cached.forEach((field) => {
        valueMap[field.name] = "";
        nullMap[field.name] = false;
      });
      setIngestValues(valueMap);
      setIngestNulls(nullMap);
      return;
    }
    fetch(
      `/api/schema?database=${encodeURIComponent(dbName)}&dataset=${encodeURIComponent(datasetName)}`
    )
      .then((res) => res.json())
      .then((data: SchemaResponse) => {
        setSchema(data.fields);
        setSchemaCache((prev) => ({ ...prev, [key]: data.fields }));
        setSelectedColumns(data.fields.map((field) => field.name));
        setAggregateField(data.fields[0]?.name ?? "");
        const valueMap: Record<string, string> = {};
        const nullMap: Record<string, boolean> = {};
        data.fields.forEach((field) => {
          valueMap[field.name] = "";
          nullMap[field.name] = false;
        });
        setIngestValues(valueMap);
        setIngestNulls(nullMap);
      })
      .catch(() => setSchema([]));
  }

  function handleDatabaseSelect(value: string) {
    setSelectedDatabase(value);
    setSelectedDataset(null);
    setSchema([]);
    setSelectedColumns([]);
    refreshDatasets(value);
  }

  function toggleDatabase(value: string) {
    setExpandedDbs((prev) => ({ ...prev, [value]: !prev[value] }));
    if (!datasetsByDb[value]) {
      refreshDatasets(value);
    }
  }

  function handleDatasetSelect(dbName: string, datasetName: string) {
    setSelectedDatabase(dbName);
    setSelectedDataset(datasetName);
    loadSchema(dbName, datasetName);
    setActiveTab("Data Preview");
  }

  function addPredicate() {
    if (!schema.length) {
      return;
    }
    const defaultField = schema[0].name;
    setPredicates((prev) => [
      ...prev,
      { field: defaultField, op: "eq", value: "" },
    ]);
  }

  function updatePredicate(index: number, key: keyof Predicate, value: string) {
    setPredicates((prev) =>
      prev.map((item, idx) => (idx === index ? { ...item, [key]: value } : item))
    );
  }

  function removePredicate(index: number) {
    setPredicates((prev) => prev.filter((_, idx) => idx !== index));
  }

  function runScan(cursorOverride?: string | null) {
    if (!selectedDataset) {
      return;
    }
    if (!ackFullScan && predicates.length === 0) {
      setScanError("add a predicate or confirm full scan");
      return;
    }
    setScanBusy(true);
    setScanError(null);
    const maxLimitValue = Number(maxLimit) || 10000;
    const requestedLimit = Number(limit) || 100;
    const effectiveLimit = Math.min(requestedLimit, maxLimitValue);
    const payload = {
      database: selectedDatabase,
      dataset: selectedDataset,
      columns: selectedColumns.length ? selectedColumns : null,
      predicates: predicates
        .filter((pred) => pred.field && pred.op)
        .map((pred) => {
          const field = fieldMap.get(pred.field);
          const type = field ? field.type : "int64";
          return {
            field: pred.field,
            op: pred.op,
            value: inferValue(pred.value, type),
          };
        }),
      limit: effectiveLimit,
      cursor: cursorOverride ?? null,
    };

    fetch("/api/scan", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    })
      .then((res) => {
        if (!res.ok) {
          return res.json().then((data) => {
            throw new Error(data.detail || "scan failed");
          });
        }
        return res.json();
      })
      .then((data: ScanResponse) => {
        setScanRows(data);
        setScanCursor(data.cursor);
        if (data.has_more) {
          setCsvWarning("Export includes only the current page (results truncated).");
        } else {
          setCsvWarning(null);
        }
      })
      .catch((err) => setScanError(err.message))
      .finally(() => setScanBusy(false));
  }

  function exportCsv() {
    if (!scanRows) {
      return;
    }
    const lines: string[] = [];
    if (csvIncludeHeaders) {
      lines.push(
        scanRows.columns
          .map((name) => `"${name.replace(/\"/g, '""')}"`)
          .join(",")
      );
    }
    scanRows.rows.forEach((row) => {
      const line = row
        .map((cell, index) => {
          if (cell === null) {
            return csvNullMode === "NULL" ? "NULL" : "";
          }
          const column = scanRows.columns[index];
          const fieldType = fieldMap.get(column)?.type ?? "string";
          const { text } = formatCell(cell, fieldType);
          const escaped = text.replace(/\"/g, '""');
          return `"${escaped}"`;
        })
        .join(",");
      lines.push(line);
    });
    const blob = new Blob([lines.join("\n")], { type: "text/csv;charset=utf-8;" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `${selectedDatabase}_${selectedDataset ?? "dataset"}_page.csv`;
    link.click();
    URL.revokeObjectURL(url);
  }

  function formatCell(
    value: string | number | boolean | null,
    fieldType: string
  ): { text: string; title?: string; className?: string } {
    if (value === null) {
      return { text: "NULL", className: "null" };
    }
    if (fieldType === "dict_int32") {
      if (showDictIds) {
        return { text: `id:${String(value)}`, className: "cell-muted" };
      }
      return { text: String(value) };
    }
    if (fieldType === "string") {
      const text = String(value);
      if (text.length > 128) {
        return {
          text: `${text.slice(0, 128)}…`,
          title: text,
        };
      }
      return { text };
    }
    if (fieldType === "bytes") {
      const asString = typeof value === "string" ? value : String(value);
      const encoder = new TextEncoder();
      const bytes = encoder.encode(asString);
      const hex = Array.from(bytes.slice(0, 24))
        .map((b) => b.toString(16).padStart(2, "0"))
        .join("");
      const preview = bytes.length > 24 ? `${hex}…` : hex;
      return {
        text: `0x${preview} (${bytes.length}b)`,
        title: asString,
        className: "cell-bytes",
      };
    }
    return { text: String(value) };
  }

  function resetScan() {
    setScanCursor(null);
    setScanRows(null);
  }

  useEffect(() => {
    if (connectStatus === "connected") {
      refreshDatabases();
    }
  }, [connectStatus]);

  function runAggregate() {
    if (!selectedDataset || !aggregateField) {
      return;
    }
    setAggregateBusy(true);
    setAggregateError(null);
    const payload = {
      database: selectedDatabase,
      dataset: selectedDataset,
      field: aggregateField,
      predicates: predicates
        .filter((pred) => pred.field && pred.op)
        .map((pred) => {
          const field = fieldMap.get(pred.field);
          const type = field ? field.type : "int64";
          return {
            field: pred.field,
            op: pred.op,
            value: inferValue(pred.value, type),
          };
        }),
    };

    fetch("/api/aggregate", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    })
      .then((res) => {
        if (!res.ok) {
          return res.json().then((data) => {
            throw new Error(data.detail || "aggregate failed");
          });
        }
        return res.json();
      })
      .then((data: AggregateResponse) => setAggregateResult(data))
      .catch((err) => setAggregateError(err.message))
      .finally(() => setAggregateBusy(false));
  }

  function handleIngestChange(name: string, value: string) {
    setIngestValues((prev) => ({ ...prev, [name]: value }));
  }

  function handleIngestNull(name: string, checked: boolean) {
    setIngestNulls((prev) => ({ ...prev, [name]: checked }));
  }

  function runIngest() {
    if (!selectedDataset || schema.length === 0) {
      return;
    }
    setIngestStatus(null);
    setIngestError(null);
    const columns: Record<string, Array<string | number | boolean | null>> = {};
    schema.forEach((field) => {
      if (ingestNulls[field.name]) {
        columns[field.name] = [null];
        return;
      }
      const raw = ingestValues[field.name] ?? "";
      if (raw === "" && field.type !== "string" && field.type !== "bytes") {
        columns[field.name] = [null];
        return;
      }
      const value = inferValue(raw, field.type);
      columns[field.name] = [value as string | number | boolean];
    });

    fetch("/api/append", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
      database: selectedDatabase,
        dataset: selectedDataset,
        columns,
      }),
    })
      .then((res) => {
        if (!res.ok) {
          return res.json().then((data) => {
            throw new Error(data.detail || "append failed");
          });
        }
        return res.json();
      })
      .then(() => setIngestStatus("append ok"))
      .catch((err) => setIngestError(err.message));
  }

  function addFieldDraft() {
    setFieldDraft((prev) => [
      ...prev,
      { id: makeId(), name: "", type: "int64" },
    ]);
  }

  function updateFieldDraft(index: number, key: "name" | "type", value: string) {
    setFieldDraft((prev) =>
      prev.map((item, idx) => (idx === index ? { ...item, [key]: value } : item))
    );
  }

  function removeFieldDraft(index: number) {
    setFieldDraft((prev) => prev.filter((_, idx) => idx !== index));
  }

  function createDatabase() {
    if (!newDatabase) {
      return;
    }
    setAdminError(null);
    setAdminStatus(null);
    fetch("/api/create_database", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name: newDatabase }),
    })
      .then((res) => {
        if (!res.ok) {
          return res.json().then((data) => {
            throw new Error(data.detail || "create database failed");
          });
        }
        return res.json();
      })
      .then(() => {
        setAdminStatus(`database created: ${newDatabase}`);
        setNewDatabase("");
        refreshDatabases();
      })
      .catch((err) => setAdminError(err.message));
  }

  function dropDatabase() {
    if (!selectedDatabase) {
      setAdminError("select a database first");
      return;
    }
    setAdminError(null);
    setAdminStatus(null);
    fetch("/api/drop_database", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name: selectedDatabase }),
    })
      .then((res) => {
        if (!res.ok) {
          return res.json().then((data) => {
            throw new Error(data.detail || "drop database failed");
          });
        }
        return res.json();
      })
      .then(() => {
        setAdminStatus(`database dropped: ${selectedDatabase}`);
        refreshDatabases();
        setDatasetsByDb((prev) => ({ ...prev, [selectedDatabase]: [] }));
        setSelectedDatabase("default");
        setSelectedDataset(null);
        setSchema([]);
      })
      .catch((err) => setAdminError(err.message));
  }

  function createDataset() {
    if (!newDataset) {
      return;
    }
    if (!selectedDatabase) {
      setAdminError("select a database first");
      return;
    }
    const fields = fieldDraft
      .map((field) => ({ name: field.name.trim(), type: field.type }))
      .filter((field) => field.name);
    if (fields.length === 0) {
      setAdminError("dataset requires at least one field");
      return;
    }
    setAdminError(null);
    setAdminStatus(null);
    fetch("/api/create_dataset", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        database: selectedDatabase,
        name: newDataset,
        fields,
      }),
    })
      .then((res) => {
        if (!res.ok) {
          return res.json().then((data) => {
            throw new Error(data.detail || "create dataset failed");
          });
        }
        return res.json();
      })
      .then(() => {
        setAdminStatus(`dataset created: ${newDataset}`);
        setNewDataset("");
        setFieldDraft([{ id: makeId(), name: "", type: "int64" }]);
        refreshDatasets(selectedDatabase);
      })
      .catch((err) => setAdminError(err.message));
  }

  function dropDataset() {
    if (!selectedDataset) {
      return;
    }
    if (!selectedDatabase) {
      setAdminError("select a database first");
      return;
    }
    setAdminError(null);
    setAdminStatus(null);
    fetch("/api/drop_dataset", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        database: selectedDatabase,
        name: selectedDataset,
      }),
    })
      .then((res) => {
        if (!res.ok) {
          return res.json().then((data) => {
            throw new Error(data.detail || "drop dataset failed");
          });
        }
        return res.json();
      })
      .then(() => {
        setAdminStatus(`dataset dropped: ${selectedDataset}`);
        setSelectedDataset(null);
        setSchema([]);
        refreshDatasets(selectedDatabase);
      })
      .catch((err) => setAdminError(err.message));
  }

  function disconnect() {
    setConnectStatus("disconnected");
    setConnectError(null);
    setDatabases([]);
    setDatasetsByDb({});
    setSelectedDataset(null);
    setSelectedDatabase("");
    setSchema([]);
    setScanRows(null);
    setScanCursor(null);
    setAggregateResult(null);
    setAdminStatus(null);
    setAdminError(null);
  }

  function openDbContextMenu(event: { preventDefault: () => void; clientX: number; clientY: number }, dbName: string) {
    event.preventDefault();
    setSelectedDatabase(dbName);
    setContextMenu({ x: event.clientX, y: event.clientY, type: "db", db: dbName });
  }

  function openDatasetContextMenu(
    event: { preventDefault: () => void; clientX: number; clientY: number },
    dbName: string,
    datasetName: string
  ) {
    event.preventDefault();
    setSelectedDatabase(dbName);
    setSelectedDataset(datasetName);
    setContextMenu({
      x: event.clientX,
      y: event.clientY,
      type: "dataset",
      db: dbName,
      dataset: datasetName,
    });
  }

  function startCreateDataset(dbName: string) {
    setSelectedDatabase(dbName);
    setActiveTab("Admin");
    setNewDataset("");
    setFieldDraft([{ id: makeId(), name: "", type: "int64" }]);
  }

  function deleteDatabase(dbName: string) {
    setSelectedDatabase(dbName);
    setConfirmDelete({ type: "db", db: dbName });
    setConfirmInput("");
    setConfirmChecked(false);
  }

  function applyQueryFromInput() {
    if (queryStyle === "native") {
      return;
    }
    setQueryError(null);
    try {
      let parsed: ParsedQuery;
      if (queryStyle === "mongo") {
        parsed = {
          columns: null,
          predicates: parseMongoPredicates(queryInput),
        };
      } else {
        parsed = parseSqlQuery(queryInput);
      }
      setPredicates(parsed.predicates);
      if (parsed.columns) {
        setSelectedColumns(parsed.columns);
      } else if (parsed.columns === null) {
        setSelectedColumns([]);
      }
      if (parsed.limit) {
        setLimit(String(parsed.limit));
      }
      if (parsed.aggregateField) {
        setAggregateField(parsed.aggregateField);
      }
      if (parsed.dataset && parsed.dataset !== selectedDataset) {
        setSelectedDataset(parsed.dataset);
        loadSchema(selectedDatabase, parsed.dataset);
      }
      setScanRows(null);
      setScanCursor(null);
      setAggregateResult(null);
    } catch (err) {
      if (err instanceof Error) {
        setQueryError(err.message);
      } else {
        setQueryError("invalid query");
      }
    }
  }

  async function loadMemoryReview() {
    setMemoryError(null);
    try {
      const response = await fetch("/api/memory/review", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ tenant_id: memoryTenant, status: memoryStatus }) });
      const payload = await response.json(); if (!response.ok) throw new Error(payload.detail ?? "memory review failed");
      setMemoryItems(payload.memories ?? []);
    } catch (err) { setMemoryError(err instanceof Error ? err.message : "memory review failed"); }
  }

  async function reviewMemory(memoryId: string, action: "confirm" | "reject") {
    setMemoryError(null);
    try {
      const response = await fetch("/api/memory/action", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ tenant_id: memoryTenant, memory_id: memoryId, action }) });
      const payload = await response.json(); if (!response.ok) throw new Error(payload.detail ?? "memory action failed");
      await loadMemoryReview();
    } catch (err) { setMemoryError(err instanceof Error ? err.message : "memory action failed"); }
  }

  return (
    <div className="app-shell">
      <header className="topbar">
        <div className="brand-row">
          <div className="brand-mark">MimicDB</div>
        </div>
        <div className="top-actions">
          <button type="button" aria-label="Notifications">🔔</button>
          <button type="button" aria-label="Settings" onClick={() => setActiveTab("Settings")}>⚙</button>
          <button type="button" aria-label="Menu">☰</button>
        </div>
      </header>

      <div className="body">
        <aside className="sidebar">
          <div className="status-card">
            <div className="status-line">
              <span className={`status-dot ${connectStatus}`}></span>
              <span>{connectStatus === "connected" ? "Connected" : "Disconnected"}</span>
            </div>
            <div className="status-meta">{host}:{port}</div>
            <div className="status-meta">Database: {database || "default"}</div>
          </div>

          <div className="sidebar-actions">
            <button type="button" className="primary" onClick={() => setActiveTab("Admin")}>+ New Database</button>
            <button type="button" className="secondary" onClick={() => setActiveTab("Admin")}>+ New Dataset</button>
          </div>

          <div className="db-section">
            <div className="section-title">Databases</div>
            <ul className="db-tree">
              {databases.length == 0 ? (
                <li className="empty">No databases</li>
              ) : (
                databases.map((db) => (
                  <li key={db}>
                    <button
                      type="button"
                      className={db === selectedDatabase ? "db-row active" : "db-row"}
                      onClick={() => {
                        handleDatabaseSelect(db);
                        toggleDatabase(db);
                      }}
                      onContextMenu={(event) => openDbContextMenu(event, db)}
                    >
                      <span className="caret">{expandedDbs[db] ? "▾" : "▸"}</span>
                      <span className="db-label">{db}</span>
                    </button>
                    {expandedDbs[db] ? (
                      <ul className="db-children">
                        {(datasetsByDb[db] ?? []).length === 0 ? (
                          <li className="empty">No tables</li>
                        ) : (
                          (datasetsByDb[db] ?? []).map((name) => (
                            <li key={`${db}.${name}`}>
                              <button
                                type="button"
                                className={
                                  db === selectedDatabase && name === selectedDataset
                                    ? "db-row child active"
                                    : "db-row child"
                                }
                                onClick={() => handleDatasetSelect(db, name)}
                                onContextMenu={(event) => openDatasetContextMenu(event, db, name)}
                              >
                                <span className="db-label">{name}</span>
                              </button>
                            </li>
                          ))
                        )}
                      </ul>
                    ) : null}
                  </li>
                ))
              )}
            </ul>
          </div>

          <div className="sidebar-footer">
            {connectStatus === "connected" ? (
              <button type="button" className="ghost" onClick={disconnect}>Disconnect</button>
            ) : (
              <button type="button" className="ghost" onClick={() => setShowConnect(true)}>Connect</button>
            )}
            <button type="button" className="ghost" onClick={() => setActiveTab("Settings")}>Settings</button>
          </div>
        </aside>

        <main className="content">
          <div className="tabs">
            {TABS.filter((tab) => tab !== "Admin").map((tab) => (
              <button
                key={tab}
                onClick={() => setActiveTab(tab)}
                className={tab === activeTab ? "tab active" : "tab"}
              >
                {tab}
              </button>
            ))}
          </div>

          <div className="content-grid">
            <section className="main-panel">
              {activeTab === "Schema" ? (
                <div className="panel">
                  <div className="panel-title">Schema</div>
                  {schema.length === 0 ? (
                    <div className="empty">No schema loaded.</div>
                  ) : (
                    <table className="data-table">
                      <thead>
                        <tr>
                          <th>Name</th>
                          <th>Type</th>
                          <th>Nullable</th>
                          <th>Encoding</th>
                        </tr>
                      </thead>
                      <tbody>
                        {schema.map((field) => (
                          <tr key={field.name}>
                            <td>{field.name}</td>
                            <td>{field.type}</td>
                            <td>{field.nullable ? "yes" : "no"}</td>
                            <td>{field.encoding}</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  )}
                </div>
              ) : activeTab === "Data Preview" ? (
                <div className="panel">
                  <div className="filter-bar">
                    <div className="filter-label">Filter:</div>
                    {predicates.slice(0, 2).map((predicate, index) => (
                      <div key={`${predicate.field}-${index}`} className="filter-chip">
                        <select
                          value={predicate.field}
                          onChange={(event) =>
                            updatePredicate(index, "field", event.target.value)
                          }
                        >
                          {schema.map((field) => (
                            <option key={field.name} value={field.name}>
                              {field.name}
                            </option>
                          ))}
                        </select>
                        <select
                          value={predicate.op}
                          onChange={(event) =>
                            updatePredicate(index, "op", event.target.value)
                          }
                        >
                          <option value="eq">=</option>
                          <option value="ne">!=</option>
                          <option value="lt">&lt;</option>
                          <option value="le">&lt;=</option>
                          <option value="gt">&gt;</option>
                          <option value="ge">&gt;=</option>
                        </select>
                        <input
                          value={predicate.value}
                          onChange={(event) =>
                            updatePredicate(index, "value", event.target.value)
                          }
                          placeholder="value"
                        />
                      </div>
                    ))}
                    <button type="button" className="apply" onClick={() => runScan(scanCursor)}>
                      Apply
                    </button>
                  </div>

                  <div className="table-wrapper">
                    {scanRows ? (
                      <table className="data-table">
                        <thead>
                          <tr>
                            {scanRows.columns.map((name) => (
                              <th key={name}>{name}</th>
                            ))}
                          </tr>
                        </thead>
                        <tbody>
                          {scanRows.rows.map((row, index) => (
                            <tr key={index}>
                              {row.map((cell, cellIndex) => {
                                const column = scanRows.columns[cellIndex];
                                const fieldType = fieldMap.get(column)?.type ?? "string";
                                const { text, title, className } = formatCell(cell, fieldType);
                                return (
                                  <td key={cellIndex} title={title} className={className}>
                                    {text}
                                  </td>
                                );
                              })}
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    ) : (
                      <div className="empty">Run a scan to preview data.</div>
                    )}
                  </div>

                  <div className="table-footer">
                    <div>Rows {scanRows ? `1-${scanRows.rows.length}` : "0"}</div>
                    <div className="footer-actions">
                      <button type="button" onClick={resetScan}>Previous</button>
                      <button type="button" onClick={() => runScan(scanCursor)}>Next</button>
                      <button type="button" className="export" onClick={exportCsv}>Export CSV</button>
                    </div>
                  </div>
                </div>
              ) : activeTab === "Aggregates" ? (
                <div className="panel">
                  <div className="panel-title">Aggregates</div>
                  <div className="filters">
                    <label>
                      Field
                      <select
                        value={aggregateField}
                        onChange={(event) => setAggregateField(event.target.value)}
                      >
                        {schema
                          .filter((field) => !["string", "bytes", "array", "object", "bool"].includes(field.type))
                          .map((field) => (
                            <option key={field.name} value={field.name}>
                              {field.name}
                            </option>
                          ))}
                      </select>
                    </label>
                    <button type="button" className="apply" onClick={runAggregate}>Apply</button>
                  </div>
                  {aggregateResult ? (
                    <div className="aggregate-grid">
                      <div>Count: {aggregateResult.count}</div>
                      <div>Sum: {aggregateResult.sum}</div>
                      <div>Min: {aggregateResult.min ?? "NULL"}</div>
                      <div>Max: {aggregateResult.max ?? "NULL"}</div>
                    </div>
                  ) : (
                    <div className="empty">Run an aggregate to see results.</div>
                  )}
                </div>
              ) : activeTab === "Ingest" ? (
                <div className="panel">
                  <div className="panel-title">Ingest</div>
                  <div className="ingest-grid">
                    {schema.map((field) => (
                      <div key={field.name} className="ingest-row">
                        <label>
                          {field.name}
                          <input
                            value={ingestValues[field.name] ?? ""}
                            onChange={(event) =>
                              handleIngestChange(field.name, event.target.value)
                            }
                          />
                        </label>
                      </div>
                    ))}
                  </div>
                  <button type="button" className="apply" onClick={runIngest}>Append Row</button>
                </div>
              ) : activeTab === "Memory" ? (
                <div className="panel">
                  <div className="panel-title">Memory review</div>
                  <div className="filters">
                    <label>Tenant<input value={memoryTenant} onChange={(event) => setMemoryTenant(event.target.value)} /></label>
                    <label>Status<select value={memoryStatus} onChange={(event) => setMemoryStatus(event.target.value)}><option value="">All</option><option value="pending_confirmation">Pending confirmation</option><option value="quarantined">Quarantined</option><option value="disputed">Disputed</option></select></label>
                    <button type="button" className="apply" onClick={loadMemoryReview}>Refresh</button>
                  </div>
                  {memoryError ? <div className="error">{memoryError}</div> : null}
                  <table className="data-table"><thead><tr><th>Subject</th><th>Namespace</th><th>Status</th><th>Sensitivity</th><th>Actions</th></tr></thead>
                    <tbody>{memoryItems.map((item) => <tr key={item.memory_id}><td>{item.subject}</td><td>{item.namespace}</td><td>{item.status}</td><td>{item.sensitivity}</td><td>{item.status === "pending_confirmation" ? <button type="button" onClick={() => reviewMemory(item.memory_id, "confirm")}>Confirm</button> : null}{["pending_confirmation", "quarantined"].includes(item.status) ? <button type="button" onClick={() => reviewMemory(item.memory_id, "reject")}>Reject</button> : null}</td></tr>)}</tbody>
                  </table>
                </div>
              ) : activeTab === "Admin" ? (
                <div className="panel">
                  <div className="panel-title">Admin</div>
                  <div className="admin-grid">
                    <div>
                      <label>New database</label>
                      <input value={newDatabase} onChange={(event) => setNewDatabase(event.target.value)} />
                      <button type="button" className="apply" onClick={createDatabase}>Create</button>
                    </div>
                    <div>
                      <label>New dataset</label>
                      <input value={newDataset} onChange={(event) => setNewDataset(event.target.value)} />
                      <div className="field-list">
                        {fieldDraft.map((field, index) => (
                          <div key={field.id} className="field-row">
                            <input
                              value={field.name}
                              onChange={(event) => updateFieldDraft(index, "name", event.target.value)}
                              placeholder="field name"
                            />
                            <select
                              value={field.type}
                              onChange={(event) => updateFieldDraft(index, "type", event.target.value)}
                            >
                              <option value="int32">int32</option>
                              <option value="int64">int64</option>
                              <option value="float64">float64</option>
                              <option value="bool">bool</option>
                              <option value="dict_int32">dict_int32</option>
                              <option value="string">string</option>
                              <option value="bytes">bytes</option>
                            </select>
                          </div>
                        ))}
                      </div>
                      <button type="button" className="apply" onClick={createDataset}>Create dataset</button>
                    </div>
                  </div>
                </div>
              ) : (
                <div className="panel">
                  <div className="panel-title">Settings</div>
                  <div className="settings-grid">
                    <label>
                      Max scan limit
                      <input value={maxLimit} onChange={(event) => setMaxLimit(event.target.value)} />
                    </label>
                    <label>
                      Default limit
                      <input value={limit} onChange={(event) => setLimit(event.target.value)} />
                    </label>
                  </div>
                </div>
              )}
            </section>

            <aside className="side-panel">
              <div className="panel-title">Aggregates</div>
              <label>
                Field
                <select
                  value={aggregateField}
                  onChange={(event) => setAggregateField(event.target.value)}
                >
                  {schema
                    .filter((field) => !["string", "bytes", "array", "object", "bool"].includes(field.type))
                    .map((field) => (
                      <option key={field.name} value={field.name}>
                        {field.name}
                      </option>
                    ))}
                </select>
              </label>
              <button type="button" className="apply" onClick={runAggregate}>Apply</button>
              {aggregateResult ? (
                <div className="aggregate-grid">
                  <div>Count: {aggregateResult.count}</div>
                  <div>Sum: {aggregateResult.sum}</div>
                  <div>Min: {aggregateResult.min ?? "NULL"}</div>
                  <div>Max: {aggregateResult.max ?? "NULL"}</div>
                </div>
              ) : (
                <div className="empty">No aggregates yet</div>
              )}
            </aside>
          </div>
        </main>
      </div>

      {showConnect ? (
        <div className="modal-backdrop" onClick={() => setShowConnect(false)}>
          <div className="modal" onClick={(event) => event.stopPropagation()}>
            <div className="modal-header">
              <h2>Connect to MimicDB</h2>
              <button onClick={() => setShowConnect(false)}>Close</button>
            </div>
            <label>
              Host
              <input value={host} onChange={(event) => setHost(event.target.value)} />
            </label>
            <label>
              Port
              <input value={port} onChange={(event) => setPort(event.target.value)} />
            </label>
              <label>
                Default database
                <input value={database} onChange={(event) => setDatabase(event.target.value)} />
              </label>
              <label>
                Identity key path
                <input
                  value={identityKeyPath}
                  onChange={(event) => setIdentityKeyPath(event.target.value)}
                  placeholder="~/.mimicdb/keys/root"
                />
              </label>
            <div className="actions">
              <button
                onClick={connect}
                disabled={connectStatus === "connected" || !identityReady}
              >
                Connect
              </button>
              <button onClick={disconnect} disabled={connectStatus !== "connected"}>Disconnect</button>
            </div>
            {!identityReady ? (
              <div className="error-text">Identity key path is required.</div>
            ) : null}
            {connectError ? <div className="error-text">{connectError}</div> : null}
          </div>
        </div>
      ) : null}
      {contextMenu ? (
        <div
          className="context-menu"
          style={{ left: contextMenu.x, top: contextMenu.y }}
          onClick={(event) => event.stopPropagation()}
        >
          {contextMenu.type === "db" ? (
            <>
              <button type="button" onClick={() => startCreateDataset(contextMenu.db)}>
                New Dataset
              </button>
              <button type="button" onClick={() => deleteDatabase(contextMenu.db)}>
                Delete Database
              </button>
            </>
          ) : (
            <button
              type="button"
              onClick={() =>
                setConfirmDelete({
                  type: "dataset",
                  db: contextMenu.db,
                  dataset: contextMenu.dataset,
                })
              }
            >
              Delete Dataset
            </button>
          )}
        </div>
      ) : null}
      {confirmDelete ? (
        <div className="modal-backdrop" onClick={() => setConfirmDelete(null)}>
          <div className="modal" onClick={(event) => event.stopPropagation()}>
            <div className="modal-header">
              <h2>Confirm delete</h2>
              <button onClick={() => setConfirmDelete(null)}>Close</button>
            </div>
            {confirmDelete.type === "db" ? (
              <p>
                Delete database “{confirmDelete.db}” and all its datasets?
                {" "}
                ({(datasetsByDb[confirmDelete.db] ?? []).length} tables)
              </p>
            ) : (
              <p>Delete dataset “{confirmDelete.dataset}” from “{confirmDelete.db}”?</p>
            )}
            {confirmDelete.type === "db" ? (
              <label>
                Type DELETE to confirm
                <input
                  value={confirmInput}
                  onChange={(event) => setConfirmInput(event.target.value)}
                  placeholder="DELETE"
                />
              </label>
            ) : (
              <label className="checkbox">
                <input
                  type="checkbox"
                  checked={confirmChecked}
                  onChange={(event) => setConfirmChecked(event.target.checked)}
                />
                I understand this will permanently delete the dataset.
              </label>
            )}
            <div className="actions">
              <button
                type="button"
                className="apply"
                disabled={
                  confirmDelete.type === "db"
                    ? confirmInput !== "DELETE"
                    : !confirmChecked
                }
                onClick={() => {
                  if (confirmDelete.type === "db") {
                    setSelectedDatabase(confirmDelete.db);
                    dropDatabase();
                  } else {
                    setSelectedDatabase(confirmDelete.db);
                    if (confirmDelete.dataset) {
                      setSelectedDataset(confirmDelete.dataset);
                    }
                    dropDataset();
                  }
                  setConfirmDelete(null);
                  setConfirmInput("");
                  setConfirmChecked(false);
                }}
              >
                Delete
              </button>
              <button type="button" onClick={() => setConfirmDelete(null)}>
                Cancel
              </button>
            </div>
          </div>
        </div>
      ) : null}
    </div>
  );
}
