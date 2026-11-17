# Authentication and authorization design

## Status

Design proposal for the next server milestone. This document does not claim that multi-user authentication or database permissions are implemented. The current server still uses the optional shared `CUSTOMDB_API_KEY` described in [SERVER.md](SERVER.md).

## Goals

- Replace the single shared API key with named users and revocable credentials.
- Enforce authorization on every database operation, including list, backup, restore, schema, index, transaction, and document routes.
- Support least-privilege database-scoped permissions.
- Keep the embedded storage engine and its on-disk database format independent of HTTP identities.
- Preserve loopback-first defaults and avoid requiring a paid hosted identity provider.

## Proposed model

### Principals and roles

Each authenticated principal has a stable user ID, username, credential-verifier metadata, status, and timestamps. Usernames are unique. Disabled users cannot authenticate and their outstanding tokens are rejected.

Initial roles:

- `admin`: manage users, grants, and all databases.
- `read_write`: read and mutate data only in explicitly granted databases.
- `read_only`: read data only in explicitly granted databases.

Authorization is database-scoped. A grant binds one user to one database and one role. Database creation and deletion, user administration, grants, backup restore, and other administrative operations require explicit administrative permission. Restore is treated as a destructive write operation.

### Credential and token handling

- Passwords are never stored in plaintext or with a fast general-purpose hash.
- Use a vetted password KDF (prefer Argon2id; use a maintained PBKDF2-HMAC-SHA-256 implementation only if the supported build matrix cannot provide Argon2id) with per-user random salts and versioned parameters.
- Compare credential verifiers and token MACs using constant-time comparison.
- Issue cryptographically random, opaque bearer tokens; store only a cryptographic hash of each token, with user ID, creation/expiry times, and revocation state.
- Require bounded token lifetimes and support explicit revocation. Do not put passwords or tokens in URLs or logs.
- Bootstrap the first administrator through a one-time local command or securely permissioned setup file. Never ship a default username/password and never silently create an administrator on network startup.

### Persistence and filesystem safety

- Keep identity metadata outside database files, in a dedicated auth store beneath the configured data directory.
- Restrict auth-store file permissions to the server account where supported; write updates atomically using a temporary file, flush, and rename.
- Validate all identifiers and derive paths on the server. No client-supplied filesystem paths.
- Serialize auth-store mutations and define recovery behavior for malformed or partially written metadata.
- Back up and restore database data without silently rolling back user credentials, grants, or token revocations.

### HTTP behavior

- Public liveness endpoint: `GET /health`; it must not disclose database names, paths, configuration, or secrets.
- Protected endpoints return `401` for missing/invalid credentials and `403` for authenticated users lacking permission.
- Use `Authorization: Bearer <token>`; do not accept credentials in query strings.
- Apply authentication and authorization consistently before opening a database handle or returning resource-existence details.
- Keep request-size and timeout limits. Add rate limiting to login/token issuance and generic authentication errors that do not reveal whether a username exists.
- Do not enable non-loopback binding merely because authentication is configured. Require an explicit operator opt-in and TLS at a trusted reverse proxy or a future native TLS configuration.

## Implementation sequence

1. **Threat model and storage design:** settle auth-store format, KDF dependency and supported platforms; add file-permission and atomic-write tests.
2. **Bootstrap and credential primitives:** one-time admin setup, password verification, opaque token issuance, expiry and revocation; unit tests for malformed input and credential handling.
3. **Central middleware:** replace per-route shared-key checks with one authentication context and centralized authorization policy. Ensure every route is covered.
4. **User and grant administration:** admin-only create/disable users, rotate credentials, grant/revoke database roles.
5. **Authorization matrix tests:** test every route for unauthenticated, read-only, read-write, admin, revoked, disabled, expired and cross-database access.
6. **Operational hardening:** audit events without secrets, login throttling, auth-store backup/recovery tests, security review and updated deployment guidance.

## Explicit non-goals for the first implementation

- Multi-factor authentication, SSO/OIDC, external identity federation, organization tenancy, fine-grained per-collection ACLs, and distributed session storage.
- Claiming production or internet-facing readiness before TLS, abuse controls, auditability, recovery, and independent security review are in place.

## Acceptance criteria

- No route other than the minimal liveness endpoint is accessible without a valid authenticated principal.
- No user can access a database without an explicit grant, except an administrator.
- Read-only users cannot mutate data or perform restore/administrative operations.
- Revoked, expired, disabled-user, and malformed tokens are rejected.
- Secrets never appear in API responses, logs, test output, or repository fixtures.
- CI exercises the full route/role matrix on the supported operating systems.
