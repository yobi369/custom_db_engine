# Authentication and authorization design

## Status

Implementation baseline on the `server-foundation` branch. Named users, password login, bearer-token sessions, centralized authentication, database-scoped grants, and administrator operations are implemented and covered by tests. The legacy shared `CUSTOMDB_API_KEY` is used only when no identity store exists; once an identity store exists, bearer authentication takes over.

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
- Current implementation: OpenSSL PBKDF2-HMAC-SHA-256 with 600,000 iterations, unique 16-byte salts, versioned parameters, and bounded verifier parsing. Argon2id remains a possible future KDF upgrade after portability evaluation.
- Compare credential verifiers and token MACs using constant-time comparison.
- Issue cryptographically random, opaque bearer tokens; store only a cryptographic hash of each token, with user ID, creation/expiry times, and revocation state.
- Require bounded token lifetimes and support explicit revocation. Do not put passwords or tokens in URLs or logs.
- The first administrator is bootstrapped with `custom_db_server --bootstrap-admin`, using hidden password entry and confirmation. Never ship a default username/password and never silently create an administrator on network startup.

### Persistence and filesystem safety

- Keep identity metadata outside database files, in a dedicated auth store beneath the configured data directory.
- Auth-store writes use a versioned JSON file, bounded parsing, a sibling temporary file, and atomic replacement. POSIX permissions are set to `0600`; Windows ACLs remain the operator's responsibility.
- Validate all identifiers and derive paths on the server. No client-supplied filesystem paths.
- Serialize in-process auth-store mutations. The server and bootstrap CLI acquire an OS-level exclusive lock so only one process uses a data directory at a time. Malformed stores fail closed; crash/power-loss recovery remains a hardening task.
- Back up and restore database data without silently rolling back user credentials, grants, or token revocations.

### HTTP behavior

- Public liveness endpoint: `GET /health`; it must not disclose database names, paths, configuration, or secrets.
- Protected endpoints return `401` for missing/invalid credentials and `403` for authenticated users lacking permission.
- Use `Authorization: Bearer <token>`; do not accept credentials in query strings.
- Central middleware applies authentication and authorization before protected route handlers open a database handle or return resource-existence details. `GET /health` and `POST /v1/auth/login` are the only unauthenticated routes in identity mode.
- Keep request-size and timeout limits. Add rate limiting to login/token issuance and generic authentication errors that do not reveal whether a username exists.
- Non-loopback binding requires `CUSTOMDB_ALLOW_NON_LOOPBACK=1`; authentication alone does not provide transport encryption. Use TLS at a trusted reverse proxy.

## Implementation sequence

1. **Threat model and storage design:** implemented versioned auth-store format, OpenSSL KDF dependency, POSIX permissions, atomic replacement, and supported-platform CI.
2. **Bootstrap and credential primitives:** implemented one-time admin setup, password verification, opaque token issuance, expiry, revocation, password reset, logout, and unit tests.
3. **Central middleware:** implemented pre-routing bearer authentication and centralized role/database authorization. Legacy API-key fallback is disabled in identity mode.
4. **User and grant administration:** implemented administrator create/list/disable, password reset, grant replacement, and token revocation.
5. **Authorization matrix tests:** implemented HTTP tests for unauthenticated, read-only, read-write, admin, revoked, disabled, expired, malformed-token, cross-database, restore, lockout, and process-lock behavior.
6. **Operational hardening:** login throttling and deployment guidance are implemented. Structured audit events, auth-store crash recovery/fault testing, independent security review, and password self-service remain outstanding.

## Explicit non-goals for the first implementation

- Multi-factor authentication, SSO/OIDC, external identity federation, organization tenancy, fine-grained per-collection ACLs, and distributed session storage.
- Claiming production or internet-facing readiness before TLS, abuse controls, auditability, recovery, and independent security review are in place.

## Acceptance criteria

- In identity mode, no protected route other than `GET /health` and `POST /v1/auth/login` is accessible without a valid authenticated principal; logout itself requires the current bearer token.
- No user can access a database without an explicit grant, except an administrator.
- Read-only users cannot mutate data or perform restore/administrative operations.
- Revoked, expired, disabled-user, and malformed tokens are rejected.
- Secrets never appear in API responses, logs, test output, or repository fixtures.
- CI exercises the full route/role matrix on the supported operating systems.
