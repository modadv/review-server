// MinReview.cpp : Defines the entry point for the application.
//

#include <iostream>
#include <chrono>
#include <boost/asio.hpp>
#include <boost/json.hpp>
// 假设之前实现的代码保存在 WebSocketClientManager.hpp 中

#include <Network.h>
#include <MinReview.h>
#include <XmlDownload.h>
#include <HttpDownload.h>

using namespace std;
using boost::asio::io_context;
using boost::asio::steady_timer;
using boost::json::object;
using namespace std::chrono_literals;

void testXmlDownload() {
    //std::string host = "127.0.0.1";
    //std::string port = "80";
    //std::string target = "/run/results/AP-M003CM-EA.2955064502/20250116/T_20241018193101867_1_NG/report.xml";
    //fs::path downloaded_file = XmlDownloader::download(host, target, port);
    //std::cout << "Download successfully, file save at: " << downloaded_file << "\n";
}

void testHttpDownload() {
    const std::string url = "http://localhost/run/results/AP-M003CM-EA.2955064502/20250116/T_20241018193101867_1_NG/images/ng/Other/0/COMP1119_1119.png";

    HTTPDownloader::getInstance().addDownloadTask(url, [](const std::string& url, const std::string& local_path, bool success) {
        std::cout << "Download callback: " << url << " Download: " << (success ? "Successfully" : "Failed") << std::endl;
    });
}

static void onReviewFinishedCallback(const std::string& host, const std::string& port, const json::object& data) {
    json::object review_msg;
    review_msg["protocol_id"] = 2;
    review_msg["data"] = data;
    WebSocketClientManager::getInstance().sendMessage(host, port, review_msg);
    std::cout << "********//////**********////////  Review Finished" << std::endl;
}

static void onProtocol1(const std::string& host, const std::string& port, int protocol_id, const json::object& data) {
    fs::path downloaded_file = XmlDownloader::download(host, port, data, onReviewFinishedCallback);
}

static void onProtocol2(const std::string& host, const std::string& port, int protocol_id, const json::object& data) {
    // test
}

static void runClient() {
    WebSocketClientManager::getInstance().getRegistry().registerHandler(1, onProtocol1);
    WebSocketClientManager::getInstance().getRegistry().registerHandler(2, onProtocol2);

    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8194");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8195");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8196");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8197");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8198");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8199");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8200");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8201");

    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8202");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8203");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8204");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8205");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8206");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8207");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8208");
    WebSocketClientManager::getInstance().addConnection("127.0.0.1", "8209");

    WebSocketClientManager::getInstance().getIOContext().run();
}

int main() {
    // testHttpDownload();
    // testXmlDownload();
    runClient();
    return 0;
}