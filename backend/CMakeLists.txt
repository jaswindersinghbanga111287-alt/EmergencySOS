cmake_minimum_required(VERSION 3.16)
project(EmergencySOSBackend CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Where MySQL Connector/C++ is installed. Pass it if CMake cannot find it, e.g.
#   -DMYSQL_CONCPP_DIR="C:/Program Files/MySQL/MySQL Connector C++ 9.1"
set(MYSQL_CONCPP_DIR "" CACHE PATH "MySQL Connector/C++ install folder")
if(MYSQL_CONCPP_DIR)
  list(APPEND CMAKE_PREFIX_PATH "${MYSQL_CONCPP_DIR}")
endif()

find_package(Threads REQUIRED)
add_executable(emergency_sos_server src/main.cpp)
target_include_directories(emergency_sos_server PRIVATE third_party src)
target_link_libraries(emergency_sos_server PRIVATE Threads::Threads)

# 1) Connector/C++ 8.0.33+ ships a CMake package with the mysql::concpp-jdbc target.
find_package(mysql-concpp QUIET)
if(TARGET mysql::concpp-jdbc)
  message(STATUS "Using MySQL Connector/C++ CMake package")
  target_link_libraries(emergency_sos_server PRIVATE mysql::concpp-jdbc)
else()
  # 2) Fallback: look for the header and library directly.
  find_path(CONCPP_INCLUDE_DIR mysql/jdbc.h
    HINTS "${MYSQL_CONCPP_DIR}/include" /usr/include /usr/local/include /opt/homebrew/include)
  find_library(CONCPP_JDBC_LIB NAMES mysqlcppconn mysqlcppconn10
    HINTS "${MYSQL_CONCPP_DIR}/lib64/vs14" "${MYSQL_CONCPP_DIR}/lib64" "${MYSQL_CONCPP_DIR}/lib"
          /usr/lib/x86_64-linux-gnu /usr/local/lib /opt/homebrew/lib)
  if(NOT CONCPP_INCLUDE_DIR OR NOT CONCPP_JDBC_LIB)
    message(FATAL_ERROR "MySQL Connector/C++ not found. Install it and pass -DMYSQL_CONCPP_DIR=<install folder>.")
  endif()
  message(STATUS "Using MySQL Connector/C++ from ${CONCPP_JDBC_LIB}")
  target_include_directories(emergency_sos_server PRIVATE "${CONCPP_INCLUDE_DIR}")
  target_link_libraries(emergency_sos_server PRIVATE "${CONCPP_JDBC_LIB}")
  if(NOT DEFINED MYSQL_CONCPP_RUNTIME_LIBRARY_DIR AND MYSQL_CONCPP_DIR)
    set(MYSQL_CONCPP_RUNTIME_LIBRARY_DIR "${MYSQL_CONCPP_DIR}/lib64")
  endif()
endif()

if(WIN32)
  target_compile_definitions(emergency_sos_server PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX)
  target_link_libraries(emergency_sos_server PRIVATE ws2_32)
  if(MSVC)
    target_compile_options(emergency_sos_server PRIVATE /utf-8 /bigobj)
  endif()
  # Copy the connector DLLs next to the .exe so it starts without PATH changes.
  if(DEFINED MYSQL_CONCPP_RUNTIME_LIBRARY_DIR)
    file(GLOB CONCPP_DLLS "${MYSQL_CONCPP_RUNTIME_LIBRARY_DIR}/*.dll")
    foreach(dll ${CONCPP_DLLS})
      add_custom_command(TARGET emergency_sos_server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" $<TARGET_FILE_DIR:emergency_sos_server>)
    endforeach()
  endif()
endif()
