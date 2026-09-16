# soob.cmake — the shared CMake build for a 2D SOOB-Core game.
#
# A game's CMakeLists.txt is five lines:
#
#     cmake_minimum_required(VERSION 2.8)
#     project(MyGame)
#     set(ENGINE ${CMAKE_SOURCE_DIR}/../SOOB-Core)
#     include(${ENGINE}/build/soob.cmake)
#     soob_add_game(${PROJECT_NAME} main.cpp)
#
# Extra sources go after main.cpp. Include-time side effects (the SDL/GL/OpenAL
# lookups and include_directories below) are directory-scoped, which is what we
# want — every target in the game's directory gets them.

option(USE_VENDOR_SDL "Use vendored SDL from the shared engine" OFF)

if(USE_VENDOR_SDL)
    set(SDL_INCLUDE_DIR ${ENGINE}/vendor/include)
    set(SDL_LIBRARY ${ENGINE}/vendor/lib/libSDL.dll.a)
    set(SDLMAIN_LIBRARY ${ENGINE}/vendor/lib/libSDLmain.a)
    set(SDL_LIBS mingw32 ${SDLMAIN_LIBRARY} ${SDL_LIBRARY})
else()
    find_package(SDL REQUIRED)
    set(SDL_LIBS ${SDL_LIBRARY})
endif()

find_package(OpenGL REQUIRED)
find_package(OpenAL REQUIRED)

include_directories(${ENGINE} ${SDL_INCLUDE_DIR} ${OPENAL_INCLUDE_DIR}
                    ${ENGINE}/vendor/lua-5.1.5/src)

# Lua 5.1.5 unity-build aggregator and stb_vorbis are C TUs.
set_source_files_properties(
    ${ENGINE}/vendor/lua-5.1.5/src/lua_all.c
    PROPERTIES COMPILE_DEFINITIONS "luaall_c;LUA_USE_POSIX"
)

# soob_add_game(<target> <sources...>) — the whole per-game build.
function(soob_add_game TARGET)
    add_executable(${TARGET}
        ${ARGN}
        ${ENGINE}/vendor/stb/stb_vorbis.c
        ${ENGINE}/vendor/lua-5.1.5/src/lua_all.c
    )
    target_link_libraries(${TARGET} ${SDL_LIBS} ${OPENGL_LIBRARIES} ${OPENAL_LIBRARY})

    # Compile the CPU rasterizer in alongside GL; config.lua's display.render
    # picks the active backend at runtime. Set SOOB_NO_SOFTWARE to skip it.
    if(NOT SOOB_NO_SOFTWARE)
        target_compile_definitions(${TARGET} PRIVATE SOOB_SOFTWARE_BACKEND)
    endif()

    # Mirror the engine's Lua modules next to the exe so shipped builds find
    # require "engine.scene" via ./scripts/?.lua without needing SOOB-Core on
    # the player's machine.
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_SOURCE_DIR}/scripts/engine
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${ENGINE}/scripts/engine
                ${CMAKE_SOURCE_DIR}/scripts/engine)
endfunction()
