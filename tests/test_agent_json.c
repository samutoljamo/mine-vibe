#include "agent.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>

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

static void test_format_snapshot_hotbar(void) {
    AgentSnapshot snap = {
        .pos           = {0.0f, 0.0f, 0.0f},
        .vel           = {0.0f, 0.0f, 0.0f},
        .selected_slot = 2,
        .hotbar        = {3, 0, 17, 0, 5, 64},
    };
    char buf[512];
    agent_format_snapshot(&snap, buf, sizeof(buf));
    /* Selected slot and per-slot counts must be reported verbatim */
    assert(strstr(buf, "\"selected_slot\":2")        != NULL);
    assert(strstr(buf, "\"hotbar\":[3,0,17,0,5,64]") != NULL);
    printf("PASS: test_format_snapshot_hotbar\n");
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

static void test_cmd_select_slot(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"select_slot\",\"slot\":3}", &cmd));
    assert(cmd.type == CMD_SELECT_SLOT);
    assert(cmd.select_slot.slot == 3);

    /* Clamping: slot=99 → 5 (HUD_SLOT_COUNT - 1) */
    assert(agent_parse_command("{\"cmd\":\"select_slot\",\"slot\":99}", &cmd));
    assert(cmd.select_slot.slot == 5);

    /* Clamping: slot=-1 → 0 */
    assert(agent_parse_command("{\"cmd\":\"select_slot\",\"slot\":-1}", &cmd));
    assert(cmd.select_slot.slot == 0);

    printf("PASS: test_cmd_select_slot\n");
}

/* ---- Harness driver + gameplay verbs + helpers (zqj/qne) ---- */

static void test_parse_step(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"step\",\"ticks\":5}", &cmd));
    assert(cmd.type == CMD_STEP);
    assert(cmd.step.ticks == 5);
    /* Missing ticks defaults to 1 */
    assert(agent_parse_command("{\"cmd\":\"step\"}", &cmd));
    assert(cmd.step.ticks == 1);
    printf("PASS: test_parse_step\n");
}

static void test_parse_verbs(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"place\",\"x\":1,\"y\":2,\"z\":3,\"block\":7}", &cmd));
    assert(cmd.type == CMD_PLACE);
    assert(cmd.place.x == 1 && cmd.place.y == 2 && cmd.place.z == 3 && cmd.place.block == 7);

    assert(agent_parse_command("{\"cmd\":\"break\",\"x\":4,\"y\":5,\"z\":6}", &cmd));
    assert(cmd.type == CMD_BREAK);
    assert(cmd.brk.x == 4 && cmd.brk.y == 5 && cmd.brk.z == 6);
    /* break with no coords -> sentinel (use current target) */
    assert(agent_parse_command("{\"cmd\":\"break\"}", &cmd));
    assert(cmd.brk.x == INT32_MIN);

    assert(agent_parse_command("{\"cmd\":\"attack\",\"mob_id\":12}", &cmd));
    assert(cmd.type == CMD_ATTACK && cmd.attack.mob_id == 12);

    assert(agent_parse_command("{\"cmd\":\"craft\",\"recipe\":2}", &cmd));
    assert(cmd.type == CMD_CRAFT && cmd.craft.recipe == 2);

    assert(agent_parse_command("{\"cmd\":\"open\",\"x\":1,\"y\":2,\"z\":3}", &cmd));
    assert(cmd.type == CMD_OPEN);

    assert(agent_parse_command("{\"cmd\":\"move_item\",\"slot\":1,\"dir\":1,\"count\":4}", &cmd));
    assert(cmd.type == CMD_MOVE_ITEM);
    assert(cmd.move_item.slot == 1 && cmd.move_item.dir == 1 && cmd.move_item.count == 4);

    assert(agent_parse_command("{\"cmd\":\"eat\"}", &cmd));
    assert(cmd.type == CMD_EAT);
    printf("PASS: test_parse_verbs\n");
}

static void test_parse_helpers(void) {
    AgentCommand cmd;
    assert(agent_parse_command("{\"cmd\":\"give\",\"item\":1,\"count\":10}", &cmd));
    assert(cmd.type == CMD_GIVE && cmd.give.item == 1 && cmd.give.count == 10);

    assert(agent_parse_command("{\"cmd\":\"tp\",\"x\":1.5,\"y\":70.0,\"z\":-2.5}", &cmd));
    assert(cmd.type == CMD_TP);
    assert(fabsf(cmd.tp.x - 1.5f) < 0.001f && fabsf(cmd.tp.z - (-2.5f)) < 0.001f);

    assert(agent_parse_command("{\"cmd\":\"spawn_mob\",\"type\":2,\"x\":0,\"y\":64,\"z\":0}", &cmd));
    assert(cmd.type == CMD_SPAWN_MOB && cmd.spawn_mob.type == 2);

    assert(agent_parse_command("{\"cmd\":\"set_time\",\"ticks\":18000}", &cmd));
    assert(cmd.type == CMD_SET_TIME && cmd.set_time.ticks == 18000);

    assert(agent_parse_command("{\"cmd\":\"set_weather\",\"kind\":1}", &cmd));
    assert(cmd.type == CMD_SET_WEATHER && cmd.set_weather.kind == 1);
    printf("PASS: test_parse_helpers\n");
}

static void test_format_snapshot_rich(void) {
    AgentSnapshot snap = {0};
    snap.health = 18; snap.food = 15; snap.air = 20;
    snap.gamemode = 0; snap.weather = 1; snap.time_of_day = 12345;
    snap.inventory_count = 6;
    snap.inventory[0].item = 24; snap.inventory[0].count = 1; snap.inventory[0].durability = 59;
    snap.inventory[3].item = 1;  snap.inventory[3].count = 10;
    snap.target_hit = 1; snap.target_x = 0; snap.target_y = 64; snap.target_z = -1; snap.target_block = 2;
    snap.mob_count = 1;
    snap.mobs[0].id = 1; snap.mobs[0].type = 0; snap.mobs[0].health = 20;
    snap.mobs[0].pos[0] = 2.0f; snap.mobs[0].pos[1] = 64.0f; snap.mobs[0].pos[2] = 2.0f;
    snap.container_open = 1; snap.container_type = 1; snap.container_slots = 3;
    snap.container[0].item = 50; snap.container[0].count = 8;
    snap.sounds[SFX_SWING] = 3; snap.sounds[SFX_BLOCK_BREAK] = 7; snap.sounds[SFX_EAT] = 1;

    char buf[4096];
    agent_format_snapshot(&snap, buf, sizeof(buf));
    assert(strstr(buf, "\"health\":18") != NULL);
    assert(strstr(buf, "\"food\":15") != NULL);
    assert(strstr(buf, "\"gamemode\":\"survival\"") != NULL);
    assert(strstr(buf, "\"weather\":\"rain\"") != NULL);
    assert(strstr(buf, "\"time_of_day\":12345") != NULL);
    assert(strstr(buf, "\"durability\":59") != NULL);
    assert(strstr(buf, "\"target\":{\"x\":0,\"y\":64,\"z\":-1,\"block\":2}") != NULL);
    assert(strstr(buf, "\"mobs\":[{\"id\":1,\"type\":0,\"health\":20") != NULL);
    assert(strstr(buf, "\"container\":{\"type\":\"furnace\"") != NULL);
    assert(strstr(buf, "\"sounds\":{\"block_break\":7,") != NULL);
    assert(strstr(buf, "\"swing\":3}") != NULL);
    assert(strstr(buf, "\"eat\":1,") != NULL);
    printf("PASS: test_format_snapshot_rich\n");
}

static void test_format_snapshot_empty_container(void) {
    AgentSnapshot snap = {0};
    snap.inventory_count = 6;
    char buf[4096];
    agent_format_snapshot(&snap, buf, sizeof(buf));
    assert(strstr(buf, "\"container\":null") != NULL);
    assert(strstr(buf, "\"target\":null") != NULL);
    assert(strstr(buf, "\"mobs\":[]") != NULL);
    printf("PASS: test_format_snapshot_empty_container\n");
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
    test_format_snapshot_hotbar();
    test_parse_with_spaces();
    test_cmd_select_slot();
    test_parse_step();
    test_parse_verbs();
    test_parse_helpers();
    test_format_snapshot_rich();
    test_format_snapshot_empty_container();
    printf("All agent JSON tests passed.\n");
    return 0;
}
