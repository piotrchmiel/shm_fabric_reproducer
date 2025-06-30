cmake_minimum_required(VERSION 3.0)

set(CMAKE_CXX_STANDARD 20 CACHE STRING "The C++ standard to use")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

project(
  Test
  VERSION 1.0
  LANGUAGES CXX)

include(ExternalProject)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
  set(EN_DEBUG "--enable-debug")
endif()

set(LIBFABRIC_PROVIDER_OPTS "${EN_DEBUG}")

set(LIBFABRIC_INSTALL_DIR ${CMAKE_BINARY_DIR}/third_party/libfabric)
message(STATUS "LIBFABRIC_INSTALL_DIR " ${LIBFABRIC_INSTALL_DIR})
message(STATUS "EN_DEBUG " ${EN_DEBUG})


include(ProcessorCount)
ProcessorCount(NUM_CPUS)

ExternalProject_Add(
    libfabric_external
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libfabric
    PREFIX ${LIBFABRIC_INSTALL_DIR}
    BUILD_IN_SOURCE TRUE
    BUILD_JOB_SERVER_AWARE TRUE
    CONFIGURE_COMMAND
      sh -c "./autogen.sh && ./configure --prefix=${LIBFABRIC_INSTALL_DIR} \
             ${LIBFABRIC_PROVIDER_OPTS}"
    BUILD_COMMAND make -j ${NUM_CPUS}
    INSTALL_COMMAND make install
)

set(LIBFABRIC_INCLUDE_DIR ${LIBFABRIC_INSTALL_DIR}/include)
include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libfabric/include)
set(LIBFABRIC_LIBRARY_PATH ${LIBFABRIC_INSTALL_DIR}/lib/${CMAKE_FIND_LIBRARY_PREFIXES}fabric.so)
set(LIBFABRIC_LIBRARY fabric)
add_library(${LIBFABRIC_LIBRARY} SHARED IMPORTED)
add_dependencies(${LIBFABRIC_LIBRARY} libfabric_external)

set_property(TARGET ${LIBFABRIC_LIBRARY} PROPERTY IMPORTED_LOCATION ${LIBFABRIC_LIBRARY_PATH})
file(MAKE_DIRECTORY ${LIBFABRIC_INCLUDE_DIR})
target_include_directories(${LIBFABRIC_LIBRARY} INTERFACE ${LIBFABRIC_INCLUDE_DIR})

add_executable(reproducer main.cpp)
add_dependencies(reproducer libfabric_external)
target_link_libraries(reproducer ${LIBFABRIC_LIBRARY})
