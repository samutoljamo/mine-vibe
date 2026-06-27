#ifndef MOB_H
#define MOB_H

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

#define MOB_MAX 64   /* hard array/wire cap */

typedef enum { MOB_ZOMBIE = 0, MOB_SKELETON, MOB_CREEPER, MOB_TYPE_COUNT } MobType;

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

/* ---- Skeleton (ranged) tunables ---- */
#define SKELETON_HEALTH         16
#define SKELETON_RETREAT_RANGE  6.0f    /* nearer than this -> back away */
#define SKELETON_SHOOT_RANGE    14.0f   /* max hitscan shot distance */
#define SKELETON_ATTACK_DAMAGE  3
#define SKELETON_ATTACK_INTERVAL 1.5f   /* seconds between shots */

/* ---- Creeper (explodes) tunables ---- */
#define CREEPER_HEALTH          20
#define CREEPER_FUSE_RANGE      3.5f    /* start the fuse hissing within this */
#define CREEPER_DETONATE_RANGE  3.0f    /* blast hits the player within this */
#define CREEPER_FUSE_TIME       1.5f    /* seconds from arming to boom */
#define CREEPER_BLAST_DAMAGE    20      /* damage at point-blank */

/* Per-type derived stats (pure lookup, no globals). */
typedef struct {
    int16_t health;
    float   speed;            /* chase speed, blocks/s */
    float   attack_range;     /* contact (melee) or shoot range */
    int     attack_damage;
    float   attack_interval;  /* seconds between hits/shots */
    bool    ranged;           /* attacks from a distance (skeleton) */
    bool    explodes;         /* self-destructs on detonation (creeper) */
} MobStats;

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
    float    attack_cooldown;/* seconds until next contact hit / shot */
    float    wander_timer;
    float    fuse_timer;     /* creeper: seconds left on the lit fuse */
    bool     fuse_lit;       /* creeper: fuse currently burning */
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

/* ---- Mob variety: per-type stats + AI helpers (all pure) ---- */

/* Per-type stat block. Unknown types fall back to the zombie profile. */
MobStats mob_stats(MobType type);

/* Skeleton: back away when the target is closer than the standoff distance. */
bool skeleton_wants_to_retreat(float dist);
/* Skeleton: target is within hitscan shooting range (point-blank included). */
bool skeleton_in_shoot_range(float dist);

/* Creeper: close enough to start the fuse hissing. */
bool creeper_should_arm_fuse(float dist);
/* Creeper: within blast radius AND the fuse has burned out. */
bool creeper_should_detonate(float dist, float fuse_timer);

/* ---- Client-side interpolated mob ---- */
#define MOB_INTERP_DELAY 0.025   /* seconds, matches remote players */

typedef struct {
    uint16_t id;
    uint8_t  type;
    float    x, y, z, yaw;
    uint8_t  health;
} ClientMobSnapshot;

typedef struct {
    uint16_t id;
    bool     active;
    uint8_t  type;
    uint8_t  health;
    vec3     positions[2];
    float    yaws[2];
    double   snapshot_times[2];
    uint8_t  snapshot_count;
    double   render_time;
} ClientMob;

typedef struct { ClientMob mobs[MOB_MAX]; } ClientMobSet;

void client_mob_set_init(ClientMobSet* s);
/* Full authoritative list: pushes snapshots for present ids, deactivates absent ones. */
void client_mob_set_apply(ClientMobSet* s, const ClientMobSnapshot* snaps,
                          int count, double recv_time);
void client_mob_interpolate(ClientMob* m, float dt, vec3 out_pos, float* out_yaw);

/* Ray vs all active client mobs. Returns nearest mob id (0 = none) whose
 * AABB is hit within max_dist. *out_t is set to the entry distance. */
uint16_t mob_ray_hit(const ClientMobSet* s, vec3 origin, vec3 dir,
                     float max_dist, float* out_t);

#endif /* MOB_H */
