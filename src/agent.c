#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <inttypes.h>

static bool g_active = false;

void  agent_init(void)    { g_active = true; }
void  agent_destroy(void) { g_active = false; }
bool  agent_is_active(void) { return g_active; }
bool  agent_pop_command(AgentCommand *out) { (void)out; return false; }
void  agent_emit_snapshot(const AgentSnapshot *s) { (void)s; }
void  agent_notify_chunk_loaded(int cx, int cz) { (void)cx; (void)cz; }
void  agent_emit_ready(void) {}
void  agent_emit_frame_saved(const char *p) { (void)p; }
void  agent_emit_error(const char *m) { (void)m; }
bool agent_parse_command(const char *line, AgentCommand *out)
{
    if (!line || !out) return false;

    /* Extract "cmd" value */
    const char *cmd_pos = strstr(line, "\"cmd\"");
    if (!cmd_pos) return false;
    char cmd_str[32] = {0};
    if (sscanf(cmd_pos, "\"cmd\":\"%31[^\"]\"", cmd_str) != 1) return false;

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
        const char *ap = strstr(line, "\"active\"");
        if (ap) sscanf(ap, "\"active\":%d", &out->sprint.active);
        return true;
    }
    if (strcmp(cmd_str, "mode") == 0) {
        out->type = CMD_MODE;
        char val[8] = {0};
        const char *vp = strstr(line, "\"value\"");
        if (!vp) return false;
        sscanf(vp, "\"value\":\"%7[^\"]\"", val);
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
        sscanf(pp, "\"path\":\"%255[^\"]\"", out->dump_frame.path);
        return true;
    }
    if (strcmp(cmd_str, "quit") == 0) {
        out->type = CMD_QUIT;
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
        "\"tick\":%" PRIu64 "}\n",
        snap->pos[0], snap->pos[1], snap->pos[2],
        snap->vel[0], snap->vel[1], snap->vel[2],
        snap->yaw, snap->pitch,
        snap->mode == 0 ? "free" : "walk",
        snap->on_ground,
        snap->tick);
}
