cmake_minimum_required(VERSION 3.20)
project(stride LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSESW REQUIRED ncursesw)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
# On macOS, Homebrew's openssl is keg-only (not symlinked onto the default
# pkg-config search path), so if this fails, either run cmake with
# PKG_CONFIG_PATH="$(brew --prefix openssl)/lib/pkgconfig:$PKG_CONFIG_PATH"
# or pass -DOPENSSL_ROOT_DIR="$(brew --prefix openssl)".
pkg_check_modules(OPENSSL REQUIRED openssl)

# GTK4 is optional: when present, `stride --quick-capture` opens a small
# floating GTK4 window instead of needing a dedicated terminal for the
# ncurses dialog (see src/quickcapture_gtk.cpp and the "Quick capture"
# section of README.md). Without it, --quick-capture just falls back to the
# ncurses dialog, same as always -- set STRIDE_NO_GTK4=1 to force that even
# when gtk4 is installed (e.g. a minimal/headless build).
set(STRIDE_NO_GTK4 OFF CACHE BOOL "Build without the GTK4 quick-capture window even if gtk4 is available")
if(NOT STRIDE_NO_GTK4)
  pkg_check_modules(GTK4 gtk4)
endif()

add_executable(stride
  src/main.cpp
  src/store.cpp
  src/input.cpp
  src/form.cpp
  src/finder.cpp
  src/app.cpp
  src/mirror.cpp
  src/mirror_import.cpp
  src/config.cpp
  src/sync.cpp
  src/things_import.cpp
  src/crypto.cpp
  src/bundle.cpp
  src/quickcapture.cpp
  src/cli_json.cpp
)
target_include_directories(stride PRIVATE ${NCURSESW_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})
target_link_libraries(stride PRIVATE ${NCURSESW_LIBRARIES} ${SQLITE3_LIBRARIES} ${OPENSSL_LIBRARIES})
target_compile_options(stride PRIVATE ${NCURSESW_CFLAGS_OTHER} ${SQLITE3_CFLAGS_OTHER} ${OPENSSL_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

if(GTK4_FOUND)
  target_sources(stride PRIVATE src/quickcapture_gtk.cpp)
  target_include_directories(stride PRIVATE ${GTK4_INCLUDE_DIRS})
  target_link_libraries(stride PRIVATE ${GTK4_LIBRARIES})
  target_compile_options(stride PRIVATE ${GTK4_CFLAGS_OTHER})
  target_compile_definitions(stride PRIVATE STRIDE_HAVE_GTK4)
  message(STATUS "stride: GTK4 found (${GTK4_VERSION}) -- --quick-capture will use a floating GTK4 window")
else()
  message(STATUS "stride: GTK4 not found -- --quick-capture will use the ncurses dialog only")
endif()

install(TARGETS stride RUNTIME DESTINATION bin)
