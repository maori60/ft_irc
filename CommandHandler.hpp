// CommandHandler.hpp

#ifndef COMMANDHANDLER_HPP
#define COMMANDHANDLER_HPP

#include <string>
#include "Server.hpp"
#include "Client.hpp"

class Server;  // forward declarations
class Client;

class CommandHandler {
public:
    static void handle(Server &server, Client &client, const std::string &line);
private:
    static void handlePass(Client &client, const std::vector<std::string> &params, const std::string &password);
    static void handleNick(Client &client, const std::vector<std::string> &params);
    static void handleUser(Client &client, const std::vector<std::string> &params);
};

#endif
