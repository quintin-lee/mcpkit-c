# Changelog

Format follows Keep a Changelog. Versions follow SemVer.

## [Unreleased]

### Added

- Phase 0 bootstrap: CMake build, C23 baseline, core
  (`types` / `error` / `result` / `version`) with unit tests.
- Phase 1: pluggable allocator, logger, context, zero-dependency JSON.
- Phase 2: JSON-RPC message envelope, initialize handshake, schema
  builder/validator, three-level validation pipeline with error mapping.
- Phase 3: server registry (tool/resource/prompt), multi-session lifecycle,
  dispatch pipeline with queue and session gating, minimal-server and
  multi-session examples.
