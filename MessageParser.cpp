#include "MessageParser.hpp"
#include <sstream>

std::vector<std::string> MessageParser::splitMessages(std::string &buffer) {
    std::vector<std::string> messages;
    size_t pos = 0;

    while ((pos = buffer.find("\r\n")) != std::string::npos) {
        std::string msg = buffer.substr(0, pos);
        messages.push_back(msg);
        buffer.erase(0, pos + 2);
    }
    return messages;
}
