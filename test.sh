/c/ProgramData/chocolatey/lib/winlibs/tools/mingw32/bin/i686-w64-mingw32-g++ -m32 -IK:/Work/CCCaster -IK:/Work/CCCaster/netplay -IK:/Work/CCCaster/lib -IK:/Work/CCCaster/tests -IK:/Work/CCCaster/3rdparty -IK:/Work/CCCaster/sequences -IK:/Work/CCCaster/3rdparty/cereal/include -IK:/Work/CCCaster/3rdparty/gtest/include -IK:/Work/CCCaster/3rdparty/minhook/include -IK:/Work/CCCaster/3rdparty/d3dhook -IK:/Work/CCCaster/3rdparty/framedisplay -IK:/Work/CCCaster/3rdparty/imgui -IK:/Work/CCCaster/3rdparty/imgui/backends -DWIN32_LEAN_AND_MEAN -DWINVER=0x501 -D_WIN32_WINNT=0x501 -D_M_IX86 -DNAMED_PIPE='"\\\\.\\pipe\\cccaster_pipe"' -DNAMED_PIPE2='"\\\\.\\pipe\\cccaster2_pipe"' -DPALETTES_FOLDER='"palettes\\"' -DREADME='"README.md"' -DMBAA_EXE='"MBAA.exe"' -DBINARY='"cccaster.v3.1.beta.1.exe"' -DFOLDER='"cccaster\\"' -DCHANGELOG='"ChangeLog.txt"' -DHOOK_DLL='"cccaster\\hook.beta.1.dll"' -DLAUNCHER='"cccaster\\launcher.exe"' -DUPDATER='"updater.exe"' -DRELAY_LIST='"relay_list.txt"' -DTAG='"beta.1"' -DLOBBY_LIST='"lobby_list.txt"' -s -Os -Ofast -fno-rtti -DNDEBUG -DRELEASE -DDISABLE_LOGGING -DDISABLE_ASSERTS -std=c++11 -o build_release_master/lib/Version.o -c lib/Version.cpp

