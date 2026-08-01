#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys

from cryptography.hazmat.primitives.asymmetric import ed25519
from cryptography.hazmat.primitives import serialization

from client.mimicdb_client import MimicDBClient, ProtocolError


KNOWN_HOSTS_PATH = Path.home() / ".mimicdb" / "known_hosts"
KEYS_DIR = Path.home() / ".mimicdb" / "keys"


def _load_known_hosts() -> dict[str, tuple[str, str]]:
    entries: dict[str, tuple[str, str]] = {}
    if not KNOWN_HOSTS_PATH.exists():
        return entries
    for line in KNOWN_HOSTS_PATH.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.strip().startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 3:
            continue
        hostport, fingerprint, key_hex = parts[:3]
        entries[hostport] = (fingerprint, key_hex)
    return entries


def _write_known_hosts(entries: dict[str, tuple[str, str]]) -> None:
    KNOWN_HOSTS_PATH.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    for hostport, (fingerprint, key_hex) in sorted(entries.items()):
        lines.append(f"{hostport} {fingerprint} {key_hex}")
    KNOWN_HOSTS_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _update_known_hosts(host: str, port: int, fingerprint: str, key_hex: str) -> None:
    hostport = f"{host}:{port}"
    entries = _load_known_hosts()
    if hostport in entries:
        known_fp, known_key = entries[hostport]
        if known_fp != fingerprint or known_key != key_hex:
            raise RuntimeError(
                f"known_hosts mismatch for {hostport}: expected {known_fp}, got {fingerprint}"
            )
        return
    entries[hostport] = (fingerprint, key_hex)
    _write_known_hosts(entries)


def _confirm_fingerprint(action: str, fingerprint: str, yes: bool) -> bool:
    if yes:
        return True
    print(f"{action} fingerprint: {fingerprint}")
    response = input("Type 'yes' to confirm: ").strip().lower()
    return response == "yes"


def _confirm_repin(host: str, port: int, fingerprint: str, yes: bool) -> bool:
    if yes:
        return True
    hostport = f"{host}:{port}"
    entries = _load_known_hosts()
    current = entries.get(hostport)
    if current:
        print(f"current fingerprint: {current[0]}")
    print(f"new fingerprint: {fingerprint}")
    response = input(f"Type 'yes' to re-pin {hostport}: ").strip().lower()
    return response == "yes"


def cmd_keygen(args: argparse.Namespace) -> int:
    KEYS_DIR.mkdir(parents=True, exist_ok=True)
    key = ed25519.Ed25519PrivateKey.generate()
    private_bytes = key.private_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PrivateFormat.Raw,
        encryption_algorithm=serialization.NoEncryption(),
    )
    public_bytes = key.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )
    priv_path = KEYS_DIR / args.name
    pub_path = KEYS_DIR / f"{args.name}.pub"
    priv_path.write_bytes(private_bytes)
    pub_path.write_bytes(public_bytes)
    print(f"private: {priv_path}")
    print(f"public: {pub_path}")
    return 0


def cmd_hostkey_show(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity))
        if args.identity
        else None,
    )
    try:
        info = client.host_key()
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        if "host key mismatch" in str(exc):
            print("hint: use `mimiccli hostkey repin` to update known_hosts", file=sys.stderr)
        return 1
    try:
        _update_known_hosts(args.host, args.port, info["fingerprint"], info["key_hex"])
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"fingerprint: {info['fingerprint']}")
    print(f"public_key_hex: {info['key_hex']}")
    return 0


def cmd_hostkey_rotate(args: argparse.Namespace) -> int:
    client = MimicDBClient(host=args.host, port=args.port)
    try:
        info = client.host_key_rotate()
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    entries = _load_known_hosts()
    hostport = f"{args.host}:{args.port}"
    entries[hostport] = (info["fingerprint"], info["key_hex"])
    _write_known_hosts(entries)
    print(f"fingerprint: {info['fingerprint']}")
    print(f"public_key_hex: {info['key_hex']}")
    return 0


def cmd_hostkey_repin(args: argparse.Namespace) -> int:
    os.environ["MIMICDB_ALLOW_KNOWN_HOSTS_SKIP"] = "1"
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity))
        if args.identity
        else None,
        known_hosts_mode="skip",
    )
    try:
        info = client.host_key()
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    if not _confirm_repin(args.host, args.port, info["fingerprint"], args.yes):
        print("aborted: repin not confirmed", file=sys.stderr)
        return 1
    entries = _load_known_hosts()
    hostport = f"{args.host}:{args.port}"
    entries[hostport] = (info["fingerprint"], info["key_hex"])
    _write_known_hosts(entries)
    print(f"fingerprint: {info['fingerprint']}")
    print(f"public_key_hex: {info['key_hex']}")
    return 0


def cmd_auth_init_root(args: argparse.Namespace) -> int:
    KEYS_DIR.mkdir(parents=True, exist_ok=True)
    priv_path = KEYS_DIR / args.key_name
    pub_path = KEYS_DIR / f"{args.key_name}.pub"
    try:
        if args.import_private:
            priv_bytes = Path(args.import_private).expanduser().read_bytes()
            key = ed25519.Ed25519PrivateKey.from_private_bytes(priv_bytes)
        else:
            key = ed25519.Ed25519PrivateKey.generate()
            priv_bytes = key.private_bytes(
                encoding=serialization.Encoding.Raw,
                format=serialization.PrivateFormat.Raw,
                encryption_algorithm=serialization.NoEncryption(),
            )
            priv_path.write_bytes(priv_bytes)
        pub_bytes = key.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw,
        )
        if args.import_public:
            imported_pub = Path(args.import_public).expanduser().read_bytes()
            if imported_pub != pub_bytes:
                raise RuntimeError("imported public key does not match private key")
        pub_path.write_bytes(pub_bytes)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    fingerprint = hashlib.sha256(pub_bytes).hexdigest()
    if not _confirm_fingerprint("init root with", fingerprint, args.yes):
        print("aborted: fingerprint not confirmed", file=sys.stderr)
        return 1
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(priv_path),
    )
    try:
        fingerprint = client.auth_init_root(pub_bytes, comment=args.comment)
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"fingerprint: {fingerprint}")
    print(f"private: {priv_path}")
    print(f"public: {pub_path}")
    return 0


def _select_identity_key(path: str | None) -> Path:
    if path:
        return Path(path).expanduser()
    return Path.home() / ".mimicdb" / "keys" / "root"


def cmd_auth_add_key(args: argparse.Namespace) -> int:
    pubkey = Path(args.pubkey).expanduser().read_bytes()
    fingerprint = hashlib.sha256(pubkey).hexdigest()
    if not _confirm_fingerprint("add key with", fingerprint, args.yes):
        print("aborted: fingerprint not confirmed", file=sys.stderr)
        return 1
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        fingerprint = client.auth_key_add(pubkey, comment=args.comment or "")
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"fingerprint: {fingerprint}")
    return 0


def cmd_auth_disable_key(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        client.auth_key_disable(args.fingerprint)
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_auth_remove_key(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        client.auth_key_remove(args.fingerprint)
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_auth_role_create(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        client.auth_role_create(args.role)
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_auth_role_delete(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        client.auth_role_delete(args.role)
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_auth_role_grant(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        client.auth_role_grant(args.role, args.cap, args.scope, revoke=False)
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_auth_role_revoke(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        client.auth_role_grant(args.role, args.cap, args.scope, revoke=True)
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_auth_assign_role(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        client.auth_assign_role(args.fingerprint, args.role, args.scope, revoke=False)
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_auth_grant(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        client.auth_grant_key(args.fingerprint, args.cap, args.scope, revoke=False)
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_auth_ratelimit_list(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        entries = client.auth_ratelimit_list()
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    for entry in entries:
        print(
            f"{entry['remote']} {entry['fingerprint']} "
            f"fail={entry['fail_count']} next={entry['next_allowed_at']} "
            f"last={entry['last_fail_at']}"
        )
    return 0


def cmd_auth_ratelimit_clear(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        client.auth_ratelimit_clear(args.remote, fingerprint=args.fingerprint or "")
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


def cmd_auth_whoami(args: argparse.Namespace) -> int:
    client = MimicDBClient(
        host=args.host,
        port=args.port,
        identity_key_path=str(_select_identity_key(args.identity)),
    )
    try:
        info = client.auth_whoami()
    except ProtocolError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"fingerprint: {info['fingerprint']}")
    for role in info["roles"]:
        print(f"role: {role['role']} scope={role['scope']}")
    for grant in info["grants"]:
        print(f"grant: {grant['capability']} scope={grant['scope']}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="mimiccli")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    sub = parser.add_subparsers(dest="command", required=True)

    keygen = sub.add_parser("keygen", help="generate an Ed25519 keypair")
    keygen.add_argument("--name", required=True)
    keygen.set_defaults(func=cmd_keygen)

    hostkey = sub.add_parser("hostkey", help="host key operations")
    hostkey_sub = hostkey.add_subparsers(dest="hostkey_cmd", required=True)
    hostkey_show = hostkey_sub.add_parser("show", help="show host key and fingerprint")
    hostkey_show.add_argument("--identity", help="path to identity private key")
    hostkey_show.set_defaults(func=cmd_hostkey_show)
    hostkey_rotate = hostkey_sub.add_parser("rotate", help="rotate host key (local only)")
    hostkey_rotate.set_defaults(func=cmd_hostkey_rotate)
    hostkey_repin = hostkey_sub.add_parser("repin", help="force re-pin known host")
    hostkey_repin.add_argument("--identity", help="path to identity private key")
    hostkey_repin.add_argument("--yes", action="store_true", help="skip confirmation prompt")
    hostkey_repin.set_defaults(func=cmd_hostkey_repin)

    auth = sub.add_parser("auth", help="authentication and authorization")
    auth.add_argument("--identity", help="path to identity private key")
    auth_sub = auth.add_subparsers(dest="auth_cmd", required=True)
    init_root = auth_sub.add_parser("init-root", help="initialize root key (local only)")
    init_root.add_argument("--key-name", default="root")
    init_root.add_argument("--import-private", dest="import_private")
    init_root.add_argument("--import-public", dest="import_public")
    init_root.add_argument("--comment", default="root")
    init_root.add_argument("--yes", action="store_true", help="skip fingerprint prompt")
    init_root.set_defaults(func=cmd_auth_init_root)

    add_key = auth_sub.add_parser("add-key", help="add a client public key")
    add_key.add_argument("--pubkey", required=True)
    add_key.add_argument("--comment", default="")
    add_key.add_argument("--yes", action="store_true", help="skip fingerprint prompt")
    add_key.set_defaults(func=cmd_auth_add_key)

    disable_key = auth_sub.add_parser("disable-key", help="disable a key")
    disable_key.add_argument("--fingerprint", required=True)
    disable_key.set_defaults(func=cmd_auth_disable_key)

    remove_key = auth_sub.add_parser("remove-key", help="remove a key")
    remove_key.add_argument("--fingerprint", required=True)
    remove_key.set_defaults(func=cmd_auth_remove_key)

    role = auth_sub.add_parser("role", help="role operations")
    role_sub = role.add_subparsers(dest="role_cmd", required=True)
    role_create = role_sub.add_parser("create", help="create role")
    role_create.add_argument("role")
    role_create.set_defaults(func=cmd_auth_role_create)
    role_delete = role_sub.add_parser("delete", help="delete role")
    role_delete.add_argument("role")
    role_delete.set_defaults(func=cmd_auth_role_delete)
    role_grant = role_sub.add_parser("grant", help="grant role capability")
    role_grant.add_argument("role")
    role_grant.add_argument("--cap", required=True)
    role_grant.add_argument("--scope", required=True)
    role_grant.set_defaults(func=cmd_auth_role_grant)
    role_revoke = role_sub.add_parser("revoke", help="revoke role capability")
    role_revoke.add_argument("role")
    role_revoke.add_argument("--cap", required=True)
    role_revoke.add_argument("--scope", required=True)
    role_revoke.set_defaults(func=cmd_auth_role_revoke)

    assign_role = auth_sub.add_parser("assign-role", help="assign role to key")
    assign_role.add_argument("--fingerprint", required=True)
    assign_role.add_argument("--role", required=True)
    assign_role.add_argument("--scope", required=True)
    assign_role.set_defaults(func=cmd_auth_assign_role)

    grant_key = auth_sub.add_parser("grant", help="grant capability to key")
    grant_key.add_argument("--fingerprint", required=True)
    grant_key.add_argument("--cap", required=True)
    grant_key.add_argument("--scope", required=True)
    grant_key.set_defaults(func=cmd_auth_grant)

    whoami = auth_sub.add_parser("whoami", help="show current identity")
    whoami.set_defaults(func=cmd_auth_whoami)

    ratelimit = auth_sub.add_parser("ratelimit", help="rate limit inspection")
    ratelimit_sub = ratelimit.add_subparsers(dest="ratelimit_cmd", required=True)
    ratelimit_list = ratelimit_sub.add_parser("list", help="list rate limits")
    ratelimit_list.set_defaults(func=cmd_auth_ratelimit_list)
    ratelimit_clear = ratelimit_sub.add_parser("clear", help="clear rate limit")
    ratelimit_clear.add_argument("--remote", required=True)
    ratelimit_clear.add_argument("--fingerprint")
    ratelimit_clear.set_defaults(func=cmd_auth_ratelimit_clear)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
