# Development Setup

## Supported Platforms
- Production target: Linux (Docker)
- Development: macOS (best-effort)

## macOS (Homebrew)
- Install build tools if needed: `cmake`, `ninja` (optional).
- Configure CMake toolchain if needed.
- Build:
  - `cmake -S . -B build`
  - `cmake --build build`

## Linux (Docker)
- Use `Dockerfile` in the repo root.

## IDE
- CLion: use the default CMake profile or point to `build/`.

## Platform Flags
- Document any platform-specific toggles here.

## Dependency Strategy
- `FetchContent` for CMake-native libraries (spdlog, nlohmann/json, test framework, HTTP/WS).
- `ExternalProject` (or scripted builds) for PJSIP and ONNX Runtime to control build flags.
  - CMake options: `SIPGATEWAY_FETCH_DEPS`, `SIPGATEWAY_BUILD_PJSIP`, `SIPGATEWAY_BUILD_ONNX`.

## Dependency Versions (Initial Proposal)
- PJSIP: 2.16
- ONNX Runtime: 1.23.2
- spdlog: 1.13.0
- nlohmann/json: 3.11.3
- cpp-httplib: 0.16.0
- websocketpp: 0.8.2
- Catch2: 3.5.4
