cmake_minimum_required(VERSION 3.16)
# Eco's version is changed only here (see CHANGELOG.md). It reaches the code as ECO_VERSION
# (common/version.hpp), the .exe properties on Windows and the Info.plist on macOS.
project(Eco VERSION 2.3.0 LANGUAGES C CXX)
add_compile_definitions($<$<COMPILE_LANGUAGE:C,CXX>:ECO_VERSION="${PROJECT_VERSION}">)

# Public key of the signed updates (common/update_key.hpp). Whoever builds Eco uses their own:
# -DECO_UPDATE_PUBLIC_KEY=<64 hex> or the file local_data_DO_NOT_DISTRIBUTE/update_public_key.txt, written by
# "eco-sign new-key". Without a key, clients don't accept any automatic update.
set(ECO_UPDATE_PUBLIC_KEY "" CACHE STRING "Public key (64 hex) of the signed updates")
set(_eco_key "${ECO_UPDATE_PUBLIC_KEY}")
set(_eco_keyfile "${CMAKE_CURRENT_SOURCE_DIR}/local_data_DO_NOT_DISTRIBUTE/update_public_key.txt")
if(_eco_key STREQUAL "" AND EXISTS "${_eco_keyfile}")
  file(STRINGS "${_eco_keyfile}" _eco_key LIMIT_COUNT 1)
  string(STRIP "${_eco_key}" _eco_key)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_eco_keyfile}")
endif()
if(_eco_key MATCHES "^[0-9a-fA-F]+$" AND _eco_key MATCHES "^................................................................$")
  add_compile_definitions(ECO_UPDATE_PUBLIC_KEY="${_eco_key}")
  message(STATUS "Eco: signed updates with the key ${_eco_key}")
elseif(NOT _eco_key STREQUAL "")
  message(FATAL_ERROR "ECO_UPDATE_PUBLIC_KEY / update_public_key.txt: has to be the public key in hexadecimal (64 characters)")
else()
  message(STATUS "Eco: no update key (eco-sign new-key): automatic updates are disabled")
endif()
if(WIN32)
  enable_language(RC)
endif()
# Version and description in the executable properties (Windows: right click > Properties > Details)
function(eco_version_info target description)
  if(WIN32)
    set(ECO_RC_DESCRIPTION "${description}")
    set(ECO_RC_NAME "${target}")
    set(ECO_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
    if(ECO_VERSION_PATCH STREQUAL "")
      set(ECO_VERSION_PATCH 0)
    endif()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc @ONLY)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc)
  endif()
endfunction()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)  # optimized for speed (-O2): Opus, RNNoise and the encryption notice it
endif()
# Link-time optimization (LTO) in optimized builds, if the compiler supports it
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg LANGUAGES C CXX)
if(_ipo_ok)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
endif()

# By default on Windows Opus, PortAudio and Asio are downloaded and built (static):
# nothing needs to be installed besides CMake and a compiler.
if(WIN32)
  set(_fetch_default ON)
else()
  set(_fetch_default OFF)
endif()
option(ECO_FETCH_DEPS "Download and build Opus/PortAudio/Asio" ${_fetch_default})
option(ECO_SERVER_ONLY "Build only the server (no audio or screen; ideal for a VPS)" OFF)

find_package(Threads REQUIRED)

# Hardening: stack protection, compile-time overflow checks and
# executables with ASLR/DEP. It costs very little and turns many memory bugs into a clean exit.
option(ECO_HARDEN "Build with memory protections (stack, FORTIFY, ASLR, RELRO)" ON)
if(ECO_HARDEN)
  if(MSVC)
    add_compile_options(/GS /guard:cf /sdl)
    add_link_options(/guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
  else()
    # FORTIFY only in our own code (see eco_fortify below): in FreeType it gives false positives with
    # its variable-size arrays that, at runtime, would close the program
    add_compile_options(-fstack-protector-strong -fno-strict-overflow)
    if(MINGW)
      # MinGW: stack protection and FORTIFY need libssp (static inside the .exe)
      add_link_options(-fstack-protector-strong -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va)
      link_libraries(ssp)
    elseif(NOT APPLE)
      add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
    endif()
  endif()
  if(NOT WIN32)
    # Position-independent executables (PIE): without this ASLR doesn't move the program code
    include(CheckPIESupported)
    check_pie_supported(OUTPUT_VARIABLE _pie_msg LANGUAGES C CXX)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  endif()
endif()

# Remove all unused code from the executable
if(NOT MSVC)
  add_compile_options(-ffunction-sections -fdata-sections)
  if(APPLE)
    add_link_options(-Wl,-dead_strip)
  else()
    add_link_options(-Wl,--gc-sections)
    if(CMAKE_BUILD_TYPE MATCHES "Rel")
      add_link_options(-s)
    endif()
  endif()
endif()

if(ECO_FETCH_DEPS)
  include(FetchContent)
  # PortAudio 19.7 asks for an old CMake version that CMake 4 no longer accepts
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(asio GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git GIT_TAG 12e0ce9e0500bf0f247dbd1ae894272656456079)  # asio-1-30-2
  FetchContent_MakeAvailable(asio)
  set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
  if(NOT ECO_SERVER_ONLY)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(opus GIT_REPOSITORY https://github.com/xiph/opus.git GIT_TAG ddbe48383984d56acd9e1ab6a090c54ca6b735a6)  # v1.5.2
    set(PA_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(PA_USE_ASIO OFF CACHE BOOL "" FORCE)
    set(PA_USE_JACK OFF CACHE BOOL "" FORCE)
    set(PA_USE_WDMKS OFF CACHE BOOL "" FORCE)  # not needed and bulky
    FetchContent_Declare(portaudio GIT_REPOSITORY https://github.com/PortAudio/portaudio.git GIT_TAG 147dd722548358763a8b649b3e4b41dfffbcfbb6)  # v19.7.0
    FetchContent_MakeAvailable(opus portaudio)
    # Opus installs its headers as <opus.h>; the code uses <opus/opus.h>
    set(OPUS_COMPAT_DIR ${CMAKE_BINARY_DIR}/opus_compat)
    file(MAKE_DIRECTORY ${OPUS_COMPAT_DIR})
    file(COPY ${opus_SOURCE_DIR}/include/ DESTINATION ${OPUS_COMPAT_DIR}/opus)
    set(ECO_AUDIO_LIBS opus portaudio_static)
    set(ECO_AUDIO_INCLUDES ${OPUS_COMPAT_DIR} ${portaudio_SOURCE_DIR}/include)
  endif()
else()
  find_path(ASIO_INCLUDE_DIR asio.hpp REQUIRED)
  if(NOT ECO_SERVER_ONLY)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPUS REQUIRED opus)
    pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
    set(ECO_AUDIO_LIBS ${OPUS_LINK_LIBRARIES} ${PORTAUDIO_LINK_LIBRARIES})
    set(ECO_AUDIO_INCLUDES ${OPUS_INCLUDEDIR} ${PORTAUDIO_INCLUDE_DIRS})
  endif()
endif()

# ---------- Libraries included in the source (third_party) ----------
# Trimmed SQLite: only what the server uses
add_library(eco_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(eco_sqlite PUBLIC third_party/sqlite)
target_compile_definitions(eco_sqlite PRIVATE
  SQLITE_THREADSAFE=0 SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_LIKE_DOESNT_MATCH_BLOBS
  SQLITE_MAX_EXPR_DEPTH=0 SQLITE_OMIT_DECLTYPE SQLITE_OMIT_DEPRECATED SQLITE_OMIT_PROGRESS_CALLBACK
  SQLITE_OMIT_SHARED_CACHE SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_JSON SQLITE_OMIT_TRACE
  SQLITE_OMIT_COMPLETE SQLITE_OMIT_GET_TABLE SQLITE_OMIT_AUTHORIZATION SQLITE_OMIT_UTF16
  SQLITE_OMIT_EXPLAIN SQLITE_OMIT_INTROSPECTION_PRAGMAS SQLITE_OMIT_COMPILEOPTION_DIAGS
  SQLITE_OMIT_TCL_VARIABLE SQLITE_OMIT_DESERIALIZE SQLITE_OMIT_BLOB_LITERAL SQLITE_OMIT_HEX_INTEGER
  SQLITE_UNTESTABLE)
if(NOT WIN32)
  target_link_libraries(eco_sqlite PUBLIC ${CMAKE_DL_LIBS})
endif()

# Monocypher: all the cryptography (X25519, EdDSA, XChaCha20-Poly1305, BLAKE2b, Argon2)
add_library(eco_monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(eco_monocypher PUBLIC third_party/monocypher)
if(NOT MSVC)
  target_compile_options(eco_monocypher PRIVATE -O3)  # encryption speed matters more than 20 KB
endif()

set(ECO_NET_LIBS Threads::Threads eco_monocypher)
set(ECO_DEFS ASIO_STANDALONE ASIO_NO_DEPRECATED)
if(WIN32)
  list(APPEND ECO_NET_LIBS ws2_32 mswsock bcrypt shell32)  # shell32: arguments in UTF-8
  list(APPEND ECO_DEFS _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(MINGW)
  # Standalone executables (no MinGW DLLs)
  list(APPEND ECO_NET_LIBS -static -static-libgcc -static-libstdc++)
endif()

# ---------- eco-sign: publish signed versions (for whoever distributes Eco, not for users) ----------
add_executable(eco-sign tools/sign.cpp common/secure.cpp)
target_link_libraries(eco-sign PRIVATE eco_monocypher)
if(WIN32)
  target_link_libraries(eco-sign PRIVATE bcrypt)
endif()
if(MINGW)
  target_link_libraries(eco-sign PRIVATE -static -static-libgcc -static-libstdc++)
endif()
eco_version_info(eco-sign "Eco - publish updates")

# ---------- eco-server ----------
add_executable(eco-server server/main.cpp server/server.cpp server/db.cpp common/secure.cpp common/i18n.cpp)
target_include_directories(eco-server PRIVATE ${ASIO_INCLUDE_DIR})
target_compile_definitions(eco-server PRIVATE ${ECO_DEFS})
target_link_libraries(eco-server PRIVATE eco_sqlite ${ECO_NET_LIBS})
eco_version_info(eco-server "Eco - server")

# ---------- eco-client (console) and eco-gui (window) ----------
# Both use the same core: network, encryption, audio and screen
option(ECO_GUI "Build eco-gui, the graphical interface (Dear ImGui + GLFW; Windows, Linux and macOS)" ON)
if(NOT ECO_SERVER_ONLY)
  set(CORE_SRC client/client.cpp client/audio.cpp client/screencodec.cpp client/e2e.cpp client/keys.cpp client/loopback.cpp client/update.cpp common/secure.cpp common/i18n.cpp)
  if(WIN32)
    list(APPEND CORE_SRC client/screen_win.cpp)
    set(ECO_PLATFORM_LIBS gdi32 user32 winmm ole32 uuid setupapi crypt32 d3d11 dxgi dwmapi)  # crypt32: DPAPI; d3d11/dxgi/dwmapi: captura
  elseif(APPLE)
    list(APPEND CORE_SRC client/screen_mac.cpp)
    set(ECO_PLATFORM_LIBS "-framework ApplicationServices")
  else()
    find_package(X11 REQUIRED)
    list(APPEND CORE_SRC client/screen_x11.cpp)
    set(ECO_PLATFORM_LIBS ${X11_LIBRARIES})
  endif()
  # RNNoise (Xiph, BSD): mic noise suppression. 0.1.1 carries the model inside the code (~85 KB)
  include(FetchContent)
  FetchContent_Declare(rnnoise GIT_REPOSITORY https://github.com/xiph/rnnoise.git GIT_TAG 6cbfd53eb348a8d394e0757b4025c6ded34eb2b6)  # v0.1.1
  FetchContent_MakeAvailable(rnnoise)
  add_library(eco_rnnoise STATIC
    ${rnnoise_SOURCE_DIR}/src/denoise.c ${rnnoise_SOURCE_DIR}/src/rnn.c ${rnnoise_SOURCE_DIR}/src/rnn_data.c
    ${rnnoise_SOURCE_DIR}/src/pitch.c ${rnnoise_SOURCE_DIR}/src/kiss_fft.c ${rnnoise_SOURCE_DIR}/src/celt_lpc.c)
  target_include_directories(eco_rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include)
  if(MSVC)
    target_compile_definitions(eco_rnnoise PRIVATE _USE_MATH_DEFINES)
  else()
    target_compile_options(eco_rnnoise PRIVATE -O2 -w)  # runs in real time: speed before size
    target_link_libraries(eco_rnnoise PUBLIC m)
  endif()

  add_library(eco_client_core STATIC ${CORE_SRC})
  target_include_directories(eco_client_core PUBLIC ${ASIO_INCLUDE_DIR} ${ECO_AUDIO_INCLUDES} PRIVATE third_party/stb)
  target_compile_definitions(eco_client_core PUBLIC ${ECO_DEFS})
  target_link_libraries(eco_client_core PUBLIC eco_rnnoise ${ECO_AUDIO_LIBS} ${ECO_PLATFORM_LIBS} ${ECO_NET_LIBS} ${CMAKE_DL_LIBS})
  if(MSVC)
    target_compile_options(eco_client_core PRIVATE /utf-8)
  endif()

  add_executable(eco-client client/main.cpp)
  target_link_libraries(eco-client PRIVATE eco_client_core)
  eco_version_info(eco-client "Eco - console client")

  if(ECO_GUI)
    include(FetchContent)
    # 1.92: glyphs are rasterized on demand (only the ones that appear on screen), so we can have
    # any character (emojis, CJK...) without a huge atlas or a slow startup
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG f1cc2ae15e53a861a874c3034aae6798fde194ab)  # v1.92.9b
    FetchContent_MakeAvailable(imgui)
    # FreeType: better rasterization and color emojis. The system one if there is one (Linux/macOS, it usually brings
    # PNG support for Noto Color Emoji); otherwise a minimal static one is downloaded (without zlib/png/harfbuzz)
    if(NOT ECO_FETCH_DEPS)
      find_package(Freetype QUIET)
    endif()
    if(NOT TARGET Freetype::Freetype AND NOT TARGET freetype)
      set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
      set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
      set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
      set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
      set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
      set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
      FetchContent_Declare(freetype GIT_REPOSITORY https://github.com/freetype/freetype.git GIT_TAG 0a0221a1347e2f1e07c395263540026e9a0aa7c7)  # VER-2-14-3
      FetchContent_MakeAvailable(freetype)
    endif()
    if(TARGET Freetype::Freetype)
      set(ECO_FREETYPE Freetype::Freetype)
    else()
      set(ECO_FREETYPE freetype)
    endif()
    # The system GLFW if there is one (Linux/macOS); otherwise it is downloaded and built statically
    if(NOT ECO_FETCH_DEPS)
      find_package(glfw3 3.3 QUIET)
    endif()
    if(NOT TARGET glfw)
      set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
      set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
      set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
      set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)  # X11 is enough (it also works on Wayland through XWayland)
      FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 7b6aead9fb88b3623e3b3725ebb42670cbe4c579)  # 3.4
      FetchContent_MakeAvailable(glfw)
    endif()
    # Windows draws with Direct3D 11 (its driver uses much less memory than OpenGL's);
    # Linux and macOS with OpenGL 3.2
    if(WIN32)
      set(ECO_RENDER_SRC client/gui_render_d3d11.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
      set(ECO_RENDER_LIBS d3d11 dxgi d3dcompiler)
    else()
      set(OpenGL_GL_PREFERENCE GLVND)
      find_package(OpenGL REQUIRED)
      set(ECO_RENDER_SRC client/gui_render_gl.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
      set(ECO_RENDER_LIBS OpenGL::GL)
    endif()

    add_library(eco_imgui STATIC
      ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
      ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
      ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp)
    target_include_directories(eco_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
                               ${imgui_SOURCE_DIR}/misc/freetype)
    # Out go the demo window and the debugging tools. WCHAR32: characters outside the
    # basic Unicode plane (emojis) both when drawing and when typing
    target_compile_definitions(eco_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS
                               IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_USE_WCHAR32 IMGUI_ENABLE_FREETYPE)
    target_link_libraries(eco_imgui PUBLIC glfw ${ECO_FREETYPE})

    add_executable(eco-gui WIN32 MACOSX_BUNDLE client/gui.cpp ${ECO_RENDER_SRC})
    target_link_libraries(eco-gui PRIVATE ${ECO_RENDER_LIBS})
    target_link_libraries(eco-gui PRIVATE eco_client_core eco_imgui)
    eco_version_info(eco-gui "Eco - voice chat")
    if(APPLE)
      # macOS only lets an app use the microphone with this description in its Info.plist
      set_target_properties(eco-gui PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME Eco MACOSX_BUNDLE_GUI_IDENTIFIER org.eco.gui
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION} MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    endif()
    if(MSVC)
      target_compile_options(eco-gui PRIVATE /utf-8)
    endif()
  endif()
endif()

if(MSVC)
  target_compile_options(eco-server PRIVATE /utf-8)
  if(TARGET eco-client)
    target_compile_options(eco-client PRIVATE /utf-8)
  endif()
endif()

# Tests of the end-to-end encryption (Double Ratchet, sender keys, signatures, invitations):
#   cmake -S . -B build -DECO_TESTS=ON && cmake --build build --target eco-test-crypto && build/eco-test-crypto
# Tests: always built (they cost little) and build.ps1 runs them before copying anything to dist/.
#   eco-test-crypto            end-to-end encryption (Double Ratchet, sender keys, signatures...)
#   eco-test-flood <port>      server flood brakes (needs a running eco-server)
option(ECO_TESTS "Build the tests (tests/)" ON)
if(ECO_TESTS)
  enable_testing()
  add_executable(eco-test-crypto tests/test_crypto.cpp client/keys.cpp client/e2e.cpp common/secure.cpp common/i18n.cpp)
  target_link_libraries(eco-test-crypto PRIVATE eco_monocypher Threads::Threads)
  add_test(NAME cifrado COMMAND eco-test-crypto)
  # eco-test-fuzz <port> [s]    random and malformed data against the server (also running)
  # eco-test-fuzz-client <p> [s] a malicious server against eco-client (which connects to it)
  # eco-test-load <p> [n] [s]    N clients talking at once: how much the server relays
  foreach(t flood fuzz fuzz-client load)
    if(t STREQUAL "flood")
      add_executable(eco-test-flood tests/test_flood.cpp common/secure.cpp)
    elseif(t STREQUAL "load")
      add_executable(eco-test-load tests/test_load.cpp common/secure.cpp)
    elseif(t STREQUAL "fuzz")
      add_executable(eco-test-fuzz tests/test_fuzz_server.cpp common/secure.cpp)
    else()
      add_executable(eco-test-fuzz-client tests/test_fuzz_client.cpp common/secure.cpp)
    endif()
    target_include_directories(eco-test-${t} PRIVATE ${ASIO_INCLUDE_DIR})
    target_compile_definitions(eco-test-${t} PRIVATE ${ECO_DEFS})
    target_link_libraries(eco-test-${t} PRIVATE ${ECO_NET_LIBS})
  endforeach()
  if(WIN32)
    target_link_libraries(eco-test-crypto PRIVATE bcrypt crypt32)
  endif()
  if(MINGW)
    target_link_libraries(eco-test-crypto PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
  if(MSVC)
    target_compile_options(eco-test-crypto PRIVATE /utf-8)
    target_compile_options(eco-test-flood PRIVATE /utf-8)
  endif()
endif()

# FORTIFY (size checks in memcpy/memset...) in our own code and in SQLite/Monocypher
if(ECO_HARDEN AND NOT MSVC)
  foreach(t eco-server eco_sqlite eco_monocypher eco_client_core eco-client eco-gui)
    if(TARGET ${t})
      target_compile_definitions(${t} PRIVATE _FORTIFY_SOURCE=2)
    endif()
  endforeach()
  # Bounds checking in vector/string/array of our own code (the one that reads network data): an
  # out-of-range index closes the program instead of reading or writing someone else's memory. Negligible cost.
  foreach(t eco-server eco_client_core eco-client eco-gui eco-test-crypto)
    if(TARGET ${t})
      target_compile_definitions(${t} PRIVATE _GLIBCXX_ASSERTIONS
                                 _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
    endif()
  endforeach()
endif()
