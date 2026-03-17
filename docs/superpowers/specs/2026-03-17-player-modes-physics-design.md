# Player Modes & Physics System

## Overview

Two player modes — **free-fly** and **walking** — sharing a common AABB collision system. Toggle between them by double-tapping space. A new `player.{h,c}` module owns the player state and delegates to the camera for view computation. A new `physics.{h,c}` module handles collision detection and resolution against the voxel world.

## Player Entity

A `Player` struct wraps the camera and adds physics state.

### Structure

```c
typedef enum PlayerMode {
    MODE_WALKING,
    MODE_FREE,
} PlayerMode;

typedef struct Player {
    Camera      camera;         // View computation (yaw, pitch, fov)
    vec3        position;       // Feet position in world space
    vec3        velocity;       // Current velocity
    PlayerMode  mode;
    bool        on_ground;
    bool        in_water;
    bool        sprinting;
    bool        noclip;         // Free mode collision toggle
    float       last_space_time; // For double-tap detection
    float       accumulator;    // Fixed-timestep dt accumulator
} Player;
```

### AABB Definition

The player AABB is defined relative to `position` (feet):
- `aabb_min = (position.x - 0.3, position.y, position.z - 0.3)`
- `aabb_max = (position.x + 0.3, position.y + 1.8, position.z + 0.3)`

### Constants

| Property | Value |
|----------|-------|
| Hitbox width | 0.6 blocks |
| Hitbox height | 1.8 blocks |
| Hitbox depth | 0.6 blocks |
| Eye height | 1.62 blocks from feet |
| Walk speed | 4.3 blocks/s |
| Sprint speed | 5.6 blocks/s |
| Swim speed | 2.0 blocks/s |
| Free-fly speed | 20.0 blocks/s (current camera speed) |
| Gravity | 25.2 blocks/s² downward |
| Jump velocity | 7.95 blocks/s upward (yields ~1.25 block jump height) |
| Terminal velocity | 78.4 blocks/s downward |
| Water sink rate | 2.0 blocks/s downward |
| Double-tap window | 300ms |

### Camera Relationship

The camera no longer owns its position. The player passes an eye position to the camera's view function. The camera retains ownership of yaw, pitch, fov, sensitivity, and mouse handling.

New camera signatures:
```c
void camera_get_view(Camera* cam, vec3 eye_pos, mat4 out);  // eye_pos provided externally
void camera_get_front(Camera* cam, vec3 out);                // unchanged
void camera_get_proj(Camera* cam, float aspect, mat4 out);   // unchanged
```

The `position`, `speed` fields, and `camera_process_keyboard()` are removed from Camera. `camera_init` simplifies to take only the `Camera*` pointer (sets default yaw, pitch, fov, sensitivity).

### World Pointer

`player_update` receives the `World*` as a parameter so the physics system can query blocks:

```c
void player_update(Player* player, GLFWwindow* window, World* world, float dt);
```

The player does not store a `World*` member — it passes it through to physics functions.

## Physics System

### Fixed Timestep

Physics runs at a fixed 60Hz tick rate using a dt accumulator:

```c
accumulator += frame_dt;
accumulator = fminf(accumulator, 0.05f);  // clamp to prevent spiral of death
while (accumulator >= PHYSICS_DT) {
    physics_tick(player, world, PHYSICS_DT);
    accumulator -= PHYSICS_DT;
}
```

Where `PHYSICS_DT = 1.0f / 60.0f`. All physics values (gravity, drag, velocity) are tuned for this fixed rate. The accumulator lives in the Player struct.

### Collision Detection (AABB vs Voxel World)

The collision system resolves player movement against the voxel world using axis-separated sweep:

1. **Decompose** movement delta into X, Y, Z components
2. **For each axis** (Y first for ground detection, then X, then Z):
   a. Compute the expanded AABB (player AABB extended by the delta on this axis)
   b. Find all block positions overlapping the expanded AABB
   c. For each overlapping block, check if it is solid via `block_defs[block_id].is_solid`
   d. If solid blocks are found, clamp the player position to the nearest block face on this axis
   e. Zero the velocity component on this axis if clamped
3. **Set `on_ground`** to true if the Y-axis resolution clamped downward movement (something below feet)

Y is resolved first so that `on_ground` is accurate before horizontal movement is processed.

### Block Query

A new helper function on the world module:

```c
BlockID world_get_block(World* world, int x, int y, int z);
```

Implementation lives in `world.c` where the opaque `World` struct is defined. `physics.c` calls it through the public API only.

Coordinate conversion must handle negative world coordinates correctly using floor-division:

```c
int cx = (int)floorf((float)x / 16.0f);
int cz = (int)floorf((float)z / 16.0f);
int lx = ((x % 16) + 16) % 16;
int lz = ((z % 16) + 16) % 16;
```

Returns `BLOCK_AIR` for unloaded chunks or out-of-range Y values (Y < 0 or Y >= 256).

### Water Detection

After collision resolution, check all blocks overlapping the player's AABB. If any block is `BLOCK_WATER`, set `in_water = true`. No distinction between partial and full submersion for now — any overlap counts.

### Friction and Drag

Horizontal velocity is damped each physics tick (fixed 60Hz) by multiplying by a drag factor:

| Surface | Drag factor (per tick at 60Hz) |
|---------|-------------------------------|
| Ground | 0.85 |
| Air | 0.98 |
| Water | 0.80 |

Applied as `velocity.xz *= drag_factor` once per fixed-timestep tick. Because the tick rate is fixed, these values are framerate-independent.

## Walking Mode

### Input Handling

- **W/S**: Forward/backward along look direction (projected onto XZ plane)
- **A/D**: Strafe left/right
- **Space**: Jump (if `on_ground`), swim up (if `in_water`)
- **Ctrl held + W**: Sprint (`sprinting = true`, speed becomes 5.6 b/s)
- Sprint deactivates when Ctrl is released or player stops moving forward

### Movement Model (Snap-to-Speed)

This uses a snap-to-speed model, not acceleration. While input is held, horizontal velocity is set directly to the target speed in the desired direction. Drag only takes effect when keys are released (velocity decays toward zero). This matches the responsive feel of Minecraft movement.

The desired direction vector must be normalized before scaling by speed to prevent faster diagonal movement.

### Per-Tick Update (60Hz Fixed)

1. Read input, compute desired movement direction on XZ plane (normalized)
2. If input is held: set horizontal velocity to `direction * speed` (walk/sprint speed, halved if `in_water`)
3. If not `in_water`: apply gravity `velocity.y -= 25.2 * PHYSICS_DT` (if not `on_ground`)
4. Clamp `velocity.y` to terminal velocity: `velocity.y = max(velocity.y, -78.4)`
5. If `in_water`:
   - Damp vertical velocity: `velocity.y *= 0.8` (prevents carrying high fall speed into water)
   - Apply sink: `velocity.y -= 2.0 * PHYSICS_DT`
   - If space held: `velocity.y = 4.0` (swim up)
6. If space pressed and `on_ground` and not `in_water`: set `velocity.y = 7.95`
7. Compute delta: `delta = velocity * PHYSICS_DT`
8. Run collision resolution (axis-separated sweep)
9. Update `on_ground` and `in_water` flags (note: these reflect previous tick's state during steps 3-6 — the one-tick lag at 60Hz is imperceptible)
10. If no input held: apply friction/drag to horizontal velocity
11. Compute eye position: `eye = position + (0, 1.62, 0)`

## Free Mode

### Input Handling

- **W/S**: Move along look direction (full 3D, including vertical component)
- **A/D**: Strafe left/right
- **Space**: Move up
- **Left Shift**: Move down
- **V**: Toggle noclip (collision on/off)

Movement direction is normalized before scaling.

### Per-Tick Update (60Hz Fixed)

1. Read input, compute desired 3D movement direction (normalized)
2. Set velocity to free-fly speed (20 b/s) in desired direction. Zero if no input.
3. Compute delta: `delta = velocity * PHYSICS_DT`
4. If noclip is off: run collision resolution
5. If noclip is on: apply delta directly
6. No gravity, no friction
7. Compute eye position: `eye = position + (0, 1.62, 0)`

Note: The 1.62 eye offset in free mode means the collision hitbox extends below the camera. This is intentional — it keeps the player's perspective consistent across mode switches and prevents jarring position jumps when toggling modes.

## Mode Switching

Double-tap space within 300ms toggles between modes:

- **Walking to Free**: Preserve position, zero velocity, enable flight. Noclip defaults to on.
- **Free to Walking**: Preserve position, zero velocity, gravity takes effect immediately.

Detection logic:
1. On space key-down, check: if `current_time - last_space_time < 0.3`, toggle mode and reset `last_space_time = 0` (prevents triple-tap re-triggering)
2. Otherwise, set `last_space_time = current_time`

In walking mode, the first space tap triggers a jump (if on ground). The mode switch happens on the second tap, so the player jumps and then enters free-fly — this is intentional and feels natural.

## File Changes

### New Files

- **`src/player.h`**: Player struct definition, function declarations
- **`src/player.c`**: Player initialization, input processing, fixed-timestep loop, mode switching
- **`src/physics.h`**: Collision resolution function declarations
- **`src/physics.c`**: AABB vs world collision, water detection

### Modified Files

- **`src/camera.h/c`**: Remove `position` field, `speed` field, and `camera_process_keyboard()`. Change `camera_get_view` to take `vec3 eye_pos` parameter. Camera becomes a view-only module.
- **`src/world.h/c`**: Add `world_get_block(World* world, int x, int y, int z)` public function. Implementation in `world.c` using floor-division for negative coordinates.
- **`src/main.c`**: Replace `Camera g_camera` with `Player g_player`. Route keyboard input through `player_update()`. Pass `g_player.camera` and eye position to rendering.

### Unchanged

- Chunk system (`chunk.h/c`, `chunk_map.h/c`, `chunk_mesh.h/c`)
- Meshing (`mesher.h/c`)
- Rendering (`renderer.h/c`, `pipeline.h/c`, `swapchain.h/c`)
- World generation (`worldgen.h/c`)
- Block definitions (`block.h/c`) — `is_solid` already exists
- Shaders (`block.vert`, `block.frag`)
- Texture system (`texture.h/c`)
- Frustum culling (`frustum.h/c`)

## Out of Scope

- Crouch/sneak mechanic (Left Shift edge-cling) — deferred to future work
- Underwater visual effects
- Fall damage
- Head-in-water vs feet-in-water distinction
