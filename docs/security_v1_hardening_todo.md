# MimicDB Security v1 Hardening TODO

Goal: close identified security gaps (session isolation, server identity proof, rate limit
hardening, audit control) and make login + key confirmation tight.

---

## A) Session + Secure Channel Isolation (Critical)
- [x] Remove global `active_channel_`/`active_session_id_`; bind channel + session to each client socket.
- [x] Store per-connection state in a struct (socket -> channel + session id).
- [x] Ensure `SendStatus` uses the correct channel for the requesting client (no cross-client reuse).
- [x] Add cleanup on disconnect: delete per-connection state and session if configured.
- [x] Add concurrency safety for session store (mutex or per-thread dispatch).

## B) Server Identity Proof (Critical)
- [x] Replace random host key bytes with a real Ed25519 keypair (private + public).
- [x] Store host keypair on disk; refuse startup if corrupted or wrong size.
- [x] Sign handshake transcript with server host private key.
- [x] Include signature in server hello or first encrypted response.
- [x] Client verifies server signature with pinned host public key.
- [x] Add CLI/tooling to rotate host keypair and update known_hosts.

## C) Client Identity Proof + Confirmation (High)
- [x] Explicitly confirm client key fingerprint on login (server logs + optional client display).
- [x] Add `mimiccli auth whoami` to show fingerprint + role summary and require explicit
      confirmation for key registration steps.
- [x] Add optional “key confirmation” step: server returns fingerprint; client must ACK
      the fingerprint before session is fully active (prevents silent key mismatch).
- [x] Reject mismatched public key fingerprint in any key registration path.

## D) Host Key Trust (TOFU) Tightening (High)
- [x] Enforce known_hosts pinning for all network connections (no silent accept except first).
- [x] Log and surface host key mismatch details clearly.
- [x] Add “force re-pin” workflow in CLI with explicit confirmation.

## E) Rate Limit + Audit DoS Controls (High)
- [x] Cap in-memory `rate_limit_state_` entries (LRU + TTL).
- [x] Add max `audit_log` and `rate_limits` rows; implement retention/compaction.
- [x] Avoid unbounded disk growth from auth failures (batch + prune).
- [x] Add configurable limits in server config.

## F) Session Store Hygiene (Medium)
- [x] Implement session TTL cleanup and idle eviction sweep.
- [x] Add cap on total sessions to prevent memory blowups.
- [x] Enforce per-fingerprint session limits (optional).

## G) Handshake/Protocol Hardening (Medium)
- [x] Include protocol version + cipher suite in transcript and signature.
- [x] Validate `client_hello` sizes and reject unexpected lengths early.
- [x] Add explicit replay protection for handshake messages.
- [x] Add tighter error timing normalization for auth failures.

## H) Tests
- [x] Multi-client session isolation test (two connections, ensure separation).
- [x] MITM simulation test: host key mismatch or missing server signature must fail.
- [x] Rate limit + audit retention tests (caps enforced).
- [x] Session cleanup tests (TTL + idle).

## I) Documentation
- [x] Update security doc with server signature step and key confirmation flow.
- [x] Add runbook for host key rotation and client re-pin.
