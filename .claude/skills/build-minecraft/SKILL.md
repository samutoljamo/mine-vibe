---
name: build-minecraft
description: Use when building, configuring, or running the minecraft Vulkan project — all cmake and make commands must run inside the cyberismo distrobox, not directly on the host.
---

# Build Minecraft (cyberismo distrobox)

All build commands must be prefixed with `distrobox enter cyberismo --`. Do not run cmake or make directly on the host.

## Configure

Only needed the first time or after CMakeLists.txt changes:

```bash
distrobox enter cyberismo -- cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
```

## Build

```bash
distrobox enter cyberismo -- cmake --build build
```

Or with parallel jobs:

```bash
distrobox enter cyberismo -- make -C build -j$(nproc)
```

## Run Tests

```bash
distrobox enter cyberismo -- cmake --build build --target test
# or
distrobox enter cyberismo -- make -C build test
```

## Notes

- Generator: Unix Makefiles (ninja is not installed in the distrobox)
- Compiler: `/usr/bin/gcc`
- Vulkan headers, `glslc`, and `glslangValidator` are available in the distrobox
- The `build/` directory is gitignored; if it exists, it's already configured
