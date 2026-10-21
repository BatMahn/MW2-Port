/* Windows (MinGW-w64) compatibility: force-included into every C file by the Makefile's Windows build
 * (CFLAGS += -include src/wincompat.h). Maps the few POSIX calls the port uses onto their MSVCRT equivalents.
 * Has no effect on other platforms. */
#ifndef MW2_WINCOMPAT_H
#define MW2_WINCOMPAT_H
#if defined(_WIN32) && !defined(__cplusplus)

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <direct.h>
#include <io.h>
#include <sys/types.h>
#include <sys/stat.h>

/* mkdir(path, mode): Windows has no permission bits */
#define mkdir(p, m) _mkdir(p)

/* realpath(path, NULL) -> malloc'd absolute path */
#define realpath(p, r) _fullpath((r), (p), 260)

/* setenv / unsetenv (MSVCRT has _putenv_s; an empty value removes the variable) */
static inline int mw2_setenv(const char *k, const char *v, int overwrite)
{
    if (!overwrite && getenv(k)) return 0;
    return _putenv_s(k, v ? v : "") == 0 ? 0 : -1;
}
static inline int mw2_unsetenv(const char *k) { return _putenv_s(k, "") == 0 ? 0 : -1; }
#define setenv mw2_setenv
#define unsetenv mw2_unsetenv

/* system() goes through cmd.exe, which mangles a command line that starts and ends with quotes and opens a console
 * window from a GUI program. mw2_system() starts the program directly (CreateProcess) and waits for its exit code. */
int mw2_system(const char *cmdline);
#define system mw2_system

/* fmemopen (read-only use: parsing built-in default files): a self-deleting temporary file holding the bytes */
#include <stdio.h>
FILE *mw2_fmemopen(void *buf, size_t size, const char *mode);
#define fmemopen mw2_fmemopen

/* this executable's full path (argv[0] is unreliable when started from Explorer) */
void mw2_exe_path(char *out, size_t n);
/* the shell's folder browser (SHBrowseForFolderW): 1 a folder chosen (out, in the ANSI code page like the port's other
 * paths), 0 cancelled */
int mw2_pick_folder(const char *title, char *out, size_t n);

#endif
#endif
