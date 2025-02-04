# CMakeList.txt : CMake project for MinReview, include source and define
# project specific logic here.
#
set(CMAKE_CXX_STANDARD 20) 
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# Add source to this project's executable.
add_executable (MinReview
"MinReview.cpp"
"MinReview.h"
"Network.h"
"GlobalConfig.h"
"HttpDownload.h"
"IDataHandler.h"
"XmlToJsonHandler.h"
)

find_package(Boost 1.87.0 REQUIRED COMPONENTS system filesystem json log)
find_package(OpenSSL REQUIRED)

set(EXPAT_ROOT $ENV{EXPAT_ROOT})
set(EXPAT_INCLUDE_DIR ${EXPAT_ROOT}/include)
set(EXPAT_LIBRARY ${EXPAT_ROOT}/lib/libexpatd.lib)
set(EXPAT_DLL_PATH ${EXPAT_ROOT}/bin/libexpatd.dll)

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

# 添加构建后命令
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${EXPAT_DLL_PATH}"  # DLL 文件源路径
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"  # 目标生成目录
    COMMENT "Copying libexpatd.dll to output directory"
)