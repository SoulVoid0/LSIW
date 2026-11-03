# LSIW test programs: Windows x86-64 executables, cross-built on Linux.
#
#   make            everything that the installed tools can build (= all)
#   make mingw      the C/C++ tests, built with MinGW-w64 (plus clang for __try)
#   make msvc       the MSVC-ABI C++ test, built with clang-cl + lld-link
#   make clean
#
# Outputs go to bin/, intermediate files to build/. A missing tool skips
# the tests that need it with a message instead of failing the build.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

.SUFFIXES:
.DELETE_ON_ERROR:

MINGW_CC   ?= x86_64-w64-mingw32-gcc
MINGW_CXX  ?= x86_64-w64-mingw32-g++
CLANG      ?= clang
# clang-cl is often installed only under a versioned name (clang-cl-18).
CLANG_CL   ?= $(firstword $(foreach c,clang-cl clang-cl-20 clang-cl-19 clang-cl-18 clang-cl-17 clang-cl-16,$(shell command -v $(c) 2>/dev/null)))
LLD_LINK   ?= $(firstword $(foreach c,lld-link lld-link-20 lld-link-19 lld-link-18 lld-link-17,$(shell command -v $(c) 2>/dev/null)))
DLLTOOL    ?= $(firstword $(foreach c,llvm-dlltool llvm-dlltool-20 llvm-dlltool-19 llvm-dlltool-18 llvm-dlltool-17,$(shell command -v $(c) 2>/dev/null)))

BIN   := bin
BUILD := build
SRC   := src

CFLAGS_W   := -O2 -g0 -Wall -Wextra
CFLAGS     := -std=gnu11 $(CFLAGS_W)
CXXFLAGS   := -std=gnu++17 $(CFLAGS_W)
# Link the GCC runtimes statically so a test imports only system DLLs
# (kernel32, msvcrt, ...), never libgcc_s / libstdc++ / libwinpthread DLLs.
CXX_LDFLAGS := -static-libgcc -static-libstdc++ -static

HAVE_MINGW    := $(if $(shell command -v $(MINGW_CC) 2>/dev/null),yes)
HAVE_MINGWXX  := $(if $(shell command -v $(MINGW_CXX) 2>/dev/null),yes)
HAVE_CLANG    := $(if $(shell command -v $(CLANG) 2>/dev/null),yes)
HAVE_MSVC     := $(if $(and $(CLANG_CL),$(LLD_LINK),$(DLLTOOL)),yes)

# The MinGW header folder (the one holding windows.h), for clang.
MINGW_INC = $(shell for d in $$($(MINGW_CC) -xc -E -v - </dev/null 2>&1 | sed -n '/search starts here/,/End of search/p' | grep '^ '); do \
                [ -f "$$d/windows.h" ] && echo "$$d"; done | head -n1)

C_TESTS := t_crt t_files t_env t_threads t_seh t_process t_registry t_net t_drives
LIBS_t_env      := -lshell32 -ladvapi32
LIBS_t_registry := -ladvapi32
LIBS_t_net      := -lws2_32

.PHONY: all mingw msvc clean mingw-c mingw-cxx mingw-ucrt seh-try

all: mingw msvc

# ---------------------------------------------------------------------------
# MinGW-w64
# ---------------------------------------------------------------------------
ifeq ($(HAVE_MINGW),yes)
mingw: mingw-c mingw-ucrt mingw-cxx seh-try
mingw-c: $(BIN)/hello.exe $(C_TESTS:%=$(BIN)/%.exe)
else
mingw:
	@echo "skip: $(MINGW_CC) not found; install mingw64-gcc mingw64-gcc-c++ (Fedora) or gcc-mingw-w64 g++-mingw-w64 (Debian/Ubuntu) to build the MinGW tests."
endif

$(BIN) $(BUILD):
	@mkdir -p $@

$(BIN)/hello.exe: $(SRC)/hello.c | $(BIN)
	$(MINGW_CC) $(CFLAGS) -o $@ $<

$(BIN)/%.exe: $(SRC)/%.c $(SRC)/lsiw_test.h | $(BIN)
	$(MINGW_CC) $(CFLAGS) -o $@ $< $(LIBS_$*)

# The same C runtime test against the Universal CRT (ucrtbase through the
# api-ms-win-crt-* DLLs), as MSVC-built programs use it: MinGW's own specs
# with -lmsvcrt replaced by -lucrt, and -D_UCRT for the headers.
$(BUILD)/ucrt.specs: | $(BUILD)
	$(MINGW_CC) -dumpspecs | sed 's/-lmsvcrt/-lucrt/g' > $@

mingw-ucrt: $(BIN)/t_crt_ucrt.exe
$(BIN)/t_crt_ucrt.exe: $(SRC)/t_crt.c $(SRC)/lsiw_test.h $(BUILD)/ucrt.specs | $(BIN)
	@if [ -f "$$($(MINGW_CC) -print-file-name=libucrt.a)" ]; then \
	    echo "$(MINGW_CC) -specs=$(BUILD)/ucrt.specs -D_UCRT $(CFLAGS) -o $@ $<"; \
	    $(MINGW_CC) -specs=$(BUILD)/ucrt.specs -D_UCRT $(CFLAGS) -o $@ $<; \
	else echo "skip: t_crt_ucrt.exe (this MinGW has no libucrt.a)"; fi

ifeq ($(HAVE_MINGWXX),yes)
mingw-cxx: $(BIN)/t_cpp.exe
$(BIN)/t_cpp.exe: $(SRC)/t_cpp.cpp $(SRC)/lsiw_test.h | $(BIN)
	$(MINGW_CXX) $(CXXFLAGS) -o $@ $< $(CXX_LDFLAGS)
else
mingw-cxx:
	@echo "skip: t_cpp.exe ($(MINGW_CXX) not found)"
endif

# __try/__except/__finally: GCC has no SEH keywords, clang has them for the
# MinGW target with -fms-extensions; -fasync-exceptions makes hardware
# faults inside a __try block catchable. Linked by MinGW gcc.
ifeq ($(HAVE_CLANG),yes)
seh-try: $(BIN)/t_seh_try.exe
$(BIN)/t_seh_try.exe: $(SRC)/t_seh_try.c $(SRC)/lsiw_test.h | $(BIN) $(BUILD)
	@inc='$(MINGW_INC)'; \
	if [ -z "$$inc" ]; then echo "skip: t_seh_try.exe (MinGW headers not found for clang)"; exit 0; fi; \
	cmd="$(CLANG) --target=x86_64-w64-mingw32 -fms-extensions -fasync-exceptions -O1 -Wall -Wextra -isystem $$inc -c -o $(BUILD)/t_seh_try.o $<"; \
	echo "$$cmd"; \
	if $$cmd; then $(MINGW_CC) -o $@ $(BUILD)/t_seh_try.o; \
	else echo "skip: t_seh_try.exe ($(CLANG) cannot compile __try for x86_64-w64-mingw32)"; fi
else
seh-try:
	@echo "skip: t_seh_try.exe ($(CLANG) not found; __try needs clang)"
endif

# ---------------------------------------------------------------------------
# MSVC ABI: clang-cl + lld-link, no Microsoft SDK. Import libraries are made
# from the .def files in msvc/.
# ---------------------------------------------------------------------------
MSVC_DEFS := vcruntime140 kernel32 api-ms-win-crt-heap-l1-1-0
MSVC_LIBS := $(MSVC_DEFS:%=$(BUILD)/msvc/%.lib)

ifeq ($(HAVE_MSVC),yes)
msvc: $(BIN)/msvc_eh.exe

$(BUILD)/msvc/%.lib: msvc/%.def
	@mkdir -p $(@D)
	$(DLLTOOL) -m i386:x86-64 -d $< -l $@

$(BUILD)/msvc/msvc_eh.obj: msvc/msvc_eh.cpp
	@mkdir -p $(@D)
	$(CLANG_CL) /nologo /c /EHsc /GR /O1 /GS- /W4 /Zc:threadSafeInit- /Fo$@ $<

$(BIN)/msvc_eh.exe: $(BUILD)/msvc/msvc_eh.obj $(MSVC_LIBS) | $(BIN)
	$(LLD_LINK) /nologo /entry:mainCRTStartup /subsystem:console /nodefaultlib /out:$@ $^
else
msvc:
	@echo "skip: msvc_eh.exe needs clang-cl, lld-link and llvm-dlltool (Fedora: clang lld llvm; Debian/Ubuntu: clang lld llvm)"
endif

clean:
	rm -rf $(BIN) $(BUILD) out
