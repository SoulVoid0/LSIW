# LSIW - Linux Subsystem for Improved Wine
#
# GNU make build for `lsiw` (the command-line runtime, native engine
# included) and `lsiw-control` (the GTK 4 settings app).
#
#   make                       lsiw, plus lsiw-control when GTK 4 is available
#   make GUI=no                lsiw only
#   make test-binaries check   build the Windows test programs, run the tests
#   sudo make install          install under /usr/local (PREFIX, DESTDIR, ...)
#   make dist                  lsiw-$(VERSION).tar.gz
#   make rpm                   Fedora packages under build/rpmbuild
#   make deb                   Debian/Ubuntu packages under build/deb
#   make appimage              LSIW-$(VERSION)-x86_64.AppImage under build/appimage
#
# SPDX-License-Identifier: AGPL-3.0-or-later

.SUFFIXES:
.DELETE_ON_ERROR:

VERSION := $(shell grep -m1 'define LSIW_VERSION' include/lsiw.h 2>/dev/null | cut -d'"' -f2)
ifeq ($(VERSION),)
$(error cannot read LSIW_VERSION from include/lsiw.h)
endif

# ---------------------------------------------------------------------------
# Installation directories (GNU-style names; all can be overridden).
# ---------------------------------------------------------------------------
PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
LIBEXECDIR  ?= $(PREFIX)/libexec
DATADIR     ?= $(PREFIX)/share
SYSCONFDIR  ?= /etc
BINFMTDIR   ?= $(PREFIX)/lib/binfmt.d
TMPFILESDIR ?= $(PREFIX)/lib/tmpfiles.d
APPDIR      ?= $(DATADIR)/applications
MIMEDIR     ?= $(DATADIR)/mime/packages
TEMPLATEDIR ?= $(DATADIR)/templates
ICONDIR     ?= $(DATADIR)/icons/hicolor/scalable/apps
MIMEICONDIR ?= $(DATADIR)/icons/hicolor/scalable/mimetypes
THUMBNAILERDIR ?= $(DATADIR)/thumbnailers
METAINFODIR ?= $(DATADIR)/metainfo

# ---------------------------------------------------------------------------
# Tools and flags
# ---------------------------------------------------------------------------
PKG_CONFIG      ?= pkg-config
INSTALL         ?= install
INSTALL_PROGRAM ?= $(INSTALL) -m 0755
INSTALL_DATA    ?= $(INSTALL) -m 0644
RPMBUILD        ?= rpmbuild
BUILDDIR        ?= build

CFLAGS ?= -O2 -g
# "-D_GNU_SOURCE=" (empty) matches the bare "#define _GNU_SOURCE" at the top
# of the sources, so the redefinition is identical and silent.
LSIW_CPPFLAGS := -D_GNU_SOURCE= -Iinclude -Isrc/native
LSIW_CFLAGS   := -std=gnu11 -Wall -Wextra -pthread -fno-strict-aliasing
# Appended after the user's CFLAGS so distribution defaults cannot undo them:
#  -fno-lto: seh.c and crt_core.c define entry points in top-level assembly
#   that call back into C; link-time optimisation cannot see those
#   references and may discard or localise the C functions.
#  -fcf-protection=branch: keeps IBT landing pads but leaves out the CET
#   shadow-stack marking. The engine switches stacks and contexts for
#   Windows code (SEH unwinding, RtlRestoreContext, fibers, longjmp), which
#   an enforced shadow stack would kill with a control-protection fault.
LSIW_LATE_CFLAGS := -fno-lto -fcf-protection=branch
LSIW_LIBS := -pthread -ldl -lm

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
LSIW_SRCS := $(wildcard src/common/*.c src/native/*.c src/cli/*.c)
GUI_SRCS  := $(wildcard src/common/*.c src/gui/*.c)
LSIW_OBJS := $(LSIW_SRCS:%.c=$(BUILDDIR)/%.o)
GUI_OBJS  := $(GUI_SRCS:%.c=$(BUILDDIR)/%.o)

# The GUI is optional: GUI=auto builds it when src/gui has sources and
# pkg-config finds gtk4, GUI=yes insists on it, GUI=no skips it.
GUI ?= auto
BUILD_GUI :=
GUI_NOTE  :=
ifeq ($(GUI),auto)
  ifeq ($(wildcard src/gui/*.c),)
    GUI_NOTE := src/gui has no sources yet, so lsiw-control was not built.
  else ifeq ($(shell $(PKG_CONFIG) --exists gtk4 2>/dev/null && echo yes),yes)
    BUILD_GUI := yes
  else
    GUI_NOTE := GTK 4 development files were not found (pkg-config gtk4), so lsiw-control was not built. Install gtk4-devel (Fedora) or libgtk-4-dev (Debian/Ubuntu), or pass GUI=no to silence this note.
  endif
else ifeq ($(GUI),yes)
  BUILD_GUI := yes
else ifneq ($(GUI),no)
  $(error GUI must be auto, yes or no (got '$(GUI)'))
endif

ifeq ($(BUILD_GUI),yes)
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk4 2>/dev/null)
GTK_LIBS   := $(shell $(PKG_CONFIG) --libs gtk4 2>/dev/null)
endif
GUI_LIBS := $(GTK_LIBS) -pthread -lm

# Quote a string for use inside '...' in a recipe.
sq = $(subst ','\'',$(1))

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
.PHONY: all gui-note check-arch check-gtk

ifeq ($(BUILD_GUI),yes)
all: lsiw lsiw-control
else
all: lsiw gui-note
endif

gui-note: lsiw
	@$(if $(GUI_NOTE),echo 'note: $(call sq,$(GUI_NOTE))',:)

lsiw: $(LSIW_OBJS)
	$(CC) $(LSIW_CFLAGS) $(CFLAGS) $(LSIW_LATE_CFLAGS) $(LDFLAGS) -o $@ $(LSIW_OBJS) $(LDLIBS) $(LSIW_LIBS)

lsiw-control: $(GUI_OBJS) | check-gtk
	$(CC) $(LSIW_CFLAGS) $(CFLAGS) $(LSIW_LATE_CFLAGS) $(LDFLAGS) -o $@ $(GUI_OBJS) $(LDLIBS) $(GUI_LIBS)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(LSIW_CPPFLAGS) $(CPPFLAGS) $(LSIW_CFLAGS) $(CFLAGS) $(LSIW_LATE_CFLAGS) $(OBJ_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/src/gui/%.o: OBJ_CFLAGS = $(GTK_CFLAGS)
$(filter $(BUILDDIR)/src/gui/%,$(GUI_OBJS)): | check-gtk
$(LSIW_OBJS): | check-arch

# The native engine executes Windows x86-64 machine code directly on the
# CPU, so it can only be built for (and run on) x86-64.
check-arch:
	@if [ "$$(printf '' | $(CC) $(CPPFLAGS) $(CFLAGS) -dM -E -x c - 2>/dev/null | grep -c -e '__x86_64__ 1' -e '__LP64__ 1')" != 2 ]; then \
	    echo "error: lsiw must be built for x86-64: its native engine runs Windows x86-64 code" >&2; \
	    echo "error: directly on the CPU, and '$(CC) $(CFLAGS)' does not generate 64-bit x86-64 code" >&2; \
	    echo "error: (compiler target: $$($(CC) -dumpmachine 2>/dev/null))." >&2; \
	    exit 1; \
	fi

check-gtk:
	@if [ -z '$(wildcard src/gui/*.c)' ]; then \
	    echo "error: lsiw-control cannot be built: src/gui has no sources." >&2; exit 1; fi
	@$(PKG_CONFIG) --exists gtk4 2>/dev/null || { \
	    echo "error: lsiw-control needs GTK 4 development files (pkg-config gtk4)." >&2; \
	    echo "error: install gtk4-devel (Fedora) or libgtk-4-dev (Debian/Ubuntu), or build with GUI=no." >&2; \
	    exit 1; }

-include $(sort $(LSIW_OBJS:.o=.d) $(GUI_OBJS:.o=.d))

# ---------------------------------------------------------------------------
# Tests (Windows programs cross-built with MinGW, see tests/)
# ---------------------------------------------------------------------------
.PHONY: test-binaries check

ifneq ($(wildcard tests/Makefile),)
test-binaries:
	$(MAKE) -C tests
else
test-binaries:
	@echo "tests/Makefile not found: no test programs to build."
endif

ifneq ($(wildcard tests/run-tests.sh),)
check: lsiw test-binaries check-cli
	LSIW='$(CURDIR)/lsiw' bash tests/run-tests.sh
else
check: lsiw check-cli
	@echo "tests/run-tests.sh not found: no Windows test programs to run."
endif

# Tests of the lsiw command itself that need no Windows tools.
.PHONY: check-cli
check-cli: lsiw
	LSIW='$(CURDIR)/lsiw' bash tests/lwlnk.sh
	LSIW='$(CURDIR)/lsiw' bash tests/cdrive.sh
	LSIW='$(CURDIR)/lsiw' bash tests/drives.sh
	LSIW='$(CURDIR)/lsiw' bash tests/playtime.sh
	LSIW='$(CURDIR)/lsiw' bash tests/proton.sh
	LSIW='$(CURDIR)/lsiw' bash tests/instances.sh

# How many functions each export table of the native engine provides
# (table entries; see docs/COMPATIBILITY.md).
.PHONY: export-stats
export-stats:
	@echo "Export table entries in src/native:"
	@awk ' \
	/^(static )?const struct lsiw_export lsiw_[a-z0-9_]+_exports\[\] *= *\{/ { \
	    match($$0, /lsiw_[a-z0-9_]+_exports/); name = substr($$0, RSTART, RLENGTH); inside = 1; n = 0; next } \
	inside { \
	    line = $$0; sub(/\/\/.*/, "", line); gsub(/\/\*[^*]*\*\//, "", line); \
	    n += 3 * gsub(/(^|[^A-Za-z0-9_])EMF\(/, " ", line); \
	    n += gsub(/(^|[^A-Za-z0-9_])(EM|EX|E)\(/, " ", line); \
	    n += gsub(/\{ *"[^"]+" *,/, " ", line); \
	    if (line ~ /^ *\} *;/ || line ~ /\{ *(NULL|0) *, *(NULL|0) *\}/) { \
	        f = FILENAME; sub(/.*\//, "", f); \
	        printf "  %-28s %5d   %s\n", name, n, f; total += n; inside = 0 } } \
	END { printf "  %-28s %5d\n", "all tables", total }' $(wildcard src/native/*.c)

# ---------------------------------------------------------------------------
# Install / uninstall
# ---------------------------------------------------------------------------
.PHONY: install install-lsiw install-gui uninstall

# binfmt_misc starts this name for every MZ file; it is the lsiw binary
# under another name (lsiw treats argv[0] "lsiw-binfmt" as `lsiw run`).
BINFMT_LINK = $(shell realpath -s -m --relative-to='$(LIBEXECDIR)/lsiw' '$(BINDIR)/lsiw' 2>/dev/null || echo ../../bin/lsiw)

ifeq ($(BUILD_GUI),yes)
install: install-lsiw install-gui
else
install: install-lsiw
endif

install-lsiw: lsiw
	$(INSTALL) -d '$(DESTDIR)$(BINDIR)' '$(DESTDIR)$(LIBEXECDIR)/lsiw' '$(DESTDIR)$(BINFMTDIR)' \
	    '$(DESTDIR)$(APPDIR)' '$(DESTDIR)$(MIMEDIR)' '$(DESTDIR)$(ICONDIR)' '$(DESTDIR)$(SYSCONFDIR)/lsiw' \
	    '$(DESTDIR)$(MIMEICONDIR)' '$(DESTDIR)$(THUMBNAILERDIR)'
	$(INSTALL_PROGRAM) lsiw '$(DESTDIR)$(BINDIR)/lsiw'
	ln -sfn '$(BINFMT_LINK)' '$(DESTDIR)$(LIBEXECDIR)/lsiw/lsiw-binfmt'
	@# The binfmt rule names the interpreter by absolute path: use LIBEXECDIR.
	sed 's|/usr/libexec/lsiw/lsiw-binfmt|$(LIBEXECDIR)/lsiw/lsiw-binfmt|' data/zz-lsiw.conf > '$(DESTDIR)$(BINFMTDIR)/zz-lsiw.conf'
	chmod 0644 '$(DESTDIR)$(BINFMTDIR)/zz-lsiw.conf'
	$(INSTALL) -d '$(DESTDIR)$(TMPFILESDIR)'
	$(INSTALL_DATA) data/lsiw-tmpfiles.conf '$(DESTDIR)$(TMPFILESDIR)/lsiw.conf'
	$(INSTALL_DATA) data/lsiw.desktop '$(DESTDIR)$(APPDIR)/lsiw.desktop'
	$(INSTALL_DATA) data/lsiw.xml '$(DESTDIR)$(MIMEDIR)/lsiw.xml'
	$(INSTALL) -d '$(DESTDIR)$(TEMPLATEDIR)/.source'
	$(INSTALL_DATA) data/lsiw-windows-link.desktop '$(DESTDIR)$(TEMPLATEDIR)/lsiw-windows-link.desktop'
	$(INSTALL_DATA) data/windows-link.lnk '$(DESTDIR)$(TEMPLATEDIR)/.source/lsiw-windows-link.lnk'
	$(INSTALL_DATA) data/icons/hicolor/scalable/apps/lsiw.svg '$(DESTDIR)$(ICONDIR)/lsiw.svg'
	$(INSTALL_DATA) data/icons/hicolor/scalable/mimetypes/application-x-lsiw-shortcut.svg '$(DESTDIR)$(MIMEICONDIR)/application-x-lsiw-shortcut.svg'
	@# Shows each LSIW shortcut's (.lwlnk) own icon in file managers.
	sed 's|^TryExec=lsiw$$|TryExec=$(BINDIR)/lsiw|; s|^Exec=lsiw |Exec=$(BINDIR)/lsiw |' data/lsiw-lwlnk.thumbnailer > '$(DESTDIR)$(THUMBNAILERDIR)/lsiw-lwlnk.thumbnailer'
	chmod 0644 '$(DESTDIR)$(THUMBNAILERDIR)/lsiw-lwlnk.thumbnailer'
	@# Never overwrite a configuration the administrator has edited.
	@if [ -e '$(DESTDIR)$(SYSCONFDIR)/lsiw/lsiw.conf' ] && ! cmp -s data/lsiw.conf '$(DESTDIR)$(SYSCONFDIR)/lsiw/lsiw.conf'; then \
	    echo "keeping the existing $(DESTDIR)$(SYSCONFDIR)/lsiw/lsiw.conf"; \
	else \
	    echo "$(INSTALL_DATA) data/lsiw.conf '$(DESTDIR)$(SYSCONFDIR)/lsiw/lsiw.conf'"; \
	    $(INSTALL_DATA) data/lsiw.conf '$(DESTDIR)$(SYSCONFDIR)/lsiw/lsiw.conf'; \
	fi
	@# On a live install (no DESTDIR) refresh the desktop caches and register
	@# the .exe handler with the kernel right away; packages do this in %post.
	@if [ -z '$(DESTDIR)' ]; then \
	    update-desktop-database -q '$(APPDIR)' 2>/dev/null || true; \
	    update-mime-database '$(DATADIR)/mime' >/dev/null 2>&1 || true; \
	    gtk-update-icon-cache -q -t '$(DATADIR)/icons/hicolor' 2>/dev/null || true; \
	    if [ -x /usr/lib/systemd/systemd-binfmt ]; then \
	        /usr/lib/systemd/systemd-binfmt '$(BINFMTDIR)/zz-lsiw.conf' 2>/dev/null \
	            && echo "registered the binfmt_misc handler for Windows programs" \
	            || echo "note: could not register binfmt_misc now (not root?); it is registered at the next boot"; \
	    fi; \
	    systemd-tmpfiles --create '$(TMPFILESDIR)/lsiw.conf' 2>/dev/null || true; \
	fi

install-gui: lsiw-control
	$(INSTALL) -d '$(DESTDIR)$(BINDIR)' '$(DESTDIR)$(APPDIR)' '$(DESTDIR)$(METAINFODIR)'
	$(INSTALL_PROGRAM) lsiw-control '$(DESTDIR)$(BINDIR)/lsiw-control'
	$(INSTALL_DATA) data/lsiw-control.desktop '$(DESTDIR)$(APPDIR)/lsiw-control.desktop'
	$(INSTALL_DATA) data/io.github.lsiw.Control.metainfo.xml '$(DESTDIR)$(METAINFODIR)/io.github.lsiw.Control.metainfo.xml'
	@if [ -z '$(DESTDIR)' ]; then update-desktop-database -q '$(APPDIR)' 2>/dev/null || true; fi

uninstall:
	@if [ -z '$(DESTDIR)' ] && [ -e /proc/sys/fs/binfmt_misc/LSIW ]; then \
	    echo "unregistering the LSIW binfmt_misc handler"; \
	    echo -1 > /proc/sys/fs/binfmt_misc/LSIW || true; \
	fi
	rm -f '$(DESTDIR)$(BINDIR)/lsiw' '$(DESTDIR)$(BINDIR)/lsiw-control' \
	    '$(DESTDIR)$(LIBEXECDIR)/lsiw/lsiw-binfmt' '$(DESTDIR)$(BINFMTDIR)/zz-lsiw.conf' \
	    '$(DESTDIR)$(TMPFILESDIR)/lsiw.conf' \
	    '$(DESTDIR)$(APPDIR)/lsiw.desktop' '$(DESTDIR)$(APPDIR)/lsiw-control.desktop' \
	    '$(DESTDIR)$(MIMEDIR)/lsiw.xml' '$(DESTDIR)$(ICONDIR)/lsiw.svg' \
	    '$(DESTDIR)$(MIMEICONDIR)/application-x-lsiw-shortcut.svg' \
	    '$(DESTDIR)$(THUMBNAILERDIR)/lsiw-lwlnk.thumbnailer' \
	    '$(DESTDIR)$(TEMPLATEDIR)/lsiw-windows-link.desktop' \
	    '$(DESTDIR)$(TEMPLATEDIR)/.source/lsiw-windows-link.lnk' \
	    '$(DESTDIR)$(METAINFODIR)/io.github.lsiw.Control.metainfo.xml'
	@rmdir '$(DESTDIR)$(LIBEXECDIR)/lsiw' 2>/dev/null || true
	@# The system configuration is removed only if nobody changed it.
	@if cmp -s data/lsiw.conf '$(DESTDIR)$(SYSCONFDIR)/lsiw/lsiw.conf'; then \
	    rm -f '$(DESTDIR)$(SYSCONFDIR)/lsiw/lsiw.conf'; rmdir '$(DESTDIR)$(SYSCONFDIR)/lsiw' 2>/dev/null || true; \
	elif [ -e '$(DESTDIR)$(SYSCONFDIR)/lsiw/lsiw.conf' ]; then \
	    echo "keeping the modified $(DESTDIR)$(SYSCONFDIR)/lsiw/lsiw.conf"; \
	fi
	@if [ -z '$(DESTDIR)' ]; then \
	    update-desktop-database -q '$(APPDIR)' 2>/dev/null || true; \
	    update-mime-database '$(DATADIR)/mime' >/dev/null 2>&1 || true; \
	    gtk-update-icon-cache -q -t '$(DATADIR)/icons/hicolor' 2>/dev/null || true; \
	fi
	@echo "Per-user data is left alone: ~/.config/lsiw and ~/.local/share/lsiw."

# ---------------------------------------------------------------------------
# Clean, dist, rpm
# ---------------------------------------------------------------------------
.PHONY: clean dist rpm deb appimage

clean:
	rm -rf $(BUILDDIR) lsiw lsiw-control lsiw-*.tar.gz
ifneq ($(wildcard tests/Makefile),)
	-$(MAKE) -C tests clean
endif

DIST_NAME    := lsiw-$(VERSION)
DIST_TARBALL := $(DIST_NAME).tar.gz

# From git: the committed tree plus uncommitted changes to tracked files
# (`git stash create` snapshots them without touching the work tree).
# Without git: the directory as it is, minus build output.
dist:
	@rm -f $(DIST_TARBALL)
	@if command -v git >/dev/null 2>&1 && [ "$$(git rev-parse --show-toplevel 2>/dev/null)" = "$$(pwd -P)" ]; then \
	    rev=$$(git -c user.name=lsiw -c user.email=lsiw@localhost stash create 2>/dev/null); rev=$${rev:-HEAD}; \
	    echo "git archive --format=tar.gz --prefix=$(DIST_NAME)/ -o $(DIST_TARBALL) $$rev"; \
	    git archive --format=tar.gz --prefix=$(DIST_NAME)/ -o $(DIST_TARBALL).tmp $$rev && \
	        mv $(DIST_TARBALL).tmp $(DIST_TARBALL) || { rm -f $(DIST_TARBALL).tmp; exit 1; }; \
	    untracked=$$(git ls-files --others --exclude-standard); \
	    if [ -n "$$untracked" ]; then \
	        echo "warning: these files are not in git and were left out (git add them):"; \
	        echo "$$untracked" | sed 's/^/    /'; \
	    fi; \
	else \
	    mkdir -p $(BUILDDIR); \
	    echo "tar -czf $(DIST_TARBALL) (no git repository)"; \
	    tar --exclude-vcs --exclude-vcs-ignores --exclude='./$(BUILDDIR)' \
	        --exclude='./lsiw' --exclude='./lsiw-control' --exclude='./lsiw-*.tar.gz' \
	        --sort=name --owner=0 --group=0 --numeric-owner \
	        --transform='flags=r;s,^\.,$(DIST_NAME),' -czf $(BUILDDIR)/$(DIST_TARBALL) . && \
	        mv $(BUILDDIR)/$(DIST_TARBALL) $(DIST_TARBALL); \
	fi
	@echo "wrote $(DIST_TARBALL)"

RPM_TOPDIR := $(CURDIR)/$(BUILDDIR)/rpmbuild
SPEC_VERSION = $(shell sed -n 's/^Version:[[:space:]]*//p' lsiw.spec)

rpm: dist
	@if [ '$(SPEC_VERSION)' != '$(VERSION)' ]; then \
	    echo "error: lsiw.spec says Version $(SPEC_VERSION) but include/lsiw.h says $(VERSION)" >&2; exit 1; \
	fi
	mkdir -p '$(RPM_TOPDIR)/SOURCES'
	cp $(DIST_TARBALL) '$(RPM_TOPDIR)/SOURCES/'
	$(RPMBUILD) -ba --define '_topdir $(RPM_TOPDIR)' lsiw.spec
	@echo "packages:"; find '$(RPM_TOPDIR)/RPMS' '$(RPM_TOPDIR)/SRPMS' -name '*.rpm' | sed 's/^/    /'

# Debian/Ubuntu: lsiw and (with the GUI) lsiw-control .deb files, split
# like the RPMs. Needs dpkg-dev. See packaging/deb/mkdeb.sh.
DEB_STAGE := $(BUILDDIR)/deb-stage

deb: all
	rm -rf '$(DEB_STAGE)'
	$(MAKE) install DESTDIR='$(CURDIR)/$(DEB_STAGE)' PREFIX=/usr LIBEXECDIR=/usr/libexec SYSCONFDIR=/etc
	sh packaging/deb/mkdeb.sh '$(DEB_STAGE)' '$(VERSION)' '$(BUILDDIR)/deb'

# AppImage: one file with lsiw, LSIW Control and the GTK 4 libraries it
# needs. Downloads linuxdeploy and its GTK plugin on first use (or set
# LINUXDEPLOY). See packaging/appimage/mkappimage.sh.
APPIMAGE_STAGE := $(BUILDDIR)/appimage-stage

appimage: all
	rm -rf '$(APPIMAGE_STAGE)'
	$(MAKE) install DESTDIR='$(CURDIR)/$(APPIMAGE_STAGE)' PREFIX=/usr LIBEXECDIR=/usr/libexec SYSCONFDIR=/etc
	sh packaging/appimage/mkappimage.sh '$(APPIMAGE_STAGE)' '$(VERSION)' '$(BUILDDIR)/appimage'
