cmake_minimum_required(VERSION 3.12)
project (lv_emscripten)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -s USE_SDL=2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -s USE_SDL=2")

# Force 16-bit color to match SquareLine, and disable the broken Chinese keyboard
add_compile_definitions(LV_COLOR_DEPTH=16 LV_USE_IME_PINYIN=0)

include_directories(${PROJECT_SOURCE_DIR})

add_subdirectory(lvgl)
add_subdirectory(lv_drivers)

# Grab the C++ main file and your UI folder
file(GLOB MY_SOURCES "./*.c" "./*.cpp" "ui/*.c")
set(SOURCES ${MY_SOURCES})

add_executable(index ${SOURCES} ${INCLUDES})

if(NOT LVGL_CHOSEN_DEMO)
    set(LVGL_CHOSEN_DEMO lv_demo_widgets)
endif()

set_source_files_properties(main.cpp PROPERTIES COMPILE_FLAGS -DCHOSEN_DEMO=${LVGL_CHOSEN_DEMO})

set(CMAKE_EXECUTABLE_SUFFIX ".html")
target_link_libraries(index lvgl lvgl_examples lvgl_demos lv_drivers)

set_target_properties(index PROPERTIES LINK_FLAGS "--shell-file ${PROJECT_SOURCE_DIR}/lvgl_shell.html -s SINGLE_FILE=1")
