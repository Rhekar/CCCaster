# Full version code derived from a git tag (leading 'v' stripped).
# When several tags share the HEAD commit (e.g. v4.0 and v4.0.1), prefer the HIGHEST version
# rather than whatever `git describe` happens to pick first. Fall back to the nearest reachable
# tag, then to a literal when no tag is reachable (e.g. a source export with no git history).
GIT_TAG := $(shell git tag --points-at HEAD --sort=-v:refname 2>/dev/null | head -n1)
GIT_TAG := $(or $(GIT_TAG),$(shell git describe --tags --abbrev=0 2>/dev/null))
GIT_VERSION := $(patsubst v%,%,$(GIT_TAG))
FULL_VERSION := $(or $(GIT_VERSION),3.1.007)

# major.minor: drives the binary/archive filenames and netplay-compat comparison
VERSION := $(shell echo '$(FULL_VERSION)' | cut -d. -f1,2)
# remaining patch part, with its leading dot (empty for tags like v2.1e)
SUFFIX := $(patsubst $(VERSION)%,%,$(FULL_VERSION))
NAME = cccaster
TAG =
BRANCH := $(shell git rev-parse --abbrev-ref HEAD)

ifneq ($(TAG),)
DOT_TAG = .$(TAG)
endif

# Main program files
ARCHIVE = $(NAME).v$(VERSION)$(SUFFIX)$(DOT_TAG).zip
BINARY = $(NAME).v$(VERSION)$(DOT_TAG).exe
FOLDER = $(NAME)
PALETTES_FOLDER = palettes
DLL = hook$(DOT_TAG).dll
LAUNCHER = launcher.exe
UPDATER = updater.exe
DEBUGGER = debugger.exe
GENERATOR = generator.exe
PALETTES = palettes.exe
MBAA_EXE = MBAA.exe
README = README.md
CHANGELOG = ChangeLog.txt
RELAY_LIST = relay_list.txt
LOBBY_LIST = lobby_list.txt

# Library sources
GTEST_CC_SRCS = 3rdparty/gtest/fused-src/gtest/gtest-all.cc
IMGUI_CPP_SRCS = $(wildcard 3rdparty/imgui/*.cpp) 3rdparty/imgui/backends/imgui_impl_win32.cpp 3rdparty/imgui/backends/imgui_impl_dx9.cpp
JLIB_CC_SRCS = $(wildcard 3rdparty/JLib/*.cc)
HOOK_CC_SRCS = $(wildcard 3rdparty/minhook/src/*.cc 3rdparty/d3dhook/*.cc)
HOOK_C_SRCS = $(wildcard 3rdparty/minhook/src/hde32/*.c)
CONTRIB_CC_SRCS = $(GTEST_CC_SRCS) $(JLIB_CC_SRCS)
CONTRIB_CPP_SRCS = $(IMGUI_CPP_SRCS)
CONTRIB_C_SRCS = $(wildcard 3rdparty/*.c)

# Main program sources
LIB_CPP_SRCS = $(wildcard lib/*.cpp)
BASE_CPP_SRCS = $(wildcard netplay/*.cpp) $(LIB_CPP_SRCS) $(wildcard sequences/*.cpp)
MAIN_CPP_SRCS = $(wildcard targets/Main*.cpp tests/*.cpp) $(BASE_CPP_SRCS)
DLL_CPP_SRCS = $(wildcard targets/Dll*.cpp) $(filter-out lib/ConsoleUi.cpp,$(BASE_CPP_SRCS))

NON_GEN_SRCS = \
	$(wildcard netplay/*.cpp tools/*.cpp targets/*.cpp lib/*.cpp tests/*.cpp sequences/*.cpp)
NON_GEN_HEADERS = \
	$(filter-out lib/Version.%.hpp lib/Protocol.%.hpp,$(wildcard netplay/*.hpp targets/*.hpp lib/*.hpp tests/*.hpp sequences/*.hpp))
AUTOGEN_HEADERS = $(wildcard lib/Version.*.hpp lib/Protocol.*.hpp)

# Main program objects
LIB_OBJECTS = $(LIB_CPP_SRCS:.cpp=.o) $(CONTRIB_C_SRCS:.c=.o)
MAIN_OBJECTS = $(MAIN_CPP_SRCS:.cpp=.o) $(CONTRIB_CC_SRCS:.cc=.o) $(CONTRIB_CPP_SRCS:.cpp=.o) $(CONTRIB_C_SRCS:.c=.o)
DLL_OBJECTS = $(DLL_CPP_SRCS:.cpp=.o) $(HOOK_CC_SRCS:.cc=.o) $(HOOK_C_SRCS:.c=.o) $(CONTRIB_C_SRCS:.c=.o) $(CONTRIB_CPP_SRCS:.cpp=.o)

# Tool chain
PREFIX = ../mingw32/bin/i686-w64-mingw32-
GCC = $(PREFIX)gcc
CXX = $(PREFIX)g++
WINDRES = windres
STRIP = strip
TOUCH = touch
ZIP = zip
UNAME := $(shell uname)
$(info VAR=$(UNAME))

# OS specific tools / settings
ifeq ($(OS),Windows_NT)
	CHMOD_X = icacls $@ /grant Everyone:F
	GRANT = icacls $@ /grant Everyone:F
	ASTYLE = 3rdparty/astyle.exe
	OPENGL_HEADERS = /usr/mingw/i686-w64-mingw32/include/GL
else
	WINDRES = $(PREFIX)windres
	STRIP = $(PREFIX)strip
	CHMOD_X = chmod +x $@
	GRANT =
	ASTYLE = 3rdparty/astyle
	TOUCH = $(STRIP)
	OPENGL_HEADERS = /usr/i686-w64-mingw32/include/GL
endif


# Build flags
DEFINES = -DWIN32_LEAN_AND_MEAN -DWINVER=0x600 -D_WIN32_WINNT=0x600 -D_M_IX86
DEFINES += -DNAMED_PIPE='"\\\\.\\pipe\\cccaster_pipe"' -DNAMED_PIPE2='"\\\\.\\pipe\\cccaster2_pipe"' -DPALETTES_FOLDER='"$(PALETTES_FOLDER)\\"' -DREADME='"$(README)"'
DEFINES += -DMBAA_EXE='"$(MBAA_EXE)"' -DBINARY='"$(BINARY)"' -DFOLDER='"$(FOLDER)\\"' -DCHANGELOG='"$(CHANGELOG)"'
DEFINES += -DHOOK_DLL='"$(FOLDER)\\$(DLL)"' -DLAUNCHER='"$(FOLDER)\\$(LAUNCHER)"' -DUPDATER='"$(UPDATER)"'
DEFINES += -DRELAY_LIST='"$(RELAY_LIST)"' -DTAG='"$(TAG)"'
DEFINES += -DLOBBY_LIST='"$(LOBBY_LIST)"'
INCLUDES = -I$(CURDIR) -I$(CURDIR)/netplay -I$(CURDIR)/lib -I$(CURDIR)/tests -I$(CURDIR)/3rdparty -I$(CURDIR)/sequences
INCLUDES += -I$(CURDIR)/3rdparty/cereal/include -I$(CURDIR)/3rdparty/gtest/include -I$(CURDIR)/3rdparty/minhook/include
INCLUDES += -I$(CURDIR)/3rdparty/d3dhook -I$(CURDIR)/3rdparty/framedisplay -I$(CURDIR)/3rdparty/imgui -I$(CURDIR)/3rdparty/imgui/backends
# -include stdint.h: some libstdc++ versions (e.g. CI's MSYS2 toolchain) don't transitively
# pull in the fixed-width int types that ~95 files rely on, so force them into every TU rather
# than hand-adding the include everywhere. Use stdint.h (not the C++-only <cstdint>) because
# CC_FLAGS is shared with the C sources (3rdparty/md5.c, miniz.c, ...); stdint.h works for both
# and still exposes the bare global uint32_t the codebase uses.
CC_FLAGS = -m32 -include stdint.h $(INCLUDES) $(DEFINES)
#	https://gcc.gnu.org/onlinedocs/gcc/x86-Options.html
# 	Intel Celeron 440 is listed as minimum CPU for melty on steam
CC_FLAGS += -mmmx -msse -msse2 -msse3 -mssse3

# Linker flags
LD_FLAGS = -m32 -static -lws2_32 -lpsapi -lwinpthread -lwinmm -lole32 -ldinput -lwininet -ldwmapi -lgdi32 -ldbghelp

# ----- Steamworks (optional Steam connection method) -----
# Disabled unless STEAM_SDK is set to the 'public' dir of the Steamworks SDK, e.g.:
#   make STEAM_SDK=../SteamworksSDK/public
# When set, -DENABLE_STEAM is defined and the 32-bit steam_api import lib is linked.
# steam_api.dll (32-bit, from <sdk>/redistributable_bin/) + a steam_appid.txt containing
# 411370 must be shipped next to MBAA.exe at runtime. All Steam code is #ifdef ENABLE_STEAM
# so builds without STEAM_SDK are byte-for-byte unaffected.
STEAM_SDK ?=
STEAM_LIB =
STEAM_PKG_ROOT = true
STEAM_PKG_FOLDER = true
ifneq ($(STEAM_SDK),)
    DEFINES += -DENABLE_STEAM
    INCLUDES += -I$(STEAM_SDK)
    # MinGW links the MSVC import lib directly (verified). If a future toolchain refuses,
    # generate one with: gendef steam_api.dll && dlltool -d steam_api.def -l libsteam_api.a
    # Linked ONLY into the main exe + hook.dll (see their recipes), NOT the build-time tools
    # (generator/debugger), so those don't acquire a steam_api.dll runtime dependency.
    STEAM_LIB = $(STEAM_SDK)/../redistributable_bin/steam_api.lib
    # Runtime files: steam_api.dll (32-bit) + steam_appid.txt(411370) must sit in the game
    # dir (next to MBAA.exe / the main exe, which is also MBAA.exe's working dir) so both the
    # launcher and the injected hook.dll can load Steam. We drop them next to the main exe and
    # bundle a copy in the cccaster/ folder for the release archive.
    STEAM_DLL = $(STEAM_SDK)/../redistributable_bin/steam_api.dll
    STEAM_PKG_ROOT = cp -f $(STEAM_DLL) ./ && printf '411370' > ./steam_appid.txt
    STEAM_PKG_FOLDER = cp -f $(STEAM_DLL) $(FOLDER)/ && printf '411370' > $(FOLDER)/steam_appid.txt
endif

# ----- Sentry / Glitchtip crash reporting (optional) -----
# Set SENTRY_DSN to your ingest DSN to enable crash/error reporting, e.g.:
#   make SENTRY_DSN='https://<key>@glitchtip.example.com/1'
# Empty (default) => the client is compiled in but disabled at runtime (no-op, no network), so
# builds without a DSN are unaffected. Like the relay IPs, the DSN is baked into the binary; keep
# real DSNs out of git (pass via CLI or source from .env). lib/SentryClient.cpp links into the main
# exe + hook.dll automatically (lib/*.cpp wildcard); the launcher recipe pulls it in explicitly.
SENTRY_DSN ?=
SENTRY_DEFINE = -DSENTRY_DSN='"$(SENTRY_DSN)"'
DEFINES += $(SENTRY_DEFINE)

# Build options
# DEFINES += -DDISABLE_LOGGING
# DEFINES += -DDISABLE_ASSERTS
# DEFINES += -DLOGGER_MUTEXED
# DEFINES += -DJLIB_MUTEXED

# Install after make, set to 0 to disable install after make
INSTALL = 1

# Build type flags
DEBUG_FLAGS = -ggdb3 -O0 -fno-inline -D_GLIBCXX_DEBUG -DDEBUG
ifeq ($(OS),Windows_NT)
	LOGGING_FLAGS = -s -Os -O2 -DLOGGING# -DRELEASE
else
	LOGGING_FLAGS = -s -Os -O2 -DLOGGING
endif
RELEASE_FLAGS = -s -Os -Ofast -fno-rtti -DNDEBUG -DRELEASE -DDISABLE_LOGGING -DDISABLE_ASSERTS

# SYMBOLS=1 keeps DWARF debug info in the optimized logging build so Sentry/crash-report addresses
# can be resolved with scripts/symbolicate.sh. It adds -ggdb (line info) and -fno-omit-frame-pointer
# (clean StackWalk64 stacks), drops -s, and skips the strip step — WITHOUT pulling in the debug
# build's -O0 or -D_GLIBCXX_DEBUG (which alter timing and abort on latent UB). Only affects logging;
# scripts/build-steam.sh passes it by default. Release stays stripped.
SYMBOLS ?= 0
ifeq ($(SYMBOLS),1)
	LOGGING_FLAGS := $(filter-out -s,$(LOGGING_FLAGS)) -ggdb -fno-omit-frame-pointer
	STRIP = touch
endif

# Build type
BUILD_TYPE = build_debug
BUILD_PREFIX = $(BUILD_TYPE)_$(BRANCH)

# Default build target
ifeq ($(OS),Windows_NT)
	DEFAULT_TARGET = debug
else
	DEFAULT_TARGET = logging
endif


all: $(DEFAULT_TARGET)

# target-profile: STRIP = $(TOUCH)
# target-profile: DEFINES += -DNDEBUG -DRELEASE -DDISABLE_LOGGING -DDISABLE_ASSERTS
# target-profile: CC_FLAGS += -O2 -fno-rtti -pg
# target-profile: LD_FLAGS += -pg -lgmon
# target-profile: $(ARCHIVE)


launcher: $(FOLDER)/$(LAUNCHER)
debugger: tools/$(DEBUGGER)
generator: tools/$(GENERATOR)
palettes: $(PALETTES)


$(ARCHIVE): $(BINARY) $(FOLDER)/$(DLL) $(FOLDER)/$(LAUNCHER) $(FOLDER)/$(UPDATER)
$(ARCHIVE): $(FOLDER)/unzip.exe $(FOLDER)/$(README) $(FOLDER)/$(CHANGELOG) $(FOLDER)/trials
	@echo
ifneq (,$(findstring release,$(MAKECMDGOALS)))
		rm -f $(wildcard $(NAME)*.zip)
		$(ZIP) $(ARCHIVE) $^
		$(ZIP) $(ARCHIVE) -j scripts/Add_Handler_Protocol.bat
		$(ZIP) $(ARCHIVE) -j $(RELAY_LIST)
		$(ZIP) $(ARCHIVE) -j $(LOBBY_LIST)
		$(ZIP) $(ARCHIVE) -j vendor/*
		cp -r res/GRP GRP
		$(ZIP) $(ARCHIVE) -r GRP
		$(ZIP) $(ARCHIVE) -r cccaster/trials
		rm -rf GRP
	        $(GRANT)
endif
	echo $(MAKECMDGOALS)

$(BINARY): $(addprefix $(BUILD_PREFIX)/,$(MAIN_OBJECTS)) res/icon.res
	rm -f $(filter-out $(BINARY),$(wildcard $(NAME)*.exe))
	$(CXX) -o $@ $(CC_FLAGS) -Wall -std=c++2a -fconcepts $^ $(LD_FLAGS) $(STEAM_LIB)
	@echo
	$(STRIP) $@
	$(CHMOD_X)
	$(STEAM_PKG_ROOT)
	@echo

$(FOLDER)/$(DLL): $(addprefix $(BUILD_PREFIX)/,$(DLL_OBJECTS)) res/rollback.o targets/CallDraw.s | $(FOLDER)
	$(CXX) -o $@ $(CC_FLAGS) -Wall -std=c++2a -fconcepts $^ -shared $(LD_FLAGS) -ld3dx9 $(STEAM_LIB)
	@echo
	$(STRIP) $@
	$(GRANT)
	$(STEAM_PKG_FOLDER)
	@echo

$(FOLDER)/$(LAUNCHER): tools/Launcher.cpp lib/SentryClient.cpp | $(FOLDER)
	$(CXX) -o $@ $^ -m32 -s -Os -O2 -Wall -static -mwindows -I$(CURDIR)/lib -I$(CURDIR)/3rdparty/cereal/include $(SENTRY_DEFINE) -lwininet -ldbghelp
	@echo
	$(STRIP) $@
	$(CHMOD_X)
	@echo

$(FOLDER)/$(UPDATER): tools/Updater.cpp lib/StringUtils.cpp | $(FOLDER)
	$(CXX) -o $@ $^ -m32 -s -Os -O2 -std=c++2a -fconcepts -I$(CURDIR)/lib -Wall -static -lpsapi
	@echo
	$(STRIP) $@
	$(CHMOD_X)
	@echo

$(FOLDER)/unzip.exe: 3rdparty/unzip.exe | $(FOLDER)
	cp -f $^ $(FOLDER)/

$(FOLDER)/trials: trials | $(FOLDER)
	cp -r $^ $(FOLDER)/

$(FOLDER)/$(README): $(README) | $(FOLDER)
	cp -f $^ $(FOLDER)/

$(FOLDER)/$(CHANGELOG): $(CHANGELOG) | $(FOLDER)
	cp -f $^ $(FOLDER)/

$(FOLDER):
	mkdir -p $@

res/rollback.bin: tools/$(GENERATOR)
ifeq ($(UNAME),Darwin)
	wine tools/$(GENERATOR) $@
else ifeq ($(UNAME),Linux)
	tools/$(GENERATOR) $@
else
	tools/$(GENERATOR) $@
endif
	@echo

res/rollback.o: res/rollback.bin
ifeq ($(UNAME),Darwin)
	$(PREFIX)objcopy -I binary -O elf32-i386 -B i386 $< $@
else
	objcopy -I binary -O elf32-i386 -B i386 $< $@
endif
	@echo

res/icon.res: res/icon.rc res/icon.ico
	$(WINDRES) -F pe-i386 res/icon.rc -O coff -o $@
	@echo


LOGGING_PREFIX = build_logging_$(BRANCH)
DEBUGGER_LIB_OBJECTS = \
	$(addprefix $(LOGGING_PREFIX)/,$(filter-out lib/Version.o lib/LoggerLogVersion.o lib/ConsoleUi.o lib/SteamManager.o lib/SteamSocket.o lib/SteamLobby.o lib/SteamMatchmaking.o,$(LIB_OBJECTS)))

tools/$(DEBUGGER): tools/Debugger.cpp $(DEBUGGER_LIB_OBJECTS)
	$(CXX) -o $@ $(CC_FLAGS) $(LOGGING_FLAGS) -Wall -std=c++2a -fconcepts $^ $(LD_FLAGS) \
	-I$(CURDIR)/3rdparty/distorm3/include -L$(CURDIR)/3rdparty/distorm3 -ldistorm3
	@echo
	$(STRIP) $@
	$(CHMOD_X)
	@echo


GENERATOR_LIB_OBJECTS = \
	$(addprefix $(LOGGING_PREFIX)/,$(filter-out lib/Version.o lib/LoggerLogVersion.o lib/ConsoleUi.o lib/SteamManager.o lib/SteamSocket.o lib/SteamLobby.o lib/SteamMatchmaking.o,$(LIB_OBJECTS)))

tools/$(GENERATOR): tools/Generator.cpp $(GENERATOR_LIB_OBJECTS)
	$(CXX) -o $@ $(CC_FLAGS) $(LOGGING_FLAGS) -Wall -std=c++2a -fconcepts $^ $(LD_FLAGS)
	@echo
	$(STRIP) $@
	$(CHMOD_X)
	@echo


PALETTES_SRC = tools/Palettes.cpp tools/PaletteEditor.cpp netplay/PaletteManager.cpp netplay/CharacterSelect.cpp
PALETTES_SRC += lib/StringUtils.cpp lib/KeyValueStore.cpp

FRAMEDISPLAY_SRC = $(wildcard 3rdparty/framedisplay/*.cc)
FRAMEDISPLAY_OBJECTS = $(FRAMEDISPLAY_SRC:.cc=.o)

FRAMEDISPLAY_INCLUDES = -I$(CURDIR) -I$(CURDIR)/netplay -I$(CURDIR)/lib -I$(CURDIR)/3rdparty/framedisplay
FRAMEDISPLAY_INCLUDES += -I$(CURDIR)/3rdparty/libpng -I$(CURDIR)/3rdparty/libz -I$(CURDIR)/3rdparty/glfw
FRAMEDISPLAY_INCLUDES += -I"$(CURDIR)/3rdparty/AntTweakBar/include" -I$(OPENGL_HEADERS)

# FRAMEDISPLAY_CC_FLAGS = -ggdb3 -O0 -fno-inline -D_GLIBCXX_DEBUG -DDEBUG
FRAMEDISPLAY_CC_FLAGS = -s -Os -Ofast -fno-rtti
FRAMEDISPLAY_CC_FLAGS += -DDISABLE_LOGGING -DDISABLE_SERIALIZATION -DPALETTES_FOLDER='"$(PALETTES_FOLDER)\\"'

FRAMEDISPLAY_LD_FLAGS = -L$(CURDIR)/3rdparty/libpng -L$(CURDIR)/3rdparty/libz -L"$(CURDIR)/3rdparty/AntTweakBar/lib"
FRAMEDISPLAY_LD_FLAGS += -L$(CURDIR)/3rdparty/glfw
FRAMEDISPLAY_LD_FLAGS += -mwindows -static -lmingw32 -lpng -lz -lglfw -lopengl32 -lglu32

3rdparty/framedisplay/%.o: 3rdparty/framedisplay/%.cc
	$(CXX) $(FRAMEDISPLAY_CC_FLAGS) $(FRAMEDISPLAY_INCLUDES) -o $@ -c $<

3rdparty/AntTweakBar/lib/libAntTweakBar.a: $(wildcard 3rdparty/AntTweakBar/src/*.c* 3rdparty/AntTweakBar/src/*.h)
	$(MAKE) --directory=3rdparty/AntTweakBar/src

$(PALETTES): $(PALETTES_SRC) $(FRAMEDISPLAY_OBJECTS) res/palettes.res 3rdparty/AntTweakBar/lib/libAntTweakBar.a
	$(CXX) $(FRAMEDISPLAY_CC_FLAGS) -o $@ $(FRAMEDISPLAY_INCLUDES) -Wall -std=c++2a -fconcepts -C $^ $(FRAMEDISPLAY_LD_FLAGS)
	@echo
	$(STRIP) $@
	$(CHMOD_X)
	@echo

res/palettes.res: res/palettes.rc res/palettes.ico
	$(WINDRES) -F pe-i386 res/palettes.rc -O coff -o $@
	@echo


define make_version
@scripts/make_version $(VERSION)$(SUFFIX) > lib/Version.local.hpp
endef

define make_protocol
@scripts/make_protocol $(NON_GEN_HEADERS)
endef

define make_depend
@scripts/make_depend "$(CXX)" "-m32 $(INCLUDES)"
endef


version:
	$(make_version)

proto:
	$(make_protocol)

reset-proto:
	rm -f lib/ProtocolEnums.hpp
	@$(MAKE) proto

depend: version proto
	$(make_depend)

.depend_$(BRANCH): $(NON_GEN_SRCS) $(NON_GEN_HEADERS)
	$(make_version)
	$(make_protocol)
	$(make_depend)


clean-proto:
	git checkout -- lib/ProtocolEnums.hpp
	rm -f $(AUTOGEN_HEADERS)

clean-res:
	rm -f res/rollback.* res/icon.res
	rm -rf GRP

clean-lib:
	$(MAKE) --directory=3rdparty/AntTweakBar/src clean
	rm -f 3rdparty/framedisplay/*.o

clean-common: clean-proto clean-res clean-lib
	rm -rf tmp*
	rm -rf $(FOLDER)/trials
	rm -f .depend_$(BRANCH) .include_$(BRANCH) *.exe *.zip tools/*.exe \
$(filter-out $(FOLDER)/$(TAG)config.ini $(wildcard $(FOLDER)/*.mappings $(FOLDER)/*.log),$(wildcard $(FOLDER)/*))

clean-debug: clean-common
	rm -rf build_debug_$(BRANCH)

clean-logging: clean-common
	rm -rf build_logging_$(BRANCH)

clean-release: clean-common
	rm -rf build_release_$(BRANCH)

clean: clean-debug clean-logging clean-release

clean-all: clean-debug clean-logging clean-release
	rm -rf .include* .depend* build*


check:
	cppcheck --enable=all $(NON_GEN_SRCS) $(NON_GEN_HEADERS)

trim:
	sed --binary --in-place 's/\\r$$//' $(NON_GEN_SRCS) $(NON_GEN_HEADERS)
	sed --in-place 's/[[:space:]]\\+$$//' $(NON_GEN_SRCS) $(NON_GEN_HEADERS)

format:
	$(ASTYLE)                   	\
    --indent=spaces=4           	\
    --convert-tabs              	\
    --indent-preprocessor       	\
    --indent-switches           	\
    --style=allman              	\
    --max-code-length=120       	\
    --pad-paren                 	\
    --pad-oper                  	\
    --suffix=none               	\
    --formatted                 	\
    --keep-one-line-blocks      	\
    --align-pointer=name        	\
    --align-reference=type      	\
$(filter-out tools/Generator.cpp netplay/CharacterSelect.cpp lib/KeyboardVKeyNames.hpp targets/DllAsmHacks.hpp,\
$(NON_GEN_SRCS) $(NON_GEN_HEADERS))

count:
	@wc -l $(NON_GEN_SRCS) $(NON_GEN_HEADERS) | sort -nr | head -n 10 && echo '    ...'


ifeq (,$(findstring version,$(MAKECMDGOALS)))
ifeq (,$(findstring proto,$(MAKECMDGOALS)))
ifeq (,$(findstring depend,$(MAKECMDGOALS)))
ifeq (,$(findstring clean,$(MAKECMDGOALS)))
ifeq (,$(findstring check,$(MAKECMDGOALS)))
ifeq (,$(findstring trim,$(MAKECMDGOALS)))
ifeq (,$(findstring format,$(MAKECMDGOALS)))
ifeq (,$(findstring count,$(MAKECMDGOALS)))
ifeq (,$(findstring install,$(MAKECMDGOALS)))
ifeq (,$(findstring palettes,$(MAKECMDGOALS)))
-include .depend_$(BRANCH)
endif
endif
endif
endif
endif
endif
endif
endif
endif
endif


pre-build:
	$(make_version)
	$(make_protocol)
	@echo
	@echo ========== Main-build ==========
	@echo

post-build: main-build
	@echo
	@echo ========== Post-build ==========
	@echo
	if [ $(INSTALL) = 1 ] && [ -s ./scripts/install ]; then ./scripts/install; fi;


debug: post-build
logging: post-build
release: post-build
profile: post-build

target-debug: $(ARCHIVE)
target-logging: $(ARCHIVE)
target-release: $(ARCHIVE)


ifneq (,$(findstring logging,$(MAKECMDGOALS)))
main-build: pre-build
	@$(MAKE) --no-print-directory target-logging BUILD_TYPE=build_logging
else
ifneq (,$(findstring release,$(MAKECMDGOALS)))
main-build: pre-build
	@$(MAKE) --no-print-directory target-release BUILD_TYPE=build_release
else
ifneq (,$(findstring profile,$(MAKECMDGOALS)))
main-build: pre-build
	@$(MAKE) --no-print-directory target-profile BUILD_TYPE=build_debug STRIP=touch
else
ifeq ($(DEFAULT_TARGET),logging)
main-build: pre-build
	@$(MAKE) --no-print-directory target-logging BUILD_TYPE=build_logging
else
main-build: pre-build
	@$(MAKE) --no-print-directory target-debug BUILD_TYPE=build_debug STRIP=touch
endif
endif
endif
endif


build_debug_$(BRANCH):
	rsync -a -f"- .git/" -f"- build_*/" -f"+ */" -f"- *" --exclude=".*" . $@

build_debug_$(BRANCH)/%.o: %.cpp | build_debug_$(BRANCH)
	$(CXX) $(CC_FLAGS) $(DEBUG_FLAGS) -Wall -Wempty-body -std=c++2a -fconcepts -o $@ -c $<

build_debug_$(BRANCH)/%.o: %.cc | build_debug_$(BRANCH)
	$(CXX) $(CC_FLAGS) $(DEBUG_FLAGS) -o $@ -c $<

build_debug_$(BRANCH)/%.o: %.c | build_debug_$(BRANCH)
	$(GCC) $(filter-out -fno-rtti,$(CC_FLAGS) $(DEBUG_FLAGS)) -Wno-attributes -o $@ -c $<


build_logging_$(BRANCH):
	rsync -a -f"- .git/" -f"- build_*/" -f"+ */" -f"- *" --exclude=".*" . $@

build_logging_$(BRANCH)/%.o: %.cpp | build_logging_$(BRANCH)
	$(CXX) $(CC_FLAGS) $(LOGGING_FLAGS) -Wall -Wempty-body -std=c++2a -fconcepts -o $@ -c $<

build_logging_$(BRANCH)/%.o: %.cc | build_logging_$(BRANCH)
	$(CXX) $(CC_FLAGS) $(LOGGING_FLAGS) -o $@ -c $<

build_logging_$(BRANCH)/%.o: %.c | build_logging_$(BRANCH)
	$(GCC) $(filter-out -fno-rtti,$(CC_FLAGS) $(LOGGING_FLAGS)) -Wno-attributes -o $@ -c $<


build_release_$(BRANCH):
	rsync -a -f"- .git/" -f"- build_*/" -f"+ */" -f"- *" --exclude=".*" . $@

build_release_$(BRANCH)/%.o: %.cpp | build_release_$(BRANCH)
	$(CXX) $(CC_FLAGS) $(RELEASE_FLAGS) -std=c++2a -fconcepts -o $@ -c $<

build_release_$(BRANCH)/%.o: %.cc | build_release_$(BRANCH)
	$(CXX) $(CC_FLAGS) $(RELEASE_FLAGS) -o $@ -c $<

build_release_$(BRANCH)/%.o: %.c | build_release_$(BRANCH)
	$(GCC) $(filter-out -fno-rtti,$(CC_FLAGS) $(RELEASE_FLAGS)) -Wno-attributes -o $@ -c $<

