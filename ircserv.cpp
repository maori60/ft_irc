#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <set>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096
#define MAX_NICK_LEN 32
#define MAX_CHANNEL_LEN 50

struct Client {
    int fd;
    std::string nick;
    std::string user;
    std::string real;
    std::string hostname;
    bool authenticated;
    bool registered;
    std::string buffer;
    std::set<std::string> channels;
    std::set<std::string> invited_channels;
    
    Client() : fd(-1), authenticated(false), registered(false) {}
    Client(int f) : fd(f), authenticated(false), registered(false) {}
};

struct Channel {
    std::string name;
    std::string topic;
    std::set<std::string> members;
    std::set<std::string> operators;
    std::string key;
    bool invite_only;
    bool topic_protected;
    int user_limit;
    
    Channel(const std::string& n) : name(n), invite_only(false), topic_protected(true), user_limit(0) {}
};

class IRCServer {
private:
    int server_fd;
    int epoll_fd;
    std::string password;
    std::map<int, Client> clients;
    std::map<std::string, Channel> channels;
    std::map<std::string, int> nick_to_fd;
    static bool shutdown_requested;
    
public:
    IRCServer(const std::string& port, const std::string& pass) 
        : server_fd(-1), epoll_fd(-1), password(pass) {
        init_server(port);
    }
    
    ~IRCServer() {
        cleanup();
    }
    
    static void signal_handler(int sig) {
        if (sig == SIGINT) {
            std::cout << "\nReceived SIGINT, shutting down gracefully..." << std::endl;
            shutdown_requested = true;
        }
    }
    
    void cleanup() {
        // Send QUIT message to all connected clients
        for (std::map<int, Client>::iterator it = clients.begin(); it != clients.end(); ++it) {
            if (it->second.registered) {
                send_raw(it->first, "ERROR :Server shutting down");
            }
            close(it->first);
        }
        clients.clear();
        channels.clear();
        nick_to_fd.clear();
        
        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
        if (epoll_fd != -1) {
            close(epoll_fd);
            epoll_fd = -1;
        }
    }
    
    void init_server(const std::string& port) {
        // Create socket
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == -1) {
            perror("socket");
            exit(1);
        }
        
        // Set socket options
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            perror("setsockopt");
            exit(1);
        }
        
        // Make socket non-blocking
        int flags = fcntl(server_fd, F_GETFL, 0);
        fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
        
        // Validate port
        if (port.empty()) {
            std::cerr << "Error: Port cannot be empty" << std::endl;
            exit(1);
        }
        
        int port_num = atoi(port.c_str());
        if (port_num < 0 || port_num > 65535) {
            std::cerr << "Error: Port must be between 0 and 65535" << std::endl;
            exit(1);
        }
        
        // Bind socket
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_num);
        
        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            perror("bind");
            exit(1);
        }
        
        // Get actual port if 0 was specified
        socklen_t addr_len = sizeof(addr);
        if (getsockname(server_fd, (struct sockaddr*)&addr, &addr_len) == -1) {
            perror("getsockname");
            exit(1);
        }
        int actual_port = ntohs(addr.sin_port);
        
        // Listen
        if (listen(server_fd, SOMAXCONN) == -1) {
            perror("listen");
            exit(1);
        }
        
        // Create epoll instance
        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) {
            perror("epoll_create1");
            exit(1);
        }
        
        // Add server socket to epoll
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = server_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
            perror("epoll_ctl");
            exit(1);
        }
        
        std::cout << "IRC Server listening on port " << actual_port << std::endl;
    }
    
    void run() {
        struct epoll_event events[MAX_EVENTS];
        
        while (!shutdown_requested) {
            int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000); // 1 second timeout
            if (nfds == -1) {
                if (errno == EINTR) continue;
                perror("epoll_wait");
                break;
            }
            
            for (int i = 0; i < nfds; i++) {
                if (events[i].data.fd == server_fd) {
                    accept_connection();
                } else {
                    handle_client_data(events[i].data.fd);
                }
            }
        }
        
        std::cout << "Server loop ended, cleaning up..." << std::endl;
        cleanup();
    }
    
private:
    void accept_connection() {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept");
            }
            return;
        }
        
        // Make client socket non-blocking
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        
        // Add to epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            perror("epoll_ctl");
            close(client_fd);
            return;
        }
        
        // Create client object
        clients[client_fd] = Client(client_fd);
        clients[client_fd].hostname = inet_ntoa(client_addr.sin_addr);
        
        std::cout << "New connection from " << clients[client_fd].hostname << std::endl;
    }
    
    void handle_client_data(int fd) {
        std::map<int, Client>::iterator it = clients.find(fd);
        if (it == clients.end()) return;
        
        char buffer[BUFFER_SIZE];
        ssize_t bytes = recv(fd, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes <= 0) {
            if (bytes == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                disconnect_client(fd);
            }
            return;
        }
        
        buffer[bytes] = '\0';
        it->second.buffer += buffer;
        
        // Process complete lines (handle both \r\n and \n endings)
        size_t pos;
        while ((pos = it->second.buffer.find('\n')) != std::string::npos) {
            std::string line = it->second.buffer.substr(0, pos);
            // Remove \r if present before \n
            if (!line.empty() && line[line.length() - 1] == '\r') {
                line.erase(line.length() - 1);
            }
            it->second.buffer.erase(0, pos + 1);
            process_message(fd, line);
            // Check if client still exists after processing
            it = clients.find(fd);
            if (it == clients.end()) {
                // Client was disconnected during message processing
                return;
            }
        }
        
        // Remove incomplete line if buffer too large
        if (it->second.buffer.size() > BUFFER_SIZE) {
            it->second.buffer.clear();
        }
    }
    
    void disconnect_client(int fd) {
        std::map<int, Client>::iterator it = clients.find(fd);
        if (it == clients.end()) return;
        
        Client& client = it->second;
        
        // Remove from channels
        std::set<std::string> channels_copy = client.channels;
        for (std::set<std::string>::iterator ch_it = channels_copy.begin();
             ch_it != channels_copy.end(); ++ch_it) {
            leave_channel(fd, *ch_it, "Client disconnected");
        }
        
        // Remove nick mapping
        if (!client.nick.empty()) {
            nick_to_fd.erase(client.nick);
        }
        
        std::cout << "Client " << client.hostname << " disconnected" << std::endl;
        
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        clients.erase(it);
    }
    
    void process_message(int fd, const std::string& line) {
        if (line.empty()) return;
        
        std::istringstream iss(line);
        std::string command;
        std::vector<std::string> params;
        std::string param;
        
        iss >> command;
        
        // Convert command to uppercase
        std::transform(command.begin(), command.end(), command.begin(), ::toupper);
        
        // Parse parameters
        std::string rest;
        getline(iss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        
        if (!rest.empty()) {
            if (rest[0] == ':') {
                params.push_back(rest.substr(1));
            } else {
                std::istringstream param_stream(rest);
                std::string p;
                while (param_stream >> p) {
                    if (p[0] == ':') {
                        std::string trailing;
                        getline(param_stream, trailing);
                        params.push_back(p.substr(1) + trailing);
                        break;
                    }
                    params.push_back(p);
                }
            }
        }
        
        handle_command(fd, command, params);
    }
    
    void handle_command(int fd, const std::string& cmd, const std::vector<std::string>& params) {
        Client& client = clients[fd];
        
        if (cmd == "PASS") {
            if (params.size() < 1) {
                send_reply(fd, "461", "PASS :Not enough parameters");
                return;
            }
            if (params[0] == password) {
                client.authenticated = true;
            } else {
                send_reply(fd, "464", ":Password incorrect");
                disconnect_client(fd);
            }
        }
        else if (cmd == "NICK") {
            if (params.size() < 1) {
                send_reply(fd, "431", ":No nickname given");
                return;
            }
            handle_nick(fd, params[0]);
        }
        else if (cmd == "USER") {
            if (params.size() < 4) {
                send_reply(fd, "461", "USER :Not enough parameters");
                return;
            }
            client.user = params[0];
            client.real = params[3];
            check_registration(fd);
        }
        else if (cmd == "JOIN") {
            if (!client.registered) {
                send_reply(fd, "451", ":You have not registered");
                return;
            }
            if (params.size() < 1) {
                send_reply(fd, "461", "JOIN :Not enough parameters");
                return;
            }
            // Parse channels and keys
            std::vector<std::string> channels;
            std::vector<std::string> keys;
            std::string ch_param = params[0];
            std::string key_param = params.size() > 1 ? params[1] : "";
            // Split channels by ','
            size_t start = 0, end;
            while ((end = ch_param.find(',', start)) != std::string::npos) {
                channels.push_back(ch_param.substr(start, end - start));
                start = end + 1;
            }
            channels.push_back(ch_param.substr(start));
            // Split keys by ','
            start = 0;
            while ((end = key_param.find(',', start)) != std::string::npos) {
                keys.push_back(key_param.substr(start, end - start));
                start = end + 1;
            }
            if (!key_param.empty())
                keys.push_back(key_param.substr(start));
            // For each channel, call handle_join with corresponding key
            for (size_t i = 0; i < channels.size(); ++i) {
                std::string key = (i < keys.size()) ? keys[i] : "";
                handle_join(fd, channels[i], key);
            }
        }
        else if (cmd == "PART") {
            if (!client.registered) {
                send_reply(fd, "451", ":You have not registered");
                return;
            }
            if (params.size() < 1) {
                send_reply(fd, "461", "PART :Not enough parameters");
                return;
            }
            std::string reason = params.size() > 1 ? params[1] : "";
            handle_part(fd, params[0], reason);
        }
        else if (cmd == "PRIVMSG") {
            if (!client.registered) {
                send_reply(fd, "451", ":You have not registered");
                return;
            }
            if (params.size() < 2) {
                send_reply(fd, "461", "PRIVMSG :Not enough parameters");
                return;
            }
            handle_privmsg(fd, params[0], params[1]);
        }
        else if (cmd == "QUIT") {
            std::string reason = params.size() > 0 ? params[0] : "Client Quit";
            handle_quit(fd, reason);
        }
        else if (cmd == "PING") {
            if (params.size() > 0) {
                send_raw(fd, "PONG :" + params[0]);
            } else {
                send_raw(fd, "PONG :IRCServer");
            }
        }
        // New operator commands
        else if (cmd == "KICK") {
            if (!client.registered) {
                send_reply(fd, "451", ":You have not registered");
                return;
            }
            if (params.size() < 2) {
                send_reply(fd, "461", "KICK :Not enough parameters");
                return;
            }
            std::string reason = params.size() > 2 ? params[2] : client.nick;
            handle_kick(fd, params[0], params[1], reason);
        }
        else if (cmd == "INVITE") {
            if (!client.registered) {
                send_reply(fd, "451", ":You have not registered");
                return;
            }
            if (params.size() < 2) {
                send_reply(fd, "461", "INVITE :Not enough parameters");
                return;
            }
            handle_invite(fd, params[0], params[1]);
        }
        else if (cmd == "TOPIC") {
            if (!client.registered) {
                send_reply(fd, "451", ":You have not registered");
                return;
            }
            if (params.size() < 1) {
                send_reply(fd, "461", "TOPIC :Not enough parameters");
                return;
            }
            std::string topic = params.size() > 1 ? params[1] : "";
            handle_topic(fd, params[0], topic);
        }
        else if (cmd == "MODE") {
            if (!client.registered) {
                send_reply(fd, "451", ":You have not registered");
                return;
            }
            if (params.size() < 1) {
                send_reply(fd, "461", "MODE :Not enough parameters");
                return;
            }
            handle_mode(fd, params);
        }
    }
    
    void handle_nick(int fd, const std::string& nick) {
        Client& client = clients[fd];
        
        if (nick.length() > MAX_NICK_LEN) {
            send_reply(fd, "432", nick + " :Erroneous nickname");
            return;
        }
        
        // Check if nick is already in use
        if (nick_to_fd.find(nick) != nick_to_fd.end()) {
            send_reply(fd, "433", nick + " :Nickname is already in use");
            return;
        }
        
        std::string old_nick = client.nick;
        bool was_registered = client.registered;
        
        // Remove old nick mapping
        if (!client.nick.empty()) {
            nick_to_fd.erase(client.nick);
        }
        
        client.nick = nick;
        nick_to_fd[nick] = fd;
        
        // If user was already registered, handle nick change
        if (was_registered && !old_nick.empty()) {
            handle_nick_change(fd, old_nick, nick);
        }
        
        check_registration(fd);
    }
    
    void handle_nick_change(int fd, const std::string& old_nick, const std::string& new_nick) {
        Client& client = clients[fd];
        std::set<int> notified;
        
        // Send NICK message to all users who share channels with this user
        std::string nick_msg = ":" + old_nick + "!" + client.user + "@" + client.hostname + " NICK :" + new_nick;

        // Send to the user themselves first
        send_raw(fd, nick_msg);
        notified.insert(fd);
        
        // Notify users in all channels this user is in
        for (std::set<std::string>::iterator ch_it = client.channels.begin();
             ch_it != client.channels.end(); ++ch_it) {
            
            std::map<std::string, Channel>::iterator chan_it = channels.find(*ch_it);
            if (chan_it != channels.end()) {
                Channel& channel = chan_it->second;
                
                // Update channel member list
                channel.members.erase(old_nick);
                channel.members.insert(new_nick);
                
                // Update operator list if user was an operator
                if (channel.operators.find(old_nick) != channel.operators.end()) {
                    channel.operators.erase(old_nick);
                    channel.operators.insert(new_nick);
                }
                
                // Notify all members of this channel
                for (std::set<std::string>::iterator m_it = channel.members.begin();
                     m_it != channel.members.end(); ++m_it) {
                    
                    std::map<std::string, int>::iterator nick_it = nick_to_fd.find(*m_it);
                    if (nick_it != nick_to_fd.end()) {
                        if (notified.find(nick_it->second) == notified.end()) {
                            send_raw(nick_it->second, nick_msg);
                            notified.insert(nick_it->second);
                        }
                    }
                }
            }
        }
    }
    
    void check_registration(int fd) {
        Client& client = clients[fd];
        
        if (!client.authenticated) {
            send_reply(fd, "464", ":Password incorrect");
            return;
        }
        
        if (!client.nick.empty() && !client.user.empty() && !client.registered) {
            client.registered = true;
            send_reply(fd, "001", ":Welcome to the IRC Network " + client.nick);
            send_reply(fd, "002", ":Your host is IRCServer");
            send_reply(fd, "003", ":This server was created recently");
            send_reply(fd, "004", "IRCServer 1.0 o o");
        }
    }
    
    void handle_join(int fd, const std::string& channel_name, const std::string& key = "") {
        if (channel_name.empty() || channel_name[0] != '#') {
            send_reply(fd, "403", channel_name + " :No such channel");
            return;
        }
        
        Client& client = clients[fd];
        
        // Find or create channel
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        bool new_channel = false;
        
        if (ch_it == channels.end()) {
            std::pair<std::map<std::string, Channel>::iterator, bool> result = 
                channels.insert(std::make_pair(channel_name, Channel(channel_name)));
            ch_it = result.first;
            new_channel = true;
        }
        
        Channel& channel = ch_it->second;
        
        // Check if user is already in channel
        if (channel.members.find(client.nick) != channel.members.end()) {
            return; // Already in channel
        }
        
        // Check channel key
        if (!channel.key.empty() && channel.key != key) {
            send_reply(fd, "475", channel_name + " :Cannot join channel (+k)");
            return;
        }
        
        // Check invite-only
        if (channel.invite_only) {
            if (client.invited_channels.find(channel_name) == client.invited_channels.end()) {
                send_reply(fd, "473", channel_name + " :Cannot join channel (+i)");
                return;
            }
            client.invited_channels.erase(channel_name);
        }
        
        // Check user limit
        if (channel.user_limit > 0 && (int)channel.members.size() >= channel.user_limit) {
            send_reply(fd, "471", channel_name + " :Cannot join channel (+l)");
            return;
        }
        
        if (new_channel) {
            channel.operators.insert(client.nick);
        }
        
        channel.members.insert(client.nick);
        client.channels.insert(channel_name);
        
        // Send JOIN message to all channel members
        std::string join_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " JOIN " + channel_name;
        send_to_channel(channel_name, join_msg);
        
        // Send topic if exists
        if (!channel.topic.empty()) {
            send_reply(fd, "332", channel_name + " :" + channel.topic);
        } else {
            send_reply(fd, "331", channel_name + " :No topic is set");
        }
        
        // Send names list
        send_names(fd, channel_name);
    }
    
    void handle_part(int fd, const std::string& channel_name, const std::string& reason) {
        leave_channel(fd, channel_name, reason);
    }
    
    void leave_channel(int fd, const std::string& channel_name, const std::string& reason) {
        Client& client = clients[fd];
        
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) {
            send_reply(fd, "403", channel_name + " :No such channel");
            return;
        }
        
        Channel& channel = ch_it->second;
        
        if (channel.members.find(client.nick) == channel.members.end()) {
            send_reply(fd, "442", channel_name + " :You're not on that channel");
            return;
        }
        
        // Send PART message to all channel members
        std::string part_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " PART " + channel_name;
        if (!reason.empty()) {
            part_msg += " :" + reason;
        }
        send_to_channel(channel_name, part_msg);
        
        // Remove from channel
        channel.members.erase(client.nick);
        channel.operators.erase(client.nick);
        client.channels.erase(channel_name);
        
        // Remove empty channel
        if (channel.members.empty()) {
            channels.erase(channel_name);
        }
    }
    
    void handle_privmsg(int fd, const std::string& target, const std::string& message) {
        Client& client = clients[fd];
        
        if (target[0] == '#') {
            // Channel message
            std::map<std::string, Channel>::iterator ch_it = channels.find(target);
            if (ch_it == channels.end()) {
                send_reply(fd, "403", target + " :No such channel");
                return;
            }
            
            Channel& channel = ch_it->second;
            if (channel.members.find(client.nick) == channel.members.end()) {
                send_reply(fd, "442", target + " :You're not on that channel");
                return;
            }
            
            std::string msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " PRIVMSG " + target + " :" + message;
            send_to_channel_except(target, msg, fd);
        } else {
            // Private message
            std::map<std::string, int>::iterator it = nick_to_fd.find(target);
            if (it == nick_to_fd.end()) {
                send_reply(fd, "401", target + " :No such nick/channel");
                return;
            }
            
            std::string msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " PRIVMSG " + target + " :" + message;
            send_raw(it->second, msg);
        }
    }
    
    void handle_quit(int fd, const std::string& reason) {
        Client& client = clients[fd];
        
        // Send QUIT message to all channels the user is in
        std::string quit_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " QUIT :" + reason;
        
        std::set<int> notified;
        for (std::set<std::string>::iterator ch_it = client.channels.begin();
             ch_it != client.channels.end(); ++ch_it) {
            
            if (channels.find(*ch_it) != channels.end()) {
                std::map<std::string, Channel>::iterator chan_it = channels.find(*ch_it);
                Channel& channel = chan_it->second;
                for (std::set<std::string>::iterator m_it = channel.members.begin();
                     m_it != channel.members.end(); ++m_it) {
                    
                    std::map<std::string, int>::iterator nick_it = nick_to_fd.find(*m_it);
                    if (nick_it != nick_to_fd.end() && nick_it->second != fd) {
                        if (notified.find(nick_it->second) == notified.end()) {
                            send_raw(nick_it->second, quit_msg);
                            notified.insert(nick_it->second);
                        }
                    }
                }
            }
        }
        
        disconnect_client(fd);
    }
    
    // New operator command implementations
    void handle_kick(int fd, const std::string& channel_name, const std::string& target_nick, const std::string& reason) {
        Client& client = clients[fd];
        
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) {
            send_reply(fd, "403", channel_name + " :No such channel");
            return;
        }
        
        Channel& channel = ch_it->second;
        
        // Check if user is in channel
        if (channel.members.find(client.nick) == channel.members.end()) {
            send_reply(fd, "442", channel_name + " :You're not on that channel");
            return;
        }
        
        // Check if user is operator
        if (channel.operators.find(client.nick) == channel.operators.end()) {
            send_reply(fd, "482", channel_name + " :You're not channel operator");
            return;
        }
        
        // Check if target is in channel
        if (channel.members.find(target_nick) == channel.members.end()) {
            send_reply(fd, "441", target_nick + " " + channel_name + " :They aren't on that channel");
            return;
        }
        
        // Send KICK message to all channel members
        std::string kick_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + 
                              " KICK " + channel_name + " " + target_nick + " :" + reason;
        send_to_channel(channel_name, kick_msg);
        
        // Remove target from channel
        channel.members.erase(target_nick);
        channel.operators.erase(target_nick);
        
        // Remove channel from target's channel list
        std::map<std::string, int>::iterator target_it = nick_to_fd.find(target_nick);
        if (target_it != nick_to_fd.end()) {
            clients[target_it->second].channels.erase(channel_name);
        }
        
        // Remove empty channel
        if (channel.members.empty()) {
            channels.erase(channel_name);
        }
    }
    
    void handle_invite(int fd, const std::string& target_nick, const std::string& channel_name) {
        Client& client = clients[fd];
        
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) {
            send_reply(fd, "403", channel_name + " :No such channel");
            return;
        }
        
        Channel& channel = ch_it->second;
        
        // Check if user is in channel
        if (channel.members.find(client.nick) == channel.members.end()) {
            send_reply(fd, "442", channel_name + " :You're not on that channel");
            return;
        }
        
        // Check if user is operator
        if (channel.operators.find(client.nick) == channel.operators.end()) {
            send_reply(fd, "482", channel_name + " :You're not channel operator");
            return;
        }
        
        // Check if target exists
        std::map<std::string, int>::iterator target_it = nick_to_fd.find(target_nick);
        if (target_it == nick_to_fd.end()) {
            send_reply(fd, "401", target_nick + " :No such nick/channel");
            return;
        }
        
        // Check if target is already in channel
        if (channel.members.find(target_nick) != channel.members.end()) {
            send_reply(fd, "443", target_nick + " " + channel_name + " :is already on channel");
            return;
        }
        
        // Add to invited list
        clients[target_it->second].invited_channels.insert(channel_name);
        
        // Send INVITE message to target
        std::string invite_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + 
                                " INVITE " + target_nick + " " + channel_name;
        send_raw(target_it->second, invite_msg);
        
        // Confirm to sender
        send_reply(fd, "341", target_nick + " " + channel_name);
    }
    
    void handle_topic(int fd, const std::string& channel_name, const std::string& new_topic) {
        Client& client = clients[fd];
        
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) {
            send_reply(fd, "403", channel_name + " :No such channel");
            return;
        }
        
        Channel& channel = ch_it->second;
        
        // Check if user is in channel
        if (channel.members.find(client.nick) == channel.members.end()) {
            send_reply(fd, "442", channel_name + " :You're not on that channel");
            return;
        }
        
        if (new_topic.empty()) {
            // View topic
            if (channel.topic.empty()) {
                send_reply(fd, "331", channel_name + " :No topic is set");
            } else {
                send_reply(fd, "332", channel_name + " :" + channel.topic);
            }
        } else {
            // Set topic
            if (channel.topic_protected && channel.operators.find(client.nick) == channel.operators.end()) {
                send_reply(fd, "482", channel_name + " :You're not channel operator");
                return;
            }
            
            channel.topic = new_topic;
            
            // Send TOPIC message to all channel members
            std::string topic_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + 
                                   " TOPIC " + channel_name + " :" + new_topic;
            send_to_channel(channel_name, topic_msg);
        }
    }
    
    void handle_mode(int fd, const std::vector<std::string>& params) {
        Client& client = clients[fd];
        std::string target = params[0];
        
        if (target[0] == '#') {
            // Channel mode
            std::map<std::string, Channel>::iterator ch_it = channels.find(target);
            if (ch_it == channels.end()) {
                send_reply(fd, "403", target + " :No such channel");
                return;
            }
            
            Channel& channel = ch_it->second;
            
            // Check if user is in channel
            if (channel.members.find(client.nick) == channel.members.end()) {
                send_reply(fd, "442", target + " :You're not on that channel");
                return;
            }
            
            if (params.size() == 1) {
                // View channel modes
                std::string modes = "+";
                if (channel.invite_only) modes += "i";
                if (channel.topic_protected) modes += "t";
                if (!channel.key.empty()) modes += "k";
                if (channel.user_limit > 0) modes += "l";
                
                send_reply(fd, "324", target + " " + modes);
                return;
            }
            
            // Check if user is operator for mode changes
            if (channel.operators.find(client.nick) == channel.operators.end()) {
                send_reply(fd, "482", target + " :You're not channel operator");
                return;
            }
            
            std::string mode_string = params[1];
            bool adding = true;
            std::string changes = "";
            std::string change_params = "";
            int param_index = 2;
            
            for (size_t i = 0; i < mode_string.length(); i++) {
                char mode = mode_string[i];
                
                if (mode == '+') {
                    adding = true;
                    continue;
                } else if (mode == '-') {
                    adding = false;
                    continue;
                }
                
                bool mode_changed = false;
                std::string mode_param = "";
                
                switch (mode) {
                    case 'i':
                        if (channel.invite_only != adding) {
                            channel.invite_only = adding;
                            mode_changed = true;
                        }
                        break;
                        
                    case 't':
                        if (channel.topic_protected != adding) {
                            channel.topic_protected = adding;
                            mode_changed = true;
                        }
                        break;
                        
                    case 'k':
                        if (adding) {
                            if (param_index < (int)params.size()) {
                                channel.key = params[param_index];
                                mode_param = params[param_index];
                                param_index++;
                                mode_changed = true;
                            }
                        } else {
                            if (!channel.key.empty()) {
                                channel.key = "";
                                mode_changed = true;
                            }
                        }
                        break;
                        
                    case 'l':
                        if (adding) {
                            if (param_index < (int)params.size()) {
                                int limit = atoi(params[param_index].c_str());
                                if (limit > 0) {
                                    channel.user_limit = limit;
                                    mode_param = params[param_index];
                                    param_index++;
                                    mode_changed = true;
                                }
                            }
                        } else {
                            if (channel.user_limit > 0) {
                                channel.user_limit = 0;
                                mode_changed = true;
                            }
                        }
                        break;
                        
                    case 'o':
                        if (param_index < (int)params.size()) {
                            std::string target_nick = params[param_index];
                            param_index++;
                            
                            // Check if target is in channel
                            if (channel.members.find(target_nick) != channel.members.end()) {
                                if (adding) {
                                    if (channel.operators.find(target_nick) == channel.operators.end()) {
                                        channel.operators.insert(target_nick);
                                        mode_changed = true;
                                        mode_param = target_nick;
                                    }
                                } else {
                                    if (channel.operators.find(target_nick) != channel.operators.end()) {
                                        channel.operators.erase(target_nick);
                                        mode_changed = true;
                                        mode_param = target_nick;
                                    }
                                }
                            }
                        }
                        break;
                        
                    default:
                        send_reply(fd, "472", std::string(1, mode) + " :is unknown mode char to me");
                        continue;
                }
                
                if (mode_changed) {
                    changes += (adding ? "+" : "-");
                    changes += mode;
                    if (!mode_param.empty()) {
                        if (!change_params.empty()) change_params += " ";
                        change_params += mode_param;
                    }
                }
            }
            
            // Send MODE message to all channel members if there were changes
            if (!changes.empty()) {
                std::string mode_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + 
                                      " MODE " + target + " " + changes;
                if (!change_params.empty()) {
                    mode_msg += " " + change_params;
                }
                send_to_channel(target, mode_msg);
            }
        }
    }
    
    void send_names(int fd, const std::string& channel_name) {
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) return;
        
        Channel& channel = ch_it->second;
        std::string names = "";
        
        for (std::set<std::string>::iterator it = channel.members.begin();
             it != channel.members.end(); ++it) {
            if (!names.empty()) names += " ";
            if (channel.operators.find(*it) != channel.operators.end()) {
                names += "@";
            }
            names += *it;
        }
        
        send_reply(fd, "353", "= " + channel_name + " :" + names);
        send_reply(fd, "366", channel_name + " :End of /NAMES list");
    }
    
    void send_to_channel(const std::string& channel_name, const std::string& message) {
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) return;
        
        Channel& channel = ch_it->second;
        for (std::set<std::string>::iterator it = channel.members.begin();
             it != channel.members.end(); ++it) {
            
            std::map<std::string, int>::iterator nick_it = nick_to_fd.find(*it);
            if (nick_it != nick_to_fd.end()) {
                send_raw(nick_it->second, message);
            }
        }
    }
    
    void send_to_channel_except(const std::string& channel_name, const std::string& message, int except_fd) {
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) return;
        
        Channel& channel = ch_it->second;
        for (std::set<std::string>::iterator it = channel.members.begin();
             it != channel.members.end(); ++it) {
            
            std::map<std::string, int>::iterator nick_it = nick_to_fd.find(*it);
            if (nick_it != nick_to_fd.end() && nick_it->second != except_fd) {
                send_raw(nick_it->second, message);
            }
        }
    }
    
    void send_reply(int fd, const std::string& code, const std::string& message) {
        Client& client = clients[fd];
        std::string nick = client.nick.empty() ? "*" : client.nick;
        std::string reply = ":IRCServer " + code + " " + nick + " " + message + "\r\n";
        send(fd, reply.c_str(), reply.length(), MSG_NOSIGNAL);
    }
    
    void send_raw(int fd, const std::string& message) {
        std::string msg = message + "\r\n";
        send(fd, msg.c_str(), msg.length(), MSG_NOSIGNAL);
    }
};

// Static member definition
bool IRCServer::shutdown_requested = false;

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <port> <password>" << std::endl;
        return 1;
    }
    
    // Set up signal handlers
    signal(SIGINT, IRCServer::signal_handler);
    signal(SIGTERM, IRCServer::signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    try {
        IRCServer server(argv[1], argv[2]);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}