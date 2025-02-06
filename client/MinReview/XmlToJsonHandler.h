#pragma once

#include <fstream>
#include <string>
#include <stack>
#include <stdexcept>
#include <expat.h>
#include <boost/json.hpp>

#include <IDataHandler.h>
#include <HttpDownload.h>

class XmlToJsonDataHandler : public IDataHandler {
public:
    XmlToJsonDataHandler(const std::string& file_path, std::function<void(const std::string& res_path)> parse_callback = nullptr) :
        ofs_(file_path, std::ios::binary),
        filePath_(file_path),
        parser_(nullptr),
        root_(boost::json::object()),
        componentDepth_(0),
        componentCaptured_(false),
        componentParseCallback_(parse_callback)
    {
        if (!ofs_) {
            throw std::runtime_error("Failed to open file:" + file_path);
        }

        // 创建 Expat 解析器
        parser_ = XML_ParserCreate(NULL);
        if (!parser_) {
            throw std::runtime_error("Failed to create Expat parser");
        }

        // 设置用户数据为 this 指针
        XML_SetUserData(parser_, this);

        // 设置回调函数：解析时将调用 startElement、endElement、characterData
        XML_SetElementHandler(parser_, startElement, endElement);
        XML_SetCharacterDataHandler(parser_, characterData);

        // 注意：初始时不需要把 root_ 放入堆栈，只有遇到第一个 <component> 时开始记录 JSON
    }

    ~XmlToJsonDataHandler() {
        if (parser_) {
            XML_ParserFree(parser_);
        }
    }

    void handleData(const char* data, std::size_t sz) override {
        if (XML_Parse(parser_, data, static_cast<int>(sz), false) == XML_STATUS_ERROR) {
            throw std::runtime_error("XML Parse error: " + std::string(XML_ErrorString(XML_GetErrorCode(parser_))));
        }
    }

    void finalize() override {
        // 结束解析：传入空缓冲区表示结束解析所有数据
        if (XML_Parse(parser_, nullptr, 0, true) == XML_STATUS_ERROR) {
            throw std::runtime_error("XML Parse error on finalize:" + std::string(XML_ErrorString(XML_GetErrorCode(parser_))));
        }

        if (!componentCaptured_) {
            throw std::runtime_error("No <component> element found in the XML");
        }

        // 此处只输出第一个 <component> 元素转换得到的 JSON 内容
        std::string json_str = boost::json::serialize(root_);
        ofs_ << json_str;

        if (!ofs_) {
            throw std::runtime_error("Failed to write JSON to file:" + filePath_);
        }

        ofs_.close();
    }

    std::string getFilePath() const override {
        return filePath_;
    }

private:
    // 回调入口函数（静态成员函数转换为成员函数调用）
    static void startElement(void* user_data, const XML_Char* name, const XML_Char** attrs) {
        XmlToJsonDataHandler* handler = static_cast<XmlToJsonDataHandler*>(user_data);
        handler->onStartElement(name, attrs);
    }

    static void endElement(void* user_data, const XML_Char* name) {
        XmlToJsonDataHandler* handler = static_cast<XmlToJsonDataHandler*>(user_data);
        handler->onEndElement(name);
    }

    static void characterData(void* user_data, const XML_Char* s, int len) {
        XmlToJsonDataHandler* handler = static_cast<XmlToJsonDataHandler*>(user_data);
        handler->onCharacterData(std::string(s, len));
    }

    // 处理开始标签事件
    void onStartElement(const std::string& name, const XML_Char** attrs) {
        // 如果当前不在 <component> 捕获区间
        if (componentDepth_ == 0) {
            // 如果还未捕获到过 <component> 且当前元素名称正好为 "component"，则开始捕获
            if (!componentCaptured_ && name == "component") {
                componentDepth_ = 1;
                // 初始化根 JSON 对象（即用于存储 <component> 的内容）
                root_ = boost::json::object();
                if (attrs) {
                    for (int i = 0; attrs[i]; i += 2) {
                        root_.as_object().emplace(attrs[i], attrs[i + 1]);
                    }
                }
                // 清空并初始化 json 堆栈，将根对象压入，用于后续构造子元素
                while (!jsonStack_.empty()) {
                    jsonStack_.pop();
                }
                jsonStack_.push(&root_);
            }
            // 如果不满足捕获条件，则忽略本次事件
            return;
        }
        else {
            // 当 componentDepth_ > 0 时，表示当前处于目标 <component> 内部
            componentDepth_++;  // 进入更深一层

            boost::json::object new_element;
            if (attrs) {
                for (int i = 0; attrs[i]; i += 2) {
                    new_element.emplace(attrs[i], attrs[i + 1]);
                }
            }

            boost::json::value* parent = jsonStack_.top();
            if (!parent->is_object()) {
                throw std::runtime_error("Parent JSON element is not an object");
            }
            boost::json::object& parent_object = parent->as_object();

            // 如果同名元素已存在，则转换为数组再添加
            if (parent_object.contains(name)) {
                if (!parent_object[name].is_array()) {
                    boost::json::array arr;
                    arr.emplace_back(std::move(parent_object[name]));
                    parent_object[name] = std::move(arr);
                }
                parent_object[name].as_array().emplace_back(boost::json::value_from(new_element));
                jsonStack_.push(&parent_object[name].as_array().back());
            }
            else {
                parent_object.emplace(name, boost::json::value_from(new_element));
                jsonStack_.push(&parent_object[name]);
            }
        }
    }

    // 处理结束标签事件
    void onEndElement(const std::string& name) {
        if (componentDepth_ > 0) {
            componentDepth_--;
            if (!jsonStack_.empty()) {
                jsonStack_.pop();
            }
            // 当 componentDepth_ 归零时，说明第一个 <component> 元素解析完毕
            if (componentDepth_ == 0) {
                componentCaptured_ = true;
            }
        }
        // 否则，不在目标捕获区间时忽略
    }

    // 处理字符数据
    void onCharacterData(const std::string& data) {
        if (componentDepth_ == 0) {
            // 不在 <component> 捕获区间时忽略字符数据
            return;
        }
        std::string trimmed = trim(data);
        if (trimmed.empty()) { return; }

        boost::json::value* current = jsonStack_.top();
        if (current->is_string()) {
            std::string existing = current->as_string().c_str();
            existing += trimmed;
            *current = existing;
        }
        else if (current->is_null()) {
            *current = trimmed;
        }
        else if (current->is_object()) {
            // 如果元素中已包含子元素，文本内容放到特殊键中（例如 "#text"）
            if (current->as_object().contains("#text")) {
                std::string existing = current->as_object()["#text"].as_string().c_str();
                existing += trimmed;
                current->as_object()["#text"] = existing;
            }
            else {
                current->as_object().emplace("#text", trimmed);
            }
        }
        else {
            // 其它情况下简单转换为字符串
            *current = trimmed;
        }
        if (!trimmed.empty() && componentParseCallback_ && (trimmed.ends_with(".jpg") || trimmed.ends_with(".jpeg") || trimmed.ends_with(".png"))) {
            componentParseCallback_(trimmed);
        }
    }

    // 辅助函数：去除字符串首尾空白字符
    std::string trim(const std::string& str) {
        const std::string whitespace = " \t\n\r";
        const auto start = str.find_first_not_of(whitespace);
        if (start == std::string::npos) {
            return "";
        }
        const auto end = str.find_last_not_of(whitespace);
        return str.substr(start, end - start + 1);
    }

    std::ofstream ofs_;
    std::string filePath_;

    XML_Parser parser_;

    // 根 JSON 对象用于存储第一个 <component> 的内容
    boost::json::value root_;
    std::stack<boost::json::value*> jsonStack_;

    // 状态控制变量：
    // componentDepth_ > 0 表示当前正在捕获 <component> 元素内部的内容
    // componentCaptured_ 为 true 表示已经完整解析过第一个 <component>
    int componentDepth_;
    bool componentCaptured_;

    std::function<void(const std::string& res_path)> componentParseCallback_;
};