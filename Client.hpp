// Client.hpp

#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <string>
#include <vector>

class Client {
private:
    int _fd;
    std::string _nickname;
    std::string _username;
    std::string _realname;
    bool _authenticated;
    bool _registered;
    std::string _buffer;

public:
    Client();
    Client(int fd);
    ~Client();

    int getFd() const;
    std::string getNickname() const;
    std::string getUsername() const;
    std::string getRealname() const;
    bool isAuthenticated() const;
    bool isRegistered() const;

    void setNickname(const std::string &nickname);
    void setUsername(const std::string &username);
    void setRealname(const std::string &realname);
    void setAuthenticated(bool value);
    void setRegistered(bool value);

    void appendBuffer(const std::string &data);
    bool extractLine(std::string &line);
};

#endif
