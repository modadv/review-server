#include <iostream>
#include <string>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <filesystem>
#include <cstdio>
#include <curl/curl.h>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

#include <Utils.h>

// HTTPDownloader 下载模块实现
class HTTPDownloader {
public:
    // 回调函数定义：参数1为下载 URL，参数2表示下载是否成功
    using DownloadCallback = std::function<void(const std::string&, bool)>;

    // 获取单例实例
    static HTTPDownloader& getInstance() {
        static HTTPDownloader instance;
        return instance;
    }

    // 添加下载任务。若相同URL的任务已在下载，则忽略重复添加
    void addDownloadTask(const std::string& url, DownloadCallback callback = nullptr) {
        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            if (activeTasks_.find(url) != activeTasks_.end()) {
                std::cout << "任务 " << url << " 已在下载中，忽略重复添加。\n";
                return;
            }
            activeTasks_.insert(url);
        }
        boost::asio::post(pool_, [this, url, callback] {
            downloadTask(url, callback);
            });
    }

    // 等待所有任务执行完成（一般在程序退出前调用）
    void waitForTasks() {
        pool_.join();
    }

private:
    // 私有构造函数，初始化线程池和 libcurl 全局环境
    HTTPDownloader()
        : pool_(std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2) {
        curl_global_init(CURL_GLOBAL_ALL);
    }

    ~HTTPDownloader() {
        // 等待线程池中的所有任务结束后释放资源
        pool_.join();
        curl_global_cleanup();
    }

    // 禁止拷贝构造和赋值
    HTTPDownloader(const HTTPDownloader&) = delete;
    HTTPDownloader& operator=(const HTTPDownloader&) = delete;

    // libcurl写数据回调函数（直接写入FILE*）
    static size_t writeData(void* ptr, size_t size, size_t nmemb, void* userdata) {
        FILE* fp = static_cast<FILE*>(userdata);
        return fwrite(ptr, size, nmemb, fp);
    }

    // 确保指定目录存在，不存在则创建
    void ensureDirectoryExists(const std::filesystem::path& dirPath) {
        if (!std::filesystem::exists(dirPath)) {
            std::filesystem::create_directories(dirPath);
        }
    }

    // 真正执行下载任务的函数，由线程池中的线程调用
    void downloadTask(const std::string& url, DownloadCallback callback) {
        bool success = false;
        std::string filePathStr;
        do {
            // 计算本地缓存文件完整路径
            std::filesystem::path localFilePath = utils::urlToFilePath(url);
            filePathStr = localFilePath.string();

            // 确保文件所在目录存在
            ensureDirectoryExists(localFilePath.parent_path());

            // 检查是否存在已下载的部分，若存在则获取当前文件大小用于续传
            curl_off_t resume_from = 0;
            if (std::filesystem::exists(localFilePath)) {
                resume_from = static_cast<curl_off_t>(std::filesystem::file_size(localFilePath));
            }

            CURL* curl = curl_easy_init();
            if (!curl) {
                std::cerr << "初始化 curl 失败: " << url << std::endl;
                break;
            }

            // 以追加模式打开文件（用于续传）
            FILE* fp = fopen(filePathStr.c_str(), "ab");
            if (!fp) {
                std::cerr << "打开文件失败: " << filePathStr << std::endl;
                curl_easy_cleanup(curl);
                break;
            }

            // 设置 curl 选项
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeData);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            // 若已下载部分存在，则设置续传起始位置
            if (resume_from > 0) {
                curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, resume_from);
            }
            //（可以根据需要添加超时、低速检测等参数设置）

            // 执行下载
            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                std::cerr << "下载失败: " << url << " 错误信息: " << curl_easy_strerror(res) << std::endl;
            }
            else {
                success = true;
                std::cout << "下载成功: " << url << std::endl;
            }

            fclose(fp);
            curl_easy_cleanup(curl);
        } while (false);

        // 如果提供了回调，则调用回调
        if (callback) {
            callback(url, success);
        }

        // 从任务管理集合中删除
        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            activeTasks_.erase(url);
        }
    }

    // 线程池，用于并行下载多个任务
    boost::asio::thread_pool pool_;
    // 正在下载的URL集合，用于检测重复任务
    std::unordered_set<std::string> activeTasks_;
    // 保护 activeTasks_ 的互斥量
    std::mutex tasksMutex_;
};


// // 示例主函数，演示如何添加下载任务
//int main() {
//    auto& downloader = HTTPDownloader::getInstance();
//
//    // 添加下载任务（附带回调）
//    downloader.addDownloadTask("http://example.com/a/b/c/d", [](const std::string& url, bool success) {
//        std::cout << "回调: " << url << " 下载" << (success ? "成功" : "失败") << std::endl;
//        });
//
//    // 添加一个不带回调的任务
//    downloader.addDownloadTask("http://www.example.org/file.txt");
//
//    // 再次添加相同的任务（此任务将被忽略）
//    downloader.addDownloadTask("http://example.com/a/b/c/d");
//
//    // 等待所有任务完成后退出程序
//    downloader.waitForTasks();
//
//    return 0;
//}