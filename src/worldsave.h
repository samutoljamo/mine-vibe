#ifndef WORLDSAVE_H
#define WORLDSAVE_H

/* Multiple-world save management for singleplayer / host mode.
 *
 * On-disk layout (relative to the working directory the game runs in):
 *
 *     saves/<name>/world.dat    block-delta overlay (see world_persist.c)
 *     saves/<name>/world.meta   tiny key=value text: name, seed, last_played
 *
 * <name> is a sanitized, path-safe directory component derived from the
 * player-facing world name. The overlay format itself is unchanged; this
 * module only adds the directory layout, a small metadata side-file, and a
 * listing of available worlds for the Load-World UI.
 *
 * The core here is deliberately pure + unit-tested (tests/test_worldsave.c):
 * name sanitization, path building, meta format/parse, new-world name/seed
 * derivation, and the in-memory world-list. The thin filesystem layer
 * (mkdir / scan saves dir / read+write meta) wraps that core and is exercised
 * by the build + integration, not the pure tests.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Root directory that holds every world. */
#define WORLDSAVE_ROOT      "saves"

/* Max length (incl. NUL) of a sanitized world directory name. */
#define WORLDSAVE_NAME_MAX  64

/* Max length (incl. NUL) of a built filesystem path. */
#define WORLDSAVE_PATH_MAX  1024

/* Buffer big enough to hold a formatted meta file. */
#define WORLDSAVE_META_BUF  256

/* Max worlds the Load-World list will hold/show. */
#define WORLDSAVE_LIST_MAX  64

/* Per-world game mode. Survival is the default (and the value old saves with
 * no gamemode field load as). Stored as an int in the meta file. */
typedef enum GameMode {
    GAMEMODE_SURVIVAL = 0,
    GAMEMODE_CREATIVE = 1
} GameMode;

/* Per-world metadata persisted alongside the overlay. */
typedef struct WorldMeta {
    char     name[WORLDSAVE_NAME_MAX]; /* sanitized, also the directory name */
    int32_t  seed;                     /* worldgen seed                       */
    int64_t  last_played;              /* monotone "recency" counter (not a   */
                                       /* wall clock; bumped on save/load)    */
    GameMode gamemode;                 /* survival/creative; default survival */
} WorldMeta;

/* In-memory list of discovered worlds (for the Load-World UI). */
typedef struct WorldList {
    WorldMeta entries[WORLDSAVE_LIST_MAX];
    int       count;
} WorldList;

/* ---- Pure core (no filesystem) — unit-tested ---- */

/* Sanitize a player-facing name into a path-safe directory component:
 * replaces spaces with '_', strips path separators / control chars / dots at
 * the ends, truncates to WORLDSAVE_NAME_MAX, and never yields "", ".", or "..".
 * Always writes a NUL-terminated, non-empty result into out. */
void worldsave_sanitize_name(const char* in, char* out, size_t cap);

/* Build saves/<name>, saves/<name>/world.dat, saves/<name>/world.meta. The
 * name is sanitized internally, so a hostile name can never escape the root. */
void worldsave_dir_path (const char* name, char* out, size_t cap);
void worldsave_dat_path (const char* name, char* out, size_t cap);
void worldsave_meta_path(const char* name, char* out, size_t cap);

/* Initialize a meta record (name is sanitized). */
void worldsave_meta_init(WorldMeta* m, const char* name,
                         int32_t seed, int64_t last_played);

/* Format meta to a text buffer. Returns bytes written (excl. NUL), 0 on error. */
size_t worldsave_meta_format(const WorldMeta* m, char* buf, size_t cap);

/* Parse a meta text buffer. Returns false if name/seed/last_played missing or
 * malformed. Field order independent (keys off "key=value" lines). */
bool worldsave_meta_parse(const char* buf, WorldMeta* out);

/* ---- Game-mode helpers (pure) ---- */

/* Return the world's game mode, defaulting to survival if m is NULL. */
GameMode world_meta_gamemode(const WorldMeta* m);

/* True if the given mode takes/deals damage (survival), false for creative.
 * The damage-gating ticket (1uh) calls this. */
bool gamemode_allows_damage(GameMode mode);

/* Derive a directory-safe new-world name from an incrementing counter (no rand
 * / no Date globals). Distinct counters yield distinct names. */
void worldsave_new_name(uint64_t counter, char* out, size_t cap);

/* Derive a deterministic seed from a counter (pure hash). */
int32_t worldsave_seed_from_counter(uint64_t counter);

/* World-list helpers. */
void worldsave_list_init(WorldList* wl);
bool worldsave_list_add(WorldList* wl, const WorldMeta* m);  /* false if full */
void worldsave_list_sort_recent(WorldList* wl);              /* last_played desc */

/* ---- Filesystem layer (wraps the pure core) ---- */

/* Ensure saves/ and saves/<name>/ exist. Returns false on mkdir failure. */
bool worldsave_ensure_dir(const char* name);

/* Write saves/<name>/world.meta from m (creating the dir if needed). */
bool worldsave_write_meta(const WorldMeta* m);

/* Read saves/<name>/world.meta into out. Returns false if missing/malformed. */
bool worldsave_read_meta(const char* name, WorldMeta* out);

/* Scan the saves/ directory and fill wl with every world that has a readable
 * meta (sorted most-recent first). Returns the number found (0 if none / no
 * saves dir yet). Never fails hard. */
int worldsave_scan(WorldList* wl);

#endif /* WORLDSAVE_H */
