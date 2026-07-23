cmake_minimum_required(VERSION 3.20)
project(stride LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSESW REQUIRED ncursesw)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

add_executable(stride src/main.cpp)
target_include_directories(stride PRIVATE ${NCURSESW_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS})
target_link_libraries(stride PRIVATE ${NCURSESW_LIBRARIES} ${SQLITE3_LIBRARIES})
target_compile_options(stride PRIVATE ${NCURSESW_CFLAGS_OTHER} ${SQLITE3_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

install(TARGETS stride RUNTIME DESTINATION bin)
