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
	XmlToJsonDataHandler(const std::string& file_path) :
		ofs_(file_path, std::ios::binary),
		filePath_(file_path),
		parser_(nullptr),
		root_(boost::json::object()) {
		if (!ofs_) {
			throw std::runtime_error("Failed to open file:" + file_path);
		}

		// Initial Expat parser
		parser_ = XML_ParserCreate(NULL);
		if (!parser_) {
			throw std::runtime_error("Failed to create Expat parser");
		}

		// Setup user data as this pointer
		XML_SetUserData(parser_, this);

		// Setup callback
		XML_SetElementHandler(parser_, startElement, endElement);
		XML_SetCharacterDataHandler(parser_, characterData);

		// Initial JSON root object, and push into stack
		jsonStack_.push(&root_);
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
		// Finished parse
		if (XML_Parse(parser_, nullptr, 0, true) == XML_STATUS_ERROR) {
			throw std::runtime_error("XML Parse error on finalize:" + std::string(XML_ErrorString(XML_GetErrorCode(parser_))));
		}

		// Ensure JSON stack only left root element
		if (jsonStack_.size() != 1) {
			throw std::runtime_error("Malformed XML: unmatched tags detected");
		}

		// Write full JSON into file
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

	// Member function used for process XML event
	void onStartElement(const std::string& name, const XML_Char** attrs) {
		boost::json::object new_element;
		// Process attributes
		if (attrs) {
			for (int i = 0; attrs[i]; i += 2) {
				new_element.emplace(attrs[i], attrs[i + 1]);
			}
		}

		// Get current JSON object
		boost::json::value* parent = jsonStack_.top();

		// Ensure parent node is an object
		if (!parent->is_object()) {
			throw std::runtime_error("Parent JSON element is not an object");
		}

		boost::json::object& parent_object = parent->as_object();

		// Process repeat element name, convert it to array
		if (parent_object.contains(name)) {
			if (!parent_object[name].is_array()) {
				// Convert element to arrary
				boost::json::array arr;
				arr.emplace_back(std::move(parent_object[name]));
				parent_object[name] = std::move(arr);
			}
			// Add new element to array
			parent_object[name].as_array().emplace_back(boost::json::value_from(new_element));
			// Push new element into stack
			jsonStack_.push(&parent_object[name].as_array().back());
		}
		else {
			// Add an object
			parent_object.emplace(name, boost::json::value_from(new_element));
			// Push new element into stack
			jsonStack_.push(&parent_object[name]);
		}
	}

	void onEndElement(const std::string& name) {
		if (jsonStack_.empty()) {
			throw std::runtime_error("JSON stack underflow on end element");
		}
		jsonStack_.pop();
	}

	void onCharacterData(const std::string& data) {
		if (data.empty()) { return; }

		boost::json::value* current = jsonStack_.top();

		// Remove front and back white character
		std::string trimmed = trim(data);

		if (trimmed.empty()) { return; }

		if (current->is_string()) {
			std::string existing = current->as_string().c_str();
			existing += trimmed;
			*current = existing;
		}
		else if (current->is_null()) {
			*current = trimmed;
		}
		else if (current->is_object()) {
			// When an element contains child elements, text nodes may be stored as a special key
			// For example, you can use the "_text" key to store text content
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
			// In other cases, you can choose how to handle it
			// Here, simply convert it to a string and overwrite it
			*current = trimmed;
		}
	}

	// Auxiliary afunction: remove whitespace characters before and after the string
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

	boost::json::value root_;
	std::stack<boost::json::value*> jsonStack_;
};