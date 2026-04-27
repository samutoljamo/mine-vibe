# Windows CI — acceptance criteria

## Goal

Ship Windows builds and tests as a first-class CI target alongside Linux,
without regressing the Linux pipeline.

## Background

Pre-existing CI shipped Linux and Windows matrix entries, but Windows had
never produced a successful build. The breakage layered:

- The Vulkan SDK action (`humbletim/setup-vulkan-sdk`) cached only the
  install dir, not the build dir where `glslangValidator.exe` lived.
- Several headers (`reliable.h`, `net_thread.h`, `server.h`, `client.h`,
  `main.c`) included `<netinet/in.h>` / `<arpa/inet.h>` directly.
- `agent.c` used raw pthreads.
- `nanosleep` was used for short sleeps in `main.c`, `server.c`,
  `net_thread.c`.
- MSVC needs `/experimental:c11atomics` to compile `<stdatomic.h>` users.
- A handful of CMake `target_link_libraries(... m)` were unconditional.

The Vulkan SDK and most porting issues are addressed in commits
`4604e36`, `8ceb267`, `a9311e4`. This spec defines what "done" means
so we stop iterating reactively.

## Acceptance criteria

**CI workflow (`.github/workflows/ci.yml`)**
1. Configure, build, and test all targets succeed on `windows-latest`
   for the matrix entry.
2. The same succeeds on `ubuntu-latest`.
3. Every test from the existing test list runs on both OSes:
   `block_physics`, `agent_json`, `net`, `ui`, `remote_player`, `mesher`,
   `client`. No skipping by OS unless a follow-up shows a test is
   genuinely platform-coupled.

**Release workflow (`.github/workflows/release.yml`)**
4. A `v*` tag push produces `minecraft-windows-x86_64.exe` and
   `minecraft-linux-x86_64` artifacts attached to the GitHub Release,
   and both binaries launch on their target OS.

## Explicit non-goals

- **Treat warnings as errors.** Not enabling `/WX` or `-Werror`. MSVC will
  emit benign warnings (e.g. `C4005 APIENTRY` from a GLFW/windows.h clash)
  that are not our code to fix.
- **Cache hits as a gate.** Vulkan SDK and FetchContent caches are
  performance optimizations. A cache miss must not fail CI.
- **Cross-compilation, MinGW, ARM Windows.** MSVC on x64 only.

## Out of scope (future work, not blocking)

- Windows-specific runtime smoke test (launching the binary and verifying
  it exits cleanly) — current CI only validates that tests pass and the
  release artifact builds.
- Reproducible-build verification across runners.
