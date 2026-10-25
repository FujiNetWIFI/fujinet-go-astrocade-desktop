/*
 * Importing the BIOS, the way fujinet-go-msdos-desktop does: the common
 * file dialog (several files at once, filtered to BIOS images and zips) or
 * the folder browser, each path through astrosession_rom_import, then one
 * message -- "BIOS Imported" or "Nothing Imported" -- with what each said.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "import.h"

#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#include "win_common.h"

static const char k_filter[] = "BIOS images and ROM sets\0*.bin;*.rom;*.zip\0All files\0*.*\0\0";

typedef struct {
    astrosession *session;
    char log[4096];
    int imported;
} import_log;

static void import_one(import_log *l, const char *path)
{
    char msg[1024];
    size_t len = strlen(l->log);
    msg[0] = '\0';
    if (astrosession_rom_import(l->session, path, msg, sizeof msg) == 0)
        l->imported++;
    snprintf(l->log + len, sizeof l->log - len, "%s%s", len ? "\n" : "", msg);
}

/* Each chosen file: the one path, or the folder then each name. */
static int pick_files(HWND owner, int single, import_log *l)
{
    static char buf[64 * 1024];
    OPENFILENAMEA ofn;
    const char *p;
    memset(&ofn, 0, sizeof ofn);
    buf[0] = buf[1] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = k_filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof buf;
    ofn.lpstrTitle = "Import BIOS";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                (single ? 0 : OFN_ALLOWMULTISELECT);
    if (!GetOpenFileNameA(&ofn))
        return 0;
    p = buf + strlen(buf) + 1;
    if (single || !*p) {
        import_one(l, buf);
        return 1;
    }
    for (; *p; p += strlen(p) + 1) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\%s", buf, p);
        import_one(l, path);
    }
    return 1;
}

static int pick_folder(HWND owner, char *path, DWORD pathsz)
{
    BROWSEINFOA bi;
    PIDLIST_ABSOLUTE pidl;
    char dir[MAX_PATH] = "";
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = owner;
    bi.lpszTitle = "Import BIOS from a Folder (MAME's astrocde set works)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderA(&bi);
    if (!pidl)
        return 0;
    if (!SHGetPathFromIDListA(pidl, dir))
        dir[0] = '\0';
    CoTaskMemFree(pidl);
    snprintf(path, pathsz, "%s", dir);
    return path[0] != '\0';
}

int astro_import_bios(HWND owner, astrosession *session, int folder, int single)
{
    static import_log l;
    memset(&l, 0, sizeof l);
    l.session = session;
    if (folder) {
        char dir[MAX_PATH];
        if (!pick_folder(owner, dir, sizeof dir))
            return -1;
        import_one(&l, dir);
    } else if (!pick_files(owner, single, &l)) {
        return -1;
    }
    if (l.log[0])
        astro_message(owner, l.log, l.imported ? "BIOS Imported" : "Nothing Imported",
                      (l.imported ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_OK);
    return l.imported;
}
