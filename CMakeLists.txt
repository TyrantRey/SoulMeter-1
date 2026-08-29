# CMake build for SoulMeter - used to cross-compile the Windows binaries from
# Linux/macOS with llvm-mingw (see README "Building on Linux"). The Visual
# Studio solution remains the primary build on Windows; keep the source lists
# here in sync with the .vcxproj files.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw.cmake
#   cmake --build build --target dist
#
# `dist` stages the same package layout as the MSBuild release
# (build/package/ and build/SoulMeter-<version>.zip).
cmake_minimum_required(VERSION 3.21)
project(SoulMeter LANGUAGES C CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "SoulMeter is a Windows program; configure with cmake/toolchain-llvm-mingw.cmake to cross-compile it.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(METER_DIR "${CMAKE_SOURCE_DIR}/Soulworker Utility")
set(HOOK_DIR  "${CMAKE_SOURCE_DIR}/SoulMeterHook")
set(THIRD     "${METER_DIR}/Third Party")

# Version: the single definition lives in SWConfig.h.
file(READ "${METER_DIR}/SWConfig.h" _swconfig)
foreach(_part MAJORNUMBER MINORNUMBER BUILDNUMBER MODIFICATIONNUMBER)
  string(REGEX MATCH "define ${_part}[ \t]+([0-9]+)" _sm_match "${_swconfig}")
  if(NOT _sm_match)
    message(FATAL_ERROR "Could not read ${_part} from SWConfig.h")
  endif()
  list(APPEND _sm_ver "${CMAKE_MATCH_1}")
endforeach()
string(JOIN "." APP_VERSION ${_sm_ver})
message(STATUS "SoulMeter ${APP_VERSION}")

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)

# Cross builds must not depend on libc++/libunwind DLLs: the hook is injected
# into the game process and the meter ships as a bare .exe.
if(MINGW)
  add_link_options(-static)
endif()

# ---------------------------------------------------------------------------
# SoulMeterHook.dll - injected capture DLL
# ---------------------------------------------------------------------------
add_library(SoulMeterHook SHARED
  "${HOOK_DIR}/blockcache.cpp"
  "${HOOK_DIR}/dllmain.cpp"
  "${HOOK_DIR}/gamecmd.cpp"
  "${HOOK_DIR}/loadopt.cpp"
  "${HOOK_DIR}/md5cache.cpp"
  "${HOOK_DIR}/sockethooks.cpp"
  "${THIRD}/minhook/src/buffer.c"
  "${THIRD}/minhook/src/hook.c"
  "${THIRD}/minhook/src/trampoline.c"
  "${THIRD}/minhook/src/hde/hde64.c"
)
set_target_properties(SoulMeterHook PROPERTIES
  PREFIX ""
  OUTPUT_NAME "SoulMeterHook"
  CXX_STANDARD 17
)
target_include_directories(SoulMeterHook PRIVATE
  "${THIRD}/minhook/include"
  "${THIRD}/minhook/src"
)
target_compile_definitions(SoulMeterHook PRIVATE
  WIN32 _WINDOWS _USRDLL SOULMETERHOOK_EXPORTS WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
  $<$<CONFIG:Release>:NDEBUG>
)
if(NOT MSVC)
  # __try/__except (SEH) is an MS extension in clang.
  target_compile_options(SoulMeterHook PRIVATE -fms-extensions)
endif()
target_link_libraries(SoulMeterHook PRIVATE kernel32 user32)

# ---------------------------------------------------------------------------
# SoulMeter.exe - the overlay
# ---------------------------------------------------------------------------
set(METER_SOURCES
  "Buff Meter/Buff Meter.cpp"
  "Buff Meter/Buff.cpp"
  "Combat Meter/Combat.cpp"
  "Combat Meter/CombatMeter.cpp"
  "Damage Meter/Damage Meter.cpp"
  "Damage Meter/History.cpp"
  "Damage Meter/MySQLite.cpp"
  "Damage Meter/SaveData.cpp"
  "Damage Meter/SWDamageMonster.cpp"
  "Damage Meter/SWDamagePlayer.cpp"
  "Damage Meter/SWDamageSkill.cpp"
  "Damage Meter/Timer.cpp"
  "Language/Language.cpp"
  "main.cpp"
  "pch.cpp"
  "Soulworker Packet/SWCPacket.cpp"
  "Soulworker Packet/SWCPing.cpp"
  "Soulworker Packet/SWCPresence.cpp"
  "Soulworker Packet/SWPacket.cpp"
  "Soulworker Packet/SWPacketAggroChanged.cpp"
  "Soulworker Packet/SWPacketAkasic.cpp"
  "Soulworker Packet/SWPacketBigParty.cpp"
  "Soulworker Packet/SWPacketBrooch.cpp"
  "Soulworker Packet/SWPacketBuffIn.cpp"
  "Soulworker Packet/SWPacketBuffOut.cpp"
  "Soulworker Packet/SWPacketCharacterUpdateSpecialOptionList.cpp"
  "Soulworker Packet/SWPacketChat.cpp"
  "Soulworker Packet/SWPacketCooldown.cpp"
  "Soulworker Packet/SWPacketDamage.cpp"
  "Soulworker Packet/SWPacketDead.cpp"
  "Soulworker Packet/SWPacketEnterAnimation.cpp"
  "Soulworker Packet/SWPacketGestureUsed.cpp"
  "Soulworker Packet/SWPacketHeartbeat.cpp"
  "Soulworker Packet/SWPacketSquad.cpp"
  "Soulworker Packet/SWPacketMaker.cpp"
  "Soulworker Packet/SWPacketMazeEnd.cpp"
  "Soulworker Packet/SWPacketMazeStart.cpp"
  "Soulworker Packet/SWPacketMazeUpdateState.cpp"
  "Soulworker Packet/SWPacketMonsterKilled.cpp"
  "Soulworker Packet/SWPacketInInfoMonster.cpp"
  "Soulworker Packet/SWPacketMonsterStatUpdate.cpp"
  "Soulworker Packet/SWPacketOtherInfosMonster.cpp"
  "Soulworker Packet/SWPacketOtherUseSkill.cpp"
  "Soulworker Packet/SWPacketParty.cpp"
  "Soulworker Packet/SWPacketPartyListInfo.cpp"
  "Soulworker Packet/SWPacketSpawnedCharInfo.cpp"
  "Soulworker Packet/SWPacketStartMove.cpp"
  "Soulworker Packet/SWPacketStatChange.cpp"
  "Soulworker Packet/SWPacketUseSkill.cpp"
  "Soulworker Packet/SWPacketWorldChange.cpp"
  "Soulworker Packet/SWSPacketMyDodgeUsed.cpp"
  "Soulworker Packet/SWSPacketMySkillUsed.cpp"
  "Soulworker Packet/HookCommand.cpp"
  "Soulworker Packet/Injector.cpp"
  "Soulworker Packet/PipeReceiver.cpp"
  "Third Party/imgui/backends/imgui_impl_dx11.cpp"
  "Third Party/imgui/backends/imgui_impl_win32.cpp"
  "Third Party/imgui/Custom.cpp"
  "Third Party/imgui/imgui.cpp"
  "Third Party/imgui/imgui_draw.cpp"
  "Third Party/imgui/imgui_filedialog.cpp"
  "Third Party/imgui/imgui_tables.cpp"
  "Third Party/imgui/imgui_widgets.cpp"
  "Third Party/imgui/implot.cpp"
  "Third Party/imgui/implot_items.cpp"
  "Third Party/tinyxml2/tinyxml2.cpp"
  "UI/DX Input.cpp"
  "UI/DX11.cpp"
  "UI/HotKey.cpp"
  "UI/MinimapTab.cpp"
  "UI/Option.cpp"
  "UI/PlayerTable.cpp"
  "UI/PlotWindow.cpp"
  "UI/SkillTimeline.cpp"
  "UI/SpecificInfomation.cpp"
  "UI/UiWindow.cpp"
  "UI/UtillWindow.cpp"
  "Util/CriticalSection.cpp"
  "Util/ExceptionHandler.cpp"
  "Util/Log.cpp"
  "Util/MemoryPool.cpp"
  "Util/MultiThreadSync.cpp"
  "Util/Type.cpp"
  "Soulworker Utility.rc"
)
list(TRANSFORM METER_SOURCES PREPEND "${METER_DIR}/")

add_executable(SoulMeter WIN32 ${METER_SOURCES})
set_target_properties(SoulMeter PROPERTIES CXX_STANDARD 20)
target_include_directories(SoulMeter PRIVATE
  "${METER_DIR}"
  "${THIRD}"
  "${THIRD}/imgui"
  "${THIRD}/FlatBuffers"
  "${THIRD}/FlatBuffers/include"
  "${THIRD}/SQLite/include"
)
target_compile_definitions(SoulMeter PRIVATE
  _CONSOLE _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING _CRT_SECURE_NO_WARNINGS
  UNICODE _UNICODE
  $<$<CONFIG:Release>:NDEBUG>
)
if(MSVC)
  target_compile_options(SoulMeter PRIVATE /utf-8 /Zc:char8_t-)
else()
  # Same dialect knobs as the MSVC build (/utf-8 /Zc:char8_t-).
  target_compile_options(SoulMeter PRIVATE -fno-char8_t -finput-charset=UTF-8)
endif()

# sqlite3 is consumed through the DLL that ships with the meter.
if(MINGW)
  target_link_libraries(SoulMeter PRIVATE "${METER_DIR}/sqlite3.dll")
else()
  target_link_libraries(SoulMeter PRIVATE "${THIRD}/SQLite/lib/SQLite.lib")
endif()
target_link_libraries(SoulMeter PRIVATE
  d3d11 d3dcompiler dxgi dinput8 dxguid dbghelp psapi advapi32 shell32
  user32 gdi32 ole32 oleaut32 imm32
)

# Lang/ next to the binary, as the MSBuild CopyLangFiles target does.
add_custom_command(TARGET SoulMeter POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SoulMeter>/Lang"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${METER_DIR}/Lang" "$<TARGET_FILE_DIR:SoulMeter>/Lang"
  VERBATIM
)

# ---------------------------------------------------------------------------
# dist - the release package (mirrors the MSBuild PackageRelease target)
# ---------------------------------------------------------------------------
set(PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
set(PACKAGE_ZIP "${CMAKE_BINARY_DIR}/SoulMeter-${APP_VERSION}.zip")
add_custom_target(dist
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${PACKAGE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PACKAGE_DIR}/Lang" "${PACKAGE_DIR}/Font"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:SoulMeter>" "$<TARGET_FILE:SoulMeterHook>"
          "${METER_DIR}/sqlite3.dll" "${METER_DIR}/SWDB.db" "${PACKAGE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${METER_DIR}/Lang" "${PACKAGE_DIR}/Lang"
  COMMAND ${CMAKE_COMMAND} -DSRC=${METER_DIR}/Font -DDST=${PACKAGE_DIR}/Font
          -P "${CMAKE_SOURCE_DIR}/cmake/copy-fonts.cmake"
  COMMAND ${CMAKE_COMMAND} -E rm -f "${PACKAGE_ZIP}"
  COMMAND ${CMAKE_COMMAND} -E chdir "${PACKAGE_DIR}" ${CMAKE_COMMAND} -E tar cf "${PACKAGE_ZIP}" --format=zip .
  COMMAND ${CMAKE_COMMAND} -E echo "Package -> ${PACKAGE_ZIP}"
  DEPENDS SoulMeter SoulMeterHook
  VERBATIM
)
