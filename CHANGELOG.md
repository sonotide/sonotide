# Changelog

All notable changes to Sonotide are documented here. The project follows
[Semantic Versioning](https://semver.org/); while the major version is `0`, a
minor release may contain API or ABI changes.

## [0.4.1] - 2026-08-14

### Changed

- Bounded playback decoder shutdown with a validated 1–5000 ms timeout and a
  recoverable `operation_timed_out` result when isolated system work outlives it.
- Added MSVC AddressSanitizer CI coverage and warning-free build gates.
- Added package version metadata, all-public-header consumer compilation, an
  archive file manifest, and extracted-archive integrity verification.
- Made invalid `result<void>::error()` access fail deterministically with
  `std::bad_optional_access`, consistent with checked standard-library access,
  instead of dereferencing an empty optional.
- Reworked the README around installation, lifecycle rules, compatibility, and
  current pre-1.0 limitations.
- Added this changelog and a private vulnerability-reporting policy.

## [0.4.0] - 2026-08-14

### Changed

- Moved decode work out of the real-time render callback and introduced a bounded decoded-audio queue.
- Hardened stream, callback, decoder, and device-loss lifecycle handling.
- Made stream wrappers safe under concurrent status, move, and close operations.
- Added strict numeric, audio-format, timestamp, and buffer-size validation.
- Strengthened Windows Debug and Release CI and installed-package relocation checks.

### Security

- Contained callback exceptions before they can escape an audio worker thread.
- Removed blocking synchronization and dynamic allocation from steady-state audio callbacks.

[0.4.1]: https://github.com/sonotide/sonotide/releases/tag/v0.4.1
[0.4.0]: https://github.com/sonotide/sonotide/releases/tag/v0.4.0
