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
    char buf[512];
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
    return false;
}

void agent_format_snapshot(const AgentSnapshot *snap, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size,
        "{\"event\":\"state\","
        "\"pos\":[%.3f,%.3f,%.3f],"
        "\"vel\":[%.3f,%.3f,%.3f],"
        "\"yaw\":%.3f,\"pitch\":%.3f,"
        "\"mode\":\"%s\","
        "\"on_ground\":%d,"
        "\"tick\":%" PRIu64 ","
        "\"selected_slot\":%d,"
        "\"hotbar\":[%d,%d,%d,%d,%d,%d]}\n",
        snap->pos[0], snap->pos[1], snap->pos[2],
        snap->vel[0], snap->vel[1], snap->vel[2],
        snap->yaw, snap->pitch,
        snap->mode == 0 ? "free" : "walk",
        snap->on_ground,
        snap->tick,
        snap->selected_slot,
        snap->hotbar[0], snap->hotbar[1], snap->hotbar[2],
        snap->hotbar[3], snap->hotbar[4], snap->hotbar[5]);
}
