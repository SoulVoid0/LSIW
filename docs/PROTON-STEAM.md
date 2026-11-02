# Wine versions, Proton and Steam

LSIW runs Windows programs with its own native engine. It can also hand
them to Wine or to Proton, and you choose which Wine and which Proton.
Every one of them uses the folders chosen in LSIW Control (Documents,
AppData, Saved Games, Desktop, Downloads, Music, Pictures, Videos and the
rest), so a program finds the same files whatever engine runs it.

## Choosing the engine and the version

| Where | What |
|---|---|
| LSIW Control › Engine | default engine, Wine version, Proton version, download buttons |
| `lsiw config engine auto\|native\|wine\|proton` | default engine of `lsiw run` |
| `lsiw config wine VERSION\|PATH\|auto` | Wine version of the wine engine |
| `lsiw config proton VERSION\|FOLDER\|steam:NAME\|auto` | Proton version of the proton engine |
| `lsiw run -e wine:VERSION …`, `-e proton:VERSION …` | one run with that version |
| `lsiw runners` | every installed version, with its ID; `*` marks the selected one |

The proton engine always runs programs in LSIW's Proton prefix with LSIW's
folders, so every Proton in LSIW Control's list is an "LSIW" version: LSIW
Proton Stable, LSIW's twin of each of Valve's Proton versions Steam lists
(`steam:proton_experimental`, `steam:proton_11`, … `steam:proton_37`,
`steam:proton_hotfix`; Steam downloads one the first time it is used), and
LSIW's twin of every other Proton found. "Automatic" is the newest official
Proton installed.

`VERSION` is an ID from `lsiw runners` (usually the folder name, such as
`GE-Proton9-20`, `Proton 9.0 (Beta)`, `wine-9.21-staging-x86_64` or
`system`).

### Where LSIW finds them

**Wine:**
- `wine` or `wine64` in `$PATH` (ID `system`; on Fedora, `sudo dnf install wine`)
- WineHQ packages in `/opt/wine-*`
- Lutris runners (`~/.local/share/lutris/runners/wine`)
- Bottles runners (native and Flatpak)
- builds downloaded by LSIW
- anything listed in `[wine] custom = /path/to/bin/wine;…`

**Proton:**
- Steam's own versions (`steamapps/common/Proton*` in every Steam library)
- tools in Steam's `compatibilitytools.d`, such as GE-Proton installed with ProtonUp-Qt
- `/usr/share/steam/compatibilitytools.d`
- builds downloaded by LSIW
- anything listed in `[proton] custom = /path/to/folder;…`

The automatic choices are:
- **Wine:** the `wine` in `$PATH`.
- **Proton:** the newest official Steam "Proton X.Y".

### Downloading versions

```sh
lsiw runners install ge              # latest GE-Proton for this processor, SHA-512 checked
lsiw runners install ge GE-Proton9-20
lsiw runners install cachyos         # Proton-CachyOS
lsiw runners install dwproton        # DW-Proton (Dawn Winery)
lsiw runners install rtsp            # Proton-GE RTSP (VRChat)
lsiw runners install tkg             # Proton-Tkg (latest automatic build)
lsiw runners install em              # Proton-EM
lsiw runners install wineland        # Proton-Wineland
lsiw runners install wine            # latest Wine build (Kron4ek/Wine-Builds)
lsiw runners install wine 9.21 --staging
lsiw runners remove GE-Proton9-20    # only versions LSIW downloaded
```

- Wine builds come from Kron4ek/Wine-Builds, whose x86-64 archives are
  named `amd64` (another name for x86-64, used for Intel and AMD
  processors alike). LSIW downloads the WoW64 build (`amd64-wow64`), which
  needs no 32-bit libraries on the computer and still runs 32-bit
  programs, and installs it as `wine-VERSION-x86_64`. The other build needs
  the 32-bit (i686) system libraries; without them wineboot fails with
  "could not load kernel32.dll". A build of that kind that LSIW 1.4.0 or
  older downloaded is replaced by the WoW64 build of the same version the
  next time the Wine prefix is set up, or by `lsiw runners install wine`.
- Wine 11.20 is skipped: it asks for 32 GB of memory at start and
  wine-preloader crashes when Linux refuses, which happens whenever RAM
  plus swap is below 32 GB. If a Wine build LSIW downloaded crashes while
  the prefix is set up, LSIW installs the release before it, makes it the
  configured Wine and tries again; `lsiw wine-setup` prints the memory,
  overcommit and SELinux settings that usually explain such a crash.
- Where each Proton build comes from:

  | ID | Build | From | Checksum |
  |---|---|---|---|
  | `ge` | GE-Proton | GloriousEggroll/proton-ge-custom | SHA-512 |
  | `cachyos` | Proton-CachyOS | CachyOS/proton-cachyos (`cachyos-…-slr` releases, x86-64 build) | SHA-512 |
  | `dwproton` | DW-Proton | dawn-winery/dwproton-mirror | SHA-512 |
  | `rtsp` | Proton-GE RTSP | SpookySkeletons/proton-rtsp | none published |
  | `tkg` | Proton-Tkg | Frogging-Family/wine-tkg-git, the latest `proton-valvexbe-sniper` build through nightly.link (needs `unzip`) | none published |
  | `em` | Proton-EM | Etaash-mathamsetty/Proton (now BananaWorks07/Proton) | when published |
  | `wineland` | Proton-Wineland | nanomatters/proton-cachyos (`cachyos-wineland-…-slr`) | SHA-512 |

  The newest release is found from the project's tags (`git ls-remote`),
  skipping alpha, beta and test builds and tags without an archive; give a
  tag to install that release instead. They are the same builds ProtonPlus
  and ProtonUp-Qt install, and builds installed with those tools in Steam's
  `compatibilitytools.d` get LSIW twins too.
- Downloads go to `~/.local/share/lsiw/runners/{proton,wine}`. They need
  `curl`, `tar` and `sha512sum`, plus `git` when GitHub's API is
  rate-limited.
- If the Steam integration is on, a new or removed Proton updates Steam's
  tool list straight away (restart Steam to see it).

## How each engine uses LSIW's folders

| Engine | Prefix | Folders |
|---|---|---|
| native | none | the variables from LSIW Control, directly |
| wine (any version) | the shared prefix, `~/.local/share/lsiw/shared/pfx` | `C:` is the C: drive (with every Linux folder); no other drive letters; the shell-folder registry points at LSIW's folders |
| proton (`lsiw run`, any version) | the same shared prefix | the same; `drive_c` is a link to the C: drive and `C:\users\steamuser` is your profile |
| LSIW Proton in Steam | the same shared prefix (`[steam] shared_prefix`) | the same |
| a game with `lsiw steam own APPID` | its own `steamapps/compatdata/<appid>` | user folders linked to LSIW's folders (below) |
| a library program with `prefix=own` | `~/.local/share/lsiw/prefixes/ID` | user folders linked to LSIW's folders (below) |

**One prefix for everything:** Wine and Proton, every version, and every
Steam game share one prefix by default: one registry, one set of
installed runtimes (Visual C++, .NET, DirectX redistributables), one C:
drive. The shared folder has the layout of a Steam compatdata folder (Proton
keeps its `version` and `tracked_files` next to `pfx/`).

**Existing game prefixes:** a game that ran before with its own prefix in
`steamapps/compatdata/<appid>` keeps it untouched as a backup. The first
time it starts in the shared prefix, the files of that prefix's user
folders (Documents, Saved Games, AppData, ...) are copied into LSIW's
folders where no file of the same name exists, and `lsiw-imported` is
written into the old compatdata folder. Saves a game keeps elsewhere in its
prefix (rare, for example under `ProgramData`) stay in the old prefix.
`lsiw steam own APPID` puts a game back on its own prefix.

**Every game, new ones too:** `lsiw steam default --all` (Steam closed;
LSIW Control: *Use for every game*) sets Steam's default compatibility tool
(`CompatToolMapping` `"0"` in `config/config.vdf`) to LSIW Proton Stable,
so every game without a tool of its own, including games installed later,
runs through LSIW. `--all` also moves every game set to one of Valve's
Protons, or to another Proton that has an LSIW twin, to that twin. A copy
of `config.vdf` is kept as `config.vdf.lsiw-backup-<date>`; `--dry-run`
only shows what would change.

**Mixing versions:** switching between Wine and Proton versions on the
shared prefix makes the next start update the prefix (a few seconds). Two
different Wine or Proton versions cannot run in the same prefix at the same
time (Wine refuses to start: "wineserver version mismatch"); give one of
them a prefix of its own.

**Wine:** every Wine version shares the same prefix. Switching versions runs
`wineboot` again on the next start, so that version updates the prefix.
Going back to an older Wine after a newer one has updated the prefix can
fail, which is how Wine behaves with any prefix. If that happens, delete the
prefix and LSIW recreates it.

**A prefix of its own (Proton or Wine):** before it starts, these folders
of the prefix's `drive_c/users/steamuser` become links to LSIW's folders:

| Folder in the prefix | LSIW variable |
|---|---|
| Desktop | `CSIDL_DESKTOPDIRECTORY` |
| Documents | `CSIDL_PERSONAL` |
| Downloads | `CSIDL_DOWNLOADS` |
| Music | `CSIDL_MYMUSIC` |
| Pictures | `CSIDL_MYPICTURES` |
| Videos | `CSIDL_MYVIDEO` |
| Favorites | `CSIDL_FAVORITES` |
| Contacts | `CSIDL_CONTACTS` |
| Saved Games | `CSIDL_SAVED_GAMES` |
| AppData/Roaming | `CSIDL_APPDATA` |
| AppData/Local | `CSIDL_LOCAL_APPDATA` |
| AppData/LocalLow | `CSIDL_APPDATA_LOCALLOW` |

The folders of `users/Public` become the `CSIDL_COMMON_*` folders.

- **Saves already in a prefix:** files a game already saved there are moved
  into LSIW's folder. If a file of the same name already exists in LSIW's
  folder, the prefix's copy is kept in "`<folder> (before LSIW)`" and nothing
  is overwritten.
- **Whole filesystem on C:\\:** `[steam] full_c_drive`, on by default, also
  adds every top-level Linux folder to `C:\` (home, mnt, media, …), as
  LSIW's own engine shows it.
- **Z: stays:** Proton itself recreates its `Z:` drive (`/`) on every
  start, so it stays.
- **Inside the runtime container:** the linked folders, and `/mnt`, `/media`,
  `/run/media` and `/srv`, are passed to the Steam Linux Runtime container in
  `STEAM_COMPAT_MOUNTS`, so games inside it can reach them.
- **Deleting a prefix:** LSIW records the links in `<compatdata>/lsiw-links`
  and removes them before Proton deletes a prefix (verb `destroyprefix`), so
  Proton never deletes your files. Steam's own removal of compat data
  leaves symbolic links unfollowed, as it already must for Proton's
  `dosdevices/z:` → `/`.

## Steam integration

LSIW Control › Steam integration, or:

```sh
lsiw steam            # status: Steam installations and LSIW tools
lsiw steam add        # add or update the tools
lsiw steam remove     # remove them
lsiw steam full-c-drive on|off
```

**What `add` does:** every Proton Steam lists gets an LSIW twin, written
to `<Steam>/compatibilitytools.d/LSIW-<name>/`. After a restart, Steam
lists them under a game's
*Properties › Compatibility › Force the use of a specific Steam Play compatibility tool*
and in *Settings › Compatibility*, next to its own versions:

| Steam's entry | LSIW twin |
|---|---|
| Proton Stable | LSIW Proton Stable |
| Proton Experimental | LSIW Proton Experimental |
| Proton 11.0-2 | LSIW Proton 11.0-2 |
| Proton 10.0-4 | LSIW Proton 10.0-4 |
| Proton 9.0-4 | LSIW Proton 9.0-4 |
| Proton 8.0-5 | LSIW Proton 8.0-5 |
| Proton 7.0-6 | LSIW Proton 7.0-6 |
| Proton 6.3, 5.13, 5.0, 4.11, 4.2, 3.16, 3.7 | LSIW Proton 6.3-8, … LSIW Proton 3.7-8 |
| Proton Hotfix | LSIW Proton Hotfix |
| Proton Next (while Valve offers it) | LSIW Proton Next |
| GE-Proton11-11, Proton-CachyOS, DW-Proton, … | LSIW GE-Proton11-11, LSIW proton-cachyos-…, … |

- **Valve's versions:** LSIW reads the list from Steam's own app cache
  (`appcache/appinfo.vdf`, the "Steam Play 2.0 Manifests"), so the names
  and versions are the ones your Steam shows, and new versions appear after
  `lsiw steam refresh`. If the cache cannot be read, LSIW uses the list
  Valve published in October 2026.
- **Not installed yet:** a twin of one of Valve's versions does not need it
  installed. The first time a game starts with the twin, LSIW asks Steam to
  install that Proton (and the Steam Linux Runtime it needs) and shows a
  notification. Start the game again when the download has finished: Steam
  does not download while a game is running.
- **LSIW Proton Stable** follows the version Steam currently calls stable
  (the tool with the alias `proton-stable`), as Steam's own entry does.
- **Older versions:** Proton 6.3 back to 3.7 and Proton Hotfix get a twin
  too. With `[steam] legacy_versions = false` they get one only when
  installed.
- **Proton Next** is in Steam's list only while Valve tests a new version;
  it gets a twin whenever Steam lists it or has it installed.
- **Proton BattlEye Runtime and Proton EasyAntiCheat Runtime** are not
  compatibility tools (Steam never offers them in the list): Steam
  installs them for games that use BattlEye or Easy Anti-Cheat and passes
  them to Proton. They work the same under every LSIW twin.
- **Proton Experimental (ARM64)** gets a twin on ARM computers only.
- **Other processors:** builds made for another processor, such as an
  `-aarch64` GE-Proton on an x86-64 PC, get no twin. `lsiw runners` marks
  them, and `lsiw runners remove ID` deletes them.

**When a game starts with one:**
1. Steam runs the tool's `lsiw-proton` script, which runs
   `lsiw steam-run`.
2. LSIW links the game's prefix as described above.
3. LSIW starts the real Proton with Steam's command line, inside the Steam
   Linux Runtime that this Proton asks for (`require_tool_appid` in its
   `toolmanifest.vdf`).

Apart from the folders, the game runs exactly as with the plain Proton of
the same version. The plain Proton versions stay in the list, unchanged.

**What else to know:**
- **Restart Steam** after `add` or `remove`. A game set to an LSIW tool that
  was removed falls back to Steam's default.
- **Flatpak and Snap Steam are not supported:** their sandbox cannot start
  programs installed outside it, so LSIW skips them with a message. Use
  Steam from Fedora's packages (`sudo dnf install steam` from RPM Fusion).
- **Steam installed somewhere unusual:** set `[steam] root = /path/to/Steam`.
- **If the runtime is missing:** for Steam games, LSIW asks Steam to install
  the Steam Linux Runtime a Proton needs, as above. With `lsiw run -e proton`
  outside Steam, Proton starts without it and LSIW prints a warning.
- **Logs:** set `LSIW_VERBOSE=1` in a game's launch options
  (`LSIW_VERBOSE=1 %command%`) to log what LSIW does, and `PROTON_LOG=1` as
  usual for Proton's own log.

## `lsiw run -e proton` outside Steam

- **Which Proton:** GE-Proton, and other builds made for umu-launcher, run
  programs without the Steam client. LSIW turns on their umu mode
  (`UMU_ID=umu-0`). Valve's Proton starts programs through the Steam
  client, so outside Steam it works only when Steam is installed.
- **Console output:** a console program's output appears in Proton's console
  window, not in the terminal. The exit code is passed back.
- **Paths:** Linux paths given as arguments are passed as the `C:\…` path
  the program sees (in a prefix of its own, `Z:\…` with `full_c_drive`
  off).

## Configuration keys

| Key | Values | Default |
|---|---|---|
| `[engine] default` | `auto`, `native`, `wine`, `proton`, `custom` | `auto` |
| `[engine] chain` | up to 4 of `native`, `auto`, `wine[:ID]`, `proton[:ID]`, comma-separated: the engines `custom` tries in order | `native,wine,proton` |
| `[drive] c_drive` | the C: drive folder | `${HOME}/.cdrive` |
| `[prefix] shared` | the shared prefix folder | `${DATA}/shared` |
| `[steam] shared_prefix` | `true`, `false`: Steam games run in the shared prefix | `true` |
| `[steam] own_prefix` | app IDs that keep their own prefix, comma-separated | |
| `[steam] default_tool` | written by `lsiw steam default` | |
| `[engine] wine` | version ID, `wine` path or command | `wine` from `$PATH` |
| `[engine] proton` | version ID or Proton folder | newest official Proton |
| `[wine] custom` | `;`-separated `wine` paths | |
| `[proton] custom` | `;`-separated Proton folders | |
| `[steam] integration` | `true`, `false` (written by `lsiw steam add/remove`) | `false` |
| `[steam] full_c_drive` | `true`, `false` (prefixes of their own; the shared prefix always has the C: drive) | `true` |
| `[steam] legacy_versions` | `true`, `false`: twins of Proton 6.3 and older, and Hotfix, even when not installed | `true` |
| `[steam] root` | Steam folder | found automatically |
