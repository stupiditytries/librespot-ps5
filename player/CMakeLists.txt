# The player's PC preview: the Spotify-TV-style screens in a borealis window or as PNGs, with
# sample data (see README.md). The console build will compile src/ with its own platform.
cmake_minimum_required(VERSION 3.20)
project(player_preview C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# borealis (xfangfang's), the GLFW it is built with, and stb_image_write: the same checkouts the
# overlay preview uses.
set(PLAYER_BOREALIS_DIR "$ENV{PLAYER_BOREALIS_DIR}" CACHE PATH "borealis' library folder")
set(PLAYER_GLFW_DIR "$ENV{PLAYER_GLFW_DIR}" CACHE PATH "borealis' GLFW fork")
set(PLAYER_STB_DIR "$ENV{PLAYER_STB_DIR}" CACHE PATH "a folder with stb_image_write.h")

set(brls "${PLAYER_BOREALIS_DIR}")
if(NOT EXISTS "${brls}/lib/core/application.cpp")
    message(FATAL_ERROR "borealis is missing: set PLAYER_BOREALIS_DIR")
endif()

file(GLOB borealis_sources
    "${brls}/lib/core/*.cpp"
    "${brls}/lib/core/touch/*.cpp"
    "${brls}/lib/views/*.cpp"
    "${brls}/lib/views/cells/*.cpp"
    "${brls}/lib/views/widgets/*.cpp"
    "${brls}/lib/extern/yoga/yoga/*.cpp"
    "${brls}/lib/extern/yoga/yoga/event/*.cpp")
# borealis' own platform choice and translations are answered by the host.
list(REMOVE_ITEM borealis_sources "${brls}/lib/core/platform.cpp" "${brls}/lib/core/i18n.cpp")
list(APPEND borealis_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/host/i18n.cpp"
    "${brls}/lib/extern/nanovg/nanovg.c"
    "${brls}/lib/extern/tinyxml2/tinyxml2.cpp"
    "${brls}/lib/extern/libretro-common/compat/compat_strl.c"
    "${brls}/lib/extern/libretro-common/encodings/encoding_utf.c"
    "${brls}/lib/extern/libretro-common/features/features_cpu.c"
    "${brls}/lib/extern/fmt/src/format.cc"
    "${brls}/lib/extern/glad/glad.c"
    "${brls}/lib/platforms/glfw/glfw_input.cpp"
    "${brls}/lib/platforms/glfw/glfw_video.cpp")

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${PLAYER_GLFW_DIR}" glfw EXCLUDE_FROM_ALL)

add_executable(player_preview
    ${borealis_sources}
    src/draw.cpp
    src/screen.cpp
    src/link.cpp
    src/canvas_video.cpp
    src/art_cache.cpp
    host/main.cpp)
target_include_directories(player_preview PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}/../.deps/nlohmann/include"
    "${brls}/include"
    "${brls}/include/borealis/extern"
    "${brls}/include/borealis/extern/nanovg"
    "${brls}/include/borealis/extern/tinyxml2"
    "${brls}/lib/extern/yoga"
    "${brls}/lib/extern/tweeny/include"
    "${brls}/lib/extern/fmt/include"
    "${PLAYER_STB_DIR}")
target_compile_definitions(player_preview PRIVATE
    "BRLS_RESOURCES=\"\"" BOREALIS_USE_OPENGL YG_ENABLE_EVENTS __GLFW__
    "PLAYER_FONTS=\"${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts\"")
target_link_libraries(player_preview PRIVATE glfw)
if(WIN32)
    target_include_directories(player_preview PRIVATE "${brls}/include/compat")
    target_compile_definitions(player_preview PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES
        BOREALIS_USE_STD_THREAD)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(player_preview PRIVATE Threads::Threads)
endif()

if(WIN32)
 target_link_libraries(player_preview PRIVATE ws2_32)
endif()
