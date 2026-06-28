# mine-vibe Roadmap

**Date:** 2026-06-28
**Status:** approved (brainstorming) — source of truth for the beads epic/task hierarchy

## Context

mine-vibe is a ~61k-LOC C11 + Vulkan voxel game. The foundational milestone is
done: rendering, chunk streaming, mobs, villages, ores, lighting, day/night,
audio, persistence, integrated-server multiplayer, HUD. Most P0/P1 bugs are
closed. This roadmap takes it from "engine + scattered features" to a deep,
explorable, shippable survival game.

Key insight from code review: the survival *items* are already scaffolded
(`item.h` defines tool tiers wood/stone/iron × pickaxe/axe/shovel, armor sets
leather/iron, food items, crafting materials), but most *mechanics* are not
wired (eating, durability decrement, armor damage reduction, mining-speed
gating, smelting). Phase 1 is therefore mostly "wire up + fill gaps + extend",
not "build from scratch".

## Decisions

- **Themes (all in scope):** survival depth, world richness, tech/netcode, polish/ship.
- **First milestone:** Phase 1 — Survival progression.
- **Dimensions (Nether/End):** backlogged, not active.
- **Granularity:** epic + agent-sized tasks (one parallel worktree-agent each;
  TDD pure module + test where logic-bearing, per `tests/test_ore.c` template).
- **Depth:** all 4 phases fleshed out now; backlog captured as epic stubs.

## Conventions every task inherits

- Logic-bearing tasks: pure module + `tests/test_*.c` wired as a CMake target (TDD).
- New blocks: enum in `block.h`, `BlockDef` in `block.c`, `draw_*` + tile in
  `tools/gen_assets.py`, regenerate `assets_generated.c`.
- State changes are server-authoritative; new packets use `net.h` helpers and
  bump `NET_PROTOCOL_VERSION` (currently 7).
- Build/test only inside the `cyberismo` distrobox.

---

## Phase 1 — Survival Progression (active milestone)

Goal: a real loop — *mine → smelt → craft better gear → fight/survive → mine deeper*.

### Epic 1.1 — Tool mechanics
- Mining-speed multiplier by tool material/kind (pure `tool_stats` + test); apply to server break-time.
- Harvest-level gating: block yields drops only with min tool tier (pure rule + test); wire to drop logic.
- Tool durability decrement + break on use (server-authoritative); inventory reflects remaining durability.
- Add **diamond** tool tier (items + atlas tiles + recipes): pickaxe/axe/shovel.
- Add **sword** tool kind across all tiers (items + atlas + base-damage values + recipes).

### Epic 1.2 — Smelting & furnaces
- `BLOCK_FURNACE` block def + atlas tiles (front/lit/side/top) + gen_assets.
- Furnace container state model: input/fuel/output slots (pure + test).
- Smelting recipe table (pure + test): ore→ingot, raw→cooked food, sand→glass, cobble→stone.
- Fuel burn-value table + furnace smelting tick (server-authoritative).
- Furnace UI screen (open-on-use, slot interaction).
- Net packets for furnace open/interact/state sync (+bump protocol).
- Remove placeholder 1:1 ore→ingot craft; route smelting through furnace.

### Epic 1.3 — Combat overhaul
- Weapon damage + attack-cooldown model (pure + test).
- Mob health pools + per-type death threshold (extend `mob.c`).
- Knockback on hit (player↔mob) in physics.
- Armor damage-reduction formula (pure + test) applied to incoming damage.
- Armor durability loss on damage taken.
- Attack/hurt feedback (damage flash, hit sound, mob hurt response).

### Epic 1.4 — Survival loop completion
- **[existing n8u]** Wire food eating: consume food → restore hunger (server-side).
- Saturation model + natural health regen when fed (pure + test).
- Starvation damage when hunger empty (verify/extend `survival.c`).
- Eating use-delay + animation + sound.

### Epic 1.5 — Crafting tree fill-out
- Recipes: diamond tools, swords (all tiers), sticks (extend recipe table + test).
- Recipes: full armor sets (leather/iron/diamond) + furnace + chest.
- Crafting-table block + 3×3 grid gating (2×2 inventory vs 3×3 table). **DECISION FLAG:** keep shapeless-table or introduce shaped grid?

### Epic 1.6 — Containers (foundation for Phase 2 dungeon loot)
- `BLOCK_CHEST` block def + atlas tiles + gen_assets.
- Chest container inventory model (pure + test).
- Chest open/place/break + server-authoritative storage + net packets (+bump protocol).
- Chest UI screen.

---

## Phase 2 — World & Exploration

### Epic 2.1 — Cave systems
- 3D-noise cave carving in worldgen (pure helper + test, `ore.c`-style).
- Cave variety (worm + cheese) + entrances reaching the surface.
- Re-distribute ores into cave-aware depth bands (tune `ore.c`).
- Underground darkness/ambiance verification + cave audio cue.

### Epic 2.2 — Structures & loot
- Loot-table model (weighted drops) pure + test.
- Dungeon room generator (mob source + loot chests) in worldgen.
- Mineshaft / corridor generator.
- Surface ruin generator.
- Place loot chests via the Epic 1.6 chest container.

### Epic 2.3 — More biomes
- Biome param expansion (temp/humidity → desert/snow/forest/plains/mountains) pure + test.
- Biome-specific surface blocks + decoration (cactus, snow layer).
- Biome-gated mob/animal spawning.
- Biome blending at borders.

### Epic 2.4 — Weather
- Weather state machine (clear/rain/snow) server-authoritative + sync.
- Rain/snow particle rendering.
- Weather visual effects (sky darken; biome-aware rain vs snow).

### Epic 2.5 — World blocks
- Add blocks: sandstone, snow, ice, mossy cobble (items + atlas + gen_assets).
- Add wood/plank variants (multiple tree types).

---

## Phase 3 — Tech Foundation & Netcode

### Epic 3.1 — Server-authoritative streaming  **[existing 0w8]**
- Stop client-side regen; client renders only server-sent chunks.
- Server streaming priority by player distance.
- Chunk unload/eviction policy (server + client).

### Epic 3.2 — WAN multiplayer  **[existing sce]**
- **[existing yjv]** UPnP / NAT-PMP automatic port forwarding.
- **[existing 75x]** Transport encryption + join token/auth.
- Connection keepalive/timeout + reconnect over WAN.
- Packet-loss resilience tuning (`reliable.c`).

### Epic 3.3 — Netcode efficiency
- **[existing gbk]** Spatial interest culling for broadcasts.
- **[existing xk5]** Delta / quantized encoding for world-state broadcasts.
- **[existing q07]** Anti-cheat: reject implausibly fast block breaks.
- Anti-cheat: movement speed / teleport validation.
- Bandwidth stats debug overlay.

### Epic 3.4 — Multiplayer social
- Chat system (text input, broadcast, render) + net packet.
- Player name tags above remote players.
- Player skin / color customization.

### Epic 3.5 — Rendering upgrades
- Ambient occlusion / smooth lighting in mesher.
- Basic sun shadow mapping.
- Improved water shader (transparency + animation).
- **[existing 98a]** Dynamic render scale / resolution downsampling.
- Frustum/occlusion culling improvements.

---

## Phase 4 — Polish & Ship

### Epic 4.1 — Building blocks
- Mesher partial-block support (slab/stair geometry).
- Slab blocks (stone/wood/cobble) + placement orientation.
- Stair blocks + orientation/placement.
- Fence + wall blocks (neighbor-connect logic).

### Epic 4.2 — Creative mode
- Creative flight + no-clip toggle.
- Instant break + infinite block placement.
- Creative inventory / block-picker UI.
- Game-mode toggle + persistence.

### Epic 4.3 — Controls & settings
- Key remapping system + UI.
- FOV + mouse-sensitivity settings.
- Render settings UI (render distance, MSAA, aniso, render scale at runtime).
- Settings persistence to disk.

### Epic 4.4 — Juice
- Particle system (block break, explosion, splash).
- Footstep / ambient SFX expansion.
- Screen feedback (low-health vignette, underwater overlay).

### Epic 4.5 — Release
- Changelog + versioning workflow doc.
- Cut release via the `publish` skill (Linux + Windows).
- In-game version display + optional update check.

---

## Backlog / Stretch (epic stubs, deferred — not active)
- **Dimensions** (Nether/End): portals + multi-world manager + boss/end goal.
- **Enchanting + XP** system.
- **Brewing / potions.**
- **Redstone / logic** blocks.
- **Villagers + trading** — reuse existing `c55` (villager mobs), `au3` (trading).

## Sequencing notes
- Epic 1.6 (chest container) precedes Epic 2.2 (dungeon loot chests).
- Epic 1.2 (furnace) closes the food loop with Epic 1.4 (cooking).
- Phase 3 streaming/WAN epics can proceed in parallel with Phase 1/2 content
  since they touch different files (`server.c`/`net*.c` vs gameplay/worldgen).
- Recurring parallel-agent merge hazard: CMakeLists test-target list — resolve
  by keeping ALL targets. Bundle issues sharing a hot file into one agent.
