cmake_minimum_required(VERSION 2.6)
PROJECT(libreddnet-cmssw)

# not sure how to do this so it's default debug
# but allow override

set(CMAKE_BUILD_TYPE "Debug")
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake_modules)

enable_testing(true)
find_package(GTest REQUIRED)
FIND_PACKAGE(FUSE REQUIRED)
include_directories(
    ${FUSE_INCLUDE_DIR}
    ${GTEST_INCLUDE_DIRS}
)

add_definitions(${FUSE_DEFINITIONS})

# How to build the library
add_library( reddnetshim SHARED reddnet.cpp )
#target_link_libraries(reddnetshim ${GTEST_BOTH_LIBRARIES})

# Testing stuff
add_executable(test_reddnet reddnet.cpp test.cpp )
target_link_libraries(test_reddnet ${GTEST_BOTH_LIBRARIES})
add_test(AllTestsInFoo test_reddnet)


SET(CMAKE_C_FLAGS  "-g -Wall  -D_FILE_OFFSET_BITS=64 ${FUSE_CFLAGS}")

