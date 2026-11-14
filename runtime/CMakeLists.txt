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
  ${GENERATED_SOURCES}
)
target_include_directories(NintendoLand PRIVATE src ${GENERATED_DIR})
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
