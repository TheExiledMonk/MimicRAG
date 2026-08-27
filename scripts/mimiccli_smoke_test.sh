#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="${ROOT_DIR}/build/server/mimicdb_server"
CLI_BIN="${ROOT_DIR}/mimiccli.py"

if [[ ! -x "${SERVER_BIN}" ]]; then
  echo "mimicdb_server not found at ${SERVER_BIN}" >&2
  exit 1
fi

if [[ ! -f "${CLI_BIN}" ]]; then
  echo "mimiccli.py not found at ${CLI_BIN}" >&2
  exit 1
fi

TEST_ROOT="$(mktemp -d /tmp/mimiccli_smoke.XXXX)"
TEST_HOME="${TEST_ROOT}/home"
TEST_DATA="${TEST_ROOT}/data"
PORT="$(python3 - <<'PY'
import socket

with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)"
mkdir -p "${TEST_HOME}" "${TEST_DATA}"

CONFIG_PATH="${TEST_ROOT}/mimicdb_test.conf"
cat > "${CONFIG_PATH}" <<EOF
bind=127.0.0.1:${PORT}
storage_root=${TEST_DATA}
auth_db_path=${TEST_DATA}/__auth__
host_key_path=${TEST_DATA}/__auth__/host_key
EOF

SERVER_LOG="${TEST_ROOT}/server.log"
${SERVER_BIN} --config "${CONFIG_PATH}" >"${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

cleanup() {
  kill "${SERVER_PID}" >/dev/null 2>&1 || true
  rm -rf "${TEST_ROOT}"
}
trap cleanup EXIT

server_ready() {
  python3 - "${PORT}" <<'PY'
import socket
import sys

try:
    with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.2):
        pass
except OSError:
    raise SystemExit(1)
PY
}

for _ in {1..100}; do
  if server_ready; then
    break
  fi
  sleep 0.2
done
if ! server_ready; then
  echo "server failed to start on ${PORT}" >&2
  cat "${SERVER_LOG}" >&2
  exit 1
fi

export HOME="${TEST_HOME}"

python3 "${CLI_BIN}" --host 127.0.0.1 --port "${PORT}" auth init-root --key-name root --yes
python3 "${CLI_BIN}" --host 127.0.0.1 --port "${PORT}" \
  hostkey show --identity "${TEST_HOME}/.mimicdb/keys/root"
python3 "${CLI_BIN}" keygen --name alice
python3 "${CLI_BIN}" --host 127.0.0.1 --port "${PORT}" \
  auth add-key --pubkey "${TEST_HOME}/.mimicdb/keys/alice.pub" --comment "alice" --yes

ls -l "${TEST_HOME}/.mimicdb/keys"
echo "ok: mimiccli smoke test passed on port ${PORT}"
