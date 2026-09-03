#ifndef Q2_RUMBLE_GAMECUBE_H
#define Q2_RUMBLE_GAMECUBE_H


/*
 * Quake2Cube-specific rumble policy.
 *
 * CarryHandle owns:
 *
 *   - motor scheduling
 *   - PAD/SI synchronization
 *   - worker-thread lifecycle
 *
 * Quake2Cube owns:
 *
 *   - which game events rumble
 *   - pulse durations
 *   - the player preference
 */


void Q2_RumbleInit(void);

void Q2_RumbleShutdown(void);


void Q2_RumbleSetEnabled(
    int enabled
);

int Q2_RumbleGetEnabled(void);


/*
 * weaponClassname is the Quake II gitem classname of the
 * currently-fired player weapon.
 */
void Q2_RumbleWeaponFire(
    const char *weaponClassname
);


/*
 * amount is the total player damage accumulated for the
 * current server frame.
 */
void Q2_RumbleDamage(
    int amount
);


#endif
