# Authentication implementation decision record

## Status

Accepted implementation plan. The OpenSSL crypto wrappers and a first versioned identity/token store are implemented and unit-tested on the development branch. The one-time local administrator CLI is implemented. HTTP login, middleware, authorization enforcement, and full security gates are not yet implemented. This is not a complete authentication system.

## Cryptographic provider

Use the maintained OpenSSL 3.x libcrypto API for the server's cryptographic operations. Do not implement cryptographic algorithms in CustomDB.

- Password verifier: PBKDF2-HMAC-SHA-256 with a unique 16-byte random salt and a versioned work factor. Start at 600,000 iterations and make the value configurable only through a bounded server-side policy.
- Randomness: OpenSSL's cryptographically secure random generator (`RAND_bytes`) for salts and opaque bearer tokens.
- Token persistence: store only SHA-256 digests of random 256-bit tokens; compare digests with OpenSSL's constant-time comparison primitive.
- Password verification: derive into a fixed-size buffer and compare in constant time.
- Persist algorithm identifier, parameters, salt, and verifier so parameters can be upgraded without silently invalidating accounts.
- Never log passwords, raw tokens, authorization headers, salts, or verifiers.

Argon2id remains the preferred future KDF if a maintained, portable implementation can be added without compromising the supported build matrix. PBKDF2 is the explicit portable first implementation, not a custom substitute.

## Build and portability

- Make OpenSSL a required dependency only when `CUSTOM_DB_ENGINE_BUILD_SERVER=ON`; the embedded library must remain dependency-light.
- Require OpenSSL 3.x and fail CMake configuration with a clear diagnostic when it is unavailable. Do not silently fall back to weak or homemade cryptography.
- CI must install or provision the supported OpenSSL development package on Ubuntu, macOS, and Windows, and verify the minimum supported version.
- Keep crypto calls behind a small server-only interface so a future provider change does not leak into the storage engine API.

## Identity store

- Keep users, grants, and token metadata in a dedicated auth store under the configured data directory, separate from database snapshots and backups.
- Use a versioned JSON format initially, with strict parsing, bounded record sizes, and validation of every field.
- Write changes to a sibling temporary file, flush and close it, then atomically replace the active file. Preserve the previous valid file if replacement fails.
- Restrict file permissions to the server account where the platform supports POSIX permission bits. Document the Windows ACL responsibility explicitly; do not imply POSIX mode bits provide Windows ACL isolation.
- Serialize auth-store mutations under a dedicated mutex. Do not hold the engine mutex while deriving passwords.
- Database backup and restore must never include or roll back identity state.

## Bootstrap and API behavior

- Bootstrap the first administrator with a one-time local CLI command. The command must refuse to overwrite an existing identity store and must not print the password or token.
- No default credentials, network bootstrap endpoint, or automatic administrator creation.
- Use `Authorization: Bearer <token>`; reject query-string credentials and do not accept the legacy shared API key as a fallback once token auth is enabled.
- Keep `GET /health` public and minimal. All other API routes must authenticate and authorize before opening a database or revealing whether it exists.
- Use `401` for missing/invalid credentials and `403` for authenticated users without the required grant.
- Non-loopback binding remains a separate explicit operator decision. Authentication alone does not make plaintext HTTP safe for untrusted networks.

## Authorization policy

- `admin`: global user/grant administration and database administration.
- `read_write`: read and mutate only databases explicitly granted to the user.
- `read_only`: read only databases explicitly granted to the user.
- Database creation, backup restore, and user/grant administration are administrative operations. Restore must never be granted by the ordinary read-write role.
- Unknown routes and methods must fail closed rather than inherit a permissive default.

## Implementation gates

1. Add and verify the OpenSSL 3.x server-only dependency across the complete CI matrix.
2. Implement and unit-test crypto wrappers, auth-store parsing, atomic persistence, file permissions, and one-time administrator bootstrap.
3. Implement password login, opaque token issuance, expiry, revocation, and disabled-user handling.
4. Add centralized authentication and authorization; remove per-route shared-key checks only after every route is covered.
5. Add route-by-role and cross-database authorization tests, including unknown routes, malformed headers, expired/revoked tokens, and restore restrictions.
6. Update operational documentation and complete a security review before describing the server as suitable for anything beyond trusted local development.

## Current implementation progress

- Implemented: PBKDF2-HMAC-SHA-256 password verifiers, random bearer-token generation, SHA-256 token digests, and constant-time verification.
- Implemented: version-1 JSON identity store with bounded record counts and file size, username/role/grant validation, first-account administrator restriction, disabled-user handling, token expiry metadata, revocation, POSIX file mode `0600`, and atomic replacement.
- Implemented: hidden-terminal, one-time `--bootstrap-admin` CLI; passwords are confirmed interactively and are never accepted as command-line arguments.
- Implemented: unit tests for password/token primitives and identity persistence, first-admin bootstrap, duplicate users, token expiry/revocation, and disabled accounts.
- Not implemented: authentication HTTP endpoints, bearer-token middleware, per-route authorization, rate limiting, audit events, or full crash/power-loss durability guarantees. Windows filesystem ACLs remain the operator's responsibility.

## Explicit non-claims

This decision record is not an implementation, security certification, or production-readiness statement. Until all implementation gates pass, the server continues to use its existing shared API-key behavior and loopback-first defaults.
