# Building LSIW

LSIW builds with GNU make and a C11 compiler. The native engine runs
Windows x86-64 machine code directly on the processor, so `lsiw` can only
be built for, and run on, **x86-64** Linux. The build stops with a clear
message on other architectures.

## Dependencies

| Purpose                         | Fedora                                       | Debian / Ubuntu                                     |
|---------------------------------|----------------------------------------------|-----------------------------------------------------|
| `lsiw` (required)               | `gcc make`                                   | `gcc make` (or `build-essential`)                   |
| `lsiw-control` (GTK 4 GUI)      | `gtk4-devel pkgconf-pkg-config`              | `libgtk-4-dev pkg-config`                           |
| Windows test programs           | `mingw64-gcc mingw64-gcc-c++`                | `gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64`         |
| Wine fallback engine (runtime)  | `wine`                                       | `wine` (or `wine64`)                                |
| Error dialogs from the desktop  | `zenity`                                     | `zenity`                                            |
| Checks used by the RPM build    | `desktop-file-utils libappstream-glib`       | `desktop-file-utils appstream-util`                 |
| RPM packaging (`make rpm`)      | `rpm-build systemd-rpm-macros git`           | `rpm` (`rpmbuild --nodeps`)                         |
| Debian packaging (`make deb`)   | `dpkg dpkg-dev`                              | `dpkg-dev`                                          |
| AppImage (`make appimage`)      | `curl file binutils` (fetches linuxdeploy)   | `curl file binutils` (fetches linuxdeploy)          |

Fedora, everything at once:

```sh
sudo dnf install gcc make gtk4-devel pkgconf-pkg-config mingw64-gcc mingw64-gcc-c++ \
                 wine zenity rpm-build systemd-rpm-macros desktop-file-utils libappstream-glib git
```

or only what the package build needs, read from the spec file:

```sh
sudo dnf install 'dnf-command(builddep)'
sudo dnf builddep lsiw.spec
```

Debian / Ubuntu:

```sh
sudo apt install build-essential pkg-config libgtk-4-dev \
                 gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 wine zenity
```

## Building

```sh
make            # lsiw, and lsiw-control when GTK 4 is found
make -j"$(nproc)"
```

The binaries are written to the top of the source tree (`./lsiw`,
`./lsiw-control`); object and dependency files go to `build/`.

Make variables:

| Variable   | Default        | Meaning                                                        |
|------------|----------------|----------------------------------------------------------------|
| `GUI`      | `auto`         | `auto`: build `lsiw-control` if `src/gui` has sources and `pkg-config gtk4` succeeds, otherwise print a note. `yes`: require it. `no`: skip it. |
| `CC`       | `cc`           | C compiler (GCC or Clang)                                      |
| `CFLAGS`   | `-O2 -g`       | Optimisation and debugging flags                               |
| `CPPFLAGS`, `LDFLAGS`, `LDLIBS` | empty | Extra preprocessor, linker flags, libraries       |
| `BUILDDIR` | `build`        | Where objects go                                               |
| `PKG_CONFIG` | `pkg-config` | Used to find GTK 4                                             |

The build always adds `-std=gnu11 -Wall -Wextra -pthread
-fno-strict-aliasing` and the include paths, and appends two flags after
your `CFLAGS` because the engine depends on them:

* `-fno-lto`: the exception and `setjmp` code defines entry points in
  top-level assembly that call back into C; link-time optimisation cannot
  see those references.
* `-fcf-protection=branch`: keeps Intel CET's indirect-branch tracking but
  leaves out the shadow-stack marking. The engine switches stacks and
  register contexts for Windows code (exception unwinding,
  `RtlRestoreContext`, fibers, `longjmp`), which an enforced shadow stack
  would treat as an attack.

`lsiw` links with `-pthread -ldl -lm`; `lsiw-control` with GTK 4. Changing
`CFLAGS` does not rebuild existing objects on its own: run `make clean`
first.

## Testing

The tests are Windows programs cross-compiled with MinGW-w64 from
`tests/`, run by `lsiw` and compared with their expected output.

```sh
make test-binaries     # build the Windows test programs (needs mingw64-gcc)
make check             # build lsiw and the test programs, run tests/run-tests.sh
```

`make check` runs `tests/run-tests.sh` from the top of the source tree with
`LSIW` set to the absolute path of the freshly built `lsiw`.

A quick manual test:

```sh
x86_64-w64-mingw32-gcc -O2 -o hello.exe tests/src/hello.c
./lsiw check hello.exe
./lsiw run hello.exe one "two three"
LSIW_VERBOSE=1 ./lsiw run hello.exe     # trace loader and engine decisions
```

`make export-stats` prints how many functions each export table of the
native engine provides (the numbers in [COMPATIBILITY.md](COMPATIBILITY.md)).

## Installing from source

```sh
make
sudo make install                 # under /usr/local
sudo make install PREFIX=/usr     # where a distribution package would go
```

| Variable      | Default                  | What goes there                                  |
|---------------|--------------------------|--------------------------------------------------|
| `PREFIX`      | `/usr/local`             | base for the directories below                   |
| `BINDIR`      | `$(PREFIX)/bin`          | `lsiw`, `lsiw-control`                           |
| `LIBEXECDIR`  | `$(PREFIX)/libexec`      | `lsiw/lsiw-binfmt`, a relative symlink to `lsiw` |
| `BINFMTDIR`   | `$(PREFIX)/lib/binfmt.d` | `zz-lsiw.conf` (rewritten to name `LIBEXECDIR`)  |
| `DATADIR`     | `$(PREFIX)/share`        | desktop entries, MIME data, icon, AppStream data |
| `SYSCONFDIR`  | `/etc`                   | `lsiw/lsiw.conf` (an existing, edited file is kept) |
| `DESTDIR`     | empty                    | staging root for packagers                       |

Without `DESTDIR`, `make install` also refreshes the desktop, MIME and icon
caches and registers the binfmt_misc rule right away through
`systemd-binfmt` (otherwise it takes effect at the next boot).
systemd reads `binfmt.d` from `/usr/lib`, `/usr/local/lib`, `/etc` and
`/run`, so both prefixes work.

```sh
sudo make uninstall               # same PREFIX as for install
```

`uninstall` removes the installed files, unregisters the binfmt_misc rule
from the running kernel and removes `/etc/lsiw/lsiw.conf` only if it is
unchanged. Per-user data (`~/.config/lsiw`, `~/.local/share/lsiw`) is never
touched.

## Fedora packages

```sh
make rpm
sudo dnf install ./build/rpmbuild/RPMS/x86_64/lsiw-*.rpm
```

`make rpm` runs `make dist`, copies the tarball into
`build/rpmbuild/SOURCES` and runs `rpmbuild -ba` with `_topdir` set to
`build/rpmbuild`. It stops early if `Version:` in `lsiw.spec` and
`LSIW_VERSION` in `include/lsiw.h` disagree. The results:

* `build/rpmbuild/RPMS/x86_64/lsiw-1.9.1-1.fcNN.x86_64.rpm`: `lsiw`, the
  binfmt rule, desktop integration, `/etc/lsiw/lsiw.conf`, documentation;
* `build/rpmbuild/RPMS/x86_64/lsiw-control-1.6.0-1.fcNN.x86_64.rpm`: the
  settings app;
* the matching debuginfo packages and `build/rpmbuild/SRPMS/lsiw-1.9.1-1.fcNN.src.rpm`.

The package builds with Fedora's standard compiler flags, applies the
binfmt rule in `%post` (`%binfmt_apply`), and removes it from the running
kernel when the package is erased.

## Debian and Ubuntu packages

```sh
make deb
sudo apt install ./build/deb/lsiw_*.deb ./build/deb/lsiw-control_*.deb
```

`make deb` installs into `build/deb-stage` and runs
`packaging/deb/mkdeb.sh`, which splits the tree like the RPMs into
`build/deb/lsiw_VERSION-1_amd64.deb` (with `/etc/lsiw/lsiw.conf` as a
conffile, the binfmt rule applied in `postinst` and removed in `prerm`) and
`build/deb/lsiw-control_VERSION-1_amd64.deb`. The `Depends:` lines come
from `dpkg-shlibdeps`, so they match the system the packages were built on:
build on the oldest release you want to support. The release packages
build `lsiw` on Ubuntu 20.04 (glibc 2.30) and `lsiw-control` on Ubuntu
24.04 (GTK 4.12 is the oldest it builds with).

## AppImage

```sh
make appimage      # build/appimage/LSIW-VERSION-x86_64.AppImage
```

`make appimage` installs into `build/appimage-stage` and runs
`packaging/appimage/mkappimage.sh`, which bundles `lsiw`, LSIW Control and
the GTK 4 libraries with [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy)
and its GTK plugin (downloaded into `build/tools` unless `LINUXDEPLOY`
names an existing one; no FUSE needed). `packaging/appimage/AppRun` starts
LSIW Control without arguments and `lsiw` with arguments, or by the name of
a link to the AppImage (`lsiw`, `lsiw-control`). Inside an AppImage,
`lsiw` writes the `.AppImage` file's path ($APPIMAGE) into desktop entries,
Steam tools and shortcuts, never the temporary mount point.

## Source tarball

```sh
make dist      # lsiw-1.9.1.tar.gz
```

In a git checkout the tarball is made with `git archive` from the current
commit plus any uncommitted changes to tracked files; files git does not
know about are listed and left out (`git add` them first). Outside git it
is a `tar` of the directory without build output.

## Continuous integration

`.github/workflows/build.yml` builds LSIW in a Fedora container, builds the
Windows test programs, runs the tests, validates the desktop and AppStream
files, and builds the RPMs; a second job builds the Debian packages on
Ubuntu 24.04, checks them with lintian and installs them.
