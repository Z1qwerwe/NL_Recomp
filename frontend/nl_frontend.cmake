# Nintendo Land Recomp: eigenes Programm NintendoLand.exe
# Nutzt Cemus Kern-Bibliotheken (OS, Grafik, Audio, Eingabe) ohne Cemus Oberflaeche.
set(NL_FRONTEND_DIR ${CMAKE_CURRENT_LIST_DIR})

set(NL_NET_SOURCES ${NL_FRONTEND_DIR}/net/nl_steam.cpp ${NL_FRONTEND_DIR}/net/nl_video.cpp ${NL_FRONTEND_DIR}/net/nl_discord.cpp)
add_executable(NintendoLand ${NL_FRONTEND_DIR}/main.cpp ${NL_FRONTEND_DIR}/nl_input.cpp ${NL_FRONTEND_DIR}/nl_menu.cpp ${NL_FRONTEND_DIR}/nl_det.cpp
  ${NL_FRONTEND_DIR}/nl_wiipad.cpp ${NL_FRONTEND_DIR}/nl_online.cpp ${NL_NET_SOURCES})
set_source_files_properties(${NL_NET_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_precompile_headers(NintendoLand PRIVATE "${CMAKE_SOURCE_DIR}/src/Common/precompiled.h")
target_include_directories(NintendoLand PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/gui/interface)

if(WIN32)
  set(NL_ICON "${NL_FRONTEND_DIR}/nl_icon.ico")
  set(NL_FONT "${CMAKE_SOURCE_DIR}/src/resource/fontawesome-webfont.ttf")
  file(TO_CMAKE_PATH "${NL_ICON}" NL_ICON)
  file(TO_CMAKE_PATH "${NL_FONT}" NL_FONT)
  configure_file(${NL_FRONTEND_DIR}/nl.rc.in ${CMAKE_CURRENT_BINARY_DIR}/nl.rc @ONLY)
  target_sources(NintendoLand PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/nl.rc ${CMAKE_SOURCE_DIR}/dist/windows/Cemu.manifest)
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/nl.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src")
endif()

if(MSVC)
  # Kein Konsolenfenster, aber normales main() als Einstieg
  target_link_options(NintendoLand PRIVATE
    "$<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS;/ENTRY:mainCRTStartup>"
    /DEBUG /OPT:REF /OPT:ICF
    "/INCREMENTAL:NO" "/STACK:8388608")
endif()

set_target_properties(NintendoLand PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/$<1:>"
  OUTPUT_NAME "NintendoLand")

target_link_libraries(NintendoLand PRIVATE
  CemuAudio CemuCafe CemuCommon CemuComponents CemuConfig CemuInput CemuUtil CemuResource imguiImpl
  SDL3::SDL3)
if(UNIX AND NOT APPLE)
  target_link_options(NintendoLand PRIVATE -z noexecstack)
endif()
message(STATUS "Nintendo Land Recomp: eigenes Programm NintendoLand")

# ---------------------------------------------------------------------------
# Online ueber Steam: steam_api64.dll (Valve-Redistributable) liegt in third_party/steam und wird
# zur Laufzeit geladen - zum Bauen ist kein Steamworks SDK noetig.
# ---------------------------------------------------------------------------
set(NL_STEAM_DLL "${NL_FRONTEND_DIR}/../third_party/steam/steam_api64.dll")
function(nl_use_steam target)
  if(EXISTS "${NL_STEAM_DLL}")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NL_STEAM_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()
file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
file(WRITE "${CMAKE_SOURCE_DIR}/bin/steam_appid.txt" "480")   # Spacewar (Steams Test-App)
nl_use_steam(NintendoLand)

# ---------------------------------------------------------------------------
# NLOnline.exe: Mitspielen bei einem Freund (ohne Spielcode)
# ---------------------------------------------------------------------------
if(WIN32)
  set(NL_IMGUI "${CMAKE_SOURCE_DIR}/dependencies/imgui")
  add_executable(NLOnline
    ${NL_FRONTEND_DIR}/client/nl_online_client.cpp ${NL_FRONTEND_DIR}/client/nl_client_input.cpp
    ${NL_FRONTEND_DIR}/client/nl_hid_wiimote.cpp ${NL_NET_SOURCES}
    ${NL_IMGUI}/imgui.cpp ${NL_IMGUI}/imgui_draw.cpp ${NL_IMGUI}/imgui_tables.cpp ${NL_IMGUI}/imgui_widgets.cpp
    ${NL_IMGUI}/backends/imgui_impl_win32.cpp ${NL_IMGUI}/backends/imgui_impl_dx11.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/nl.rc)
  target_include_directories(NLOnline PRIVATE ${NL_IMGUI} ${NL_FRONTEND_DIR})
  target_link_libraries(NLOnline PRIVATE SDL3::SDL3 d3d11 dxgi xaudio2 mfplat mfuuid wmcodecdspuuid ole32 hid setupapi)
  set_target_properties(NLOnline PROPERTIES
    CXX_STANDARD 20
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/$<1:>"
    OUTPUT_NAME "NLOnline")
  if(MSVC)
    target_compile_options(NLOnline PRIVATE /utf-8)
    target_link_options(NLOnline PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS;/ENTRY:mainCRTStartup>" /DEBUG /INCREMENTAL:NO)
  endif()
  nl_use_steam(NLOnline)
  add_dependencies(NintendoLand NLOnline)   # "Spiel bauen" baut den Client gleich mit
endif()
