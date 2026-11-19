# Security and test-readiness acceptance checklist

This checklist is a release gate, not a statement that the current implementation has passed every item. Record evidence and an owner for each item before changing the status to accepted.

## Automated checks currently covered

- [x] Cross-platform CI: Ubuntu, macOS, and Windows Debug/Release.
- [x] AddressSanitizer and UndefinedBehaviorSanitizer job.
- [x] Authentication and authorization matrix: invalid/missing tokens, role and database grants, revocation, disabled accounts, password changes, and login throttling.
- [x] Exclusive data-directory process lock.
- [x] HTTP integration: malformed requests, bounded operations, backup/restore, concurrent request correctness, signal-driven graceful shutdown on POSIX/Windows, restart persistence, and integrity checks.
- [x] Storage fault injection: partial WAL append, checkpoint cleanup, snapshot publication, abandoned temporary snapshot recovery, backup/restore publication, and auth-store replacement.
- [x] OpenAPI structural contract validation and benchmark smoke tests.

## Required before controlled external testing

- [ ] Run the documented staging recovery drill on a clean host and retain the results, including restore duration, record-count comparison, sample reads, and restart verification.
- [ ] Perform a manual API security review against the deployed build, including authorization on every route, path traversal, malformed/oversized bodies, credential leakage, and error handling.
- [ ] Verify service-account ownership and least-privilege permissions on the complete data directory, backups, auth store, and audit log. Windows ACL hardening is not currently automatic.
- [ ] Verify reverse-proxy TLS, firewall rules, request limits, trusted forwarding headers, and secret handling in the actual deployment environment. The server itself does not provide TLS.
- [ ] Verify the actual service manager's stop signal/control event, timeout, and restart on each target deployment platform. CI covers POSIX SIGTERM and Windows console CTRL_BREAK against an idle server; it does not establish service-manager integration or shutdown behavior under long-running requests.
- [ ] Capture repeatable benchmark results on the intended hardware and workload. CI smoke benchmarks are not capacity or latency guarantees.
- [ ] Review backup retention, off-host encrypted copies, auth-store recovery, audit-log retention, disk exhaustion response, and incident procedures.

## Known product constraints

- Single host and one server process per data directory; operations are serialized.
- No distributed coordination, replication, failover, high availability, or independent multi-client transaction isolation.
- No native TLS; remote access requires a correctly configured HTTPS reverse proxy.
- Audit events are local, bounded, best-effort, and not tamper-evident or remotely delivered.
- Login throttling is in-memory and per remote address; it is not a distributed abuse-control system.
- Windows file ACLs must be configured and verified by the operator.
- Automated fault injection cannot prove behavior under real power loss, defective storage hardware, or every filesystem implementation.

## Release decision

Do not describe CustomDB as production-ready solely because CI is green. Until every required item above has evidence, restrict use to development, evaluation, and controlled non-critical testing. Keep critical or sensitive workloads on established storage systems with independently reviewed backup and security controls.
