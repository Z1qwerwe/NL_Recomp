# Nintendo Land Recomp: bindet den vorab kompilierten Spielcode in Cemu ein.
# Wird von Cemus CMakeLists.txt eingebunden, wenn NL_AOT_DIR gesetzt ist.
#   NL_AOT_DIR        = dieser Ordner (cemu/aot)
#   NL_AOT_GENERATED  = Ordner mit dem generierten Code (recomp_*.cpp, tables.cpp, sources.cmake)

if(NOT NL_AOT_GENERATED)
  message(FATAL_ERROR "NL_AOT_GENERATED fehlt")
endif()
file(TO_CMAKE_PATH "${NL_AOT_GENERATED}" NL_AOT_GENERATED)
file(TO_CMAKE_PATH "${NL_AOT_DIR}" NL_AOT_DIR)

set(GENERATED_DIR ${NL_AOT_GENERATED})
include(${NL_AOT_GENERATED}/sources.cmake)

# Der generierte Code als eigene Bibliothek, ohne Cemus vorkompilierte Header
add_library(NLAotGenerated STATIC ${GENERATED_SOURCES})
target_include_directories(NLAotGenerated BEFORE PRIVATE ${NL_AOT_DIR} ${NL_AOT_GENERATED})
set_target_properties(NLAotGenerated PROPERTIES CXX_STANDARD 20
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"   # wie Cemu (statische CRT)
  INTERPROCEDURAL_OPTIMIZATION OFF)   # keine Link-Time-Optimierung: sonst dauert das Linken ewig
set(NL_AOT_OPT "/O1" CACHE STRING "MSVC-Optimierung fuer den generierten Code")
if(MSVC)
  target_compile_options(NLAotGenerated PRIVATE /bigobj ${NL_AOT_OPT} /wd4102 /wd4100 /wd4319 /wd4146)
else()
  target_compile_options(NLAotGenerated PRIVATE -O1 -Wno-unused-label -Wno-unused-variable)
endif()

# Die Verbindung zu Cemu
target_sources(CemuCafe PRIVATE ${NL_AOT_DIR}/nl_aot_glue.cpp
  ${NL_AOT_DIR}/../../frontend/net/nl_capture_vk.cpp)   # TV-Bild fuer Online-Streaming
target_compile_definitions(CemuCafe PRIVATE NL_AOT)
target_include_directories(CemuCafe PRIVATE ${NL_AOT_DIR}/../../frontend)   # nl_screens.h
target_link_libraries(CemuCafe PRIVATE NLAotGenerated)
message(STATUS "Nintendo Land Recomp: AOT-Code aus ${NL_AOT_GENERATED}")

# Unser eigenes Programm (eigenes Fenster, eigene Einstellungen)
if(NL_FRONTEND_DIR_OVERRIDE)
  include(${NL_FRONTEND_DIR_OVERRIDE}/nl_frontend.cmake)
else()
  include(${NL_AOT_DIR}/../../frontend/nl_frontend.cmake)
endif()
