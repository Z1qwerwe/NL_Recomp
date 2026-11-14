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
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # wie Cemu (statische CRT)
set(NL_AOT_OPT "/O1" CACHE STRING "MSVC-Optimierung fuer den generierten Code")
if(MSVC)
  target_compile_options(NLAotGenerated PRIVATE /bigobj ${NL_AOT_OPT} /wd4102 /wd4100 /wd4319 /wd4146)
else()
  target_compile_options(NLAotGenerated PRIVATE -O1 -Wno-unused-label -Wno-unused-variable)
endif()

# Die Verbindung zu Cemu
target_sources(CemuCafe PRIVATE ${NL_AOT_DIR}/nl_aot_glue.cpp)
target_compile_definitions(CemuCafe PRIVATE NL_AOT)
target_link_libraries(CemuCafe PRIVATE NLAotGenerated)
message(STATUS "Nintendo Land Recomp: AOT-Code aus ${NL_AOT_GENERATED}")
