/* test_gamefolder: finding the game folder (portcfg.c) - next to a macOS .app bundle, App Translocation, a chosen
 * folder (the game folder or the folder holding MW2-game/). Builds its own fake layout:
 *   ./test_gamefolder [BASE]   (default /tmp/test_gamefolder_<pid>; removed at the end) */
#include "portcfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void mk(const char *base, const char *rel)
{
    char p[PORTCFG_PATH], *q;
    snprintf(p, sizeof p, "%s/%s", base, rel);
    for (q = p + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
    mkdir(p, 0755);
}
static int found(const char *exe_dir, const char *want)
{
    portcfg c;
    portcfg_defaults(&c);
    if (portcfg_resolve(&c, exe_dir, NULL) != 0) return want == NULL;
    if (!want) { printf("  (found %s)\n", c.game); return 0; }
    if (strcmp(c.game, want) != 0) { printf("  (found %s, wanted %s)\n", c.game, want); return 0; }
    return 1;
}

int main(int argc, char **argv)
{
    char base[512], b[PORTCFG_PATH], e[PORTCFG_PATH], w[PORTCFG_PATH], out[PORTCFG_PATH];
    if (argc > 1) snprintf(base, sizeof base, "%s", argv[1]);
    else snprintf(base, sizeof base, "/tmp/test_gamefolder_%d", (int)getpid());
    mk(base, "home");
    mk(base, "elsewhere");
    snprintf(b, sizeof b, "%s/home", base);
    setenv("HOME", b, 1);   /* no ~/MW2-game */
    snprintf(b, sizeof b, "%s/elsewhere", base);
    if (chdir(b) != 0) return 1;   /* not the current folder either (a double-clicked app's is "/") */

    /* the bundle's folder */
    CHECK(portcfg_app_folder("/tmp/x/Some.app/Contents/Resources/", out, sizeof out) && strcmp(out, "/tmp/x") == 0, "Resources -> /tmp/x (%s)", out);
    CHECK(portcfg_app_folder("/tmp/x/MechWarrior 2.app/Contents/MacOS/", out, sizeof out) && strcmp(out, "/tmp/x") == 0, "MacOS, a name with a space (%s)", out);
    CHECK(portcfg_app_folder("/Some.app/Contents/Resources/", out, sizeof out) && strcmp(out, "/") == 0, "at the root (%s)", out);
    CHECK(portcfg_app_folder("Some.app/Contents/Resources/", out, sizeof out) && strcmp(out, ".") == 0, "relative (%s)", out);
    CHECK(!portcfg_app_folder("/usr/local/bin/", out, sizeof out) && !portcfg_app_folder(NULL, out, sizeof out), "not a bundle");
    CHECK(!portcfg_app_folder("/tmp/x/my.apps/Contents/", out, sizeof out), "\".apps\" is not a bundle");
    CHECK(portcfg_translocated("/private/var/folders/ab/cd/T/AppTranslocation/1F2E-33/d/MechWarrior 2.app/Contents/Resources/"),
          "App Translocation detected");
    CHECK(!portcfg_translocated("/Applications/MechWarrior 2.app/Contents/Resources/") && !portcfg_translocated(NULL), "not translocated");

    /* <base>/Some.app/Contents/Resources + <base>/MW2-game: found next to the .app */
    mk(base, "Some.app/Contents/Resources");
    mk(base, "Some.app/Contents/MacOS");
    snprintf(e, sizeof e, "%s/Some.app/Contents/Resources/", base);
    CHECK(found(e, NULL), "an empty layout: nothing found");
    mk(base, "MW2-game/install");
    snprintf(w, sizeof w, "%s/MW2-game", base);
    { char *r = realpath(w, NULL); snprintf(w, sizeof w, "%s", r ? r : ""); free(r); }
    CHECK(found(e, w), "Resources/: MW2-game next to the .app");
    snprintf(e, sizeof e, "%s/Some.app/Contents/MacOS/", base);
    CHECK(found(e, w), "MacOS/: MW2-game next to the .app");
    snprintf(e, sizeof e, "%s/bin/", base);
    CHECK(found(e, NULL), "a plain executable folder elsewhere: not found");
    /* one level up: <base>/Apps/Some.app with <base>/MW2-game */
    mk(base, "Apps/Some.app/Contents/Resources");
    snprintf(e, sizeof e, "%s/Apps/Some.app/Contents/Resources/", base);
    CHECK(found(e, w), "MW2-game one level above the .app's folder");
    /* the .app inside the game folder itself: <base>/MW2-game/Some.app */
    mk(base, "MW2-game/Some.app/Contents/Resources");
    snprintf(e, sizeof e, "%s/MW2-game/Some.app/Contents/Resources/", base);
    CHECK(found(e, w), "the .app inside the game folder");

    /* a chosen folder: the game folder, or the folder holding MW2-game/, else refused */
    CHECK(portcfg_game_in(base, out, sizeof out) && strcmp(out, w) == 0, "the parent of MW2-game accepted (%s)", out);
    snprintf(b, sizeof b, "%s/MW2-game", base);
    CHECK(portcfg_game_in(b, out, sizeof out) && strcmp(out, w) == 0, "MW2-game itself accepted (%s)", out);
    snprintf(b, sizeof b, "%s/elsewhere", base);
    CHECK(!portcfg_game_in(b, out, sizeof out), "a folder without the game refused");
    snprintf(b, sizeof b, "%s/gone", base);
    CHECK(!portcfg_game_in(b, out, sizeof out), "a missing folder refused");

    if (argc <= 1) { char cmd[600]; if (chdir("/") == 0) { snprintf(cmd, sizeof cmd, "rm -rf '%s'", base); if (system(cmd) != 0) printf("(could not remove %s)\n", base); } }
    printf("test_gamefolder: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
