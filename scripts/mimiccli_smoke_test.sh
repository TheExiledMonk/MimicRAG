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
PORT="$(shuf -i 15000-20000 -n 1)"
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

for _ in {1..30}; do
  if ss -ltn | rg -q ":${PORT}"; then
    break
  fi
  sleep 0.2
done
if ! ss -ltn | rg -q ":${PORT}"; then
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
