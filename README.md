# mine-vibe

A small Minecraft-like voxel game written in C11 + Vulkan. Single binary that
runs an authoritative UDP server thread plus the client/renderer; the same
server powers both singleplayer and multiplayer.

## Download / Play

Prebuilt binaries for **Linux (x86_64)** and **Windows (x86_64)** are published
automatically for every tagged release.

1. Go to the [**Releases** page](https://github.com/samutoljamo/mine-vibe/releases).
2. Download the binary for your platform from the latest release:
   - `minecraft-linux-x86_64` — Linux
   - `minecraft-windows-x86_64.exe` — Windows
3. Run it:
   - **Linux:** `chmod +x minecraft-linux-x86_64 && ./minecraft-linux-x86_64`
   - **Windows:** double-click `minecraft-windows-x86_64.exe` (or run it from a terminal).

Builds are also produced on every push/PR by CI and attached to each run as
**artifacts** (visible under the Actions tab) if you want a bleeding-edge build.

### Runtime requirements

- A **Vulkan-capable GPU** with up-to-date graphics drivers (Vulkan 1.1+).
  Most GPUs from the last decade qualify; make sure your drivers are current.
- **Linux:** the Vulkan loader must be installed (`libvulkan.so.1`). Install it
  with your distro's package manager if it is missing:
  - Debian/Ubuntu: `sudo apt install libvulkan1`
  - Fedora: `sudo dnf install vulkan-loader`
  - Arch: `sudo pacman -S vulkan-icd-loader`
  You also need an audio device for sound, but the game falls back to silent
  audio if none is present.
- **Windows:** the Vulkan runtime ships with modern GPU drivers — keep your
  graphics drivers updated. No separate install is normally needed.

### Common launch flags

```
--render-distance N    # chunk render distance (default 12)
--msaa 1|2|4           # multisampling (default 1 = off)
--aniso N              # anisotropic filtering (default 4)
--stats                # performance overlay
--host                 # host a server (default; singleplayer is host+local client)
--client <ip>          # connect to a remote host
--server               # dedicated (headless) server
```

## Multiplayer

The same authoritative server powers singleplayer and multiplayer. One player
**hosts** and others **join**; the host streams the world to each client
(server-authoritative — clients don't generate terrain themselves).

```
# Host (machine A):
./minecraft --host

# Join (machine B), pointing at A's address:
./minecraft --client <A-ip-address>

# Or run a dedicated headless server:
./minecraft --server
```

Default UDP port: **25565**.

### Over a LAN

Use the host's local IP (e.g. `192.168.1.x`): `./minecraft --client 192.168.1.42`.
Make sure the host's firewall allows inbound UDP on port 25565.

### Over the internet

The netcode is plain UDP/IPv4, so it works over the internet — but a host on a
typical home connection is behind NAT, so one of these is needed:

- **Easiest — a mesh VPN (recommended):** install
  [Tailscale](https://tailscale.com/) or [ZeroTier](https://www.zerotier.com/)
  on both machines, then connect to the host's VPN IP:
  `./minecraft --client 100.x.y.z`. The VPN handles NAT traversal and encrypts
  traffic — no router config, behaves just like LAN.
- **Manual port-forwarding:** forward **UDP 25565** on the host's router to the
  host machine, then have others connect to the host's **public IPv4** address.

**Current limitations** (on the roadmap): `--client` takes a literal IPv4
address only (no hostnames/DNS or IPv6 yet), there's no automatic NAT
traversal/UPnP, and traffic is unencrypted and unauthenticated — fine for
playing with friends, not for exposing a server on a hostile network.

## Building from source

All `cmake`/`make` commands must run inside the `cyberismo` distrobox (it has
the Vulkan headers, `glslangValidator`/`glslc`, and toolchain).

```bash
# Configure (defaults to Release)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
# Build
cmake --build build
# Run the unit tests
ctest --test-dir build --output-on-failure
# Run the game
./build/minecraft
```

Dependencies (GLFW, volk, VMA, cglm, stb, FastNoiseLite, miniaudio) are fetched
automatically via CMake `FetchContent`. The Vulkan SDK (headers +
`glslangValidator`) must be installed on the system — shaders are compiled at
build time.

## Continuous integration

- **CI** (`.github/workflows/ci.yml`): every push and pull request is built and
  unit-tested on both `ubuntu-latest` and `windows-latest`. The unit tests are
  pure logic and do not require a GPU or display, so they run in headless CI;
  the game itself is not launched there.
- **Release** (`.github/workflows/release.yml`): pushing a tag matching `v*`
  builds release binaries on both OSes and publishes a GitHub Release with the
  Linux and Windows binaries attached.
