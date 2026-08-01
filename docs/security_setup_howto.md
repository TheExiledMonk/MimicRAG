# MimicDB Security Setup How-To

This guide walks through a complete, clean setup: server config, host key trust,
root bootstrap, user keys, role assignments, and UI/service wiring.

## 0) Prerequisites

- Built server binary at `build/server/mimicdb_server`
- Python 3 available for `mimiccli.py`
- `mimiccli.py` uses `cryptography` (see `requirements.txt`)

## 1) Choose paths

Pick a storage root and auth directory. Example:

- Storage root: `/srv/mimicdb/data`
- Auth DB: `/srv/mimicdb/data/__auth__`
- Host key: `/srv/mimicdb/data/__auth__/host_key`

## 2) Create server config

Create `mimicdb.conf` (or edit your existing one):

```conf
bind=127.0.0.1:9000
storage_root=/srv/mimicdb/data
auth_db_path=/srv/mimicdb/data/__auth__
host_key_path=/srv/mimicdb/data/__auth__/host_key
session_idle_timeout_sec=900
session_max_lifetime_sec=86400
auth_rate_limit_burst=3
auth_rate_limit_backoff_sec=30,60,120,240,480
```

Notes:
- Before root init, the server must bind locally only.
- Use a Unix socket or `127.0.0.1` during bootstrap.

## 3) Start the server (local-only bootstrap)

```bash
./build/server/mimicdb_server --config ./mimicdb.conf
```

The server generates a host key on first run at `host_key_path`.

## 4) Initialize root admin (local only)

Generate and register the root key in one step:

```bash
python3 mimiccli.py --host 127.0.0.1 --port 9000 auth init-root --key-name root
```

Outputs:
- `~/.mimicdb/keys/root` (private key)
- `~/.mimicdb/keys/root.pub` (public key)
- Root fingerprint printed to stdout

Root initialization flips `root_initialized` in the auth DB and assigns the root role.

## 5) (Optional) Pin host key

You can explicitly pin the host key after root init:

```bash
python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  hostkey show --identity ~/.mimicdb/keys/root
```

This updates `~/.mimicdb/known_hosts`.

Notes:
- The server signs the handshake transcript with its host private key.
- The client verifies the server signature against the pinned host key.
- The server returns your client fingerprint in the accept message and waits for
  an ACK before activating the session (prevents silent key mismatch).

## 6) Generate a user key

```bash
python3 mimiccli.py keygen --name alice
```

Outputs:
- `~/.mimicdb/keys/alice`
- `~/.mimicdb/keys/alice.pub`

## 7) Add user key to the server

```bash
python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  auth add-key --pubkey ~/.mimicdb/keys/alice.pub --comment "alice laptop"
```

This prints a key fingerprint for the user and prompts you to confirm it.
Use `--yes` to skip the prompt in automation.

## 8) Create roles and grants (example)

Create a reader role and grant it read + query caps:

```bash
python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  auth role create reader

python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  auth role grant reader --cap db.list --scope "*"
python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  auth role grant reader --cap dataset.list --scope "*"
python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  auth role grant reader --cap dataset.read --scope database:default
python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  auth role grant reader --cap query.scan --scope database:default
python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  auth role grant reader --cap query.aggregate --scope database:default
```

Assign the role to the user fingerprint:

```bash
python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  auth assign-role --fingerprint <ALICE_FP> --role reader --scope "*"
```

## 9) Verify identity

```bash
python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  auth whoami --identity ~/.mimicdb/keys/alice
```

## 9.1) Host key rotation + re-pin runbook

Rotate the server host key (local-only):

```bash
python3 mimiccli.py --host 127.0.0.1 --port 9000 hostkey rotate
```

Re-pin on each client after rotation:

```bash
python3 mimiccli.py --host 127.0.0.1 --port 9000 \
  hostkey repin --identity ~/.mimicdb/keys/root
```

If a host key mismatch happens, clients will fail with a mismatch error; use
`hostkey repin` to update `known_hosts` with explicit confirmation.

## 10) Switch server to remote bind (optional)

After root init you can bind to non-local addresses:

```conf
bind=0.0.0.0:9000
```

Restart the server after changing the config.

## 11) Management UI setup with local key

The UI service uses a MimicDB client identity key.

Option A: set an env var for the service:

```bash
export MIMICDB_UI_IDENTITY_KEY=~/.mimicdb/keys/alice
```

Option B: enter it in the UI:
- Open the UI connect dialog.
- Fill **Identity key path** with `~/.mimicdb/keys/alice`.
- The UI stores it in browser localStorage and sends it to `/api/connect`.

## 12) Smoke test

Run the local smoke test script:

```bash
scripts/mimiccli_smoke_test.sh
```

It brings up a temp server, bootstraps root, and registers a user key.
