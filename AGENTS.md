# Repository Guidelines

## Project Structure & Module Organization
- `src/` holds C++ sources for the new service entry point and core modules.
- `include/` contains public headers for the C++ service.
- `python/` contains the legacy Python SIP implementation used for parity and API reference.
- `docs/` contains migration and contract documentation.
- `CMakeLists.txt` defines the CMake project and targets (C++17).
- `migration.md` is the design/migration plan for the intended SIP gateway microservice.

## Build, Test, and Development Commands
- `cmake -S . -B build` configures a fresh out-of-source build directory.
- `cmake --build build` compiles the `sip_gateway` executable.
- `./build/sip_gateway` runs the built binary locally.
- If using CLion, the default run target is `cmake-build-debug/sip_gateway`.

## Coding Style & Naming Conventions
- Use C++17 features only (project standard).
- Follow 4-space indentation and braces on the same line as control statements.
- Keep names descriptive: `snake_case` for files and `lowerCamelCase` for variables/functions unless project conventions change.
- No formatter or linter is configured yet; keep diffs tidy and consistent with existing style in `main.cpp`.
- Code and comments must be in English only.
- Add comments only when necessary; focus on why, not what.
- Do not use emojis.
- Keep code as compact as possible without harming clarity.
- Use meaningful identifiers; avoid cryptic abbreviations.

## Testing Guidelines
- No automated tests are wired up yet.
- When adding tests, create a `tests/` directory and use `test_*.cpp` filenames (e.g., `tests/test_vad.cpp`).
- Prefer a single test framework (Catch2 or GoogleTest) and document the setup in this file.

## Commit & Pull Request Guidelines
- No Git history is available in this working tree, so no established commit convention is detectable.
- Use concise, imperative commit messages (e.g., "Add SIP config loader").
- PRs should include a clear description, the motivation for the change, and any local commands run (build/test).

## Configuration & Environment
- The long-term plan in `migration.md` expects environment-driven configuration; keep new config values centralized and documented there.
- Avoid hard-coding paths or secrets; prefer environment variables and document defaults.
