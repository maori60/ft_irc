// Channel.cpp

#include "Channel.hpp"

Channel::Channel() {}

Channel::Channel(const std::string &name) : _name(name) {}

const std::string &Channel::getName() const {
    return _name;
}

void Channel::addMember(int fd) {
    _members.insert(fd);
}

void Channel::removeMember(int fd) {
    _members.erase(fd);
}

const std::set<int> &Channel::getMembers() const {
    return _members;
}
