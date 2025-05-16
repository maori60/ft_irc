// CommandHandler.cpp

#include "CommandHandler.hpp"
#include "utils.hpp"
#include <iostream>

void CommandHandler::handle(Server &server, Client &client, const std::string &line) {
    std::cout << "REÇU: " << line << std::endl;

    std::vector<std::string> tokens = split(line, ' ');
    if (tokens.empty())
        return;

    std::string command = tokens[0];
    for (size_t i = 0; i < command.length(); ++i)
        command[i] = toupper(command[i]);

    if (command == "PASS")
        handlePass(client, tokens, server.getPassword());
    else if (command == "NICK")
        handleNick(client, tokens);
    else if (command == "USER")
        handleUser(client, tokens);
    else
        std::cout << "Unrecognized command: " << command << std::endl;
}

void CommandHandler::handlePass(Client &client, const std::vector<std::string> &params, const std::string &password) {
    if (params.size() < 2) return;
    if (params[1] == password)
        client.setAuthenticated(true);
}

void CommandHandler::handleNick(Client &client, const std::vector<std::string> &params) {
    if (params.size() < 2) return;
    client.setNickname(params[1]);
}

void CommandHandler::handleUser(Client &client, const std::vector<std::string> &params) {
    if (params.size() < 5) return;
    client.setUsername(params[1]);
    client.setRealname(params[4]);
    if (client.isAuthenticated())
        client.setRegistered(true);
}
