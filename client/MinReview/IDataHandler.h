#pragma once

#include <string>
#include <cstddef>
#include <system_error>

class IDataHandler {
public:
	virtual ~IDataHandler() = default;
	// Processing data block
	virtual void handleData(const char* data, std::size_t sz) = 0;

	// Post-Process, such as release resource
	virtual void finalize() = 0;

	// Get final file path
	virtual std::string getFilePath() const = 0;
};