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
)
target_include_directories(stride PRIVATE ${NCURSESW_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})
target_link_libraries(stride PRIVATE ${NCURSESW_LIBRARIES} ${SQLITE3_LIBRARIES} ${OPENSSL_LIBRARIES})
target_compile_options(stride PRIVATE ${NCURSESW_CFLAGS_OTHER} ${SQLITE3_CFLAGS_OTHER} ${OPENSSL_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

install(TARGETS stride RUNTIME DESTINATION bin)
