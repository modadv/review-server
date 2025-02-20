#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <boost/lexical_cast.hpp>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <set>
#include <deque>
#include <mutex>
#include <sstream>
#include <map>

using tcp = boost::asio::ip::tcp;
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace json = boost::json;

// ---------------------------------------
// 常量定义，与 Golang 代码中定义类似
// ---------------------------------------
std::chrono::seconds const write_wait(10);
std::chrono::seconds const pong_wait(60);
// 这里使用 54 秒（即 9/10 * 60）作为 Ping 周期
std::chrono::seconds const ping_period(54);

// 前缀常量，用于剥离 tasks URL 中 address 参数的前缀
std::string const resultPrefix = "/home/aoi/aoi";

// ---------------------------------------
// 共享状态（Hub）: 管理所有活跃的 WebSocket 客户端
// ---------------------------------------
class ws_session;
class shared_state : public std::enable_shared_from_this<shared_state> {
public:
    // 存放所有活跃的 WebSocket 会话（用 shared_ptr 管理会话生命周期）
    std::set<std::shared_ptr<ws_session>> sessions_;
    std::mutex mutex_;

    void join(std::shared_ptr<ws_session> session) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.insert(session);
        std::cout << "Client registered: " << session->id() << "\n";
    }

    void leave(std::shared_ptr<ws_session> session) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(session);
        std::cout << "Client unregistered: " << session->id() << "\n";
    }

    void broadcast(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "Broadcasting message: " << message << "\n";
        for (auto const& session : sessions_) {
            session->deliver(message);
        }
    }
};
// ---------------------------------------
// WebSocket 会话对象：处理单个 WebSocket 客户端连接
// ---------------------------------------
class ws_session : public std::enable_shared_from_this<ws_session> {
    websocket::stream<tcp::socket> ws_;
    std::shared_ptr<shared_state> state_;
    beast::flat_buffer buffer_;
    std::deque<std::string> write_queue_;

    // 定时发送 ping 消息
    net::steady_timer ping_timer_;

    // 客户端标识（客户端的 IP:Port）
    std::string id_;

public:
    explicit ws_session(tcp::socket socket, std::shared_ptr<shared_state> const& state)
        : ws_(std::move(socket))
        , state_(state)
        , ping_timer_(ws_.get_executor(), ping_period)
    {
        try {
            auto ep = ws_.next_layer().remote_endpoint();
            id_ = ep.address().to_string() + ":" + std::to_string(ep.port());
        }
        catch (std::exception &ex) {
            id_ = "unknown";
        }
    }

    std::string id() const { return id_; }

    // 启动 WebSocket 会话：完成握手并开始读取消息
    void start() {
        // 设置 websocket 参数，如设置读缓存大小（与 Golang 中 Upgrade 参数类似）
        ws_.binary(false);
        // 接受 WebSocket 握手
        ws_.async_accept(
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) {
                    std::cerr << "WebSocket Accept error: " << ec.message() << "\n";
                    return;
                }
                // 注册到共享状态（Hub）
                self->state_->join(self);
                // 启动 Ping 定时器
                self->do_ping();
                // 开始读取消息
                self->do_read();
            });
    }

    // 异步读取 WebSocket 消息
    void do_read() {
        ws_.async_read(buffer_,
            [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
                self->on_read(ec, bytes_transferred);
            });
    }

    void on_read(beast::error_code ec, std::size_t /*bytes_transferred*/) {
        if (ec == websocket::error::closed) {
            return;
        }
        if (ec) {
            std::cerr << "WebSocket Read error (" << id_ << "): " << ec.message() << "\n";
            state_->leave(shared_from_this());
            return;
        }

        // 将收到的消息转换为 string
        std::string msg = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        // 尝试解析收到的 JSON 数据
        json::error_code jec;
        json::value jv = json::parse(msg, jec);
        if (jec) {
            std::cerr << "JSON parse error from " << id_ << ": " << jec.message() << "\n";
        }
        else if (jv.is_object()) {
            json::object obj = jv.as_object();
            if (!obj.contains("protocol_id") || !obj.contains("data")) {
                std::cerr << "Received message missing protocol fields from " << id_ << "\n";
            }
            else {
                int protocol_id = 0;
                try {
                    protocol_id = static_cast<int>(obj["protocol_id"].as_int64());
                }
                catch (...) {
                    std::cerr << "Invalid protocol_id type from " << id_ << "\n";
                }
                // 根据 protocol_id 选择处理分支
                if (protocol_id == 1) {
                    // 例如：data字段是一个字符串，回显时附加 "# Review Finished"
                    std::string original;
                    try {
                        // 这里假设 data 为字符串类型
                        original = obj["data"].as_string().c_str();
                    }
                    catch (...) {
                        original = "";
                    }
                    // 构造响应数据
                    json::object data;
                    data["msg"] = original + " # Review Finished";
                    json::object resp;
                    resp["protocol_id"] = 2;
                    resp["data"] = data;
                    std::string resp_str = json::serialize(resp);
                    // 回写给当前客户端
                    deliver(resp_str);
                    std::cout << "Echoed message to " << id_ << ": " << resp_str << "\n";
                }
                else if (protocol_id == 2) {
                    // 对于 protocol_id = 2，表示复判结果（ReviewResult）
                    // 例如，data 中包含 host、target、model、version 信息
                    try {
                        json::object data_obj = obj["data"].as_object();
                        std::string host = data_obj["host"].as_string().c_str();
                        std::string target = data_obj["target"].as_string().c_str();
                        std::cout << "////////Review_999:Received_review_result////////" 
                                  << host << " " << target << "\n";
                    }
                    catch (...) {
                        std::cerr << "Error parsing review result from " << id_ << "\n";
                    }
                }
                else {
                    std::cerr << "Unsupported protocol_id " << protocol_id << " from " << id_ << "\n";
                }
            }
        }

        // 继续读取下一条消息
        do_read();
    }

    // 将消息添加到写队列中，并启动写操作（如果队列中没有写操作在进行）
    void deliver(const std::string& msg) {
        net::post(ws_.get_executor(),
            [self = shared_from_this(), msg]() {
                bool write_in_progress = !self->write_queue_.empty();
                self->write_queue_.push_back(msg);
                if (!write_in_progress)
                    self->do_write();
            });
    }

    void do_write() {
        ws_.async_write(net::buffer(write_queue_.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t bytes_transferred) {
                self->on_write(ec, bytes_transferred);
            });
    }

    void on_write(beast::error_code ec, std::size_t /*bytes_transferred*/) {
        if (ec) {
            std::cerr << "WebSocket Write error (" << id_ << "): " << ec.message() << "\n";
            state_->leave(shared_from_this());
            return;
        }
        write_queue_.pop_front();
        if (!write_queue_.empty()) {
            do_write();
        }
    }

    // 定时发送 Ping 消息以维持连接
    void do_ping() {
        ping_timer_.expires_after(ping_period);
        ping_timer_.async_wait(
            [self = shared_from_this()](beast::error_code ec) {
                if (ec)
                    return;
                self->ws_.async_ping({},
                    [self](beast::error_code ec) {
                        if (ec) {
                            std::cerr << "WebSocket Ping error (" << self->id() << "): " << ec.message() << "\n";
                            self->state_->leave(self);
                            return;
                        }
                        self->do_ping();
                    });
            });
    }
};



// ---------------------------------------
// 辅助函数：解析 URL 查询参数，返回 key->value 映射
// ---------------------------------------

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> query_map;
    std::istringstream iss(query);
    std::string token;
    while (std::getline(iss, token, '&')) {
        auto pos = token.find('=');
        if (pos != std::string::npos) {
            std::string key = token.substr(0, pos);
            std::string value = token.substr(pos + 1);
            query_map[key] = value;
        }
    }
    return query_map;
}

// ---------------------------------------
// HTTP 会话：处理一个 TCP 连接上的 HTTP 请求
// ---------------------------------------
class http_session : public std::enable_shared_from_this<http_session> {
    tcp::socket socket_;
    beast::flat_buffer buffer_;
    std::shared_ptr<shared_state> state_;
    http::request<http::string_body> req_;

public:
    http_session(tcp::socket socket, std::shared_ptr<shared_state> const& state)
        : socket_(std::move(socket))
        , state_(state)
    {
    }

    void run() {
        do_read();
    }

private:
    void do_read() {
        auto self = shared_from_this();
        http::async_read(socket_, buffer_, req_,
            [self](beast::error_code ec, std::size_t bytes_transferred) {
                self->on_read(ec, bytes_transferred);
            });
    }

    void on_read(beast::error_code ec, std::size_t /*bytes_transferred*/) {
        if (ec == http::error::end_of_stream)
            return do_close();
        if (ec) {
            std::cerr << "HTTP Read error: " << ec.message() << "\n";
            return;
        }
        handle_request();
    }

    // 根据请求路径分发处理：
    //   - /ws：升级为 WebSocket 连接
    //   - /tasks：执行 tasksHandler 业务逻辑
    //   - /setting：执行 settingHandler 业务逻辑
    void handle_request() {
        std::string target = req_.target().to_string();
        // 如果请求路径以 "/ws" 开头，则升级为 WebSocket 会话
        if (target.find("/ws") == 0) {
            // 创建 WebSocket 会话对象，并让其接管 socket
            std::make_shared<ws_session>(std::move(socket_), state_)->start();
            return;
        }
        else if (target.find("/tasks") == 0) {
            // tasks handler
            // 获取客户端 IP、Port（用于日志输出）
            std::string client_ip = "unknown";
            std::string client_port = "unknown";
            try {
                auto ep = socket_.remote_endpoint();
                client_ip = ep.address().to_string();
                client_port = std::to_string(ep.port());
            }
            catch (...) {}
            std::cout << "Request /tasks has been processed from IP: " << client_ip
                      << ", Port: " << client_port << "\n";

            // 解析目标字符串（例如： "/tasks?address=...&model=...&version=..."）
            std::string path, query;
            auto pos = target.find("?");
            if (pos != std::string::npos) {
                path = target.substr(0, pos);
                query = target.substr(pos + 1);
            }
            else {
                path = target;
            }

            auto params = parse_query(query);
            std::string addressParam = params["address"];
            std::string modelParam   = params["model"];
            std::string versionParam = params["version"];

            // 移除 address 的固定前缀
            std::string relativeAddress = addressParam;
            if (relativeAddress.find(resultPrefix) == 0) {
                relativeAddress = relativeAddress.substr(resultPrefix.size());
            }
            std::cout << "////////Review_1:Received_from_Inspector////////" 
                      << client_ip << " " << relativeAddress << "\n";

            // 构造 JSON 数据
            json::object data;
            data["host"]    = client_ip;
            data["target"]  = relativeAddress;
            data["model"]   = modelParam;
            data["version"] = versionParam;
            json::object messageWrapper;
            messageWrapper["protocol_id"] = 1;
            messageWrapper["data"] = data;
            std::string json_msg = json::serialize(messageWrapper);

            std::cout << "////////Review_2:Start_broadcast////////" 
                      << client_ip << " " << relativeAddress << "\n";

            // 广播给所有 WebSocket 客户端
            state_->broadcast(json_msg);

            // 返回纯文本响应
            http::response<http::string_body> res{ http::status::ok, req_.version() };
            res.set(http::field::server, "Boost.Beast");
            res.set(http::field::content_type, "text/plain; charset=utf-8");
            res.keep_alive(req_.keep_alive());
            res.body() = "Request /tasks processed and info broadcasted to websocket clients.";
            res.prepare_payload();
            return write_response(std::move(res));
        }
        else if (target.find("/setting") == 0) {
            // setting handler
            std::string client_ip = "unknown";
            std::string client_port = "unknown";
            try {
                auto ep = socket_.remote_endpoint();
                client_ip = ep.address().to_string();
                client_port = std::to_string(ep.port());
            }
            catch (...) {}

            std::cout << "Request /setting has been processed from IP: " << client_ip
                      << ", Port: " << client_port << "\n";

            http::response<http::string_body> res{ http::status::ok, req_.version() };
            res.set(http::field::server, "Boost.Beast");
            res.set(http::field::content_type, "text/plain; charset=utf-8");
            res.keep_alive(req_.keep_alive());
            res.body() = "Request /setting has been processed: " + std::string(req_.at(http::field::host));
            res.prepare_payload();
            return write_response(std::move(res));
        }
        else {
            // 未找到匹配的处理路径，返回 404
            http::response<http::string_body> res{ http::status::not_found, req_.version() };
            res.set(http::field::server, "Boost.Beast");
            res.set(http::field::content_type, "text/plain; charset=utf-8");
            res.keep_alive(req_.keep_alive());
            res.body() = "The resource '" + target + "' was not found.";
            res.prepare_payload();
            return write_response(std::move(res));
        }
    }

    void write_response(http::response<http::string_body>&& res) {
        auto self = shared_from_this();
        auto sp = std::make_shared<http::response<http::string_body>>(std::move(res));
        http::async_write(socket_, *sp,
            [self, sp](beast::error_code ec, std::size_t) {
                self->socket_.shutdown(tcp::socket::shutdown_send, ec);
            });
    }

    void do_close() {
        beast::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_send, ec);
    }
};

// ---------------------------------------
// 监听器：接受传入的 TCP 连接，并为每个连接创建一个 HTTP 会话
// ---------------------------------------
class listener : public std::enable_shared_from_this<listener> {
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    std::shared_ptr<shared_state> state_;

public:
    listener(net::io_context& ioc, tcp::endpoint endpoint, std::shared_ptr<shared_state> state)
        : ioc_(ioc)
        , acceptor_(net::make_strand(ioc))
        , state_(state)
    {
        beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            std::cerr << "Open error: " << ec.message() << "\n";
            return;
        }
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            std::cerr << "Set_option error: " << ec.message() << "\n";
            return;
        }
        acceptor_.bind(endpoint, ec);
        if (ec) {
            std::cerr << "Bind error: " << ec.message() << "\n";
            return;
        }
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cerr << "Listen error: " << ec.message() << "\n";
            return;
        }
    }

    void run() {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept(net::make_strand(ioc_),
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                self->on_accept(ec, std::move(socket));
            });
    }

    void on_accept(beast::error_code ec, tcp::socket socket) {
        if (ec) {
            std::cerr << "Accept error: " << ec.message() << "\n";
        } else {
            // 创建一个新的 HTTP 会话，处理该连接
            std::make_shared<http_session>(std::move(socket), state_)->run();
        }
        do_accept();
    }
};

// ---------------------------------------
// 主函数
// ---------------------------------------
int main(int argc, char* argv[]) {
    try {
        // 默认监听地址和端口，例如 "0.0.0.0:8194"
        std::string address = "0.0.0.0";
        unsigned short port = 8194;
        if (argc == 2) {
            // 可传入形如 "0.0.0.0:8194" 的字符串
            std::string arg = argv[1];
            auto pos = arg.find(":");
            if (pos != std::string::npos) {
                address = arg.substr(0, pos);
                port = static_cast<unsigned short>(std::stoi(arg.substr(pos + 1)));
            }
        }
        net::io_context ioc{ 1 };

        // 创建共享状态（Hub）
        auto state = std::make_shared<shared_state>();

        // 创建并运行监听器
        tcp::endpoint endpoint{ net::ip::make_address(address), port };
        std::make_shared<listener>(ioc, endpoint, state)->run();

        std::cout << "Service started, listening on: " << address << ":" << port << "\n";

        ioc.run();
    }
    catch (std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
    }
    return 0;
}