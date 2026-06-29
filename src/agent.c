#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include "platform_thread.h"
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/*  Ring buffer (capacity must be power of two)                       */
/* ------------------------------------------------------------------ */
#define CMD_RING_CAP 64

typedef struct {
    AgentCommand    cmds[CMD_RING_CAP];
    int             head;   /* main thread reads here */
    int             tail;   /* I/O thread writes here */
    PT_Mutex        mtx;
} CmdRing;

static atomic_bool g_active = false;
static CmdRing      g_ring;
static PT_Thread    g_io_thread;
static PT_Mutex     g_stdout_mtx;

/* ------------------------------------------------------------------ */
/*  Internal: stdout helper                                           */
/* ------------------------------------------------------------------ */
static void emit_raw(const char *s)
{
    pt_mutex_lock(&g_stdout_mtx);
    fputs(s, stdout);
    fflush(stdout);
    pt_mutex_unlock(&g_stdout_mtx);
}

/* ------------------------------------------------------------------ */
/*  I/O thread: reads stdin, pushes commands                          */
/* ------------------------------------------------------------------ */
static void *io_thread_func(void *arg)
{
    (void)arg;
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        AgentCommand cmd;
        if (!agent_parse_command(line, &cmd)) {
            char err[128];
            snprintf(err, sizeof(err),
                "{\"event\":\"error\",\"msg\":\"unknown command\"}\n");
            emit_raw(err);
            continue;
        }

        pt_mutex_lock(&g_ring.mtx);
        int next_tail = (g_ring.tail + 1) % CMD_RING_CAP;
        if (next_tail == g_ring.head) {
            /* Queue full: tail drop */
            pt_mutex_unlock(&g_ring.mtx);
            emit_raw("{\"event\":\"error\",\"msg\":\"command queue full, command dropped\"}\n");
            continue;
        }
        g_ring.cmds[g_ring.tail] = cmd;
        g_ring.tail = next_tail;
        pt_mutex_unlock(&g_ring.mtx);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
void agent_init(void)
{
    g_active    = true;
    g_ring.head = 0;
    g_ring.tail = 0;
    pt_mutex_init(&g_ring.mtx);
    pt_mutex_init(&g_stdout_mtx);
    pt_thread_create(&g_io_thread, io_thread_func, NULL);
}

void agent_destroy(void)
{
    if (!g_active) return;
    g_active = false;
    /* I/O thread exits on its own when stdin closes */
    pt_thread_join(g_io_thread);
    pt_mutex_destroy(&g_ring.mtx);
    pt_mutex_destroy(&g_stdout_mtx);
}

bool agent_is_active(void) { return g_active; }

bool agent_pop_command(AgentCommand *out)
{
    pt_mutex_lock(&g_ring.mtx);
    if (g_ring.head == g_ring.tail) {
        pt_mutex_unlock(&g_ring.mtx);
        return false;
    }
    *out = g_ring.cmds[g_ring.head];
    g_ring.head = (g_ring.head + 1) % CMD_RING_CAP;
    pt_mutex_unlock(&g_ring.mtx);
    return true;
}

void agent_emit_snapshot(const AgentSnapshot *snap)
{
    char buf[4096];
    agent_format_snapshot(snap, buf, sizeof(buf));
    emit_raw(buf);
}

void agent_notify_chunk_loaded(int cx, int cz)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"chunk_loaded\",\"cx\":%d,\"cz\":%d}\n", cx, cz);
    emit_raw(buf);
}

void agent_emit_ready(void)
{
    emit_raw("{\"event\":\"ready\"}\n");
}

void agent_emit_frame_saved(const char *path)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"frame_saved\",\"path\":\"%s\"}\n", path);
    emit_raw(buf);
}

void agent_emit_error(const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"error\",\"msg\":\"%s\"}\n", msg);
    emit_raw(buf);
}

/* ---- Pure functions from Task 2 — kept exactly as implemented ---- */

/* Extract an integer field "name":<int> from a JSON line. Returns true and
 * writes *out when present (default left untouched when absent). */
static bool json_int(const char *line, const char *name, int *out)
{
    char key[40];
    snprintf(key, sizeof(key), "\"%s\"", name);
    const char *p = strstr(line, key);
    if (!p) return false;
    p = strchr(p + strlen(key), ':');
    if (!p) return false;
    return sscanf(p + 1, "%d", out) == 1;
}

/* Extract a float field "name":<float> from a JSON line. */
static bool json_float(const char *line, const char *name, float *out)
{
    char key[40];
    snprintf(key, sizeof(key), "\"%s\"", name);
    const char *p = strstr(line, key);
    if (!p) return false;
    p = strchr(p + strlen(key), ':');
    if (!p) return false;
    return sscanf(p + 1, "%f", out) == 1;
}

bool agent_parse_command(const char *line, AgentCommand *out)
{
    if (!line || !out) return false;

    /* Extract "cmd" value */
    const char *cmd_pos = strstr(line, "\"cmd\"");
    if (!cmd_pos) return false;
    const char *colon = strchr(cmd_pos + 5, ':');
    if (!colon) return false;
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;
    char cmd_str[32] = {0};
    if (*colon != '"' || sscanf(colon, "\"%31[^\"]\"", cmd_str) != 1) return false;

    if (strcmp(cmd_str, "move") == 0) {
        out->type = CMD_MOVE;
        out->move.forward = 0.0f;
        out->move.right   = 0.0f;
        const char *fp = strstr(line, "\"forward\"");
        if (fp) sscanf(fp, "\"forward\":%f", &out->move.forward);
        const char *rp = strstr(line, "\"right\"");
        if (rp) sscanf(rp, "\"right\":%f", &out->move.right);
        return true;
    }
    if (strcmp(cmd_str, "look") == 0) {
        out->type = CMD_LOOK;
        out->look.yaw   = 0.0f;
        out->look.pitch = 0.0f;
        const char *yp = strstr(line, "\"yaw\"");
        if (yp) sscanf(yp, "\"yaw\":%f", &out->look.yaw);
        const char *pp = strstr(line, "\"pitch\"");
        if (pp) sscanf(pp, "\"pitch\":%f", &out->look.pitch);
        /* Clamp pitch */
        if (out->look.pitch >  90.0f) out->look.pitch =  90.0f;
        if (out->look.pitch < -90.0f) out->look.pitch = -90.0f;
        return true;
    }
    if (strcmp(cmd_str, "jump") == 0) {
        out->type = CMD_JUMP;
        return true;
    }
    if (strcmp(cmd_str, "sprint") == 0) {
        out->type = CMD_SPRINT;
        out->sprint.active = 0;
        /* missing "active" field defaults to 0 (stop sprinting) */
        const char *ap = strstr(line, "\"active\"");
        if (ap) sscanf(ap, "\"active\":%d", &out->sprint.active);
        return true;
    }
    if (strcmp(cmd_str, "mode") == 0) {
        out->type = CMD_MODE;
        char val[16] = {0};
        const char *vp = strstr(line, "\"value\"");
        if (!vp) return false;
        const char *colon_val = strchr(vp + 7, ':');
        if (!colon_val) return false;
        colon_val++;
        while (*colon_val == ' ' || *colon_val == '\t') colon_val++;
        if (*colon_val != '"' || sscanf(colon_val, "\"%15[^\"]\"", val) != 1) return false;
        if      (strcmp(val, "walk") == 0) out->mode.mode = 1;
        else if (strcmp(val, "free") == 0) out->mode.mode = 0;
        else return false;
        return true;
    }
    if (strcmp(cmd_str, "get_state") == 0) {
        out->type = CMD_GET_STATE;
        return true;
    }
    if (strcmp(cmd_str, "dump_frame") == 0) {
        out->type = CMD_DUMP_FRAME;
        out->dump_frame.path[0] = '\0';
        const char *pp = strstr(line, "\"path\"");
        if (!pp) return false;
        const char *colon_path = strchr(pp + 6, ':');
        if (!colon_path) return false;
        colon_path++;
        while (*colon_path == ' ' || *colon_path == '\t') colon_path++;
        if (*colon_path != '"' || sscanf(colon_path, "\"%255[^\"]\"", out->dump_frame.path) != 1) return false;
        return true;
    }
    if (strcmp(cmd_str, "quit") == 0) {
        out->type = CMD_QUIT;
        return true;
    }
    if (strcmp(cmd_str, "select_slot") == 0) {
        out->type = CMD_SELECT_SLOT;
        out->select_slot.slot = 0;
        const char *sp = strstr(line, "\"slot\"");
        if (sp) sscanf(sp, "\"slot\":%d", &out->select_slot.slot);
        /* Clamp to valid range */
        if (out->select_slot.slot < 0) out->select_slot.slot = 0;
        if (out->select_slot.slot >= HUD_SLOT_COUNT)
            out->select_slot.slot = HUD_SLOT_COUNT - 1;
        return true;
    }
    /* ---- Deterministic harness driver (zqj) ---- */
    if (strcmp(cmd_str, "step") == 0) {
        out->type = CMD_STEP;
        out->step.ticks = 1;
        json_int(line, "ticks", &out->step.ticks);
        if (out->step.ticks < 0)    out->step.ticks = 0;
        if (out->step.ticks > 100000) out->step.ticks = 100000;
        return true;
    }
    /* ---- Gameplay verbs (qne) ---- */
    if (strcmp(cmd_str, "place") == 0) {
        out->type = CMD_PLACE;
        out->place.x = out->place.y = out->place.z = 0;
        out->place.block = 0;
        json_int(line, "x", &out->place.x);
        json_int(line, "y", &out->place.y);
        json_int(line, "z", &out->place.z);
        json_int(line, "block", &out->place.block);
        return true;
    }
    if (strcmp(cmd_str, "break") == 0) {
        out->type = CMD_BREAK;
        out->brk.x = out->brk.y = out->brk.z = 0;
        out->brk.x = out->brk.y = out->brk.z = INT32_MIN; /* sentinel: use target */
        json_int(line, "x", &out->brk.x);
        json_int(line, "y", &out->brk.y);
        json_int(line, "z", &out->brk.z);
        return true;
    }
    if (strcmp(cmd_str, "attack") == 0) {
        out->type = CMD_ATTACK;
        out->attack.mob_id = 0;
        json_int(line, "mob_id", &out->attack.mob_id);
        return true;
    }
    if (strcmp(cmd_str, "craft") == 0) {
        out->type = CMD_CRAFT;
        out->craft.recipe = 0;
        json_int(line, "recipe", &out->craft.recipe);
        return true;
    }
    if (strcmp(cmd_str, "open") == 0) {
        out->type = CMD_OPEN;
        out->open.x = out->open.y = out->open.z = 0;
        json_int(line, "x", &out->open.x);
        json_int(line, "y", &out->open.y);
        json_int(line, "z", &out->open.z);
        return true;
    }
    if (strcmp(cmd_str, "move_item") == 0) {
        out->type = CMD_MOVE_ITEM;
        out->move_item.slot = 0;
        out->move_item.dir = 0;     /* 0 = container->inv, 1 = inv->container */
        out->move_item.count = 1;
        out->move_item.x = out->move_item.y = out->move_item.z = INT32_MIN;
        json_int(line, "slot",  &out->move_item.slot);
        json_int(line, "dir",   &out->move_item.dir);
        json_int(line, "count", &out->move_item.count);
        json_int(line, "x", &out->move_item.x);
        json_int(line, "y", &out->move_item.y);
        json_int(line, "z", &out->move_item.z);
        return true;
    }
    if (strcmp(cmd_str, "eat") == 0) {
        out->type = CMD_EAT;
        return true;
    }
    /* ---- Test-only helpers (server backdoor) (qne) ---- */
    if (strcmp(cmd_str, "give") == 0) {
        out->type = CMD_GIVE;
        out->give.item = 0;
        out->give.count = 1;
        json_int(line, "item",  &out->give.item);
        json_int(line, "count", &out->give.count);
        if (out->give.count < 1) out->give.count = 1;
        return true;
    }
    if (strcmp(cmd_str, "tp") == 0) {
        out->type = CMD_TP;
        out->tp.x = out->tp.y = out->tp.z = 0.0f;
        json_float(line, "x", &out->tp.x);
        json_float(line, "y", &out->tp.y);
        json_float(line, "z", &out->tp.z);
        return true;
    }
    if (strcmp(cmd_str, "spawn_mob") == 0) {
        out->type = CMD_SPAWN_MOB;
        out->spawn_mob.type = 0;
        out->spawn_mob.x = out->spawn_mob.y = out->spawn_mob.z = 0.0f;
        json_int(line, "type", &out->spawn_mob.type);
        json_float(line, "x", &out->spawn_mob.x);
        json_float(line, "y", &out->spawn_mob.y);
        json_float(line, "z", &out->spawn_mob.z);
        return true;
    }
    if (strcmp(cmd_str, "set_time") == 0) {
        out->type = CMD_SET_TIME;
        out->set_time.ticks = 0;
        json_int(line, "ticks", &out->set_time.ticks);
        if (out->set_time.ticks < 0) out->set_time.ticks = 0;
        return true;
    }
    if (strcmp(cmd_str, "set_weather") == 0) {
        out->type = CMD_SET_WEATHER;
        out->set_weather.kind = 0;
        json_int(line, "kind", &out->set_weather.kind);
        if (out->set_weather.kind < 0 || out->set_weather.kind > 2)
            out->set_weather.kind = 0;
        return true;
    }
    return false;
}

void agent_format_snapshot(const AgentSnapshot *snap, char *buf, size_t buf_size)
{
    /* Append helper: write into buf at *off, never overrunning buf_size. */
    size_t off = 0;
    #define APP(...) do { \
        int _n = snprintf(buf + off, (off < buf_size) ? buf_size - off : 0, __VA_ARGS__); \
        if (_n > 0) off += (size_t)_n; \
    } while (0)

    APP("{\"event\":\"state\","
        "\"pos\":[%.3f,%.3f,%.3f],"
        "\"vel\":[%.3f,%.3f,%.3f],"
        "\"yaw\":%.3f,\"pitch\":%.3f,"
        "\"mode\":\"%s\","
        "\"on_ground\":%d,"
        "\"tick\":%" PRIu64 ","
        "\"selected_slot\":%d,"
        "\"hotbar\":[%d,%d,%d,%d,%d,%d],",
        snap->pos[0], snap->pos[1], snap->pos[2],
        snap->vel[0], snap->vel[1], snap->vel[2],
        snap->yaw, snap->pitch,
        snap->mode == 0 ? "free" : "walk",
        snap->on_ground,
        snap->tick,
        snap->selected_slot,
        snap->hotbar[0], snap->hotbar[1], snap->hotbar[2],
        snap->hotbar[3], snap->hotbar[4], snap->hotbar[5]);

    const char *gm  = (snap->gamemode == 1) ? "creative" : "survival";
    const char *wx  = (snap->weather == 2) ? "storm"
                    : (snap->weather == 1) ? "rain" : "clear";
    APP("\"health\":%d,\"food\":%d,\"air\":%d,"
        "\"gamemode\":\"%s\",\"weather\":\"%s\",\"time_of_day\":%u,",
        snap->health, snap->food, snap->air, gm, wx, snap->time_of_day);

    /* Full inventory: [{item,count,durability}, ...] */
    APP("\"inventory\":[");
    for (int i = 0; i < snap->inventory_count; i++) {
        APP("%s{\"item\":%d,\"count\":%d,\"durability\":%d}",
            i ? "," : "",
            snap->inventory[i].item, snap->inventory[i].count,
            snap->inventory[i].durability);
    }
    APP("],");

    /* Targeted block. */
    if (snap->target_hit)
        APP("\"target\":{\"x\":%d,\"y\":%d,\"z\":%d,\"block\":%d},",
            snap->target_x, snap->target_y, snap->target_z, snap->target_block);
    else
        APP("\"target\":null,");

    /* Nearby mobs. */
    APP("\"mobs\":[");
    for (int i = 0; i < snap->mob_count; i++) {
        APP("%s{\"id\":%d,\"type\":%d,\"health\":%d,\"pos\":[%.3f,%.3f,%.3f]}",
            i ? "," : "",
            snap->mobs[i].id, snap->mobs[i].type, snap->mobs[i].health,
            snap->mobs[i].pos[0], snap->mobs[i].pos[1], snap->mobs[i].pos[2]);
    }
    APP("],");

    /* Open container contents, if any. */
    if (snap->container_open) {
        APP("\"container\":{\"type\":\"%s\",\"slots\":[",
            snap->container_type == 1 ? "furnace" : "chest");
        for (int i = 0; i < snap->container_slots; i++) {
            APP("%s{\"item\":%d,\"count\":%d}",
                i ? "," : "",
                snap->container[i].item, snap->container[i].count);
        }
        APP("]}");
    } else {
        APP("\"container\":null");
    }

    APP("}\n");
    #undef APP
}
