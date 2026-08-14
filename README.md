# Sonotide

Sonotide is a C++20 audio framework for Windows. It provides a compact API over
WASAPI for render, capture, and loopback streams, plus a higher-level playback
session built on Media Foundation and a configurable equalizer.

Version `0.4.1` is a public beta. It is suitable for evaluation, prototypes,
desktop applications, and early integrations, but its API and ABI may still
change between minor `0.x` releases.

## Highlights

- enumerate audio endpoints and resolve system-default devices;
- open event-driven shared-mode render, capture, and loopback streams;
- load and play media sources through Media Foundation;
- apply up to 10 configurable peaking-EQ bands with output headroom control;
- sample the exact EQ response for UI rendering and preview draft settings;
- inspect negotiated formats, stream statistics, playback state, and device loss;
- handle failures explicitly through `sonotide::result<T>`;
- consume the installed SDK through `find_package(Sonotide CONFIG REQUIRED)`.

The real backend targets Windows. Other platforms build a stub backend whose
runtime operations return `unsupported_platform`.

## Requirements

For the official Windows x64 SDK:

- a 64-bit Windows application;
- a C++20-capable MSVC toolchain and the Microsoft C++ runtime;
- CMake 3.24 or newer when consuming the CMake package;
- Media Foundation for high-level playback.

The packaged release is a static Release library built with the dynamic MSVC
runtime (`/MD`). Build consumers with a compatible MSVC toolset, architecture,
configuration, and runtime library. Debug and Release static libraries must not
be mixed.

## Use a release SDK

1. Download the `.zip` and adjacent `.zip.sha256` from the GitHub release.
2. Verify the download:

   ```powershell
   $expected = (Get-Content .\sonotide-0.4.1-windows-msvc-x64-release.zip.sha256).Split()[0]
   $actual = (Get-FileHash .\sonotide-0.4.1-windows-msvc-x64-release.zip -Algorithm SHA256).Hash.ToLower()
   if ($actual -ne $expected) { throw "Sonotide archive checksum mismatch" }
   ```

3. Extract the archive and point CMake at its root:

   ```powershell
   cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\sdk\sonotide-0.4.1-windows-msvc-x64-release
   cmake --build build --config Release
   ```

4. Link the imported target from your project:

   ```cmake
   find_package(Sonotide 0.4 CONFIG REQUIRED)
   target_link_libraries(your_app PRIVATE Sonotide::sonotide)
   target_compile_features(your_app PRIVATE cxx_std_20)
   ```

The archive also contains `SHA256SUMS.txt`, which lists hashes for individual
SDK files, and `share/Sonotide/VERSION.txt`.

## Build from source

Open an x64 MSVC Developer Command Prompt. The portable public and CI path uses
Ninja with the `ci-ninja-debug` and `ci-ninja-release` presets:

```powershell
cmake --preset ci-ninja-release --fresh
cmake --build --preset ci-ninja-release
ctest --preset ci-ninja-release
cmake --install build/ci-ninja-release
```

The `msvc-x64-debug` and `msvc-x64-release` convenience presets intentionally
name the Visual Studio 18 / Visual Studio 2026 generator used by this repository's
development environment. They are not the portable choice for machines with a
different Visual Studio generator. On those machines, use the Ninja flow above
or configure a separate local build directory with an installed generator.

If CMake reports a compiler path from an old Visual Studio installation, rerun
configuration with `--fresh`. Ensure CMake, Ninja (for Ninja presets), and the
MSVC toolchain are available in `PATH`; reopening the terminal or development
environment may be necessary after changing `PATH`.

Useful project options:

| Option | Default | Purpose |
| --- | ---: | --- |
| `SONOTIDE_BUILD_EXAMPLES` | `ON` | Build runnable examples. |
| `SONOTIDE_BUILD_TESTS` | `ON` | Build Sonotide tests when `BUILD_TESTING` is enabled. |
| `SONOTIDE_WARNINGS_AS_ERRORS` | `OFF` | Treat library warnings as build failures. |
| `SONOTIDE_ENABLE_ADDRESS_SANITIZER` | `OFF` | Instrument MSVC library, example, and test targets with AddressSanitizer. |

Sonotide intentionally builds only as a static library. `BUILD_SHARED_LIBS` is
ignored until a supported DLL ABI and export/import surface are introduced.

## Minimal render example

The following opens a shared-mode render stream, supplies silence for a short
period, and closes the stream while the callback is still alive:

```cpp
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <thread>

#include "sonotide/runtime.h"

class silence_callback final : public sonotide::render_callback {
public:
    sonotide::result<void> on_render(
        sonotide::audio_buffer_view buffer,
        sonotide::stream_timestamp) override {
        std::fill(buffer.bytes.begin(), buffer.bytes.end(), std::byte{0});
        return sonotide::result<void>::success();
    }
};

int main() {
    auto runtime_result = sonotide::runtime::create();
    if (!runtime_result) return 1;

    auto audio_runtime = std::move(runtime_result.value());
    silence_callback callback;
    auto stream_result = audio_runtime.open_render_stream({}, callback);
    if (!stream_result) return 1;

    auto& stream = stream_result.value();
    if (!stream.start()) return 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return stream.close() ? 0 : 1;
}
```

See [`examples/README.md`](examples/README.md) for device enumeration, render,
and playback examples.

## Lifecycle rules

Low-level callback objects are borrowed, not owned. Keep the callback alive
until `close()` succeeds from a thread other than the audio callback. Calling
`stop()` or `close()` from inside the callback requests shutdown but returns
`invalid_state`; retry from a control thread before destroying either object.
Exceptions thrown by user callbacks are contained and reported as stream faults.

Endpoint invalidation is reported through `on_stream_error`,
`stream_status::device_lost`, and the `faulted` state. The
`auto_recover_device_loss` field is a policy hint: low-level streams do not
automatically select and open a replacement endpoint. The application must
close and reopen them. The high-level playback session owns its recovery policy.

`playback_session::close()` limits how long it waits for its decoder worker by
`decoder_shutdown_timeout_ms` (1500 ms by default; valid range 1–5000 ms).
Invalid values are rejected when the session is opened. On timeout, `close()`
returns a recoverable `operation_timed_out` error and the session is already
logically closed. A stuck synchronous Media Foundation handler may retain its
isolated worker and related resources until the handler returns or the process
exits. Call `close()` from a control thread rather than an audio callback.

## Current limitations

- The official binary package targets Windows x64 and MSVC Release consumers.
- Low-level streams currently implement shared mode and event-driven callbacks;
  `exclusive` mode is not implemented.
- `stream_timing::engine_period` is reserved for future backend support.
- Device recovery for low-level streams is application-managed.
- Sonotide is pre-1.0: minor versions may contain source or ABI changes.

## Repository layout

- `include/sonotide/` — public API;
- `src/` — platform-neutral orchestration and internal implementations;
- `src/internal/win/` — WASAPI and Media Foundation backend;
- `examples/` — runnable usage and device smoke scenarios;
- `tests/` — unit, lifecycle, and Windows validation tests;
- `packaging/smoke-consumer/` — clean installed-package consumer;
- `cmake/` — CMake package configuration.

End-user documentation is maintained separately at
[sonotide.mintlify.app](https://sonotide.mintlify.app/en); it is not stored in a
`docs/` directory in this repository.

## Releases and integrity

Tags must have the exact form `vMAJOR.MINOR.PATCH` and match both
`CMakeLists.txt` and `include/sonotide/version.h`. The release workflow builds
and tests Release, installs and relocates the SDK, compiles a clean consumer,
verifies the extracted per-file manifest, and publishes the archive with its
SHA-256 checksum.

Do not create or move a public release tag before the version bump is committed
and CI passes. If an unpublished tag triggered a failed workflow, fix the commit
and recreate the tag deliberately. If a GitHub Release or its assets were
already published, keep that tag immutable and publish a new patch version.
Transient failures can be rerun against the same unchanged tag only before a
GitHub Release is created. The workflow refuses to overwrite an existing
release or same-named assets.

Notable changes are recorded in [`CHANGELOG.md`](CHANGELOG.md). Security issues
should be reported privately according to [`SECURITY.md`](SECURITY.md).

## License

Sonotide is available under the permissive [MIT License](LICENSE). You may use,
modify, distribute, and include it in commercial or closed-source applications,
provided that the copyright and license notice remain with copies or substantial
portions of the software.

## Design principles

- keep raw COM and Windows implementation types out of public headers;
- keep object lifetimes predictable and explicit;
- return structured errors instead of throwing in normal or real-time paths;
- avoid blocking work and allocation in steady-state audio callbacks;
- keep the framework independent from application-specific domain logic.
