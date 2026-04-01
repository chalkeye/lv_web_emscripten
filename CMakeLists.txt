cmake_minimum_required(VERSION 3.12)
project (lv_emscripten)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -s USE_SDL=2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -s USE_SDL=2")

# Force memory growth so your high-res graphics don't crash the browser
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s ALLOW_MEMORY_GROWTH=1 -s SINGLE_FILE=1 --shell-file ${PROJECT_SOURCE_DIR}/lvgl_shell.html")

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/ui)

add_subdirectory(lvgl)
add_subdirectory(lv_drivers)

# THE KEY: GLOB_RECURSE finds every single .c and .cpp file in the ui folder and subfolders
file(GLOB_RECURSE MY_SOURCES "./*.c" "./*.cpp" "ui/*.c" "ui/*.cpp")
set(SOURCES ${MY_SOURCES})

add_executable(index ${SOURCES})

set_source_files_properties(main.cpp PROPERTIES COMPILE_FLAGS -DCHOSEN_DEMO=0)
set(CMAKE_EXECUTABLE_SUFFIX ".html")
target_link_libraries(index lvgl lvgl_examples lvgl_demos lv_drivers)
