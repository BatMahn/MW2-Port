/* lance.c - see lance.h. */
#include "lance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *code, *file, *name; } MECHS[18] = {
    {"frm", "firemoth", "Firemoth"}, {"ktf", "kitfox", "Kit Fox"}, {"jnr", "jenner", "Jenner IIC"},
    {"nva", "nova", "Nova"}, {"stm", "strmcrow", "Stormcrow"}, {"mdg", "maddog", "Mad Dog"},
    {"hlb", "hellbrgr", "Hellbringer"}, {"rfl", "rifleman", "Rifleman IIC"}, {"smn", "summoner", "Summoner"},
    {"tbr", "timbrwlf", "Timber Wolf"}, {"grg", "gargoyle", "Gargoyle"}, {"whm", "warhammr", "Warhammer IIC"},
    {"mrd", "marauder", "Marauder IIC"}, {"whk", "warhawk", "Warhawk"}, {"drw", "direwolf", "Dire Wolf"},
    {"ele", "elementl", "Elemental"}, {"tar", "tarantul", "Tarantula"}, {"btm", "btllmstr", "Battle Master IIC"},
};
/* skill rows (0x7a7a8): skill, range, 1, range, pilot level */
static const short SKILL[9][5] = {
    {0, 0, 0, 0, 0}, {1, 850, 1, 850, 1}, {2, 700, 1, 700, 2}, {3, 650, 1, 650, 2}, {3, 550, 1, 550, 3},
    {4, 450, 1, 450, 3}, {4, 350, 1, 350, 4}, {5, 300, 1, 300, 4}, {5, 250, 1, 250, 5}};
static const char *const CAMO[6] = {"l1wolfcl", "l1jadefn", "l1gostbr", "l1smojag", "l1novact", "l1steelv"};

int lance_mech_count(void) { return 18; }
const char *lance_mech_code(int i) { return i >= 0 && i < 18 ? MECHS[i].code : ""; }
const char *lance_mech_file(int i) { return i >= 0 && i < 18 ? MECHS[i].file : ""; }
const char *lance_mech_name(int i) { return i >= 0 && i < 18 ? MECHS[i].name : ""; }

typedef struct { unsigned char b[2048]; int len, maxc; } bwdbuf;
static void put16(unsigned char *p, int v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static unsigned char *chunk(bwdbuf *w, const char *tag, int size)
{
    unsigned char *p;
    if (w->len + size > (int)sizeof w->b) return NULL;
    p = w->b + w->len;
    memset(p, 0, (size_t)size);
    memcpy(p, tag, 4);
    put32(p + 4, (unsigned)size);
    w->len += size;
    if (size > w->maxc) w->maxc = size;
    return p;
}
static void begin(bwdbuf *w)   /* FUN_0001d140: header, REV 1.22, DTBL */
{
    unsigned char *p;
    memset(w, 0, sizeof *w);
    memcpy(w->b, "BWD", 4);
    w->len = 12;
    p = chunk(w, "REV", 12); memcpy(p + 8, "1.22", 4);
    chunk(w, "DTBL", 28);
}
static int finish(bwdbuf *w, const char *path)
{
    FILE *f;
    put32(w->b + 4, (unsigned)w->len);
    put32(w->b + 8, (unsigned)w->maxc);
    f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(w->b, 1, (size_t)w->len, f) != (size_t)w->len) { fclose(f); return -1; }
    return fclose(f);
}
static int find_id(prj_archive *a, const char *tag, const char *name)
{
    int t = prj_find_type(a, tag);
    return t < 0 ? -1 : prj_find_id(a, t, name);
}

/* FUN_0001d310: one GPS record (92 bytes) */
static void gps(prj_archive *a, bwdbuf *w, const lance_slot *s, int slot, int file_index, int skill)
{
    unsigned char *p;
    char load[9];
    int mek;
    if (s->mech < 0 || s->mech >= 18) return;
    if (skill < 1) skill = 1;
    if (skill > 8) skill = 8;
    if (file_index == 0) skill = 0;
    if (s->loadout[0]) snprintf(load, sizeof load, "%s", s->loadout);
    else snprintf(load, sizeof load, "%s00std", MECHS[s->mech].code);
    p = chunk(w, "GPS", 92);
    if (!p) return;
    mek = strcmp(load + 5, "std") == 0 ? find_id(a, "MEK", load) : 0xfffe;   /* standard loadouts by record id */
    put16(p + 8, mek);
    put16(p + 10, find_id(a, "BWD", MECHS[s->mech].file));
    p[12] = (unsigned char)file_index;
    p[13] = slot == 0;                                   /* lance leader */
    p[14] = (slot == 0 && file_index == 0) ? 0 : 2;      /* 0 = the player, 2 = AI */
    put16(p + 16, SKILL[skill][0]); put16(p + 18, SKILL[skill][1]); put16(p + 20, SKILL[skill][2]);
    put16(p + 22, SKILL[skill][3]); put16(p + 24, SKILL[skill][4]);
    put16(p + 32, 6); put16(p + 34, 0x400);
    memcpy(p + 36, MECHS[s->mech].file, strlen(MECHS[s->mech].file) < 8 ? strlen(MECHS[s->mech].file) : 8);
    memcpy(p + 45, load, strlen(load) < 8 ? strlen(load) : 8);
    memcpy(p + 54, s->pilot, strlen(s->pilot) < 15 ? strlen(s->pilot) : 15);
}

int lance_write_star(prj_archive *a, const char *path, const lance_slot *slots, int n, int file_index, int skill)
{
    bwdbuf w;
    int i;
    begin(&w);
    for (i = 0; i < n; i++) gps(a, &w, &slots[i], i, file_index, skill);
    return finish(&w, path);
}

int lance_write_all(prj_archive *a, const char *dir, const lance_slot friendly[3], const lance_slot enemy[3], int difficulty)
{
    char path[1024];
    int k, skill = 8 - (difficulty == 0 ? 1 : difficulty == 2 ? 3 : 2);   /* FUN_0001d540: base 8, MW2DIF.CFG */
    snprintf(path, sizeof path, "%s/USERSTAR.BWD", dir);
    if (lance_write_star(a, path, friendly, 3, 0, 0)) return -1;
    for (k = 1; k < 6; k++, skill--) {
        snprintf(path, sizeof path, "%s/EN%02dSTAR.BWD", dir, k);
        if (lance_write_star(a, path, enemy, 3, k, skill)) return -1;
    }
    return 0;
}

static void bmpj(prj_archive *a, bwdbuf *w, const char *name, int slot)
{
    int size = ((int)strlen(name) + 1 + 0xd) & ~3;      /* tag, size, u16 id, name, padded */
    unsigned char *p = chunk(w, "BMPJ", size);
    if (!p) return;
    put16(p + 8, find_id(a, "CEL", name) >= 0 ? find_id(a, "CEL", name) : 0);   /* camo records are CEL entries */
    memcpy(p + 10, name, strlen(name));
    p = chunk(w, "BMID", 12);
    if (!p) return;
    put16(p + 8, slot); put16(p + 10, 0xffff);
}

int lance_write_instmap(prj_archive *a, const char *dir, int fc, int ec)
{
    bwdbuf w;
    char path[1024];
    begin(&w);
    bmpj(a, &w, CAMO[fc < 0 || fc > 5 ? 0 : fc], 0x114);
    bmpj(a, &w, CAMO[ec < 0 || ec > 5 ? 1 : ec], 0x115);
    bmpj(a, &w, "jscamoia", 0x101);
    snprintf(path, sizeof path, "%s/INSTMAP1.BWD", dir);
    return finish(&w, path);
}
