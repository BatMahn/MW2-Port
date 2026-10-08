/* wtable.h - the simulation's own weapon table (3dfx MW2.DLL .data 0x1025a6c0, 0x58-byte
 * entries, same order as mek_weapons[]). Read by the fire routine 0x100437a0 and the
 * projectile spawn 0x10043cc0:
 *   type       +0x00  projectile kind (0-2 lasers L/M/S, 3 SRM/Narc, 4 LRM/Streak, 5 ballistic, 6 PPC, 7 Gauss/flamer)
 *   repeat     +0x10  re-fires while the trigger is held
 *   per_volley +0x1c  projectiles per trigger
 *   speed      +0x2c  projectile speed, cm per tick (182 Hz)
 *   damage     +0x30  damage per projectile (float)
 *   heat       +0x34  heat per projectile (engine heat scale; warn 65, shutdown 80)
 *   target_heat +0x38 heat added to the target on a hit (0x10044f60: ER PPC 3.5, flamer)
 *   ammo_per_ton +0x20 volleys per ton of ammunition (= the shell's figures)
 *   range      +0x40  maximum range, cm (LRM 1001 m, Gauss 1820 m)
 *   field3c    +0x3c  LRM 7500, Streak 25, else 0 (minimum range? not used)
 *   drop       +0x28  1 = the projectile falls under gravity (0x10043cc0)
 *   sound      +0x24  SNDS index played on a volley's first projectile (0x100437a0)
 *   visual     +0x04  projectile object: 0-2 LASER1-3, 3 missile, 5 MG bullet, 6 PPC, 8 Narc, 11 slug, 22 nuke
 *   refire     +0x44  ticks from the end of a volley until ready again
 *   interval   +0x48  ticks between projectiles in a volley
 *   life       +0x4c  projectile lifetime, ticks (range = speed x life: ER LL 1014 m, Gauss 1809 m)
 */
#ifndef MW2_WTABLE_H
#define MW2_WTABLE_H

typedef struct { int type, repeat, per_volley, speed; float damage, heat; int refire, interval, life; float target_heat; int ammo_per_ton, range, field3c, drop, sound, visual; } sim_weapon;

static const sim_weapon sim_weapons[31] = {
    {4, 0, 20, 100, 2.0f, 0.6000000238418579f, 2534, 11, 905, 0.0f, 6, 100100, 7500, 0, 189, 3},  /* LRM 20 */
    {4, 0, 15, 100, 2.0f, 0.6000000238418579f, 2172, 22, 905, 0.0f, 8, 100100, 7500, 0, 189, 3},  /* LRM 15 */
    {4, 0, 10, 100, 2.0f, 0.800000011920929f, 1810, 45, 905, 0.0f, 12, 100100, 7500, 0, 189, 3},  /* LRM 10 */
    {4, 0, 5, 100, 2.0f, 0.800000011920929f, 1448, 45, 905, 0.0f, 24, 100100, 7500, 0, 189, 3},  /* LRM 5 */
    {3, 0, 6, 70, 3.0f, 1.340000033378601f, 1448, 60, 543, 0.0f, 15, 49686, 0, 0, 258, 3},  /* SRM 6 */
    {3, 0, 4, 70, 3.0f, 1.5f, 1267, 45, 543, 0.0f, 25, 49686, 0, 0, 258, 3},  /* SRM 4 */
    {3, 0, 2, 100, 3.0f, 2.0f, 1086, 45, 543, 0.0f, 50, 49686, 0, 0, 258, 3},  /* SRM 2 */
    {4, 0, 6, 70, 3.0f, 1.3339999914169312f, 1086, 45, 543, 0.0f, 15, 49686, 25, 1, 189, 3},  /* Streak SRM-6 */
    {4, 0, 4, 70, 3.0f, 1.5f, 1448, 45, 543, 0.0f, 25, 49686, 25, 1, 189, 3},  /* Streak SRM-4 */
    {4, 0, 2, 70, 3.0f, 2.0f, 1448, 45, 543, 0.0f, 50, 49686, 25, 1, 189, 3},  /* Streak SRM-2 */
    {5, 1, 1, 300, 1.0f, 0.0f, 11, 11, 57, 0.0f, 200, 17500, 0, 1, 226, 5},  /* Machine Gun */
    {7, 0, 1, 280, 18.0f, 0.5f, 724, 90, 646, 0.0f, 8, 182000, 0, 0, 218, 11},  /* Gauss Rifle */
    {5, 1, 1, 240, 3.0f, 0.5f, 45, 45, 331, 0.0f, 45, 80000, 0, 0, 1, 11},  /* LB 2-X AC */
    {5, 1, 1, 240, 7.5f, 0.5f, 45, 45, 289, 0.0f, 20, 70000, 0, 0, 1, 11},  /* LB 5-X AC */
    {5, 1, 1, 240, 15.0f, 1.0f, 45, 11, 247, 0.0f, 10, 60000, 0, 0, 1, 11},  /* LB 10-X AC */
    {5, 1, 1, 240, 30.0f, 3.0f, 45, 11, 186, 0.0f, 5, 45000, 0, 0, 1, 11},  /* LB 20-X AC */
    {5, 1, 1, 220, 3.0f, 0.5f, 22, 11, 316, 0.0f, 45, 70000, 0, 0, 187, 11},  /* Ultra AC/2 */
    {5, 1, 1, 220, 7.5f, 0.5f, 22, 11, 271, 0.0f, 20, 60000, 0, 0, 187, 11},  /* Ultra AC/5 */
    {5, 1, 1, 220, 15.0f, 1.5f, 22, 11, 226, 0.0f, 10, 50000, 0, 0, 187, 11},  /* Ultra AC/10 */
    {5, 1, 1, 220, 30.0f, 3.5f, 22, 11, 181, 0.0f, 5, 40000, 0, 0, 187, 11},  /* Ultra AC/20 */
    {7, 0, 1, 80, 20.0f, 12.0f, 90, 181, 181, 0.0f, 8, 80000, 1000, 0, 227, -1},  /* Flamer */
    {6, 0, 1, 70, 17.0f, 12.5f, 452, 22, 1705, 3.5f, -1, 120000, 0, 0, 227, 6},  /* ER PPC */
    {0, 1, 1, 280, 8.0f, 6.0f, 108, 60, 362, 0.0f, -1, 101920, 0, 0, 199, 0},  /* ER Laser (Large) */
    {1, 1, 1, 280, 5.0f, 2.5f, 90, 60, 181, 0.0f, -1, 50960, 0, 0, 228, 1},  /* ER Laser (Medium) */
    {2, 1, 1, 280, 3.0f, 1.0f, 72, 60, 90, 0.0f, -1, 25480, 0, 0, 243, 2},  /* ER Laser (Small) */
    {0, 1, 2, 280, 4.0f, 3.5f, 45, 60, 289, 0.0f, -1, 81536, 0, 0, 277, 0},  /* Pulse Laser (Large) */
    {1, 1, 2, 280, 2.0f, 1.5f, 36, 11, 144, 0.0f, -1, 40768, 0, 0, 234, 1},  /* Pulse Laser (Medium) */
    {2, 1, 2, 280, 1.0f, 0.5f, 18, 11, 72, 0.0f, -1, 20384, 0, 0, 278, 2},  /* Pulse Laser (Small) */
    {3, 0, 1, 80, 0.0f, 4.0f, 2534, 45, 1448, 0.0f, -1, 200000, 0, 1, 189, 8},  /* Narc Missile Beacon */
    {5, 1, 1, 600, 1.0f, 0.0f, 11, 5, 271, 0.0f, 24, 10000, 0, 1, 1, 5},  /* Anti-Missile System */
    {22, 1, 1, 0, 100.0f, 0.0f, 36200, 181, 5430, 0.0f, 1, 200000, 0, 0, 189, 22},  /* Nuke */
};

/* short names (+0x50, 8 chars), as the HUD weapon list shows them */
static const char *const sim_weapon_short[31] = {
    "LRM20", "LRM15", "LRM10", "LRM5", "SRM6", "SRM4", "SRM2", "SSRM6", "SSRM4", "SSRM2", "MGun", "GAUSS", "xAC2", "xAC5", "xAC10", "xAC20", "uAC2", "uAC5", "uAC10", "uAC20", "FLAMER", "PPC", "LLASER", "MLASER", "SLASER", "LPLAS", "MPLAS", "SPLAS", "NARC", "AMS", "NUKE"
};

#endif
