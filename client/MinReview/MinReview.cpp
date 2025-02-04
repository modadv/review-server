// MinReview.cpp : Defines the entry point for the application.
//

#include <iostream>
#include <chrono>
#include <boost/asio.hpp>
#include <boost/json.hpp>
// 假设之前实现的代码保存在 WebSocketClientManager.hpp 中

#include "Network.h"
#include "MinReview.h"
#include "HttpDownload.h"

using namespace std;
using boost::asio::io_context;
using boost::asio::steady_timer;
using boost::json::object;
using namespace std::chrono_literals;

int main() {
    // 创建 io_context 对象用于异步 IO
    io_context ioc;

    // 实例化协议处理注册中心，并注册协议处理回调函数
    ProtocolHandlerRegistry registry;

    registry.registerHandler(1, [](const std::string& host, int protocol_id, const json::object& data) {
        std::cout << "Handler for protocol " << protocol_id << " from " << host << " received data: " << data.at("address").as_string() << std::endl;
        });

    registry.registerHandler(2, [](const std::string& host, int protocol_id, const json::object& data) {
        std::cout << "Handler for protocol " << protocol_id << " from " << host << " received data: " << data.at("msg").as_string() << std::endl;
        });

    // 创建 WebSocket 客户端管理器，管理与不同服务端的连接
    WebSocketClientManager clientManager(ioc, registry);

    // 添加两个连接
    // 连接到 127.0.0.1:9002
    clientManager.addConnection("127.0.0.1", "8194");

    // 利用 asio::steady_timer 模拟延时发送消息，确保连接成功建立后再进行发送
    steady_timer timer(ioc, std::chrono::seconds(2));
    timer.async_wait([&clientManager](const boost::system::error_code& ec) {
        if (!ec) {
            // 构造 JSON 格式协议数据，此处仅包含 protocol_id 与 data 字段，
            // 服务器收到后可能会结合客户端标记的 host 信息进行处理
            object msg;
            msg["protocol_id"] = 1;
            msg["data"] = "Hello, server! This is a Review message.";

            // 通过指定 host 与 port ，选择向目标服务器发送数据
            clientManager.sendMessage("127.0.0.1", "8194", msg);
        }
        });

    // 运行 io_context 的事件循环，处理所有异步任务（连接、重连、消息收发等）
    ioc.run();

    return 0;
}