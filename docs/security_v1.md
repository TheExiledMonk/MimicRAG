# MimicDB Security v1

This document describes the v1 security protocol, capabilities, and bootstrap
workflow for MimicDB.

## Protocol versioning

- Security protocol magic: `MSEC` (0x4D534543)
- Security protocol version: `1`
- Wire protocol magic: `MCDB` (0x4D434442)
- Wire protocol version: `1`
- Cipher suite: `X25519 + HKDF-SHA256 + ChaCha20-Poly1305`

### Handshake (SEC_PROTO=1)

ClientHello (plaintext, fixed-size):
- `magic` (u32)
- `version` (u16)
- `cipher` (u16)
- `client_nonce` (32 bytes)
- `client_ephemeral_pub` (32 bytes, X25519)
- `client_identity_pub` (32 bytes, Ed25519)

ServerHello (plaintext, fixed-size):
- `magic` (u32)
- `version` (u16)
- `cipher` (u16)
- `server_nonce` (32 bytes)
- `server_ephemeral_pub` (32 bytes)
- `server_host_key` (32 bytes, placeholder identity key)
- `server_host_fingerprint` (32 bytes, SHA-256)

Key derivation:
- `salt = client_nonce || server_nonce`
- `session_key = HKDF_SHA256(shared_secret, salt, "mimicdb-session")`
- `aead_key_c2s` (32 bytes)
- `aead_key_s2c` (32 bytes)
- `session_id` (16 bytes)

ClientAuth (encrypted):
- `signature` (64 bytes, Ed25519 over SHA-256 transcript)

ServerAccept (encrypted):
- `status` (u8) 1 = ok, 2 = rate-limited
- `wait_seconds` (u32)
- `idle_timeout_sec` (u32)
- `max_lifetime_sec` (u32)
- `session_id` (16 bytes)

### Secure framing

Each encrypted frame:
- `seq` (u64)
- `ciphertext_len` (u32)
- `ciphertext + tag` (ChaCha20-Poly1305)

Server and client enforce strictly increasing sequence numbers.

### Session token in requests

When `session_id` is established, requests include:
- `flags |= 0x2`
- payload prefix: `session_id` (16 bytes)

The server validates and strips this prefix before normal request handling.

## Capabilities and scope rules

Capabilities are additive and deny-by-default. Scopes:
- `*` (global)
- `database:<db>`
- `dataset:<db>.<dataset>`

Scope precedence:
- dataset scope > database scope > global
- `db.admin` expands to all db.* capabilities at the same scope

### Capability list (v1)

Server/system:
- `server.health`
- `server.metrics`
- `server.shutdown`
- `server.config`
- `server.key.rotate`

Auth/security:
- `auth.read`
- `auth.manage`
- `auth.assign`

Database:
- `db.list`
- `db.read`
- `db.write`
- `db.create`
- `db.drop`
- `db.truncate`
- `db.admin`

Dataset:
- `dataset.list`
- `dataset.read`
- `dataset.write`
- `dataset.create`
- `dataset.drop`
- `dataset.truncate`
- `dataset.schema.read`
- `dataset.schema.modify`

Query:
- `query.scan`
- `query.aggregate`
- `query.export`
- `query.explain`

## Example role setups

### Reader

```
mimiccli auth role create reader
mimiccli auth role grant reader --cap db.list --scope *
mimiccli auth role grant reader --cap dataset.list --scope *
mimiccli auth role grant reader --cap dataset.read --scope *
mimiccli auth role grant reader --cap query.scan --scope *
mimiccli auth role grant reader --cap query.aggregate --scope *
```

### Writer

```
mimiccli auth role create writer
mimiccli auth role grant writer --cap db.list --scope *
mimiccli auth role grant writer --cap dataset.list --scope *
mimiccli auth role grant writer --cap dataset.read --scope *
mimiccli auth role grant writer --cap dataset.write --scope *
mimiccli auth role grant writer --cap query.scan --scope *
mimiccli auth role grant writer --cap query.aggregate --scope *
```

### Admin

```
mimiccli auth role create admin
mimiccli auth role grant admin --cap db.admin --scope *
mimiccli auth role grant admin --cap dataset.create --scope *
mimiccli auth role grant admin --cap dataset.drop --scope *
mimiccli auth role grant admin --cap dataset.truncate --scope *
mimiccli auth role grant admin --cap query.scan --scope *
mimiccli auth role grant admin --cap query.aggregate --scope *
```

## Bootstrap walkthrough (local-only)

1) Start the server on localhost:

```
./mimicdb_server --config ./mimicdb.conf
```

2) Initialize root locally (generates key if not provided):

```
python3 mimiccli.py auth init-root --key-name root
```

3) Add a user key and assign a role:

```
python3 mimiccli.py keygen --name alice
python3 mimiccli.py auth add-key --pubkey ~/.mimicdb/keys/alice.pub --comment "alice laptop"
python3 mimiccli.py auth assign-role --fingerprint <fp> --role reader --scope database:default
```

After root init, the server can bind to non-local addresses.
