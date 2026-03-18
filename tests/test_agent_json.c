#include "agent.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

static void test_parse_move(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"move\",\"forward\":1.0,\"right\":-0.5}", &cmd));
    assert(cmd.type == CMD_MOVE);
    assert(fabsf(cmd.move.forward - 1.0f) < 0.001f);
    assert(fabsf(cmd.move.right - (-0.5f)) < 0.001f);
}

static void test_parse_look(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"look\",\"yaw\":90.0,\"pitch\":-45.0}", &cmd));
    assert(cmd.type == CMD_LOOK);
    assert(fabsf(cmd.look.yaw - 90.0f) < 0.001f);
    assert(fabsf(cmd.look.pitch - (-45.0f)) < 0.001f);
}

static void test_parse_look_pitch_clamp(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"look\",\"yaw\":0.0,\"pitch\":95.0}", &cmd));
    assert(cmd.type == CMD_LOOK);
    assert(fabsf(cmd.look.pitch - 90.0f) < 0.001f);

    assert(agent_parse_command("{\"cmd\":\"look\",\"yaw\":0.0,\"pitch\":-95.0}", &cmd));
    assert(fabsf(cmd.look.pitch - (-90.0f)) < 0.001f);
}

static void test_parse_jump(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"jump\"}", &cmd));
    assert(cmd.type == CMD_JUMP);
}

static void test_parse_sprint(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"sprint\",\"active\":1}", &cmd));
    assert(cmd.type == CMD_SPRINT);
    assert(cmd.sprint.active == 1);
}

static void test_parse_mode(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"mode\",\"value\":\"walk\"}", &cmd));
    assert(cmd.type == CMD_MODE);
    assert(cmd.mode.mode == 1);

    assert(agent_parse_command("{\"cmd\":\"mode\",\"value\":\"free\"}", &cmd));
    assert(cmd.mode.mode == 0);
}

static void test_parse_get_state(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"get_state\"}", &cmd));
    assert(cmd.type == CMD_GET_STATE);
}

static void test_parse_dump_frame(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"dump_frame\",\"path\":\"frame_001.png\"}", &cmd));
    assert(cmd.type == CMD_DUMP_FRAME);
    assert(strcmp(cmd.dump_frame.path, "frame_001.png") == 0);
}

static void test_parse_quit(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"quit\"}", &cmd));
    assert(cmd.type == CMD_QUIT);
}

static void test_parse_unknown_returns_false(void) {
    AgentCommand cmd;
    assert(!agent_parse_command("{\"cmd\":\"fly\"}", &cmd));
}

static void test_format_snapshot(void) {
    AgentSnapshot snap = {
        .pos      = {1.5f, 65.0f, -3.0f},
        .vel      = {0.0f,  0.0f,   0.0f},
        .yaw      = 45.0f,
        .pitch    = -10.0f,
        .on_ground = 1,
        .mode     = 1,
        .tick     = 42,
    };
    char buf[512];
    agent_format_snapshot(&snap, buf, sizeof(buf));
    /* Must contain required keys */
    assert(strstr(buf, "\"event\":\"state\"") != NULL);
    assert(strstr(buf, "\"tick\":42")        != NULL);
    assert(strstr(buf, "\"on_ground\":1")    != NULL);
}

static void test_parse_with_spaces(void) {
    AgentCommand cmd;
    /* Python json.dumps() produces spaces after colons */
    assert(agent_parse_command("{\"cmd\": \"jump\"}", &cmd));
    assert(cmd.type == CMD_JUMP);

    assert(agent_parse_command("{\"cmd\": \"move\", \"forward\": 1.0, \"right\": 0.0}", &cmd));
    assert(cmd.type == CMD_MOVE);
    assert(fabsf(cmd.move.forward - 1.0f) < 0.001f);

    assert(agent_parse_command("{\"cmd\": \"mode\", \"value\": \"walk\"}", &cmd));
    assert(cmd.type == CMD_MODE);
    assert(cmd.mode.mode == 1);
}

int main(void) {
    test_parse_move();
    test_parse_look();
    test_parse_look_pitch_clamp();
    test_parse_jump();
    test_parse_sprint();
    test_parse_mode();
    test_parse_get_state();
    test_parse_dump_frame();
    test_parse_quit();
    test_parse_unknown_returns_false();
    test_format_snapshot();
    test_parse_with_spaces();
    printf("All agent JSON tests passed.\n");
    return 0;
}
