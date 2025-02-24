#
# Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
# Copyright (c) 2024 Mohammad Nejati
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/boostorg/beast
#

find_package(Boost QUIET COMPONENTS system filesystem json log url beast)

add_executable(websocket-chat-multi
    beast.hpp
    chat_client.html
    http_session.cpp
    http_session.hpp
    Jamfile
    listener.cpp
    listener.hpp
    main.cpp
    net.hpp
    shared_state.cpp
    shared_state.hpp
    websocket_session.cpp
    websocket_session.hpp)

source_group("" FILES
    beast.hpp
    chat_client.html
    http_session.cpp
    http_session.hpp
    Jamfile
    listener.cpp
    listener.hpp
    main.cpp
    net.hpp
    shared_state.cpp
    shared_state.hpp
    websocket_session.cpp
    websocket_session.hpp)



target_include_directories(websocket-chat-multi PUBLIC
    $<$<BOOL:${WIN32}>:${VCPKG_INCLUDE_DIR}>
    ${Boost_INCLUDE_DIRS}
    ${PROJECT_SOURCE_DIR}
)
target_link_libraries(websocket-chat-multi PRIVATE
    ${Boost_LIBRARIES}
)
