#pragma once

#include <boost/url.hpp>
#include <iostream>
#include <string>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <filesystem>
#include <cstdio>
#include <stdexcept>

namespace urls = boost::urls;

namespace utils {

    // 辅助函数：拼接 base URL 与相对路径，确保只有一个斜杠相连
    std::string joinHttpUrl(const std::string& base_str, const std::string& path_str) {
        // 解析基础 URL
        std::string base_str_with_scheme = base_str;
        if (!base_str.empty() && (!base_str.starts_with("http://") && !base_str.starts_with("https://"))) {
            base_str_with_scheme = std::string("http://") + base_str;
        }
        auto base_res = urls::parse_uri(base_str_with_scheme);
        if (!base_res.has_value())
            throw std::runtime_error("Cannot parse base URL: " + base_str_with_scheme);
        urls::url base = base_res.value();

        // 解析相对 URL（作为相对引用处理）
        auto rel_res = urls::parse_relative_ref(path_str);
        if (!rel_res.has_value())
            throw std::runtime_error("无法解析相对 URL: " + path_str);
        urls::url rel = rel_res.value();

        // 获取基础 URL 和相对 URL 的路径部分，并显式转换到 std::string
        std::string base_path(base.encoded_path().data(), base.encoded_path().size());
        std::string rel_path(rel.encoded_path().data(), rel.encoded_path().size());

        // 如果基础 URL 的路径非空且不以 '/' 结尾，则补上 '/'
        if (!base_path.empty() && base_path.back() != '/')
            base_path.push_back('/');

        // 如果相对路径以 '/' 开头，则移除，以免重复
        if (!rel_path.empty() && rel_path.front() == '/')
            rel_path.erase(0, 1);

        // 拼接新路径
        std::string new_path = base_path + rel_path;

        // 处理 scheme 与 host，同样需要显式转换
        std::string scheme(base.scheme().data(), base.scheme().size());
        std::string host(base.host().data(), base.host().size());

        // 构造最终 URL
        std::string joined_url = scheme + "://" + host + new_path;
        return joined_url;
    }

    // 根据 URL 构建本地缓存文件完整路径
    // 例如：URL "http://example.com/a/b/c/d" 将被保存到 ".cache/example.com/a/b/c/d"
    std::filesystem::path urlToFilePath(const std::string& url) {
        std::string stripped = url;
        // 去掉协议头 "http://" 或 "https://"
        if (stripped.rfind("http://", 0) == 0) {
            stripped = stripped.substr(7);
        }
        else if (stripped.rfind("https://", 0) == 0) {
            stripped = stripped.substr(8);
        }

        // 分离主机名和路径部分
        size_t pos = stripped.find('/');
        std::string host;
        std::string pathPart;
        if (pos != std::string::npos) {
            host = stripped.substr(0, pos);
            pathPart = stripped.substr(pos);  // 包含初始 '/'
        }
        else {
            host = stripped;
            pathPart = "/index.html";  // 若没有路径，默认文件名
        }

        // 拼接文件路径：当前运行目录/.cache/host/后续路径
        std::filesystem::path filePath = std::filesystem::current_path() / ".cache" / host;
        // 使用 relative_path 去除 pathPart 开头的 '/'（确保路径拼接正确）
        filePath /= std::filesystem::path(pathPart).relative_path();
        return filePath;
    }
}