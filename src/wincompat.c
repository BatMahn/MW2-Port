/* Windows (MinGW-w64) compatibility - see wincompat.h. Built into mw2 only on Windows. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* system() replacement: run the command line directly (no cmd.exe, no console window) and wait for it.
 * Returns the program's exit code, or -1 if it could not be started. The child inherits this process's
 * environment, which is how the shell passes the simulation its paths. */
int mw2_system(const char *cmdline)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-1;
    char *line;
    size_t n;
    if (!cmdline) return 1;   /* "is a command processor available" */
    n = strlen(cmdline) + 1;
    if (!(line = (char *)malloc(n))) return -1;
    memcpy(line, cmdline, n);   /* CreateProcessA may write to its command line */
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "mw2_system: could not start %s (error %lu)\n", cmdline, (unsigned long)GetLastError());
        free(line);
        return -1;
    }
    free(line);
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &code)) code = (DWORD)-1;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

FILE *mw2_fmemopen(void *buf, size_t size, const char *mode)
{
    char dir[MAX_PATH], path[MAX_PATH];
    FILE *f;
    (void)mode;   /* only "r" is used */
    if (!GetTempPathA(sizeof dir, dir) || !GetTempFileNameA(dir, "mw2", 0, path)) return NULL;
    if (!(f = fopen(path, "w+bTD"))) return NULL;   /* T: keep in cache, D: delete on close */
    if (size && fwrite(buf, 1, size, f) != size) { fclose(f); return NULL; }
    rewind(f);
    return f;
}

void mw2_exe_path(char *out, size_t n)
{
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (k == 0 || k >= n) { if (n) out[0] = 0; }
}

int mw2_pick_folder(const char *title, char *out, size_t n)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    WCHAR wt[256], wp[MAX_PATH];
    BOOL lost = FALSE;
    int r = 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   /* BIF_NEWDIALOGSTYLE needs COM */
    if (!MultiByteToWideChar(CP_ACP, 0, title, -1, wt, 256)) wt[0] = 0;
    memset(&bi, 0, sizeof bi);
    bi.lpszTitle = wt;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if ((pidl = SHBrowseForFolderW(&bi))) {
        if (SHGetPathFromIDListW(pidl, wp) && WideCharToMultiByte(CP_ACP, 0, wp, -1, out, (int)n, NULL, &lost) > 0 && !lost) r = 1;
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return r;
}
#else
typedef int mw2_wincompat_unused;   /* ISO C: a translation unit must not be empty */
#endif
