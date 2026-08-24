# Contributing to Sonotide

Thank you for helping improve Sonotide. Contributions can include bug reports,
feature proposals, documentation corrections, tests, and code changes.

Sonotide is currently a public beta. The public API is deliberately small, so
please discuss substantial API or architectural changes in an issue before
investing in an implementation.

## Before opening an issue

- Search the existing issues to avoid duplicates.
- Use the matching issue form and include enough information to reproduce or
  understand the problem.
- Do not report security vulnerabilities in a public issue. Follow the private
  reporting instructions in [SECURITY.md](SECURITY.md) instead.
- For usage questions, check the
  [documentation](https://sonotide.mintlify.app/en) and the examples first.

## Development requirements

The production backend targets 64-bit Windows. To build and test it locally,
install:

- a C++20-capable MSVC toolchain;
- CMake 3.24 or newer;
- Ninja;
- Git.

Run commands from an x64 MSVC Developer Command Prompt. The portable local and
CI-compatible verification path is:

```powershell
cmake --preset ci-ninja-debug --fresh
cmake --build --preset ci-ninja-debug
ctest --preset ci-ninja-debug
```

For changes that may behave differently under optimization, also run:

```powershell
cmake --preset ci-ninja-release --fresh
cmake --build --preset ci-ninja-release
ctest --preset ci-ninja-release
```

## Making a change

1. Create a focused branch from the latest `main`.
2. Keep the change limited to one problem or feature.
3. Add or update tests when behavior changes.
4. Update examples and documentation when the public API or user workflow
   changes.
5. Run the relevant Debug and Release verification commands.
6. Open a pull request explaining the problem, the chosen solution, and how the
   change was tested.

## Code expectations

- Keep public headers in `include/sonotide/` concise and free from raw Windows,
  COM, WASAPI, and Media Foundation implementation types.
- Keep Windows-specific implementation code in `src/internal/win/`.
- Preserve RAII ownership and explicit object lifetimes.
- Return failures through `sonotide::result<T>` in normal control flow.
- Avoid blocking operations, exceptions, and unnecessary allocations in audio
  callbacks.
- Keep non-Windows stub builds working when changing portable code.
- Follow the style of the surrounding code and avoid unrelated formatting or
  refactoring.

## Tests and examples

Unit tests live in `tests/`, and runnable examples live in `examples/`. A pull
request that changes observable behavior should normally include a regression
test. Changes to public workflows should also update the closest example.

The project currently has no separate lint or type-check command. The CMake
presets build with project warnings treated as errors.

## Pull request checklist

Before requesting review, confirm that:

- the change has a clear purpose and no unrelated edits;
- new behavior and important failure paths are tested;
- Debug configuration builds and all tests pass;
- Release configuration was checked when relevant;
- public API, examples, changelog, and documentation were updated when needed;
- no secrets, generated build outputs, or machine-specific paths were added.

By contributing, you agree that your contribution will be licensed under the
project's [MIT License](LICENSE).
