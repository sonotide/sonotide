# Sonotide

Sonotide is an independent C++20 audio framework for Windows. It wraps the
low-level WASAPI layer and exposes a compact public API for device
enumeration, render/capture/loopback streams, and higher-level playback
sessions built on Media Foundation and the built-in equalizer.

The repository is intentionally framework-focused rather than app-specific. It
contains the library itself, runnable examples, unit tests, and framework
documentation. That makes Sonotide easy to reuse across multiple projects
without copying the same Windows-specific audio layer around.

## Features

- enumerate audio devices and query the system default device;
- open shared-mode WASAPI render streams with an event-driven callback model;
- open shared-mode WASAPI capture streams;
- open shared-mode loopback capture streams;
- build playback sessions on top of `runtime` and the decoder pipeline;
- load and decode media sources through Media Foundation on Windows;
- apply the built-in equalizer with a dynamic band count up to 10, per-band `Q`, presets, and output headroom compensation;
- sample the exact steady-state EQ response curve for UI rendering or diagnostics instead of approximating it on the frontend;
- preview draft EQ layouts through `playback_session::preview_equalizer_response(...)` without mutating the live session state;
- expose playback-state snapshots, including the negotiated format and active device;
- use an explicit error model through `sonotide::result<T>`.

On non-Windows hosts the project still configures and builds, but `runtime`
uses the stub backend and returns `unsupported_platform`.

## Repository layout

- `include/sonotide/` - public framework API;
- `src/` - runtime, playback, and internal backend implementation;
- `examples/` - small programs that demonstrate common usage patterns;
- `tests/` - unit tests for platform-neutral logic;
- `docs/` - architecture notes, API docs, migration notes, and design rationale;
- `cmake/` - packaging helpers and CMake support files.

## Build

The recommended development preset for Windows and WSL is
`msvc-x64-debug`. For a build that is closer to production output, use the
matching `msvc-x64-release` preset.

At the moment Sonotide is intentionally built only as a static library.
`BUILD_SHARED_LIBS` is not used on purpose so the project cannot be
accidentally switched to a DLL build before the public API has proper
export/import support.

If `cmake` and `ctest` are already available in `PATH`, use:

```bash
cmake --preset msvc-x64-debug
cmake --build --preset msvc-x64-debug
cmake --install build/msvc-x64-debug --config Debug
ctest --preset msvc-x64-debug
```

For `Release`, use the same flow with the release preset:

```bash
cmake --preset msvc-x64-release
cmake --build --preset msvc-x64-release
cmake --install build/msvc-x64-release --config Release
ctest --preset msvc-x64-release
```

If you launch `ctest` directly against a Visual Studio build directory, pass
the configuration explicitly:

```bash
ctest --test-dir build/msvc-x64-debug -C Debug
```

For single-config and CI scenarios the repository also provides
`ci-ninja-debug` and `ci-ninja-release`. GitHub Actions runs both variants,
executes the tests, and verifies `cmake --install` for the install tree:

```bash
cmake --preset ci-ninja-debug
cmake --build --preset ci-ninja-debug
ctest --preset ci-ninja-debug
```

If `cmake` is not available in `PATH`, invoke the CMake binary provided by
your Visual Studio or local toolchain installation instead of hardcoding a
workspace-specific path in scripts or documentation.

## Releases

Releases are published from git tags that match `v*`, for example `v0.3.0`.
A dedicated workflow builds the `Release` configuration, runs the tests,
executes `cmake --install`, validates the installed package through a minimal
consumer project, and automatically creates a GitHub Release with SDK assets
like:

```text
sonotide-0.3.0-windows-msvc-x64-release.zip
sonotide-0.3.0-windows-msvc-x64-release.zip.sha256
```

The archive contains the install tree. Users can unpack it and consume the SDK
through `find_package(Sonotide CONFIG REQUIRED)`. The adjacent `sha256` file is
published as an integrity check for the downloaded archive.

## Quick example

The snippet below opens a render stream and fills it with silence. It is not a
complete player; it is just the smallest useful smoke test for `runtime` and
the render path.

```cpp
#include <algorithm>

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
    if (!runtime_result) {
        return 1;
    }

    sonotide::runtime runtime = std::move(runtime_result.value());
    silence_callback callback;

    auto stream_result = runtime.open_render_stream({}, callback);
    if (!stream_result) {
        return 1;
    }

    auto start_result = stream_result.value().start();
    return start_result ? 0 : 1;
}
```

For a playback-oriented scenario, see `examples/playback_session.cpp`. It
shows how the framework opens a file, builds a playback session, and manages
transport state without tying that logic to any specific application.

## Design principles

- the public API should not expose raw COM types;
- object lifetimes should remain predictable and explicit;
- errors should be returned explicitly, without exceptions in the hot path;
- the backend should stay close to the real WASAPI model instead of hiding it completely;
- the playback layer should not absorb application-specific domain logic.

More detail is available in [docs](https://sonotide.mintlify.app/en).
