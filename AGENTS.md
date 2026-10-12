# Repository Guidelines

## Project Structure & Module Organization

Mixer Bridge is a headless OBS Studio plugin written in C. `src/` contains plugin lifecycle (`plugin-main.c`), loudness metering (`meter.c`), audio capture (`tap.c`), JSON protocol handling (`protocol.c`), and platform servers. Windows uses `server-win.c`; macOS/Linux use `server-stub.c` and currently support builds only.

`deps/` vendors libebur128 and cJSON; preserve their licenses and avoid unrelated edits. `data/locale/` holds OBS translations. `docs/protocol.md` describes the wire protocol, and `examples/minimal-client.mjs` demonstrates it. CMake helpers live in `cmake/`; CI and packaging live in `.github/`. Generated builds use `build_*` directories.

## Build, Test, and Development Commands

Use CMake 3.28+ and Visual Studio 2022 with Desktop development with C++ on Windows. Run from the repository root:

```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
node examples/minimal-client.mjs "Microphone"
```

The first two commands configure and compile the plugin. The client requires Node.js 18+, OBS running with the plugin installed, and an optional exact source name; it lists sources and streams meters for five seconds. For Visual Studio 2026, append `-G "Visual Studio 18 2026"` to configuration. Other platforms have `macos` and `ubuntu-x86_64` configure/build presets.

## Coding Style & Naming Conventions

Follow `.clang-format`: tabs with eight-column indentation, a 120-column limit, and function opening braces on separate lines. Use `snake_case` identifiers, module prefixes such as `meter_` and `session_`, and uppercase constants. CMake uses two-space indentation via `.gersemirc`.

CI uses clang-format 19 and gersemi. On Linux/macOS with zsh and the formatters installed, check with `./build-aux/run-clang-format --check --fail-error` and `./build-aux/run-gersemi --check --fail-error`.

## Testing Guidelines

There is no automated test framework, test naming convention, or coverage threshold. Build affected platforms and smoke-test Windows behavior in OBS. For metering changes, verify silence, active audio, mute/fader effects, and disconnect/reconnect behavior. For protocol changes, exercise requests and errors described in `docs/protocol.md`; update the specification and example when behavior changes.

## Commit & Pull Request Guidelines

History uses concise imperative subjects, such as `Add loudness metering and named pipe server`; follow that pattern. Keep commits focused. PRs should explain the behavior change, link relevant issues, and record build commands, platform/OBS versions, and manual results. Include logs or protocol output when useful; this plugin has no UI requiring screenshots.
