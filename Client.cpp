// Client.cpp

#include "Client.hpp"
#include <iostream>

Client::Client() : _fd(-1), _authenticated(false), _registered(false) {}

Client::Client(int fd) : _fd(fd), _authenticated(false), _registered(false) {}

Client::~Client() {}

int Client::getFd() const { return _fd; }

std::string Client::getNickname() const { return _nickname; }

std::string Client::getUsername() const { return _username; }

std::string Client::getRealname() const { return _realname; }

bool Client::isAuthenticated() const { return _authenticated; }

bool Client::isRegistered() const { return _registered; }

void Client::setNickname(const std::string &nickname) { _nickname = nickname; }

void Client::setUsername(const std::string &username) { _username = username; }

void Client::setRealname(const std::string &realname) { _realname = realname; }

void Client::setAuthenticated(bool value) { _authenticated = value; }

void Client::setRegistered(bool value) { _registered = value; }

void Client::appendBuffer(const std::string &data) {
    _buffer += data;
}

bool Client::extractLine(std::string &line) {
    size_t pos = _buffer.find("\r\n");
    if (pos == std::string::npos)
        return false;
    line = _buffer.substr(0, pos);
    _buffer.erase(0, pos + 2);
    return true;
}
