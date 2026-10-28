# CustomDB release gates

This checklist defines evidence required before changing the project from preview to stable. A checked box means the evidence is recorded for the exact release commit; it must not be checked merely because a workflow or test exists.

## Gate 1 — Build and package

- [ ] Linux, macOS, and Windows Debug and Release builds pass on the release commit.
- [ ] CTest passes on each supported platform/configuration.
- [ ] ASan/UBSan test job passes on the release commit.
- [ ] Install succeeds into a clean prefix.
- [ ] A separate consumer project finds and links the installed package.
- [ ] Dependency-fetch and offline/local dependency behavior are documented.
- [ ] Compiler and CMake support matrix is explicit.

## Gate 2 — Persistence and recovery

- [ ] Tests cover empty, missing, truncated, malformed, checksum-invalid, non-monotonic, and generation-mismatched WAL records.
- [ ] Tests prove incomplete final WAL records are handled without concatenating new writes onto torn bytes.
- [ ] Tests cover interrupted checkpoint publication and WAL truncation.
- [ ] Tests cover snapshot write, flush, rename/publication, and directory-sync failures where supported.
- [ ] Every recovery test checks both the returned error/status and the bytes/state after close and reopen.
- [ ] Unsupported or corrupt persisted state is never silently replaced with an empty database.
- [ ] Format compatibility fixtures cover every supported historical format.
- [ ] Backup/restore tests include invalid backups, restore followed by reopen, and post-restore integrity checks.

## Gate 3 — Transaction semantics

- [ ] Public documentation states precisely what commit and rollback guarantee.
- [ ] Tests cover commit, rollback, nested/busy transaction attempts, and failed commits.
- [ ] Tests cover process termination at relevant transaction/WAL boundaries.
- [ ] Concurrency and locking claims match tested behavior.
- [ ] No multi-process or multi-writer guarantee is claimed without a dedicated design and stress-test suite.

## Gate 4 — Security and operational behavior

- [ ] Database and lock paths are validated and tested against traversal and symlink edge cases.
- [ ] File permissions and platform-specific locking behavior are documented and tested.
- [ ] Fuzz targets cover persisted snapshot and WAL parsers, with a recorded smoke-run result.
- [ ] Long-running randomized tests are reproducible from a seed and preserve failing seeds.
- [ ] Backup and restore instructions have been followed by someone other than the author.
- [ ] Known limitations and data-loss scenarios are visible before adoption.

## Gate 5 — Performance and product usability

- [ ] Benchmark workload, record count, payload shape, hardware, OS, compiler, build type, and commit are recorded.
- [ ] Baselines include write latency percentiles, bulk reads, reopen time, snapshot size, and WAL size.
- [ ] Performance comparisons use equivalent workloads and do not claim capacity from a tiny smoke test.
- [ ] Public API examples compile as part of CI.
- [ ] Empty values and missing keys can be handled unambiguously through the documented API.
- [ ] API and on-disk format versioning policy is published.

## Gate 6 — Release and support

- [ ] Release notes describe user-visible changes, compatibility, and recovery considerations.
- [ ] Version and package metadata agree.
- [ ] License and third-party dependency notices are present.
- [ ] Changelog, support policy, and issue-reporting instructions are available.
- [ ] Release assets are generated from the tagged commit and checksums are published if binaries are distributed.
- [ ] A rollback or downgrade policy is documented for format-affecting releases.

## Release decision

- **Preview:** useful for evaluation; known limitations are explicit; no claim of production durability.
- **Release candidate:** all critical persistence, package, and CI gates have evidence; external review is invited.
- **Stable:** all applicable gates pass on the exact release commit and guarantees have been reviewed.
- **Production-critical use:** requires workload-specific validation and an independent recovery/operations review in addition to a stable release.

Do not mark a gate complete without linking to a workflow run, test output, benchmark artifact, or review record.
