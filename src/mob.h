#ifndef MOB_H
#define MOB_H

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

#define MOB_MAX 64   /* hard array/wire cap */

typedef enum { MOB_ZOMBIE = 0, MOB_TYPE_COUNT } MobType;

/* ---- Tunables (v1) ---- */
#define MOB_CAP             8       /* live mobs kept near the anchor */
#define MOB_HALF_W          0.3f
#define MOB_HEIGHT          1.8f
#define MOB_SPEED           2.0f    /* blocks/s chase speed */
#define MOB_JUMP_SPEED      JUMP_VEL/* = player jump; clears a 1-block step */
#define MOB_AGGRO_RANGE     16.0f
#define MOB_DEAGGRO_RANGE   24.0f
#define MOB_ATTACK_RANGE    1.5f
#define MOB_ATTACK_DAMAGE   4
#define MOB_ATTACK_INTERVAL 1.0f    /* seconds between contact hits */
#define MOB_HEALTH          20
#define MOB_SPAWN_INTERVAL  3.0f
#define MOB_SPAWN_MIN       12.0f
#define MOB_SPAWN_MAX       28.0f
#define MOB_DESPAWN_RANGE   44.0f
#define MOB_WANDER_INTERVAL 4.0f
#define MOB_STATE_MAX_WIRE  24      /* mobs per PKT_MOB_STATE (512B cap) */

/* ---- Server simulation struct ---- */
typedef struct {
    uint16_t id;             /* 1.. ; 0 = none */
    bool     active;
    MobType  type;
    vec3     position;       /* feet */
    vec3     velocity;
    float    yaw;            /* facing, radians (atan2(dz,dx)) */
    bool     on_ground;
    int16_t  health;
    uint16_t target_player;  /* player_id chased, 0 = none */
    float    attack_cooldown;/* seconds until next contact hit */
    float    wander_timer;
} Mob;

typedef struct { Mob mobs[MOB_MAX]; } MobSet;

/* Player position record passed to targeting. */
typedef struct {
    uint16_t player_id;
    vec3     position;       /* feet */
} MobTargetInfo;

void mob_set_init(MobSet* s);
Mob* mob_set_spawn(MobSet* s, MobType type, vec3 pos);
Mob* mob_set_get(MobSet* s, uint16_t id);
void mob_set_remove(MobSet* s, uint16_t id);

/* Pure: nearest player within aggro, with deaggro hysteresis on current target. */
uint16_t mob_acquire_target(const Mob* m, const MobTargetInfo* players, int count);

/* Pure: desired horizontal velocity + facing yaw toward target. */
void mob_steer(const Mob* m, vec3 target_pos,
               float* out_vx, float* out_vz, float* out_yaw);

/* Pure: subtract dmg, returns true if this hit dropped health to <= 0. */
bool mob_combat_apply(int16_t* health, int dmg);

#endif /* MOB_H */
