/* mek.c - see mek.h. Weapon and equipment tables extracted from MW2SHELL.EXE. */
#include "mek.h"
#include "prj.h"
#include "datapath.h"
#include <stdlib.h>

#include <stdio.h>
#include <string.h>

const mek_weapon mek_weapons[] = {
    /* name                      heat  dmg  unk  shrt  med  range_m  wt*100 crits ammo/t */
    { "LRM 20",                     6,   -1,   -1,    7,   14,   1000,    500,    4,    6 },
    { "LRM 15",                     5,   -1,   -1,    7,   14,   1000,    350,    2,    8 },
    { "LRM 10",                     4,   -1,   -1,    7,   14,   1000,    250,    1,   12 },
    { "LRM 5",                      2,   -1,   -1,    7,   14,   1000,    100,    1,   24 },
    { "SRM 6",                      4,   -2,   -1,    3,    6,    497,    150,    1,   15 },
    { "SRM 4",                      3,   -2,   -1,    3,    6,    497,    100,    1,   25 },
    { "SRM 2",                      2,   -2,   -1,    3,    6,    497,     50,    1,   50 },
    { "Streak SRM-6",               4,   -2,   -1,    4,    8,    497,    300,    2,   15 },
    { "Streak SRM-4",               3,   -2,   -1,    4,    8,    497,    200,    1,   25 },
    { "Streak SRM-2",               2,   -2,   -1,    4,    8,    497,    100,    1,   50 },
    { "Machine Gun",                0,    2,   -1,    1,    2,    175,     25,    1,  200 },
    { "Gauss Rifle",                1,   15,    2,    7,   15,   1820,   1200,    6,    8 },
    { "LB 2-X AC",                  1,    2,    4,   10,   20,    800,    500,    3,   45 },
    { "LB 5-X AC",                  1,    5,    3,    8,   15,    700,    700,    4,   20 },
    { "LB 10-X AC",                 2,   10,   -1,    6,   12,    600,   1000,    5,   10 },
    { "LB 20-X AC",                 6,   20,   -1,    4,    8,    450,   1200,    9,    5 },
    { "Ultra AC/2",                 1,    2,    2,    9,   18,    700,    500,    2,   45 },
    { "Ultra AC/5",                 1,    5,   -1,    7,   14,    600,    700,    3,   20 },
    { "Ultra AC/10",                3,   10,   -1,    6,   12,    500,   1000,    4,   10 },
    { "Ultra AC/20",                7,   20,   -1,    4,    8,    400,   1200,    8,    5 },
    { "Flamer",                     3,    2,   -1,    1,    2,      3,     50,    1,    0 },
    { "ER PPC",                    15,   15,   -1,    7,   14,    746,    600,    2,    0 },
    { "ER Laser (Large)",          12,   10,   -1,    8,   15,   1019,    400,    1,    0 },
    { "ER Laser (Medium)",          5,    7,   -1,    5,   10,    510,    100,    1,    0 },
    { "ER Laser (Small)",           2,    5,   -1,    2,    4,    255,     50,    1,    0 },
    { "Pulse Laser (Large)",       10,   10,   -1,    6,   14,    815,    600,    2,    0 },
    { "Pulse Laser (Medium)",       4,    7,   -1,    4,    8,    408,    200,    1,    0 },
    { "Pulse Laser (Small)",        2,    3,   -1,    2,    4,    204,    100,    1,    0 },
    { "Narc Missile Beacon",        0,    0,   -1,    4,    8,     12,    200,    1,    0 },
    { "Anti-Missile System",        1,    0,   -1,   -1,   -1,     -1,     50,    1,    0 },
    { "Nuke",                       0,    0,   -1,   -1,   -1,     -1,      0,    1,    0 },
};
const int mek_weapon_count = (int)(sizeof mek_weapons / sizeof mek_weapons[0]);

static const struct { uint32_t id; const char *name; } equipment[] = {
    { 5000, "MASC" },
    { 5050, "Targeting Computer" },
    { 5100, "ECM" },
    { 5150, "Artemis IV" },
    { 5200, "Beagle Active Probe" },
    { 5250, "TAG" },
    { 5300, "Shoulder" },
    { 5350, "Upper Arm Actuator" },
    { 5400, "Lower Arm Actuator" },
    { 5450, "Hand Actuator" },
    { 5500, "Hip" },
    { 5550, "Upper Leg Actuator" },
    { 5600, "Lower Leg Actuator" },
    { 5650, "Foot Actuator" },
    { 5700, "Sensors" },
    { 5750, "Cockpit" },
    { 5800, "Gyro" },
    { 5850, "Engine" },
    { 5900, "Life Support" },
    { 6000, "Heat Sink" },
    { 7000, "Jump Jet" },
    { 8000, "Endo Steel" },
    { 9000, "Ferro-Fibrous" },
};
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

#define MEK_HEADER 0x18
#define MEK_LOC_SIZE 40
#define MEK_ITEMS_AT (MEK_HEADER + MEK_LOC_COUNT * MEK_LOC_SIZE)
#define MEK_NAME_LEN 50

int mek_parse(const uint8_t *d, size_t len, mek_def *m)
{
    uint32_t items;
    uint16_t links;
    size_t need, i, k;

    memset(m, 0, sizeof *m);
    if (len < MEK_ITEMS_AT + MEK_NAME_LEN) return -1;
    items = rd32(d + 0x10);
    links = rd16(d + 0x14);
    if (items + links > MEK_MAX_ITEMS) return -1;
    need = MEK_ITEMS_AT + (size_t)(items + links) * 8 + MEK_NAME_LEN;
    if (len < need && (need - len) % 8 == 0 && (need - len) / 8 <= links) {
        /* two 3D-edition records (GRG03STD, STM04STD) are one entry shorter than their counts: the trailing ammo
         * link(s) taken as missing (ASSUMED) */
        links = (uint16_t)(links - (need - len) / 8);
        need = len;
    }
    if (len != need) return -1;

    m->tonnage = rd32(d);
    m->walk_mp = rd32(d + 4);
    m->jump_mp = rd32(d + 8);
    m->heat_sinking = rd32(d + 12);
    for (i = 0; i < MEK_LOC_COUNT; i++) {
        const uint8_t *p = d + MEK_HEADER + i * MEK_LOC_SIZE;
        mek_loc *l = &m->loc[i];
        l->armor = rd32(p);
        l->rear_armor = rd32(p + 4);
        l->internal = rd32(p + 8);
        for (k = 0; k < MEK_SLOTS; k++) l->slots[k] = rd16(p + 12 + k * 2);
        l->slot_count = rd16(p + 36);
        l->flags = rd16(p + 38);
        if (l->slot_count > MEK_SLOTS) return -1;
    }
    for (i = 0; i < items + links; i++) {
        m->items[i].id = rd32(d + MEK_ITEMS_AT + i * 8);
        m->items[i].linked_id = (int32_t)rd32(d + MEK_ITEMS_AT + i * 8 + 4);
    }
    m->item_count = (int)items;
    m->link_count = links;
    m->unknown16 = rd16(d + 0x16);
    memcpy(m->config_raw, d + len - MEK_NAME_LEN, MEK_NAME_LEN);
    memcpy(m->config_name, d + len - MEK_NAME_LEN, MEK_NAME_LEN);
    m->config_name[MEK_NAME_LEN] = '\0';
    return 0;
}

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
size_t mek_write(const mek_def *m, uint8_t *out, size_t cap)
{
    size_t n = MEK_ITEMS_AT + (size_t)(m->item_count + m->link_count) * 8 + MEK_NAME_LEN, i, k;
    int any_raw = 0;
    if (cap < n || m->item_count + m->link_count > MEK_MAX_ITEMS) return 0;
    memset(out, 0, n);
    wr32(out, m->tonnage); wr32(out + 4, m->walk_mp); wr32(out + 8, m->jump_mp); wr32(out + 12, m->heat_sinking);
    wr32(out + 0x10, (uint32_t)m->item_count); wr16(out + 0x14, (uint16_t)m->link_count); wr16(out + 0x16, m->unknown16);
    for (i = 0; i < MEK_LOC_COUNT; i++) {
        uint8_t *p = out + MEK_HEADER + i * MEK_LOC_SIZE;
        const mek_loc *l = &m->loc[i];
        wr32(p, l->armor); wr32(p + 4, l->rear_armor); wr32(p + 8, l->internal);
        for (k = 0; k < MEK_SLOTS; k++) wr16(p + 12 + k * 2, l->slots[k]);
        wr16(p + 36, l->slot_count); wr16(p + 38, l->flags);
    }
    for (i = 0; i < (size_t)(m->item_count + m->link_count); i++) {
        wr32(out + MEK_ITEMS_AT + i * 8, m->items[i].id);
        wr32(out + MEK_ITEMS_AT + i * 8 + 4, (uint32_t)m->items[i].linked_id);
    }
    for (i = 0; i < MEK_NAME_LEN; i++) if (m->config_raw[i]) any_raw = 1;
    if (any_raw) memcpy(out + n - MEK_NAME_LEN, m->config_raw, MEK_NAME_LEN);
    else { size_t L = strlen(m->config_name); memcpy(out + n - MEK_NAME_LEN, m->config_name, L < MEK_NAME_LEN ? L : MEK_NAME_LEN - 1); }
    return n;
}

int mek_load(struct prj_archive *a, const char *name, mek_def *out)
{
    prj_record r;
    char path[64];
    unsigned char *data = NULL;
    size_t len = 0;
    int rc;
    if (a && prj_read_named((prj_archive *)a, "MEK", name, &r) == PRJ_OK) {
        rc = mek_parse(r.data, r.size, out);
        prj_record_free(&r);
        return rc;
    }
    snprintf(path, sizeof path, "MEK\\%s.MEK", name);
    if (dp_read_file(path, &data, &len) != 0) return -1;
    rc = mek_parse(data, len, out);
    free(data);
    return rc;
}

const mek_weapon *mek_weapon_for(uint32_t id)
{
    if (id >= 10000 || id % 100 == 0) return NULL;
    return (id / 100 < (uint32_t)mek_weapon_count) ? &mek_weapons[id / 100] : NULL;
}

const char *mek_item_name(uint32_t id, char *buf, size_t buflen)
{
    const mek_weapon *w;
    size_t i;

    if (id == 0) return "-";
    if (id >= 10000 && (w = mek_weapon_for(id - 10000)) != NULL) {
        snprintf(buf, buflen, "%s ammo", w->name);
        return buf;
    }
    if ((w = mek_weapon_for(id)) != NULL) return w->name;
    /* equipment: largest base id not above this one */
    for (i = sizeof equipment / sizeof equipment[0]; i-- > 0;) {
        if (id >= equipment[i].id) {
            uint32_t extra = id - equipment[i].id;
            if (extra == 0 || equipment[i].id >= 6000) return equipment[i].name;
            snprintf(buf, buflen, "%s (%s)", equipment[i].name,
                     extra == 1 ? "L" : extra == 2 ? "R" : "?");
            return buf;
        }
    }
    snprintf(buf, buflen, "item %u", (unsigned)id);
    return buf;
}

const char *mek_location_name(int loc)
{
    static const char *const n[] = {"Head", "Right Torso", "Center Torso", "Left Torso",
                                    "Right Arm", "Left Arm", "Right Leg", "Left Leg"};
    return (loc >= 0 && loc < MEK_LOC_COUNT) ? n[loc] : "?";
}
