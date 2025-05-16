// Channel.hpp

#ifndef CHANNEL_HPP
#define CHANNEL_HPP

#include <string>
#include <set>

class Channel {
private:
    std::string _name;
    std::set<int> _members;

public:
    Channel();
    Channel(const std::string &name);

    const std::string &getName() const;
    void addMember(int fd);
    void removeMember(int fd);
    const std::set<int> &getMembers() const;
};

#endif
