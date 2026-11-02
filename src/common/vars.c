/* The LSIW variable table.
 *
 * Every variable Microsoft's USMT documentation recognises is listed here
 * (57 system + 40 user = 97), plus the handful of standard Windows
 * environment variables USMT does not list (LOCALAPPDATA, PROGRAMDATA,
 * PUBLIC, ...).
 *
 * Defaults are templates for a *Linux* location. Tokens:
 *   ${HOME} ${USER} ${UID} ${HOSTNAME}
 *   ${HOMES}    the folder that holds $HOME (usually /home)
 *   ${DATA}     ~/.local/share/lsiw          ${ROOT}  the C: drive ([drive] c_drive)
 *   ${XDG:NAME} an XDG user directory (DOCUMENTS, MUSIC, DESKTOP, ...)
 *   ${TRASH}    the freedesktop trash files directory
 *   ${VAR:X}    the Linux location of another variable
 *
 * The Windows path a program sees is derived from the Linux location (see
 * lsiw_var_windows in config.c): "anchored" folders always appear at a fixed
 * name under C:\, everything else appears at C:\<linux path>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "lsiw.h"
#include <string.h>
#include <strings.h>

#define F  LSIW_VAR_FOLDER
#define V  LSIW_VAR_VIRTUAL
#define VAL LSIW_VAR_VALUE
#define A  LSIW_VAR_ALIAS
#define D  LSIW_VAR_DERIVED
#define U  LSIW_SCOPE_USER
#define S  LSIW_SCOPE_SYSTEM

#define RDIR "${VAR:CSIDL_APPDATA}/Microsoft/Windows"
#define LDIR "${VAR:CSIDL_LOCAL_APPDATA}/Microsoft/Windows"
#define DDIR "${VAR:DEFAULTUSERPROFILE}"

static const struct lsiw_var g_vars[] = {
/*  name                              kind scope default                                     alias_of                 anchor                 csidl shell value                     usmt description */

/* ---- Windows directory ---- */
{ "WINDIR",                          F, S, "${HOME}/Windows",                              NULL,                    "windows",              0x24, NULL,                            1, "The Windows folder (C:\\windows)" },
{ "SYSTEMROOT",                      A, S, NULL,                                           "WINDIR",                NULL,                   -1,   NULL,                            1, "Same as WINDIR" },
{ "CSIDL_WINDOWS",                   A, S, NULL,                                           "WINDIR",                NULL,                   -1,   NULL,                            1, "Same as WINDIR" },
{ "SYSTEM32",                        D, S, "${VAR:WINDIR}/system32",                       NULL,                    NULL,                   0x25, NULL,                            1, "%WINDIR%\\system32" },
{ "SYSTEM",                          A, S, NULL,                                           "SYSTEM32",              NULL,                   -1,   NULL,                            1, "Same as SYSTEM32" },
{ "CSIDL_SYSTEM",                    A, S, NULL,                                           "SYSTEM32",              NULL,                   -1,   NULL,                            1, "The Windows System folder" },
{ "SYSTEM16",                        D, S, "${VAR:WINDIR}/system",                         NULL,                    NULL,                   -1,   NULL,                            1, "%WINDIR%\\system" },
{ "CSIDL_FONTS",                     D, S, "${VAR:WINDIR}/Fonts",                          NULL,                    NULL,                   0x14, NULL,                            1, "Fonts folder" },
{ "CSIDL_RESOURCES",                 D, S, "${VAR:WINDIR}/resources",                      NULL,                    NULL,                   0x38, NULL,                            1, "Resource data folder" },
{ "SYSTEMPROFILE",                   D, S, "${VAR:WINDIR}/system32/config/systemprofile",  NULL,                    NULL,                   -1,   NULL,                            1, "Profile of the SYSTEM account" },
{ "SYSTEMDRIVE",                     VAL,S, "C:",                                          NULL,                    NULL,                   -1,   NULL,                            1, "The drive that holds Windows (always C: in LSIW)" },
{ "COMSPEC",                         D, S, "${VAR:SYSTEM32}/cmd.exe",                      NULL,                    NULL,                   -1,   NULL,                            0, "Command interpreter" },

/* ---- Program Files ---- */
{ "CSIDL_PROGRAM_FILES",             F, S, "${HOME}/Program Files",                        NULL,                    "Program Files",        0x26, "ProgramFilesDir",               1, "Program Files folder" },
{ "PROGRAMFILES",                    A, S, NULL,                                           "CSIDL_PROGRAM_FILES",   NULL,                   -1,   NULL,                            1, "Same as CSIDL_PROGRAM_FILES" },
{ "PROGRAMW6432",                    A, S, NULL,                                           "CSIDL_PROGRAM_FILES",   NULL,                   -1,   NULL,                            0, "64-bit Program Files" },
{ "CSIDL_PROGRAM_FILESX86",          F, S, "${HOME}/Program Files (x86)",                  NULL,                    "Program Files (x86)",  0x2a, "ProgramFilesDir (x86)",         1, "32-bit Program Files folder" },
{ "PROGRAMFILES(X86)",               A, S, NULL,                                           "CSIDL_PROGRAM_FILESX86",NULL,                   -1,   NULL,                            1, "Same as CSIDL_PROGRAM_FILESX86" },
{ "CSIDL_PROGRAM_FILES_COMMON",      F, S, "${VAR:CSIDL_PROGRAM_FILES}/Common Files",      NULL,                    NULL,                   0x2b, "CommonFilesDir",                1, "Shared components" },
{ "COMMONPROGRAMFILES",              A, S, NULL,                                           "CSIDL_PROGRAM_FILES_COMMON", NULL,              -1,   NULL,                            1, "Same as CSIDL_PROGRAM_FILES_COMMON" },
{ "COMMONPROGRAMW6432",              A, S, NULL,                                           "CSIDL_PROGRAM_FILES_COMMON", NULL,              -1,   NULL,                            0, "64-bit Common Files" },
{ "CSIDL_PROGRAM_FILES_COMMONX86",   F, S, "${VAR:CSIDL_PROGRAM_FILESX86}/Common Files",   NULL,                    NULL,                   0x2c, "CommonFilesDir (x86)",          1, "32-bit shared components" },
{ "COMMONPROGRAMFILES(X86)",         A, S, NULL,                                           "CSIDL_PROGRAM_FILES_COMMONX86", NULL,           -1,   NULL,                            1, "Same as CSIDL_PROGRAM_FILES_COMMONX86" },

/* ---- ProgramData / all users ---- */
{ "CSIDL_COMMON_APPDATA",            F, S, "${HOME}/ProgramData",                          NULL,                    "ProgramData",          0x23, "Common AppData",                1, "Application data for all users" },
{ "PROGRAMDATA",                     A, S, NULL,                                           "CSIDL_COMMON_APPDATA",  NULL,                   -1,   NULL,                            0, "Same as CSIDL_COMMON_APPDATA" },
{ "ALLUSERSAPPDATA",                 A, S, NULL,                                           "CSIDL_COMMON_APPDATA",  NULL,                   -1,   NULL,                            1, "Same as CSIDL_COMMON_APPDATA" },
{ "ALLUSERSPROFILE",                 F, S, "${VAR:CSIDL_COMMON_APPDATA}",                  NULL,                    NULL,                   -1,   NULL,                            1, "All-users profile" },
{ "CSIDL_COMMON_STARTMENU",          F, S, "${VAR:CSIDL_COMMON_APPDATA}/Microsoft/Windows/Start Menu", NULL,        NULL,                   0x16, "Common Start Menu",             1, "Start menu for all users" },
{ "CSIDL_COMMON_PROGRAMS",           F, S, "${VAR:CSIDL_COMMON_STARTMENU}/Programs",       NULL,                    NULL,                   0x17, "Common Programs",               1, "Program groups for all users" },
{ "CSIDL_COMMON_STARTUP",            F, S, "${VAR:CSIDL_COMMON_PROGRAMS}/Startup",         NULL,                    NULL,                   0x18, "Common Startup",                1, "Startup folder for all users" },
{ "CSIDL_COMMON_ALTSTARTUP",         F, S, "${VAR:CSIDL_COMMON_PROGRAMS}/Startup",         NULL,                    NULL,                   0x1e, NULL,                            1, "Non-localised startup group for all users" },
{ "CSIDL_COMMON_TEMPLATES",          F, S, "${VAR:CSIDL_COMMON_APPDATA}/Microsoft/Windows/Templates", NULL,         NULL,                   0x2d, "Common Templates",              1, "Templates for all users" },
{ "CSIDL_COMMON_ADMINTOOLS",         F, S, "${VAR:CSIDL_COMMON_PROGRAMS}/Administrative Tools", NULL,               NULL,                   0x2f, "Common Administrative Tools",   1, "Administrative tools for all users" },

/* ---- Profiles ---- */
{ "PROFILESFOLDER",                  F, S, "${ROOT}/users",                               NULL,                    "users",                -1,   NULL,                            1, "Folder that holds every user profile" },
{ "DEFAULTUSERPROFILE",              F, S, "${HOMES}/Default",                            NULL,                    NULL,                   -1,   NULL,                            1, "Default user profile" },
{ "PUBLIC",                          F, S, "${HOMES}/Public",                             NULL,                    NULL,                   -1,   NULL,                            0, "Public profile" },
{ "CSIDL_COMMON_DESKTOPDIRECTORY",   F, S, "${VAR:PUBLIC}/Desktop",                        NULL,                    NULL,                   0x19, "Common Desktop",                1, "Desktop for all users" },
{ "CSIDL_COMMON_DOCUMENTS",          F, S, "${VAR:PUBLIC}/Documents",                      NULL,                    NULL,                   0x2e, "Common Documents",              1, "Documents for all users" },
{ "CSIDL_COMMON_FAVORITES",          F, S, "${VAR:PUBLIC}/Favorites",                      NULL,                    NULL,                   0x1f, "Common Favorites",              1, "Favorites for all users" },
{ "CSIDL_COMMON_MUSIC",              F, S, "${VAR:PUBLIC}/Music",                          NULL,                    NULL,                   0x35, "CommonMusic",                   1, "Music for all users" },
{ "CSIDL_COMMON_PICTURES",           F, S, "${VAR:PUBLIC}/Pictures",                       NULL,                    NULL,                   0x36, "CommonPictures",                1, "Pictures for all users" },
{ "CSIDL_COMMON_VIDEO",              F, S, "${VAR:PUBLIC}/Videos",                         NULL,                    NULL,                   0x37, "CommonVideo",                   1, "Videos for all users" },

/* ---- Default user profile (CSIDL_DEFAULT_*) ---- */
{ "CSIDL_DEFAULT_APPDATA",           F, S, DDIR "/AppData/Roaming",                        NULL,                    NULL,                   -1,   NULL,                            1, "Default user: AppData\\Roaming" },
{ "CSIDL_DEFAULT_LOCAL_APPDATA",     F, S, DDIR "/AppData/Local",                          NULL,                    NULL,                   -1,   NULL,                            1, "Default user: AppData\\Local" },
{ "CSIDL_DEFAULT_COOKIES",           F, S, DDIR "/AppData/Roaming/Microsoft/Windows/Cookies", NULL,                 NULL,                   -1,   NULL,                            1, "Default user: Cookies" },
{ "CSIDL_DEFAULT_CONTACTS",          F, S, DDIR "/Contacts",                               NULL,                    NULL,                   -1,   NULL,                            1, "Default user: Contacts" },
{ "CSIDL_DEFAULT_DESKTOP",           F, S, DDIR "/Desktop",                                NULL,                    NULL,                   -1,   NULL,                            1, "Default user: Desktop" },
{ "CSIDL_DEFAULT_DOWNLOADS",         F, S, DDIR "/Downloads",                              NULL,                    NULL,                   -1,   NULL,                            1, "Default user: Downloads" },
{ "CSIDL_DEFAULT_FAVORITES",         F, S, DDIR "/Favorites",                              NULL,                    NULL,                   -1,   NULL,                            1, "Default user: Favorites" },
{ "CSIDL_DEFAULT_HISTORY",           F, S, DDIR "/AppData/Local/Microsoft/Windows/History",NULL,                    NULL,                   -1,   NULL,                            1, "Default user: History" },
{ "CSIDL_DEFAULT_INTERNET_CACHE",    F, S, DDIR "/AppData/Local/Microsoft/Windows/INetCache", NULL,                 NULL,                   -1,   NULL,                            1, "Default user: Internet cache" },
{ "CSIDL_DEFAULT_MYDOCUMENTS",       F, S, DDIR "/Documents",                              NULL,                    NULL,                   -1,   NULL,                            1, "Default user: Documents" },
{ "CSIDL_DEFAULT_PERSONAL",          A, S, NULL,                                           "CSIDL_DEFAULT_MYDOCUMENTS", NULL,               -1,   NULL,                            1, "Default user: Personal (= Documents)" },
{ "CSIDL_DEFAULT_MYPICTURES",        F, S, DDIR "/Pictures",                               NULL,                    NULL,                   -1,   NULL,                            1, "Default user: Pictures" },
{ "CSIDL_DEFAULT_MYMUSIC",           F, S, DDIR "/Music",                                  NULL,                    NULL,                   -1,   NULL,                            1, "Default user: Music" },
{ "CSIDL_DEFAULT_MYVIDEO",           F, S, DDIR "/Videos",                                 NULL,                    NULL,                   -1,   NULL,                            1, "Default user: Videos" },
{ "CSIDL_DEFAULT_RECENT",            F, S, DDIR "/AppData/Roaming/Microsoft/Windows/Recent", NULL,                  NULL,                   -1,   NULL,                            1, "Default user: Recent" },
{ "CSIDL_DEFAULT_SENDTO",            F, S, DDIR "/AppData/Roaming/Microsoft/Windows/SendTo", NULL,                  NULL,                   -1,   NULL,                            1, "Default user: Send To" },
{ "CSIDL_DEFAULT_STARTMENU",         F, S, DDIR "/AppData/Roaming/Microsoft/Windows/Start Menu", NULL,              NULL,                   -1,   NULL,                            1, "Default user: Start Menu" },
{ "CSIDL_DEFAULT_PROGRAMS",          F, S, "${VAR:CSIDL_DEFAULT_STARTMENU}/Programs",      NULL,                    NULL,                   -1,   NULL,                            1, "Default user: Programs" },
{ "CSIDL_DEFAULT_STARTUP",           F, S, "${VAR:CSIDL_DEFAULT_PROGRAMS}/Startup",        NULL,                    NULL,                   -1,   NULL,                            1, "Default user: Startup" },
{ "CSIDL_DEFAULT_TEMPLATES",         F, S, DDIR "/AppData/Roaming/Microsoft/Windows/Templates", NULL,               NULL,                   -1,   NULL,                            1, "Default user: Templates" },
{ "CSIDL_DEFAULT_QUICKLAUNCH",       F, S, DDIR "/AppData/Roaming/Microsoft/Internet Explorer/Quick Launch", NULL,  NULL,                   -1,   NULL,                            1, "Default user: Quick Launch" },

/* ---- Current user ---- */
{ "CSIDL_PROFILE",                   F, U, "${HOME}",                                      NULL,                    NULL,                   0x28, NULL,                            1, "The user's profile folder" },
{ "USERPROFILE",                     A, U, NULL,                                           "CSIDL_PROFILE",         NULL,                   -1,   NULL,                            1, "Same as CSIDL_PROFILE" },
{ "HOMEDRIVE",                       VAL,U, "C:",                                          NULL,                    NULL,                   -1,   NULL,                            0, "Drive of the profile (always C:)" },
{ "HOMEPATH",                        D, U, "${VAR:CSIDL_PROFILE}",                         NULL,                    NULL,                   -1,   NULL,                            1, "Profile path without the drive" },
{ "USERNAME",                        VAL,U, "${USER}",                                     NULL,                    NULL,                   -1,   NULL,                            0, "User name" },
{ "COMPUTERNAME",                    VAL,S, "${HOSTNAME}",                                 NULL,                    NULL,                   -1,   NULL,                            0, "Computer name" },
{ "USERSID",                         VAL,U, "S-1-5-21-0-0-0-1000",                         NULL,                    NULL,                   -1,   NULL,                            1, "Security identifier of the user" },
{ "CSIDL_APPDATA",                   F, U, "${VAR:CSIDL_PROFILE}/AppData/Roaming",         NULL,                    NULL,                   0x1a, "AppData",                       1, "Roaming application data" },
{ "APPDATA",                         A, U, NULL,                                           "CSIDL_APPDATA",         NULL,                   -1,   NULL,                            1, "Same as CSIDL_APPDATA" },
{ "CSIDL_LOCAL_APPDATA",             F, U, "${VAR:CSIDL_PROFILE}/AppData/Local",           NULL,                    NULL,                   0x1c, "Local AppData",                 1, "Local (non-roaming) application data" },
{ "LOCALAPPDATA",                    A, U, NULL,                                           "CSIDL_LOCAL_APPDATA",   NULL,                   -1,   NULL,                            0, "Same as CSIDL_LOCAL_APPDATA" },
{ "CSIDL_APPDATA_LOCALLOW",          F, U, "${VAR:CSIDL_PROFILE}/AppData/LocalLow",        NULL,                    NULL,                   0x4e, "{A520A1A4-1780-4FF6-BD18-167343C5AF16}", 0, "Low-integrity application data" },
{ "TEMP",                            F, U, "${VAR:CSIDL_LOCAL_APPDATA}/Temp",              NULL,                    NULL,                   -1,   NULL,                            1, "Temporary files" },
{ "TMP",                             F, U, "${VAR:TEMP}",                                  NULL,                    NULL,                   -1,   NULL,                            1, "Temporary files" },
{ "CSIDL_PERSONAL",                  F, U, "${XDG:DOCUMENTS}",                             NULL,                    NULL,                   0x05, "Personal",                      1, "Documents" },
{ "CSIDL_MYDOCUMENTS",               A, U, NULL,                                           "CSIDL_PERSONAL",        NULL,                   -1,   NULL,                            1, "Same as CSIDL_PERSONAL" },
{ "CSIDL_DESKTOPDIRECTORY",          F, U, "${XDG:DESKTOP}",                               NULL,                    NULL,                   0x10, "Desktop",                       1, "Desktop folder" },
{ "CSIDL_DESKTOP",                   A, U, NULL,                                           "CSIDL_DESKTOPDIRECTORY",NULL,                   -1,   NULL,                            1, "The desktop (backed by CSIDL_DESKTOPDIRECTORY)" },
{ "CSIDL_MYMUSIC",                   F, U, "${XDG:MUSIC}",                                 NULL,                    NULL,                   0x0d, "My Music",                      1, "Music" },
{ "CSIDL_MYPICTURES",                F, U, "${XDG:PICTURES}",                              NULL,                    NULL,                   0x27, "My Pictures",                   1, "Pictures" },
{ "CSIDL_MYVIDEO",                   F, U, "${XDG:VIDEOS}",                                NULL,                    NULL,                   0x0e, "My Videos",                     1, "Videos" },
{ "CSIDL_DOWNLOADS",                 F, U, "${XDG:DOWNLOAD}",                              NULL,                    NULL,                   0x47, "{374DE290-123F-4565-9164-39C4925E467B}", 0, "Downloads" },
{ "CSIDL_SAVED_GAMES",               F, U, "${VAR:CSIDL_PROFILE}/Saved Games",             NULL,                    NULL,                   -1,   "{4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4}", 0, "Saved Games (FOLDERID_SavedGames)" },
{ "CSIDL_PLAYLISTS",                 F, U, "${VAR:CSIDL_MYMUSIC}/Playlists",               NULL,                    NULL,                   -1,   "{DE92C1C7-837F-4F69-A3BB-86E631204A23}", 1, "Playlists" },
{ "CSIDL_CONTACTS",                  F, U, "${VAR:CSIDL_PROFILE}/Contacts",                NULL,                    NULL,                   0x43, "{56784854-C6CB-462B-8169-88E350ACB882}", 1, "Contacts" },
{ "CSIDL_FAVORITES",                 F, U, "${VAR:CSIDL_PROFILE}/Favorites",               NULL,                    NULL,                   0x06, "Favorites",                     1, "Favorites" },
{ "CSIDL_STARTMENU",                 F, U, RDIR "/Start Menu",                             NULL,                    NULL,                   0x0b, "Start Menu",                    1, "Start menu" },
{ "CSIDL_PROGRAMS",                  F, U, "${VAR:CSIDL_STARTMENU}/Programs",              NULL,                    NULL,                   0x02, "Programs",                      1, "Program groups" },
{ "CSIDL_STARTUP",                   F, U, "${VAR:CSIDL_PROGRAMS}/Startup",                NULL,                    NULL,                   0x07, "Startup",                       1, "Startup group" },
{ "CSIDL_ALTSTARTUP",                F, U, "${VAR:CSIDL_PROGRAMS}/Startup",                NULL,                    NULL,                   0x1d, NULL,                            1, "Non-localised startup group" },
{ "CSIDL_ADMINTOOLS",                F, U, "${VAR:CSIDL_PROGRAMS}/Administrative Tools",   NULL,                    NULL,                   0x30, "Administrative Tools",          1, "Administrative tools" },
{ "CSIDL_COOKIES",                   F, U, RDIR "/Cookies",                                NULL,                    NULL,                   0x21, "Cookies",                       1, "Internet cookies" },
{ "CSIDL_NETHOOD",                   F, U, RDIR "/Network Shortcuts",                      NULL,                    NULL,                   0x13, "NetHood",                       1, "Network shortcuts" },
{ "CSIDL_PRINTHOOD",                 F, U, RDIR "/Printer Shortcuts",                      NULL,                    NULL,                   0x1b, "PrintHood",                     1, "Printer shortcuts" },
{ "CSIDL_RECENT",                    F, U, RDIR "/Recent",                                 NULL,                    NULL,                   0x08, "Recent",                        1, "Recently used documents" },
{ "CSIDL_SENDTO",                    F, U, RDIR "/SendTo",                                 NULL,                    NULL,                   0x09, "SendTo",                        1, "Send To menu items" },
{ "CSIDL_TEMPLATES",                 F, U, RDIR "/Templates",                              NULL,                    NULL,                   0x15, "Templates",                     1, "Document templates" },
{ "CSIDL_HISTORY",                   F, U, LDIR "/History",                                NULL,                    NULL,                   0x22, "History",                       1, "Internet history" },
{ "CSIDL_INTERNET_CACHE",            F, U, LDIR "/INetCache",                              NULL,                    NULL,                   0x20, "Cache",                         1, "Temporary Internet files" },
{ "CSIDL_CDBURN_AREA",               F, U, LDIR "/Burn/Burn",                              NULL,                    NULL,                   0x3b, "CD Burning",                    1, "CD burning staging area" },

/* ---- Virtual shell folders (not real folders on Windows either) ---- */
{ "CSIDL_BITBUCKET",                 V, U, "${TRASH}",                                     NULL,                    NULL,                   0x0a, NULL,                            1, "Recycle Bin (mapped to the Linux trash)" },
{ "CSIDL_DRIVES",                    V, U, "${ROOT}",                                      NULL,                    NULL,                   0x11, NULL,                            1, "This PC (the whole Linux filesystem as C:)" },
{ "CSIDL_NETWORK",                   V, U, "/run/user/${UID}/gvfs",                        NULL,                    NULL,                   0x12, NULL,                            1, "Network (GVFS network mounts)" },
{ "CSIDL_CONNECTIONS",               V, U, "",                                             NULL,                    NULL,                   0x31, NULL,                            1, "Network Connections (no folder)" },
{ "CSIDL_CONTROLS",                  V, U, "",                                             NULL,                    NULL,                   0x03, NULL,                            1, "Control Panel (no folder)" },
{ "CSIDL_INTERNET",                  V, U, "",                                             NULL,                    NULL,                   0x01, NULL,                            1, "Internet Explorer (no folder)" },
{ "CSIDL_PRINTERS",                  V, U, "",                                             NULL,                    NULL,                   0x04, NULL,                            1, "Printers (no folder)" },
};

const struct lsiw_var *lsiw_vars(size_t *count)
{
    if (count) *count = sizeof g_vars / sizeof g_vars[0];
    return g_vars;
}

const struct lsiw_var *lsiw_var_find(const char *name)
{
    for (size_t i = 0; i < sizeof g_vars / sizeof g_vars[0]; i++)
        if (strcasecmp(g_vars[i].name, name) == 0) return &g_vars[i];
    return NULL;
}

const struct lsiw_var *lsiw_var_resolve(const struct lsiw_var *v)
{
    int guard = 0;
    while (v && v->kind == LSIW_VAR_ALIAS && guard++ < 8)
        v = lsiw_var_find(v->alias_of);
    return v;
}

const struct lsiw_var *lsiw_var_by_csidl(int csidl)
{
    if (csidl < 0) return NULL;
    for (size_t i = 0; i < sizeof g_vars / sizeof g_vars[0]; i++)
        if (g_vars[i].csidl == csidl) return &g_vars[i];
    /* CSIDL aliases that Windows itself treats as the same folder. */
    if (csidl == 0x00) return lsiw_var_find("CSIDL_DESKTOPDIRECTORY");  /* CSIDL_DESKTOP */
    if (csidl == 0x0c) return lsiw_var_find("CSIDL_PERSONAL");          /* CSIDL_MYDOCUMENTS */
    if (csidl == 0x29) return lsiw_var_find("SYSTEM32");                /* CSIDL_SYSTEMX86 */
    return NULL;
}
