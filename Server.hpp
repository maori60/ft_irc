// Server.hpp

#ifndef SERVER_HPP
#define SERVER_HPP

#include <string>
#include <map>
#include <vector>
#include <poll.h>
#include "Client.hpp"
#include "Channel.hpp"

class Server {
private:
    int _port;
    std::string _password;
    int _server_fd;
    std::vector<struct pollfd> _poll_fds;
    std::map<int, Client> _clients;
    std::map<std::string, Channel> _channels;

    void initSocket();
    void acceptNewClient();
    void handleClientInput(int client_fd);
    void disconnectClient(int client_fd);
    void processCommand(Client &client, const std::string &line);

public:
    Server(int port, const std::string &password);
    ~Server();

    void run();
    const std::string &getPassword() const;
};

#endif
