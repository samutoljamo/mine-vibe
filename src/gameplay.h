#ifndef GAMEPLAY_H
#define GAMEPLAY_H

/* Maximum distance (in blocks) the player may break or place from. */
#define MAX_REACH 6.0f

/* Player AABB used by client physics AND server reach/self-trap checks. */
#define PLAYER_HALF_W  0.3f
#define PLAYER_HEIGHT  1.8f
#define PLAYER_EYE_H   1.62f

#define PLAYER_MAX_HEALTH      20
#define PLAYER_ATTACK_DAMAGE   5
#define PLAYER_ATTACK_COOLDOWN 0.25   /* seconds between accepted player hits */

#endif
