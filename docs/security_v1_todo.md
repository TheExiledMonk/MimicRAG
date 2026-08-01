# MimicDB Security v1 Todo (Auth + Authz)

Scope: server + mimiccli + API adjustments for login and permission errors.
Out of scope: UI implementation details, TLS, multi-node federation.

## Server: bootstrap + config
- [x] Add `root_initialized` check on startup.
- [x] Refuse non-local bind when root not initialized.
- [x] Enforce local-only root initialization (127.0.0.1/localhost/Unix socket).
- [x] Add config entries for auth DB path, host key path, idle timeout, max session lifetime.
- [x] Add config entries for rate limit thresholds and backoff schedule.

## Server: host key + identity
- [x] Generate host key on first run and persist at configured path.
- [x] Expose host public key + fingerprint in logs and CLI query path.
- [x] Add host key rotation support (server-side).

## Server: auth DB (internal __auth__)
- [x] Create internal auth DB namespace `__auth__` with hidden flag.
- [x] Implement schema: keys, roles, role_grants, key_roles, key_grants.
- [x] Implement schema: rate_limits, audit_log.
- [x] Ensure `__auth__` is not returned by normal `db.list`.
- [x] Add migration/bootstrapping for built-in roles.

## Server: handshake + secure channel
- [x] Define SEC_PROTO=1 version and negotiation fields.
- [x] Implement Hello step (nonce, fingerprints, cipher suite choice).
- [x] Implement X25519 ECDH and HKDF-SHA256 derivation.
- [x] Implement AEAD framing with sequence numbers and replay protection.
- [x] Implement transcript hashing and client signature verification.
- [x] Add session establishment response with session id/timeout.
- [x] Drop connection on auth failure with low-information response.

## Server: session + authorization enforcement
- [x] Create session store keyed by session id and fingerprint.
- [x] Cache derived permissions per session for O(1) checks.
- [x] Implement capability + scope evaluator (deny by default).
- [x] Expand role grants and `db.admin` at scope.
- [x] Map every API endpoint to required capability + scope.
- [x] Enforce authz at server entrypoints before executing work.

## Server: rate limiting + bruteforce
- [x] Track failures per remote addr and per fingerprint.
- [x] Implement exponential backoff schedule and next_allowed_at check.
- [x] Store and load backoff state in auth DB (with periodic flush if in-memory).
- [x] Return `RATE_LIMITED` with wait seconds when blocked.

## Server: auditing
- [x] Record root init, key add/remove/disable, role/grant changes.
- [x] Record failed auth attempts.
- [x] Record permission denied events (optional sampling).
- [x] Add audit record helper to reduce call-site boilerplate.

## Server: API error adjustments
- [x] Add `AUTH_FAILED` error response (uniform wording/timing).
- [x] Add `PERMISSION_DENIED` response with required cap + scope.
- [x] Add `RATE_LIMITED` response with wait seconds.
- [x] Update handlers to return new auth-related errors.

## Server: internal API for CLI
- [x] Add local-only root init endpoint/command path.
- [x] Add key management endpoints: add/disable/remove/list.
- [x] Add role/grant management endpoints.
- [x] Add ratelimit inspection and clear endpoints.

## CLI: key and host management
- [x] `mimiccli keygen --name <name>` (Ed25519).
- [x] `mimiccli hostkey show` (fingerprint + public key).
- [x] `mimiccli hostkey rotate` (local-only).
- [x] Implement `known_hosts` store with TOFU and mismatch detection.

## CLI: root bootstrap
- [x] `mimiccli auth init-root` (local-only).
- [x] Generate or import client key during root init.
- [x] Set `root_initialized=true` and assign root role at `*`.

## CLI: auth + roles + grants
- [x] `mimiccli auth add-key --pubkey <file> --comment <text>`.
- [x] `mimiccli auth disable-key --fingerprint <fp>`.
- [x] `mimiccli auth remove-key --fingerprint <fp>`.
- [x] `mimiccli auth role create <role>`.
- [x] `mimiccli auth role grant <role> --cap <cap> --scope <scope>`.
- [x] `mimiccli auth role revoke <role> --cap <cap> --scope <scope>`.
- [x] `mimiccli auth role delete <role>`.
- [x] `mimiccli auth assign-role --fingerprint <fp> --role <role> --scope <scope>`.
- [x] `mimiccli auth grant --fingerprint <fp> --cap <cap> --scope <scope>`.
- [x] `mimiccli auth whoami`.

## CLI: rate limit inspection
- [x] `mimiccli auth ratelimit list`.
- [x] `mimiccli auth ratelimit clear --remote <ip>`.

## Client library integration
- [x] Implement secure client handshake (host key pinning + TOFU).
- [x] Store client identity keys and select active identity.
- [x] Attach session token to all requests after handshake.

## Documentation + examples
- [x] Document protocol fields and versioning.
- [x] Document capability list and scope rules.
- [x] Provide example role/grant setup for reader/writer/admin.
- [x] Provide bootstrap walkthrough (local-only root init).

## Testing
- [x] Unit tests for key parsing, fingerprinting, and signature verify.
- [x] Handshake happy-path integration test.
- [x] Reject invalid signature/fingerprint test.
- [x] Rate limit backoff progression test.
- [x] Authz matrix tests (reader/writer/admin/restricted).
- [x] `__auth__` hidden in db list test.
- [x] Local-only bootstrap enforcement test.
