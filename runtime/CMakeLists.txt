cmake_minimum_required(VERSION 3.16)
project(NintendoLandRecomp CXX)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT GENERATED_DIR)
  message(FATAL_ERROR "GENERATED_DIR fehlt (Ordner mit dem generierten Code)")
endif()
file(TO_CMAKE_PATH "${GENERATED_DIR}" GENERATED_DIR)
include(${GENERATED_DIR}/sources.cmake)

add_executable(NintendoLand
  src/runtime.cpp
  src/hle.cpp
  src/threads.cpp
  src/fs.cpp
  src/heap.cpp
  src/gx2.cpp
  src/input.cpp
  ${GENERATED_SOURCES}
)
target_include_directories(NintendoLand PRIVATE src ${GENERATED_DIR})

# Fenster, Controller und Mikrofon ueber SDL3 (wird beim ersten Bauen von GitHub geladen)
option(NL_TRACE_FUNCS "Funktions-Tracer zum Debuggen" OFF)
if(NL_TRACE_FUNCS)
  target_compile_definitions(NintendoLand PRIVATE NL_TRACE_FUNCS)
endif()
option(NL_USE_SDL "SDL3 fuer Fenster/Eingabe/Mikrofon verwenden" ON)
if(NL_USE_SDL)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(SDL3)
  target_sources(NintendoLand PRIVATE src/platform_sdl.cpp)
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(NintendoLand PRIVATE SDL3::SDL3-static)
  else()
    target_link_libraries(NintendoLand PRIVATE SDL3::SDL3)
  endif()
else()
  target_sources(NintendoLand PRIVATE src/platform_null.cpp)
endif()
target_link_libraries(NintendoLand PRIVATE Threads::Threads)

if(NOT NL_JOBS)
  set(NL_JOBS 2)
endif()

if(MSVC)
  # Riesige generierte Dateien: /bigobj, mehrere Dateien parallel (/MP), Optimierung moderat
  target_compile_options(NintendoLand PRIVATE /bigobj /MP${NL_JOBS} /wd4102 /wd4100)
  set_source_files_properties(${GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS "/Od")
  # Der recompilierte Code ruft sich tief verschachtelt auf: 256 MB Stack statt 1 MB
  target_link_options(NintendoLand PRIVATE /STACK:268435456)
else()
  target_compile_options(NintendoLand PRIVATE -O1 -Wno-unused-label -Wno-unused-variable)
  set_source_files_properties(${GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS "-O0")
endif()
