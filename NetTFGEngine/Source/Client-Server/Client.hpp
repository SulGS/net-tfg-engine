#include <string>

class Client {
	virtual int RunClient(const std::string& hostStr = "0.0.0.0", uint16_t port = 0) = 0;
};