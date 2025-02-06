#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <iostream>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <chrono>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace json = boost::json;

using tcp = asio::ip::tcp;

/*
 * 修改后的协议处理回调类型：除了 protocol_id 与 data 外，还增加了 host 字段，
 * 用于标识数据来源的服务器地址。
 */
using ProtocolHandler = std::function<void(const std::string& host, int protocol_id, const json::object& data)>;

//
// 协议处理注册与调度类
//
class ProtocolHandlerRegistry {
public:
	// 注册某个协议号对应处理函数
	void registerHandler(int protocol_id, ProtocolHandler handler) {
		handlers_[protocol_id] = std::move(handler);
	}

	// 调用对应协议号的处理函数，传入 host、protocol_id 与 data
	void handleProtocol(const std::string& host, int protocol_id, const json::object& data) {
		auto it = handlers_.find(protocol_id);
		if (it != handlers_.end()) {
			it->second(host, protocol_id, data);
		}
		else {
			std::cerr << "No handler found for protocol: " << protocol_id << " from host: " << host << std::endl;
		}
	}

private:
	std::unordered_map<int, ProtocolHandler> handlers_;
};

//
// 基于 boost::beast 实现的 WebSocket 客户端，封装了连接、发送数据、以及异步接收数据的逻辑。
// 修改的地方在于：
// - 在成功连接后保存 host 信息。
// - 在数据解析后，调用统一的 ProtocolHandlerRegistry，并将 host 一同传递出去。
// - 当异步读取出现错误时，会调用 on_disconnect_ 回调（由上层管理类进行重连）
//
class WebSocketClient {
public:
	WebSocketClient(asio::io_context& ioc, ProtocolHandlerRegistry& registry)
		: ws_(ioc),
		resolver_(ioc),
		registry_(registry) {
	}

	// 连接到指定 host:port 的 WebSocket 服务端（同步方式）
	void connect(const std::string& host, const std::string& port) {
		host_ = host;
		auto const results = resolver_.resolve(host, port);
		asio::connect(ws_.next_layer(), results.begin(), results.end());
		ws_.handshake(host, "/ws");
		std::cout << "Connected to WebSocket server: " << host << ":" << port << std::endl;
		receiveData();
	}

	// 发送 JSON 格式数据（会序列化为字符串）
	void sendProtocol(const json::object& data) {
		std::string message = json::serialize(data);
		ws_.write(asio::buffer(message));
		std::cout << "Send protocol " << data.at("protocol_id").as_int64() << ": " << message << std::endl;
	}

	// 断线回调，当内部异步读取出现错误时会调用该回调通知上层。
	std::function<void()> on_disconnect_;

private:
	// 异步接收数据，通过 async_read 读取数据并进行 JSON 解析，
	// 成功后调用 ProtocolHandlerRegistry::handleProtocol，传入 host、protocol_id 和 data。
	void receiveData() {
		ws_.async_read(buffer_, [this](beast::error_code ec, std::size_t /*bytes_transferred*/) {
			if (ec) {
				std::cerr << "Error receiving data from " << host_ << ": " << ec.message() << std::endl;
				if (on_disconnect_) {
					on_disconnect_();
				}
				return;
			}

			std::string received_data(
				boost::asio::buffers_begin(buffer_.data()),
				boost::asio::buffers_end(buffer_.data())
			);

			std::cout << "Received data from " << host_ << ": " << received_data << std::endl;
			try {
				auto parsed_json = json::parse(received_data);
				auto& obj = parsed_json.as_object();
				int protocol_id = obj.at("protocol_id").as_int64();
				if (obj.at("data").is_string()) {
					std::cout << "Simple message from " << host_ << " : " << obj.at("data").as_string() << std::endl;
				}
				else {
					auto& data = obj.at("data").as_object();
					// 将接收到的数据（附带服务端 host 信息）传递给统一的数据处理入口
					registry_.handleProtocol(host_, protocol_id, data);
				}
			}
			catch (const std::exception& e) {
				std::cerr << "Error parsing received data: " << e.what() << std::endl;
			}

			buffer_.consume(buffer_.size());
			receiveData();
			});
	}

	websocket::stream<tcp::socket> ws_;
	tcp::resolver resolver_;
	beast::flat_buffer buffer_;
	ProtocolHandlerRegistry& registry_;
	std::string host_;
};

//
// ManagedWebSocketClient 封装了 WebSocketClient，并增加了断线自动重连逻辑。
// 当连接断开（或连接出错）后，会利用 asio::steady_timer 延时后尝试重连。
//
class ManagedWebSocketClient : public std::enable_shared_from_this<ManagedWebSocketClient> {
public:
	ManagedWebSocketClient(asio::io_context& ioc, const std::string& host, const std::string& port, ProtocolHandlerRegistry& registry)
		: ioc_(ioc),
		host_(host),
		port_(port),
		registry_(registry),
		client_(ioc, registry),
		reconnect_timer_(ioc) {
	}

	// 启动连接
	void start() {
		do_connect();
	}

	// 发送消息，直接调用内部 WebSocketClient 的 sendProtocol
	void send(const json::object& msg) {
		client_.sendProtocol(msg);
	}

private:
	// 尝试建立连接
	void do_connect() {
		try {
			client_.connect(host_, port_);
			// 设置当 WebSocketClient 在异步读取中出错时的回调
			client_.on_disconnect_ = [self = shared_from_this()]() {
				self->on_disconnect();
				};
		}
		catch (const std::exception& e) {
			std::cerr << "Failed to connect to " << host_ << ":" << port_ << " Error: " << e.what() << std::endl;
			schedule_reconnect();
		}
	}

	// 当连接断开时，打印日志并调用 schedule_reconnect
	void on_disconnect() {
		std::cerr << "Disconnected from " << host_ << ":" << port_ << ", trying to reconnect..." << std::endl;
		schedule_reconnect();
	}

	// 使用 asio::steady_timer 在延时后触发重连
	void schedule_reconnect() {
		reconnect_timer_.expires_after(std::chrono::seconds(5));
		reconnect_timer_.async_wait([self = shared_from_this()](const beast::error_code& ec) {
			if (!ec) {
				std::cout << "Attempting reconnection to " << self->host_ << ":" << self->port_ << std::endl;
				self->do_connect();
			}
			});
	}

	asio::io_context& ioc_;
	std::string host_;
	std::string port_;
	ProtocolHandlerRegistry& registry_;
	WebSocketClient client_;
	asio::steady_timer reconnect_timer_;
};

//
// WebSocketClientManager 类：管理多个 ManagedWebSocketClient 连接，上层可通过 host + port 作为 key 快速查找，并调用发送函数。
// 同时也可以根据需要扩展删除连接等接口。
//
class WebSocketClientManager {
public:
	WebSocketClientManager(asio::io_context& ioc, ProtocolHandlerRegistry& registry)
		: ioc_(ioc),
		registry_(registry) {
	}

	// 创建并启动到目标服务端的连接
	void addConnection(const std::string& host, const std::string& port) {
		std::string key = host + ":" + port;
		if (connections_.find(key) != connections_.end()) {
			std::cerr << "Connection already exists for " << key << std::endl;
			return;
		}
		auto managed_client = std::make_shared<ManagedWebSocketClient>(ioc_, host, port, registry_);
		connections_[key] = managed_client;
		managed_client->start();
	}

	// 根据 host:port 删除一个连接（可扩展为连接关闭逻辑）
	void removeConnection(const std::string& host, const std::string& port) {
		std::string key = host + ":" + port;
		auto it = connections_.find(key);
		if (it != connections_.end()) {
			// 这里可以增加关闭连接的逻辑
			connections_.erase(it);
		}
	}

	// 向指定 host:port 的连接发送数据
	void sendMessage(const std::string& host, const std::string& port, const json::object& msg) {
		std::string key = host + ":" + port;
		auto it = connections_.find(key);
		if (it != connections_.end()) {
			it->second->send(msg);
		}
		else {
			std::cerr << "No connection found for " << key << std::endl;
		}
	}

private:
	asio::io_context& ioc_;
	ProtocolHandlerRegistry& registry_;
	std::unordered_map<std::string, std::shared_ptr<ManagedWebSocketClient>> connections_;
};