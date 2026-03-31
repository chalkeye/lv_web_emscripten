cmake_minimum_required(VERSION 3.12)
project (lv_emscripten)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -s USE_SDL=2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -s USE_SDL=2")

# Force Emscripten to auto-size the memory for your heavy UI arrays
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s ALLOW_MEMORY_GROWTH=1 -s SINGLE_FILE=1 --shell-file ${PROJECT_SOURCE_DIR}/lvgl_shell.html")

# Force 16-bit color and disable the broken Chinese keyboard
add_compile_definitions(LV_COLOR_DEPTH=16 LV_USE_IME_PINYIN=0)

include_directories(${PROJECT_SOURCE_DIR})

add_subdirectory(lvgl)
add_subdirectory(lv_drivers)

file(GLOB MY_SOURCES "./*.c" "./*.cpp" "ui/*.c")
set(SOURCES ${MY_SOURCES})

add_executable(index ${SOURCES} ${INCLUDES})

if(NOT LVGL_CHOSEN_DEMO)
    set(LVGL_CHOSEN_DEMO lv_demo_widgets)
endif()

set_source_files_properties(main.cpp PROPERTIES COMPILE_FLAGS -DCHOSEN_DEMO=${LVGL_CHOSEN_DEMO})

set(CMAKE_EXECUTABLE_SUFFIX ".html")
target_link_libraries(index lvgl lvgl_examples lvgl_demos lv_drivers)
