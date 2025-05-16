#ifndef MESSAGEPARSER_HPP
#define MESSAGEPARSER_HPP

#include <vector>
#include <string>

class MessageParser {
public:
    static std::vector<std::string> splitMessages(std::string &buffer);
};

#endif
