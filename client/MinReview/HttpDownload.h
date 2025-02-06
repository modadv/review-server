#pragma once

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

using DownloadCallback = std::function<void(const std::string& url, const std::string& local_path, bool)>;

class HTTPDownloader {
public:
    static HTTPDownloader& getInstance() {
        static HTTPDownloader instance;
        return instance;
    }

    void addDownloadTask(const std::string& url, DownloadCallback callback = nullptr) {
        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            if (activeTasks_.find(url) != activeTasks_.end()) {
                std::cout << "Task " << url << " under downloading, do not repeat add.\n";
                return;
            }
            activeTasks_.insert(url);
        }
        boost::asio::post(pool_, [this, url, callback] {
            downloadTask(url, callback);
            });
    }

    void waitForTasks() {
        pool_.join();
    }

private:
    HTTPDownloader()
        : pool_(std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 2) {
        curl_global_init(CURL_GLOBAL_ALL);
    }

    ~HTTPDownloader() {
        pool_.join();
        curl_global_cleanup();
    }

    HTTPDownloader(const HTTPDownloader&) = delete;
    HTTPDownloader& operator=(const HTTPDownloader&) = delete;

    static size_t writeData(void* ptr, size_t size, size_t nmemb, void* userdata) {
        FILE* fp = static_cast<FILE*>(userdata);
        return fwrite(ptr, size, nmemb, fp);
    }

    void ensureDirectoryExists(const std::filesystem::path& dirPath) {
        if (!std::filesystem::exists(dirPath)) {
            std::filesystem::create_directories(dirPath);
        }
    }

    void downloadTask(const std::string& url, DownloadCallback callback) {
        bool success = false;
        std::string filePathStr;
        do {
            std::filesystem::path localFilePath = utils::urlToFilePath(url);
            filePathStr = localFilePath.string();

            ensureDirectoryExists(localFilePath.parent_path());

            curl_off_t resume_from = 0;
            if (std::filesystem::exists(localFilePath)) {
                resume_from = static_cast<curl_off_t>(std::filesystem::file_size(localFilePath));
            }

            CURL* curl = curl_easy_init();
            if (!curl) {
                std::cerr << "Init curl failed: " << url << std::endl;
                break;
            }

            // 以追加模式打开文件（用于续传）
            FILE* fp = fopen(filePathStr.c_str(), "ab");
            if (!fp) {
                std::cerr << "Open file failed: " << filePathStr << std::endl;
                curl_easy_cleanup(curl);
                break;
            }

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeData);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            if (resume_from > 0) {
                curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, resume_from);
            }

            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                std::cerr << "Download failed: " << url << " error msg: " << curl_easy_strerror(res) << std::endl;
            }
            else {
                success = true;
                std::cout << "Download successfully: " << url << std::endl;
            }

            fclose(fp);
            curl_easy_cleanup(curl);
        } while (false);

        if (callback) {
            callback(url, filePathStr, success);
        }

        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            activeTasks_.erase(url);
        }
    }

    boost::asio::thread_pool pool_;
    std::unordered_set<std::string> activeTasks_;
    std::mutex tasksMutex_;
};
