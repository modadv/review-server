#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>

// 必须包含，以获得 boost::asio::buffer_cast 和 buffer_size 的定义
#include <boost/asio/buffer.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

namespace beast = boost::beast;         // Boost.Beast 核心
namespace http = beast::http;           // HTTP 相关
namespace asio = boost::asio;           // Boost.Asio
namespace ip = asio::ip;
namespace fs = std::filesystem;

/*
  HttpDownloader 封装了 HTTP 下载文件的接口：
    - download(host, target[, port]) 用于从指定 host 与 target 下载文件，
      保存至当前工作目录下的 ".cache/..." 目录结构中；
    - 当本地已有缓存文件时，会自动采用续传模式（使用 Range 请求）；
    - 为降低内存开销，采用分块读取方式，每次从 socket 中读取 8KB 数据，
      逐步提交给 HTTP response_parser 解析并从 dynamic_body 中提取数据写入文件；
    - 对连接和读写均设置超时（30 秒），以应对网络环境较差的情况。
*/
class HttpDownloader {
public:
    // 下载文件接口：
    //   host   : 服务器主机名（例如 "www.example.com"）
    //   target : 文件在服务器的 URL 路径（例如 "/path/to/file"）
    //   port   : 服务端口，默认为 "80"
    // 返回值:
    //   本地保存下载文件的完整路径（fs::path）
    static fs::path download(const std::string& host,
        const std::string& target,
        const std::string& port = "80")
    {
        // 根据 URL 路径构造本地保存路径：
        // 例如 target "/path/to/file" ——> 当前工作目录/.cache/path/to/file
        fs::path targetPath(target);
        if (targetPath.is_absolute())
            targetPath = targetPath.relative_path();

        std::string strTargetPath = targetPath.string();
        if (!strTargetPath.empty() && (strTargetPath.front() == '/' || strTargetPath.front() == '\\')) {
            strTargetPath.erase(0, 1);
            targetPath = fs::path(strTargetPath);
        }

        fs::path output_file = fs::current_path() / ".cache" / targetPath;
        fs::create_directories(output_file.parent_path());
        std::cout << "Save file at: " << output_file << "\n";

        // 判断是否存在部分下载，若存在则采用续传
        std::uintmax_t existing_file_size = 0;
        bool resume = false;
        if (fs::exists(output_file)) {
            existing_file_size = fs::file_size(output_file);
            if (existing_file_size > 0) {
                std::cout << "Detected partial file(" << existing_file_size
                    << " bytes), try to continue download...\n";
                resume = true;
            }
        }
        else {
            std::cout << "Had not detect partial file, download completely...\n";
        }

        // 创建 io_context、解析器与 TCP 流对象，并设置超时（30秒）
        asio::io_context ioc;
        ip::tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);
        stream.expires_after(std::chrono::seconds(30));

        // 解析域名并建立连接
        auto const results = resolver.resolve(host, port);
        stream.connect(results);

        // 构造 HTTP GET 请求，若需要续传则添加 Range 头
        http::request<http::empty_body> req{ http::verb::get, target, 11 };
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        if (resume) {
            req.set(http::field::range, "bytes=" + std::to_string(existing_file_size) + "-");
        }

        // 发送请求
        http::write(stream, req);

        // 使用 dynamic_body 构造响应解析器，允许逐步解析收到的数据
        http::response_parser<http::dynamic_body> parser;
        parser.body_limit(std::numeric_limits<std::uint64_t>::max());
        beast::flat_buffer buffer;
        constexpr std::size_t kBufferSize = 8192;
        bool header_done = false;
        std::size_t content_length = 0;
        std::size_t bytes_downloaded = resume ? existing_file_size : 0;

        // 打开文件，续传时采用追加模式，否则采用覆盖模式
        std::ofstream file(output_file, std::ios::binary | std::ios::app);
        if (!file)
            throw std::runtime_error("Cannot open output file " + output_file.string());

        // 主循环：不断从 socket 读取数据，提交给 parser，再写入文件
        for (;;) {
            beast::error_code ec;
            // 从 socket 读取最多 kBufferSize 字节到 buffer 中
            std::size_t bytes = stream.socket().read_some(buffer.prepare(kBufferSize), ec);
            if (ec == asio::error::eof) {
                // 连接正常关闭，下载完成
                break;
            }
            if (ec)
                throw beast::system_error{ ec };

            buffer.commit(bytes);

            // 将收到的数据提交给 parser 进行解析
            std::size_t bytes_parsed = parser.put(buffer.data(), ec);
            if (ec && ec != http::error::need_more)
                throw beast::system_error{ ec };

            buffer.consume(bytes_parsed);

            // 当 header 解析完成时进行处理
            if (!header_done && parser.is_header_done()) {
                header_done = true;
                auto res = parser.get();

                // 续传请求，状态码应为 206；否则为 200
                if (resume && res.result() != http::status::partial_content)
                    throw std::runtime_error("Continue download error, server response code:" + std::to_string(res.result_int()));
                else if (!resume && res.result() != http::status::ok)
                    throw std::runtime_error("Download failed, server response code:" + std::to_string(res.result_int()));

                // 尝试解析 Content-Length（如果存在）
                if (res.find(http::field::content_length) != res.end()) {
                    content_length = std::stoull(std::string(res[http::field::content_length]));
                }
            }

            // 当 header 解析完成后，从 parser 得到的 body 中提取数据并写入文件
            if (header_done) {
                auto& body = parser.get().body();
                while (body.size() > 0) {
                    // 使用基于范围的 for 循环遍历缓冲区链
                    auto buffers = body.data();
                    for (auto const& buffer_elem : buffers) {
                        // 通过 boost::asio::buffer_cast 获取数据指针和大小
                        const char* data_ptr = static_cast<const char*>(buffer_elem.data());
                        std::size_t chunk_size = boost::asio::buffer_size(buffer_elem);
                        file.write(data_ptr, chunk_size);
                        bytes_downloaded += chunk_size;

                        // 可选：输出下载进度（若存在 Content-Length）
                        if (content_length > 0) {
                            double progress = (static_cast<double>(bytes_downloaded) / content_length) * 100.0;
                            std::cout << "\rDownload progress:" << progress << "%" << std::flush;
                        }
                    }
                    body.consume(body.size());
                }
            }

            // 如果 parser 已完成解析，则退出循环
            if (parser.is_done())
                break;
        }
        file.close();

        // 关闭连接，忽略未连接错误
        beast::error_code shutdown_ec;
        stream.socket().shutdown(ip::tcp::socket::shutdown_both, shutdown_ec);
        if (shutdown_ec && shutdown_ec != beast::errc::not_connected)
            throw beast::system_error{ shutdown_ec };

        std::cout << "\nDownload Successfully, save file at:" << output_file << "\n";
        return output_file;
    }
};

//
// 示例：调用 HttpDownloader::download 下载文件
//
// 
// void testHTTPDownload() {
//try {
//    std::string host = "127.0.0.1";
//    std::string port = "80";
//    std::string target = "/run/results/AP-M003CM-EA.2955064502/20250116/T_20241018193101867_1_NG/report.xml";
//    fs::path downloaded_file = HttpDownloader::download(host, target, port);
//    std::cout << "Download successfully, file save at: " << downloaded_file << "\n";
//}
//catch (const std::exception& e) {
//    std::cerr << "Error: " << e.what() << "\n";
//}
//}
//int main() {
//    testHTTPDownload();
//    return EXIT_SUCCESS;
//}