#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

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
bool  agent_parse_command(const char *line, AgentCommand *out) { (void)line; (void)out; return false; }
void  agent_format_snapshot(const AgentSnapshot *s, char *buf, size_t n) { (void)s; (void)buf; (void)n; }
