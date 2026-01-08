<?php

final class MimicDBClient {
    private string $host;
    private int $port;
    private string $defaultDb;
    private int $nextId = 1;
    private $socket = null;

    private const MAGIC = 0x4D434442;
    private const VERSION = 1;

    private const OP_PING = 1;
    private const OP_CREATE_DATASET = 2;
    private const OP_APPEND_BATCH = 3;
    private const OP_QUERY_AGG = 4;
    private const OP_HEALTH = 5;
    private const OP_CREATE_DATABASE = 6;
    private const OP_LIST_DATABASES = 7;

    private const STATUS_OK = 0;

    private const FIELD_TYPES = [
        "int32" => 0,
        "int64" => 1,
        "float64" => 2,
        "bool" => 3,
        "dict_int32" => 4,
        "string" => 5,
        "bytes" => 6,
    ];

    public function __construct(?string $host = null, ?int $port = null, string $defaultDb = "default") {
        $this->host = $host ?? "127.0.0.1";
        $this->port = $port ?? 9000;
        $this->defaultDb = $defaultDb;
    }

    public function connect(): void {
        if ($this->socket !== null) {
            return;
        }
        $addr = "tcp://{$this->host}:{$this->port}";
        $this->socket = @stream_socket_client($addr, $errno, $errstr, 2.0);
        if ($this->socket === false) {
            throw new RuntimeException("connect failed: {$errstr}");
        }
        stream_set_timeout($this->socket, 5);
    }

    public function close(): void {
        if ($this->socket !== null) {
            fclose($this->socket);
            $this->socket = null;
        }
    }

    public function setDefaultDb(string $name): void {
        $this->defaultDb = $name;
    }

    public function ping(): void {
        $this->request(self::OP_PING, "");
    }

    public function createDatabase(string $name): void {
        $payload = $this->packString($name);
        $this->request(self::OP_CREATE_DATABASE, $payload);
    }

    public function listDatabases(): array {
        $payload = $this->request(self::OP_LIST_DATABASES, "");
        if (strlen($payload) < 2) {
            throw new RuntimeException("short list_databases response");
        }
        $count = unpack("v", substr($payload, 0, 2))[1];
        $cursor = 2;
        $names = [];
        for ($i = 0; $i < $count; $i++) {
            if ($cursor + 2 > strlen($payload)) {
                throw new RuntimeException("short list_databases response");
            }
            $len = unpack("v", substr($payload, $cursor, 2))[1];
            $cursor += 2;
            $names[] = substr($payload, $cursor, $len);
            $cursor += $len;
        }
        return $names;
    }

    public function createDataset(string $name, array $fields, ?string $database = null): void {
        $db = $database ?? $this->defaultDb;
        $payload = $this->packString($db) . $this->packString($name);
        $payload .= pack("v", count($fields));
        foreach ($fields as $field) {
            [$fieldName, $fieldType] = $field;
            if (!array_key_exists($fieldType, self::FIELD_TYPES)) {
                throw new InvalidArgumentException("unsupported field type {$fieldType}");
            }
            $payload .= $this->packString($fieldName);
            $payload .= pack("C", self::FIELD_TYPES[$fieldType]);
        }
        $this->request(self::OP_CREATE_DATASET, $payload);
    }

    public function appendBatch(string $dataset, array $fields, array $columns, ?string $database = null, ?int $batchId = null): void {
        $db = $database ?? $this->defaultDb;
        $payload = $this->packString($db) . $this->packString($dataset);
        if ($batchId === null) {
            $batchId = (int)(hrtime(true) & 0xFFFFFFFFFFFFFFFF);
        }
        $payload .= pack("P", $batchId);
        $rowCount = $this->columnLength($columns, $fields);
        $payload .= pack("V", $rowCount);
        $payload .= pack("v", count($fields));
        foreach ($fields as $index => $field) {
            [$fieldName, $fieldType] = $field;
            $values = $columns[$fieldName] ?? [];
            [$validityMode, $validity, $packed] = $this->packValues($values, $fieldType);
            $payload .= pack("v", $index);
            $payload .= pack("C", self::FIELD_TYPES[$fieldType]);
            $payload .= pack("C", $validityMode);
            $payload .= pack("V", $rowCount);
            $payload .= $packed;
            if ($validityMode === 1) {
                $payload .= $validity;
            }
        }
        $this->request(self::OP_APPEND_BATCH, $payload);
    }

    public function queryAgg(string $dataset, int $fieldIndex, ?string $database = null): array {
        $db = $database ?? $this->defaultDb;
        $payload = $this->packString($db) . $this->packString($dataset) . pack("v", $fieldIndex);
        $payload .= pack("v", 0);
        $response = $this->request(self::OP_QUERY_AGG, $payload);
        if (strlen($response) < 41) {
            throw new RuntimeException("short query_agg response");
        }
        $count = unpack("P", substr($response, 0, 8))[1];
        $sum = unpack("e", substr($response, 8, 8))[1];
        $min = unpack("e", substr($response, 16, 8))[1];
        $max = unpack("e", substr($response, 24, 8))[1];
        $hasValue = ord($response[32]) === 1;
        $rowsScanned = unpack("P", substr($response, 33, 8))[1];
        return [
            "count" => $count,
            "sum" => $sum,
            "min" => $hasValue ? $min : null,
            "max" => $hasValue ? $max : null,
            "rows_scanned" => $rowsScanned,
        ];
    }

    public function health(): array {
        $response = $this->request(self::OP_HEALTH, "");
        if (strlen($response) < 18) {
            throw new RuntimeException("short health response");
        }
        $datasetCount = unpack("v", substr($response, 0, 2))[1];
        $segmentCount = unpack("P", substr($response, 2, 8))[1];
        $rowCount = unpack("P", substr($response, 10, 8))[1];
        return [
            "datasets" => $datasetCount,
            "segments" => $segmentCount,
            "rows" => $rowCount,
        ];
    }

    private function request(int $opcode, string $payload): string {
        $this->connect();
        $requestId = $this->nextId++;
        $header = pack("VvvvvVV", self::MAGIC, self::VERSION, 0, $opcode, 0, strlen($payload), $requestId);
        $this->writeAll($header);
        if ($payload !== "") {
            $this->writeAll($payload);
        }
        $respHeader = $this->readExact(20);
        $parts = unpack("Vmagic/vversion/vflags/vopcode/vstatus/Vsize/Vid", $respHeader);
        if ($parts["magic"] !== self::MAGIC || $parts["version"] !== self::VERSION) {
            throw new RuntimeException("invalid response header");
        }
        if ($parts["opcode"] !== $opcode || $parts["id"] !== $requestId) {
            throw new RuntimeException("mismatched response");
        }
        $payload = $parts["size"] > 0 ? $this->readExact($parts["size"]) : "";
        if ($parts["status"] !== self::STATUS_OK) {
            throw new RuntimeException("server error status={$parts['status']}");
        }
        return $payload;
    }

    private function writeAll(string $data): void {
        $total = strlen($data);
        $sent = 0;
        while ($sent < $total) {
            $written = fwrite($this->socket, substr($data, $sent));
            if ($written === false) {
                throw new RuntimeException("socket write failed");
            }
            $sent += $written;
        }
    }

    private function readExact(int $size): string {
        $data = "";
        while (strlen($data) < $size) {
            $chunk = fread($this->socket, $size - strlen($data));
            if ($chunk === false || $chunk === "") {
                throw new RuntimeException("socket read failed");
            }
            $data .= $chunk;
        }
        return $data;
    }

    private function packString(string $value): string {
        return pack("v", strlen($value)) . $value;
    }

    private function columnLength(array $columns, array $fields): int {
        $lengths = [];
        foreach ($fields as $field) {
            $name = $field[0];
            $lengths[] = count($columns[$name] ?? []);
        }
        $lengths = array_unique($lengths);
        if (count($lengths) !== 1) {
            throw new InvalidArgumentException("all columns must have the same length");
        }
        return $lengths[0];
    }

    private function packValues(array $values, string $fieldType): array {
        $validity = [];
        $packed = "";
        foreach ($values as $value) {
            if ($value === null) {
                $validity[] = 0;
                $value = 0;
            } else {
                $validity[] = 1;
            }
            switch ($fieldType) {
                case "int32":
                case "dict_int32":
                    $packed .= pack("l", (int)$value);
                    break;
                case "int64":
                    $packed .= pack("q", (int)$value);
                    break;
                case "float64":
                    $packed .= pack("e", (float)$value);
                    break;
                case "bool":
                    $packed .= pack("C", $value ? 1 : 0);
                    break;
                default:
                    throw new InvalidArgumentException("unsupported field type {$fieldType}");
            }
        }
        $allValid = true;
        foreach ($validity as $bit) {
            if ($bit === 0) {
                $allValid = false;
                break;
            }
        }
        if ($allValid) {
            return [0, "", $packed];
        }
        return [1, $this->packValidity($validity), $packed];
    }

    private function packValidity(array $bits): string {
        $out = str_repeat("\0", (int)ceil(count($bits) / 8));
        foreach ($bits as $i => $bit) {
            if ($bit) {
                $byte = intdiv($i, 8);
                $offset = $i % 8;
                $out[$byte] = chr(ord($out[$byte]) | (1 << $offset));
            }
        }
        return $out;
    }
}

?>
