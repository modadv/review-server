# CMakeLists.txt : CMake project for MinReview, include source and define
# project specific logic here.

cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0167 NEW)

# 指定 vcpkg 工具链文件
set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenSSL 配置
if(DEFINED ENV{OPENSSL_ROOT_DIR})
  set(OPENSSL_ROOT_DIR $ENV{OPENSSL_ROOT_DIR})
endif()
find_package(OpenSSL REQUIRED)
if(NOT OpenSSL_FOUND)
  message(FATAL_ERROR "OpenSSL library not found! Please set the environment variable OPENSSL_ROOT_DIR to point to the OpenSSL installation directory.")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  set(VCPKG_TARGET_TRIPLET "x64-windows")
endif()
# 配置 CURL 和 EXPAT 的查找路径（基于 vcpkg 环境变量）
set(CURL_INCLUDE_DIR "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/include")
set(CURL_LIBRARY "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/lib/libcurl.lib")
set(CURL_DLL "$ENV{VCPKG_ROOT}/packages/curl_${VCPKG_TARGET_TRIPLET}/bin/libcurl.dll")

set(EXPAT_INCLUDE_DIR "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/include")
set(EXPAT_LIBRARY "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/lib/libexpat.lib")
set(EXPAT_DLL "$ENV{VCPKG_ROOT}/packages/expat_${VCPKG_TARGET_TRIPLET}/bin/libexpat.dll")

# 查找 Boost 库（采用 QUIET 模式，之后再手动检查）
find_package(Boost QUIET COMPONENTS system filesystem json log url)
if(NOT Boost_FOUND)
  message(FATAL_ERROR "Boost library not found! Please set the BOOST_ROOT environment variable to point to your Boost installation directory.")
endif()

# 添加项目源码并创建可执行文件
add_executable(MinReview
    "MinReview.cpp"
    "MinReview.h"
    "Network.h"
    "GlobalConfig.h"
    "XmlDownload.h"
    "IDataHandler.h"
    "XmlToJsonHandler.h"
    "HttpDownload.h"
    "Utils.h"
)

target_include_directories(MinReview PUBLIC
    ${PROJECT_SOURCE_DIR}/MinReview
    ${Boost_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIR}
)

target_link_libraries(MinReview PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    ${Boost_LIBRARIES}
    ${EXPAT_LIBRARY}
    ${CURL_LIBRARY}
)

# TODO: Add tests and install targets if needed.

# 添加构建后命令，复制 DLL 文件到目标生成目录
add_custom_command(TARGET MinReview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CURL_DLL}"  # DLL 文件源路径
        "$<TARGET_FILE_DIR:MinReview>"  # 目标生成目录
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${EXPAT_DLL}"  # DLL 文件源路径
        "$<TARGET_FILE_DIR:MinReview>"  # 目标生成目录
    COMMENT "Copying libexpatd.dll, ${CURL_DLL} to output directory"
)