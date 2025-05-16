// utils.cpp

#include "utils.hpp"
#include <sstream>
#include <cctype>

std::vector<std::string> split(const std::string &str, char delim) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string item;

    while (std::getline(ss, item, delim)) {
        tokens.push_back(trim(item));
    }

    return tokens;
}

std::string trim(const std::string &s) {
    size_t start = 0;
    while (start < s.length() && std::isspace(s[start]))
        ++start;

    size_t end = s.length();
    while (end > start && std::isspace(s[end - 1]))
        --end;

    return s.substr(start, end - start);
}
