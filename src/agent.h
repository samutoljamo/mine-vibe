#ifndef AGENT_H
#define AGENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui/hud.h"

/* ------------------------------------------------------------------ */
/*  Command types                                                      */
/* ------------------------------------------------------------------ */

typedef enum AgentCommandType {
    CMD_MOVE,
    CMD_LOOK,
    CMD_JUMP,
    CMD_SPRINT,
    CMD_MODE,
    CMD_GET_STATE,
    CMD_DUMP_FRAME,
    CMD_SELECT_SLOT,
    CMD_QUIT,
    /* --- Deterministic harness driver (zqj) --- */
    CMD_STEP,         /* advance the sim a fixed number of ticks, no real sleep */
    /* --- Gameplay verbs routed through the authoritative server (qne) --- */
    CMD_PLACE,
    CMD_BREAK,
    CMD_ATTACK,
    CMD_CRAFT,
    CMD_OPEN,
    CMD_MOVE_ITEM,
    CMD_EAT,
    /* --- Test-only helpers (in-process server backdoor) (qne) --- */
    CMD_GIVE,
    CMD_TP,
    CMD_SPAWN_MOB,
    CMD_SET_TIME,
    CMD_SET_WEATHER,
    CMD_SET_FOOD,
    CMD_SET_HEALTH,
} AgentCommandType;

typedef struct AgentCommand {
    AgentCommandType type;
    union {
        struct { float forward; float right; }  move;
        struct { float yaw;     float pitch;  } look;   /* degrees, pitch clamped [-90,90] */
        struct { int   active;                } sprint;
        struct { int   mode;                  } mode;   /* 0=free, 1=walk */
        struct { char  path[256];             } dump_frame;
        struct { int   slot;                  } select_slot;
        struct { int   ticks;                 } step;
        struct { int   x, y, z, block;        } place;     /* place `block` into cell (x,y,z) */
        struct { int   x, y, z;               } brk;       /* break cell (x,y,z) */
        struct { int   mob_id;                } attack;
        struct { int   recipe;                } craft;     /* shapeless recipe index */
        struct { int   x, y, z;               } open;      /* open container at cell */
        struct { int   slot; int dir; int count; int x, y, z; } move_item;
        struct { int   item; int count;       } give;
        struct { float x, y, z;               } tp;
        struct { int   type; float x, y, z;   } spawn_mob;
        struct { int   ticks;                 } set_time;  /* world_ticks of day */
        struct { int   kind;                  } set_weather;
        struct { int   value;                 } set_food;   /* 0..20 hunger      */
        struct { int   value;                 } set_health; /* 0..20 hit points  */
    };
} AgentCommand;

/* ------------------------------------------------------------------ */
/*  Snapshot (main thread -> stdout)                                  */
/* ------------------------------------------------------------------ */

/* Rich per-slot inventory entry for the snapshot (c0j). */
typedef struct AgentInvSlot {
    int item;        /* ItemId (block id in the low range); 0 = empty/air */
    int count;
    int durability;  /* remaining uses for tools; 0 for blocks */
} AgentInvSlot;

/* Nearby-mob entry for the snapshot (c0j). */
typedef struct AgentMobInfo {
    int   id;
    int   type;      /* MobType */
    int   health;
    float pos[3];
} AgentMobInfo;

/* One container slot for the open-container view in the snapshot (c0j). */
typedef struct AgentContainerSlot {
    int item;
    int count;
} AgentContainerSlot;

#define AGENT_MAX_MOBS       16
#define AGENT_MAX_CONTAINER  27   /* chest holds 27; furnace uses first 3 */

typedef struct AgentSnapshot {
    float    pos[3];
    float    vel[3];
    float    yaw;        /* degrees */
    float    pitch;      /* degrees */
    int      on_ground;
    int      mode;       /* 0=free, 1=walk */
    uint64_t tick;
    int      selected_slot;
    int      hotbar[HUD_SLOT_COUNT];   /* legacy: per-slot counts */

    /* --- Rich state (c0j) --- */
    int      health;     /* 0..20, -1 if unknown (no networking) */
    int      food;       /* 0..20 hunger */
    int      air;        /* 0..20 oxygen bubbles */
    int      gamemode;   /* GameMode: 0=survival, 1=creative */
    int      weather;    /* WeatherKind: 0=clear,1=rain,2=storm */
    uint32_t time_of_day;/* server world_ticks (estimated) */

    AgentInvSlot inventory[HUD_SLOT_COUNT];
    int          inventory_count;       /* == INVENTORY_SLOTS */

    int      target_hit;                /* 1 if a block is targeted */
    int      target_x, target_y, target_z;
    int      target_block;              /* BlockID at the targeted cell */

    AgentMobInfo mobs[AGENT_MAX_MOBS];
    int          mob_count;

    int                 container_open; /* 1 if a container is open */
    int                 container_type; /* 0=chest, 1=furnace */
    AgentContainerSlot  container[AGENT_MAX_CONTAINER];
    int                 container_slots;
} AgentSnapshot;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Lifecycle — call from main thread only */
void agent_init(void);
void agent_destroy(void);
bool agent_is_active(void);

/* Main thread: drain one pending command, returns false when queue empty */
bool agent_pop_command(AgentCommand *out);

/* Main thread: emit events to stdout (thread-safe, mutex-protected) */
void agent_emit_snapshot(const AgentSnapshot *snap);
void agent_notify_chunk_loaded(int cx, int cz);
void agent_emit_ready(void);
void agent_emit_frame_saved(const char *path);
void agent_emit_error(const char *msg);

/* Testable pure functions (no global state) */
bool agent_parse_command(const char *line, AgentCommand *out);
void agent_format_snapshot(const AgentSnapshot *snap, char *buf, size_t buf_size);

#endif /* AGENT_H */
