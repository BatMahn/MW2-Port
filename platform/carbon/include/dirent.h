/* dirent.h - directory reading for the classic Mac OS / Carbon build (posix_mac.c, on the File Manager) */
#ifndef MW2_DIRENT_H
#define MW2_DIRENT_H
struct dirent { char d_name[256]; };
typedef struct mw2_dir DIR;
DIR *mw2_opendir(const char *path);
struct dirent *mw2_readdir(DIR *d);
int mw2_closedir(DIR *d);
#define opendir(p) mw2_opendir(p)
#define readdir(d) mw2_readdir(d)
#define closedir(d) mw2_closedir(d)
#endif
