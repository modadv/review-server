# CMakeList.txt : CMake project for MinReview, include source and define
# project specific logic here.
#

# Add source to this project's executable.
add_executable (MinReview "MinReview.cpp" "MinReview.h" "Network.h" "GlobalConfig.h")

find_package(Boost 1.87.0 REQUIRED COMPONENTS system filesystem json log)
find_package(OpenSSL REQUIRED)

message(STATUS "Include Dir: ${CMAKE_CURRENT_SOURCE_DIR}/MinReview")

target_include_directories(${PROJECT_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/MinReview
    ${Boost_INCLUDE_DIRS}
    ${EXPAT_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    ${Boost_LIBRARIES}
    ${EXPAT_LIBRARY}
)

# TODO: Add tests and install targets if needed.
