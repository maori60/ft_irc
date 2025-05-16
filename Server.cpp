// Server.cpp

#include "Server.hpp"
#include "CommandHandler.hpp"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

Server::Server(int port, const std::string &password)
    : _port(port), _password(password), _server_fd(-1) {
    initSocket();
}

Server::~Server() {
    close(_server_fd);
}

void Server::initSocket() {
    _server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_server_fd < 0)
        throw std::runtime_error("socket() failed");

    int enable = 1;
    if (setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
        throw std::runtime_error("setsockopt() failed");

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(_port);

    if (bind(_server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
        throw std::runtime_error("bind() failed");

    if (listen(_server_fd, 10) < 0)
        throw std::runtime_error("listen() failed");

    fcntl(_server_fd, F_SETFL, O_NONBLOCK);

    struct pollfd pfd;
    pfd.fd = _server_fd;
    pfd.events = POLLIN;
    _poll_fds.push_back(pfd);

    std::cout << "Server listening on port " << _port << std::endl;
}

void Server::run() {
    while (true) {
        if (poll(&_poll_fds[0], _poll_fds.size(), -1) < 0) {
            std::cerr << "poll() failed" << std::endl;
            break;
        }

        for (size_t i = 0; i < _poll_fds.size(); ++i) {
            if (_poll_fds[i].revents & POLLIN) {
                if (_poll_fds[i].fd == _server_fd)
                    acceptNewClient();
                else
                    handleClientInput(_poll_fds[i].fd);
            }
        }
    }
}

void Server::acceptNewClient() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(_server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0)
        return;

    fcntl(client_fd, F_SETFL, O_NONBLOCK);
    struct pollfd pfd;
    pfd.fd = client_fd;
    pfd.events = POLLIN;
    _poll_fds.push_back(pfd);

    _clients[client_fd] = Client(client_fd);

    std::cout << "New connection: " << client_fd << std::endl;
}

void Server::handleClientInput(int client_fd) {
    char buffer[512];
    std::memset(buffer, 0, sizeof(buffer));
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        disconnectClient(client_fd);
        return;
    }

    std::string input(buffer);
    Client &client = _clients[client_fd];
    client.appendBuffer(input);

    std::string line;
    while (client.extractLine(line)) {
        processCommand(client, line);
    }
}

void Server::disconnectClient(int client_fd) {
    close(client_fd);
    for (size_t i = 0; i < _poll_fds.size(); ++i) {
        if (_poll_fds[i].fd == client_fd) {
            _poll_fds.erase(_poll_fds.begin() + i);
            break;
        }
    }
    _clients.erase(client_fd);
    std::cout << "Disconnected client: " << client_fd << std::endl;
}

void Server::processCommand(Client &client, const std::string &line) {
    CommandHandler::handle(*this, client, line);
}

const std::string &Server::getPassword() const {
    return _password;
}
