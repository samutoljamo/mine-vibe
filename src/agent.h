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
    };
} AgentCommand;

/* ------------------------------------------------------------------ */
/*  Snapshot (main thread -> stdout)                                  */
/* ------------------------------------------------------------------ */

typedef struct AgentSnapshot {
    float    pos[3];
    float    vel[3];
    float    yaw;        /* degrees */
    float    pitch;      /* degrees */
    int      on_ground;
    int      mode;       /* 0=free, 1=walk */
    uint64_t tick;
    int      selected_slot;
    int      hotbar[HUD_SLOT_COUNT];
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
