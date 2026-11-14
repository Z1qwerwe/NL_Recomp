# Nintendo Land Recomp: eigenes Programm NintendoLand.exe
# Nutzt Cemus Kern-Bibliotheken (OS, Grafik, Audio, Eingabe) ohne Cemus Oberflaeche.
set(NL_FRONTEND_DIR ${CMAKE_CURRENT_LIST_DIR})

add_executable(NintendoLand ${NL_FRONTEND_DIR}/main.cpp)
cemu_use_precompiled_header(NintendoLand)
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
    "/INCREMENTAL:NO" "/STACK:8388608")
endif()

set_target_properties(NintendoLand PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/$<1:>"
  OUTPUT_NAME "NintendoLand")

target_link_libraries(NintendoLand PRIVATE
  CemuAudio CemuCafe CemuCommon CemuComponents CemuConfig CemuInput CemuUtil CemuResource
  SDL3::SDL3)
if(UNIX AND NOT APPLE)
  target_link_options(NintendoLand PRIVATE -z noexecstack)
endif()
message(STATUS "Nintendo Land Recomp: eigenes Programm NintendoLand")
