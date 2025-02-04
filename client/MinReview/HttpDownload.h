#include <curl/curl.h>
#include <iostream>
#include <vector>
#include <string>
#include <cstdio>
#include <stdexcept>
#include <utility>

using namespace std;

/*
 * HTTPDownloader 类封装了 libcurl multi 下载逻辑，包括：
 * 1. 初始化全局 libcurl 环境和 multi handle；
 * 2. 通过 addDownload 添加单个下载任务（为指定 URL 创建 easy handle 并设置文件保存路径）；
 * 3. processDownloads 利用 curl_multi_perform 和 curl_multi_wait 让所有任务并发执行，
 *    直到所有任务完成；
 * 4. 在析构中清理 easy handle、multi handle、以及所有打开的文件。
 */
class HTTPDownloader {
public:
    HTTPDownloader() {
        // 初始化全局 libcurl 环境（建议在整个应用范围只调用一次）
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw runtime_error("curl_global_init failed");
        }
        // 初始化 multi handle
        multiHandle = curl_multi_init();
        if (!multiHandle) {
            throw runtime_error("curl_multi_init failed");
        }
    }

    ~HTTPDownloader() {
        // 清理所有 easy handle
        for (auto easy : easyHandles) {
            curl_multi_remove_handle(multiHandle, easy);
            curl_easy_cleanup(easy);
        }
        // 清理 multi handle
        curl_multi_cleanup(multiHandle);

        // 关闭所有文件句柄
        for (auto fp : fileHandles) {
            if (fp) fclose(fp);
        }
        // 清理全局 libcurl 环境
        curl_global_cleanup();
    }

    // 添加下载任务——url 为下载地址，filename 为保存的本地文件路径
    bool addDownload(const string &url, const string &filename) {
        CURL *easy = curl_easy_init();
        if (!easy) {
            cerr << "curl_easy_init failed for url: " << url << endl;
            return false;
        }

        // 打开文件用于写入下载数据
        FILE *fp = fopen(filename.c_str(), "wb");
        if (!fp) {
            cerr << "Cannot open file " << filename << " for writing" << endl;
            curl_easy_cleanup(easy);
            return false;
        }
        fileHandles.push_back(fp);

        // 设置 URL、回调函数等参数
        curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
        // 若目标 URL 发起重定向，则跟随重定向
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        // 设置回调函数，在接收到数据时将其写入文件
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, HTTPDownloader::writeData);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, fp);
        
        // 将 easy handle 加入 multi handle 管理
        curl_multi_add_handle(multiHandle, easy);
        easyHandles.push_back(easy);

        return true;
    }

    // 执行所有下载任务（阻塞直到所有任务完成）
    void processDownloads() {
        int still_running = 0;
        // 初始执行一次
        curl_multi_perform(multiHandle, &still_running);

        while (still_running) {
            int numfds = 0;
            // 等待活动发生，超时时间设为 1000 毫秒
            CURLMcode mc = curl_multi_wait(multiHandle, nullptr, 0, 1000, &numfds);
            if (mc != CURLM_OK) {
                cerr << "curl_multi_wait() failed: " << curl_multi_strerror(mc) << endl;
                break;
            }
            // 每当等待返回后，继续执行多任务
            curl_multi_perform(multiHandle, &still_running);
        }

        // 此处可以进一步通过 curl_multi_info_read 检查每个 easy handle 的执行结果，
        // 并进行错误处理或下载重试等操作
    }

private:
    CURLM *multiHandle{nullptr};
    vector<CURL*> easyHandles;
    vector<FILE*> fileHandles;

    // 回调函数：将libcurl接收到的数据写入文件
    static size_t writeData(void *ptr, size_t size, size_t nmemb, void *userData) {
        FILE *fp = static_cast<FILE*>(userData);
        return fwrite(ptr, size, nmemb, fp);
    }
};

int main() {
    try {
        HTTPDownloader downloader;

        // 示例下载任务列表，你可以根据需要替换成实际的下载链接及保存路径
        vector<pair<string, string>> downloads = {
            {"http://example.com/file1.jpg", "file1.jpg"},
            {"http://example.com/file2.jpg", "file2.jpg"},
            {"http://example.com/file3.jpg", "file3.jpg"}
            // 如果需要下载大量文件，可继续增加任务，
            // 同时也可以考虑设置最大并发数，完成一个任务后再添加新的任务到 multi handle 中
        };

        // 添加下载任务
        for (const auto &item : downloads) {
            if (!downloader.addDownload(item.first, item.second)) {
                cerr << "Failed to add download: " << item.first << endl;
            }
        }

        // 执行所有下载任务
        downloader.processDownloads();
        cout << "所有下载任务完成!" << endl;
    } catch (const exception &ex) {
        cerr << "Exception: " << ex.what() << endl;
    }

    return 0;
}