/* filepick.c - OS-native pickers. Win32: GetSaveFileName/GetOpenFileName/
 * SHBrowseForFolder (linked via pragma comments). Linux: zenity via popen.
 * (extracted from panel.c so any module can pick paths) */
#define _CRT_SECURE_NO_WARNINGS
#include "filepick.h"
#include "app.h"
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#pragma comment(lib, "comdlg32")
#pragma comment(lib, "shell32")
#endif

int pick_save_path(char *out, int n, const char *title,
                   const char *filter, const char *ext) {
#ifdef _WIN32
    memset(out, 0, n);
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = glfwGetWin32Window(A.win);
    ofn.lpstrFile = out; ofn.nMaxFile = n;
    ofn.lpstrFilter = filter;
    ofn.lpstrDefExt = ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = title;
    return GetSaveFileNameA(&ofn);
#else
    (void)title; (void)ext; (void)filter;
    FILE *pp = popen("zenity --file-selection --save 2>/dev/null", "r");
    if (!pp) return 0;
    if (!fgets(out, n, pp)) { pclose(pp); return 0; }
    out[strcspn(out, "\r\n")] = 0;
    return pclose(pp) == 0 && out[0];
#endif
}

int pick_open_path(char *out, int n, const char *title,
                   const char *filter) {
#ifdef _WIN32
    memset(out, 0, n);
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = glfwGetWin32Window(A.win);
    ofn.lpstrFile = out; ofn.nMaxFile = n;
    ofn.lpstrFilter = filter;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = title;
    return GetOpenFileNameA(&ofn);
#else
    (void)title; (void)filter;
    FILE *pp = popen("zenity --file-selection 2>/dev/null", "r");
    if (!pp) return 0;
    if (!fgets(out, n, pp)) { pclose(pp); return 0; }
    out[strcspn(out, "\r\n")] = 0;
    return pclose(pp) == 0 && out[0];
#endif
}

int pick_dir(char *out, int n) {
#ifdef _WIN32
    BROWSEINFOA bi; memset(&bi, 0, sizeof bi);
    bi.hwndOwner = glfwGetWin32Window(A.win);
    bi.lpszTitle = "Choose world save folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return 0;
    int ok = SHGetPathFromIDListA(pidl, out);
    CoTaskMemFree(pidl);
    (void)n;
    return ok;
#else
    FILE *pp = popen("zenity --file-selection --directory 2>/dev/null", "r");
    if (!pp) return 0;
    if (!fgets(out, n, pp)) { pclose(pp); return 0; }
    out[strcspn(out, "\r\n")] = 0;
    return pclose(pp) == 0 && out[0];
#endif
}
