#include "q2_rumble_gamecube.h"

#ifdef HW_DOL

#include <carryhandle/ch_rumble.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>


#define Q2GC_RUMBLE_PORT 0u


/*
 * Default ON.
 *
 * Q2CF stores an inverted "disabled" flag so existing Q2CF v2
 * records automatically inherit this default without migration.
 */
static int q2gcRumbleEnabled = 1;


/*
 * Quake II weapon policy for the GameCube's binary motor.
 *
 * Repeated automatic-weapon pulses compose naturally because
 * CH_RumblePulse() extends an existing pulse rather than
 * truncating it.
 */
static uint32_t Q2_RumbleWeaponDuration(
    const char *weaponClassname)
{
    if (!weaponClassname)
        return 45u;


    if (!strcmp(
            weaponClassname,
            "weapon_blaster"))
    {
        return 100u;
    }


    if (!strcmp(
            weaponClassname,
            "weapon_machinegun"))
    {
        return 30u;
    }


    if (!strcmp(
            weaponClassname,
            "weapon_chaingun"))
    {
        return 35u;
    }


    if (!strcmp(
            weaponClassname,
            "weapon_hyperblaster"))
    {
        return 80u;
    }


    if (!strcmp(
            weaponClassname,
            "weapon_shotgun"))
    {
        return 70u;
    }


    if (!strcmp(
            weaponClassname,
            "weapon_supershotgun"))
    {
        return 110u;
    }


    /*
     * Hand grenades are represented by Quake II's ammo item.
     */
    if (!strcmp(
            weaponClassname,
            "ammo_grenades"))
    {
        return 90u;
    }


    if (!strcmp(
            weaponClassname,
            "weapon_grenadelauncher"))
    {
        return 95u;
    }


    if (!strcmp(
            weaponClassname,
            "weapon_rocketlauncher"))
    {
        return 110u;
    }


    if (!strcmp(
            weaponClassname,
            "weapon_railgun"))
    {
        return 95u;
    }


    if (!strcmp(
            weaponClassname,
            "weapon_bfg"))
    {
        return 170u;
    }


    /*
     * Safe fallback for expansion/custom weapons that still use
     * the ordinary player weapon-noise path.
     */
    return 55u;
}


static uint32_t Q2_RumbleDamageDuration(
    int amount)
{
    if (amount <= 0)
        return 0u;


    if (amount <= 5)
        return 120u;


    if (amount <= 15)
        return 160u;


    if (amount <= 30)
        return 210u;


    if (amount <= 60)
        return 260u;


    return 320u;
}


void Q2_RumbleInit(void)
{
    if (!CH_RumbleInit())
    {
        fprintf(
            stderr,
            "Q2GC RUMBLE: CarryHandle init failed\n"
        );

        return;
    }


    fprintf(
        stderr,
        "Q2GC RUMBLE: initialized enabled=%d\n",
        q2gcRumbleEnabled
    );
}


void Q2_RumbleShutdown(void)
{
    CH_RumbleShutdown();


    fprintf(
        stderr,
        "Q2GC RUMBLE: shutdown\n"
    );
}


void Q2_RumbleSetEnabled(
    int enabled)
{
    int normalized =
        enabled
            ? 1
            : 0;


    if (q2gcRumbleEnabled ==
        normalized)
    {
        return;
    }


    q2gcRumbleEnabled =
        normalized;


    if (!q2gcRumbleEnabled)
    {
        /*
         * A menu toggle to OFF takes effect immediately.
         */
        CH_RumbleStop(
            Q2GC_RUMBLE_PORT,
            true
        );
    }


    fprintf(
        stderr,
        "Q2GC RUMBLE: %s\n",
        q2gcRumbleEnabled
            ? "enabled"
            : "disabled"
    );
}


int Q2_RumbleGetEnabled(void)
{
    return
        q2gcRumbleEnabled;
}


void Q2_RumbleWeaponFire(
    const char *weaponClassname)
{
    uint32_t duration;


    if (!q2gcRumbleEnabled)
        return;


    duration =
        Q2_RumbleWeaponDuration(
            weaponClassname
        );


    if (duration == 0u)
        return;


    CH_RumblePulse(
        Q2GC_RUMBLE_PORT,
        duration
    );
}


void Q2_RumbleDamage(
    int amount)
{
    uint32_t duration;


    if (!q2gcRumbleEnabled)
        return;


    duration =
        Q2_RumbleDamageDuration(
            amount
        );


    if (duration == 0u)
        return;



    CH_RumblePulse(
        Q2GC_RUMBLE_PORT,
        duration
    );
}


#else


/*
 * Non-GameCube stubs keep this source harmless if another build
 * happens to discover it through a broad source wildcard.
 */


void Q2_RumbleInit(void)
{
}


void Q2_RumbleShutdown(void)
{
}


void Q2_RumbleSetEnabled(
    int enabled)
{
    (void)enabled;
}


int Q2_RumbleGetEnabled(void)
{
    return 0;
}


void Q2_RumbleWeaponFire(
    const char *weaponClassname)
{
    (void)weaponClassname;
}


void Q2_RumbleDamage(
    int amount)
{
    (void)amount;
}


#endif
