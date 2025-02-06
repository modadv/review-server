#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>

#include <Utils.h>
#include <XmlToJsonHandler.h>

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

class XmlDownloader {
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
        std::unique_ptr<IDataHandler> data_handler = nullptr;
        // Construct local path base on the URL：
        // Such as target "/path/to/file" ——> current/working/directory/.cache/path/to/file
        std::string url = utils::joinHttpUrl(host, target);
        std::cout << "Join url:" << url << std::endl;

        fs::path output_file = utils::urlToFilePath(url);
        std::cout << "Save file at: " << output_file << "\n";

        fs::create_directories(output_file.parent_path());

        // Get result path
        std::string result_url = url;
        size_t result_pos = result_url.rfind("/report.xml");
        if (result_pos != std::string::npos) {
            result_url = result_url.substr(0, result_pos + 1);
        }

        // 判断是否存在部分下载，若存在则采用续传
        std::uintmax_t existing_file_size = 0;
        bool resume = false;
        if (fs::exists(output_file)) {
            existing_file_size = fs::file_size(output_file);
            if (existing_file_size > 0) {
                std::cout << "Detected partial file(" << existing_file_size << " bytes), try to continue download...\n";
                resume = true;
            }
        }
        else {
            std::cout << "Had not detect partial file, download whole file...\n";
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

        const std::string project_prefix("../../../../../program/projects/");
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
                if (resume && res.result() == http::status::range_not_satisfiable) {
                    std::cout << "File had been downloaded completly." << std::endl;
                    return output_file;
                }
                else if (resume && res.result() != http::status::partial_content) {
                    throw std::runtime_error("Continue download error, server response code:" + std::to_string(res.result_int()));
                }
                else if (!resume && res.result() != http::status::ok) {
                    throw std::runtime_error("Download failed, server response code:" + std::to_string(res.result_int()));
                }

                // 尝试解析 Content-Length（如果存在）
                if (res.find(http::field::content_length) != res.end()) {
                    content_length = std::stoull(std::string(res[http::field::content_length]));
                }

                if (existing_file_size == 0) {
                    data_handler = std::make_unique<XmlToJsonDataHandler>(output_file.replace_extension(".json").string(),
                        [&host, &result_url, &project_prefix](std::string res_path) {
                            std::string comp_res_url;
                            size_t prefix_pos = res_path.find(project_prefix);
                            if (prefix_pos != std::string::npos) {
                                comp_res_url = "http://" + host + "/program/projects/" + res_path.substr(project_prefix.size(), res_path.size() - project_prefix.size());
                            }
                            else {
                                comp_res_url = result_url + res_path;
                            }

                            std::cout << "Download task url:" << comp_res_url << std::endl;
                            HTTPDownloader::getInstance().addDownloadTask(comp_res_url, [](const std::string& url, const std::string& local_path, bool success) {
                                std::cout << "Download callback: " << local_path << " : " << (success ? "Successfully" : "Failed") << std::endl;
                                });
                        });
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
                        if (data_handler) {
                            data_handler->handleData(static_cast<const char*>(buffer_elem.data()), chunk_size);
                        }
                        file.write(data_ptr, chunk_size);
                        bytes_downloaded += chunk_size;

                        // 可选：输出下载进度（若存在 Content-Length）
                        if (content_length > 0) {
                            double progress = (static_cast<double>(bytes_downloaded) / content_length) * 100.0;
                            std::cout << "\rDownload progress:" << progress << "%\n" << std::flush;
                        }
                    }
                    body.consume(body.size());
                }
            }

            // 如果 parser 已完成解析，则退出循环
            if (parser.is_done())
                break;
        }

        if (data_handler) {
            data_handler->finalize();
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
