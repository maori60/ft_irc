// ircserv.cpp — C++98, un seul epoll, FD non-bloquants, pas de S2S.

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

#define MAX_EVENTS     1024
#define BUFFER_SIZE    4096
#define MAX_NICK_LEN   32
#define MAX_CHANNEL_LEN 50

struct Client {
    int fd;
    std::string nick;
    std::string user;
    std::string real;
    std::string hostname;
    bool authenticated;
    bool registered;
    bool closing;

    std::string buffer;
    std::string outbuf;

    std::set<std::string> channels;
    std::set<std::string> invited_channels;

    Client() : fd(-1), authenticated(false), registered(false), closing(false) {}
    Client(int f) : fd(f), authenticated(false), registered(false), closing(false) {}
};

struct Channel {
    std::string name;
    std::string topic;
    std::set<std::string> members;     // nicks
    std::set<std::string> operators;   // nicks
    std::string key;
    bool invite_only;
    bool topic_protected;
    int user_limit;

    Channel() : invite_only(false), topic_protected(true), user_limit(0) {}
    Channel(const std::string& n) : name(n), invite_only(false), topic_protected(true), user_limit(0) {}
};

class IRCServer {
private:
    int server_fd;
    int epoll_fd;
    std::string password;
    std::map<int, Client> clients;               // fd -> client
    std::map<std::string, Channel> channels;     // name -> channel
    std::map<std::string, int> nick_to_fd;       // nick -> fd
    static bool shutdown_requested;

public:
    IRCServer(const std::string& port, const std::string& pass)
        : server_fd(-1), epoll_fd(-1), password(pass) {
        init_server(port);
    }

    ~IRCServer() { cleanup(); }

    static void signal_handler(int sig) {
        if (sig == SIGINT || sig == SIGTERM) {
            std::cout << "\nReceived signal, shutting down gracefully..." << std::endl;
            shutdown_requested = true;
        }
    }

    void cleanup() {
        for (std::map<int, Client>::iterator it = clients.begin(); it != clients.end(); ++it) {
            queue_send(it->first, "ERROR :Server shutting down\r\n");
            flush_client(it->first);
            close(it->first);
        }
        clients.clear();
        channels.clear();
        nick_to_fd.clear();
        if (server_fd != -1) { close(server_fd); server_fd = -1; }
        if (epoll_fd  != -1) { close(epoll_fd ); epoll_fd  = -1; }
    }

    void init_server(const std::string& port) {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == -1) { perror("socket"); exit(1); }

        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
            perror("setsockopt"); exit(1);
        }

        // ---- Conformité stricte : un seul flag autorisé
        fcntl(server_fd, F_SETFL, O_NONBLOCK);

        if (port.empty()) { std::cerr << "Error: Port cannot be empty" << std::endl; exit(1); }
        int port_num = atoi(port.c_str());
        if (port_num < 0 || port_num > 65535) { std::cerr << "Error: Port must be between 0 and 65535" << std::endl; exit(1); }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_num);

        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) { perror("bind"); exit(1); }
        if (listen(server_fd, SOMAXCONN) == -1) { perror("listen"); exit(1); }

        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) { perror("epoll_create1"); exit(1); }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = server_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) { perror("epoll_ctl"); exit(1); }

        std::cout << "IRC Server listening on port " << port_num << std::endl;
    }

    void run() {
        struct epoll_event events[MAX_EVENTS];

        while (!shutdown_requested) {
            int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
            if (nfds == -1) {
                if (errno == EINTR) continue;
                perror("epoll_wait");
                break;
            }

            for (int i = 0; i < nfds; i++) {
                int fd = events[i].data.fd;
                uint32_t ev = events[i].events;

                if (fd == server_fd) { accept_connection(); continue; }

                if (ev & (EPOLLERR | EPOLLHUP)) {
                    disconnect_client(fd);
                    continue;
                }

                if (ev & EPOLLIN)  handle_client_data(fd);
                if (ev & EPOLLOUT) flush_client(fd);
            }
        }

        std::cout << "Server loop ended, cleaning up..." << std::endl;
        cleanup();
    }

private:
    Client* get_client(int fd) {
        std::map<int, Client>::iterator it = clients.find(fd);
        if (it == clients.end()) return NULL;
        return &it->second;
    }

    void epoll_mod(int fd, bool want_out) {
        struct epoll_event ev;
        ev.data.fd = fd;
        ev.events = EPOLLIN;
        if (want_out) ev.events |= EPOLLOUT;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }

    void epoll_add_client(int fd) {
        struct epoll_event ev;
        ev.data.fd = fd;
        ev.events = EPOLLIN;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }

    void accept_connection() {
        for (;;) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                perror("accept");
                break;
            }
            // ---- Conformité stricte : pas de F_GETFL
            fcntl(client_fd, F_SETFL, O_NONBLOCK);
            epoll_add_client(client_fd);

            clients[client_fd] = Client(client_fd);
            clients[client_fd].hostname = inet_ntoa(client_addr.sin_addr);
            std::cout << "New connection from " << clients[client_fd].hostname << std::endl;
        }
    }

    void handle_client_data(int fd) {
        std::map<int, Client>::iterator it = clients.find(fd);
        if (it == clients.end()) return;
        Client& c = it->second;

        char buf[BUFFER_SIZE];
        for (;;) {
            ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                c.buffer.append(buf, n);

                for (;;) {
                    size_t pos = c.buffer.find('\n');
                    if (pos == std::string::npos) break;
                    std::string line = c.buffer.substr(0, pos);
                    if (!line.empty() && line[line.length() - 1] == '\r')
                        line.erase(line.length() - 1);
                    c.buffer.erase(0, pos + 1);

                    process_message(fd, line);
                    if (clients.find(fd) == clients.end()) return;
                }

                if (c.buffer.size() > (1u << 20)) c.buffer.clear();
            }
            else if (n == 0) {
                disconnect_client(fd);
                return;
            }
            else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                disconnect_client(fd);
                return;
            }
        }
    }

    void queue_send(int fd, const std::string& data) {
        std::map<int, Client>::iterator it = clients.find(fd);
        if (it == clients.end()) return;
        Client& c = it->second;

        if (c.closing) return;

        if (c.outbuf.empty()) {
            ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
            if (n == (ssize_t)data.size()) {
                return;
            } else if (n >= 0) {
                c.outbuf.append(data.data() + n, data.size() - n);
                epoll_mod(fd, true);
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    c.outbuf.append(data);
                    epoll_mod(fd, true);
                } else {
                    disconnect_client(fd);
                }
            }
        } else {
            c.outbuf.append(data);
            epoll_mod(fd, true);
        }
    }

    void flush_client(int fd) {
        std::map<int, Client>::iterator it = clients.find(fd);
        if (it == clients.end()) return;
        Client& c = it->second;

        while (!c.outbuf.empty()) {
            ssize_t n = ::send(fd, c.outbuf.data(), c.outbuf.size(), MSG_NOSIGNAL);
            if (n > 0) {
                c.outbuf.erase(0, n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                epoll_mod(fd, true);
                return;
            } else {
                disconnect_client(fd);
                return;
            }
        }
        epoll_mod(fd, false);
    }

    void send_from_server(int fd, const std::string& payload) {
        if (!get_client(fd)) return;
        std::string msg = ":IRCServer " + payload + "\r\n";
        queue_send(fd, msg);
    }

    void disconnect_client(int fd, bool broadcast_part = true) {
        std::map<int, Client>::iterator it = clients.find(fd);
        if (it == clients.end()) return;

        Client& clientRef = it->second;
        if (clientRef.closing) return;
        clientRef.closing = true;

        std::set<std::string> channels_copy = clientRef.channels;
        for (std::set<std::string>::iterator ch_it = channels_copy.begin();
             ch_it != channels_copy.end(); ++ch_it) {
            if (broadcast_part)
                leave_channel(fd, *ch_it, "Client disconnected", true);
            else {
                std::map<std::string, Channel>::iterator chan_it = channels.find(*ch_it);
                if (chan_it != channels.end()) {
                    Channel& ch = chan_it->second;
                    ch.members.erase(clientRef.nick);
                    ch.operators.erase(clientRef.nick);
                    if (ch.members.empty()) channels.erase(chan_it);
                }
            }
        }

        if (!clientRef.nick.empty()) nick_to_fd.erase(clientRef.nick);

        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        clients.erase(it);

        std::cout << "Client " << clientRef.hostname << " disconnected" << std::endl;
    }

    void process_message(int fd, const std::string& line) {
        if (line.empty()) return;

        std::istringstream iss(line);
        std::string command;
        std::vector<std::string> params;

        iss >> command;
        std::transform(command.begin(), command.end(), command.begin(), ::toupper);

        std::string rest;
        std::getline(iss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);

        if (!rest.empty()) {
            if (rest[0] == ':') {
                params.push_back(rest.substr(1));
            } else {
                std::istringstream param_stream(rest);
                std::string p;
                while (param_stream >> p) {
                    if (!p.empty() && p[0] == ':') {
                        std::string trailing;
                        std::getline(param_stream, trailing);
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
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;

        if (cmd == "PASS") {
            if (params.size() < 1) { send_reply(fd, "461", "PASS :Not enough parameters"); return; }
            if (params[0] == password) {
                client.authenticated = true;
            } else {
                send_reply(fd, "464", ":Password incorrect");
                disconnect_client(fd);
            }
        }
        else if (cmd == "NICK") {
            if (params.size() < 1) { send_reply(fd, "431", ":No nickname given"); return; }
            handle_nick(fd, params[0]);
        }
        else if (cmd == "USER") {
            if (params.size() < 4) { send_reply(fd, "461", "USER :Not enough parameters"); return; }
            client.user = params[0];
            client.real = params[3];
            check_registration(fd);
        }
        else if (cmd == "JOIN") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.size() < 1) { send_reply(fd, "461", "JOIN :Not enough parameters"); return; }

            // Parse channels and keys (CSV)
            std::vector<std::string> chans;
            std::vector<std::string> keys;
            std::string ch_param = params[0];
            std::string key_param = params.size() > 1 ? params[1] : "";

            size_t start = 0, end;
            while ((end = ch_param.find(',', start)) != std::string::npos) {
                chans.push_back(ch_param.substr(start, end - start));
                start = end + 1;
            }
            if (start <= ch_param.size()) chans.push_back(ch_param.substr(start));

            start = 0;
            while ((end = key_param.find(',', start)) != std::string::npos) {
                keys.push_back(key_param.substr(start, end - start));
                start = end + 1;
            }
            if (!key_param.empty() && start <= key_param.size())
                keys.push_back(key_param.substr(start));

            for (size_t i = 0; i < chans.size(); ++i) {
                std::string key = (i < keys.size()) ? keys[i] : "";
                handle_join(fd, chans[i], key);
                if (!get_client(fd)) return;
            }
        }
        else if (cmd == "PART") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.size() < 1) { send_reply(fd, "461", "PART :Not enough parameters"); return; }
            std::string reason = params.size() > 1 ? params[1] : "";
            handle_part(fd, params[0], reason);
        }
        else if (cmd == "PRIVMSG") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.size() < 2) { send_reply(fd, "461", "PRIVMSG :Not enough parameters"); return; }
            handle_privmsg(fd, params[0], params[1]);
        }
        else if (cmd == "QUIT") {
            std::string reason = params.size() > 0 ? params[0] : "Client Quit";
            handle_quit(fd, reason);
        }
        else if (cmd == "PING") {
            if (params.size() > 0) send_from_server(fd, "PONG :" + params[0]);
            else send_from_server(fd, "PONG :IRCServer");
        }
        else if (cmd == "KICK") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.size() < 2) { send_reply(fd, "461", "KICK :Not enough parameters"); return; }
            std::string reason = params.size() > 2 ? params[2] : client.nick;
            handle_kick(fd, params[0], params[1], reason);
        }
        else if (cmd == "INVITE") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.size() < 2) { send_reply(fd, "461", "INVITE :Not enough parameters"); return; }
            handle_invite(fd, params[0], params[1]);
        }
        else if (cmd == "TOPIC") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.size() < 1) { send_reply(fd, "461", "TOPIC :Not enough parameters"); return; }
            std::string topic = params.size() > 1 ? params[1] : "";
            handle_topic(fd, params[0], topic);
        }
        else if (cmd == "MODE") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.size() < 1) { send_reply(fd, "461", "MODE :Not enough parameters"); return; }
            handle_mode(fd, params);
        }
        else if (cmd == "NAMES") {
            if (params.size() < 1) return;
            send_names(fd, params[0]);
        }
    }

    void handle_nick(int fd, const std::string& nick) {
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;

        if (nick.length() > MAX_NICK_LEN) { send_reply(fd, "432", nick + " :Erroneous nickname"); return; }
        if (nick_to_fd.find(nick) != nick_to_fd.end()) { send_reply(fd, "433", nick + " :Nickname is already in use"); return; }

        std::string old_nick = client.nick;
        bool was_registered = client.registered;

        if (!client.nick.empty()) nick_to_fd.erase(client.nick);

        client.nick = nick;
        nick_to_fd[nick] = fd;

        if (was_registered && !old_nick.empty()) handle_nick_change(fd, old_nick, nick);
        check_registration(fd);
    }

    void handle_nick_change(int fd, const std::string& old_nick, const std::string& new_nick) {
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;
        std::set<int> notified;

        std::string nick_msg = ":" + old_nick + "!" + client.user + "@" + client.hostname + " NICK :" + new_nick;

        send_raw(fd, nick_msg);
        notified.insert(fd);

        for (std::set<std::string>::iterator ch_it = client.channels.begin(); ch_it != client.channels.end(); ++ch_it) {
            std::map<std::string, Channel>::iterator chan_it = channels.find(*ch_it);
            if (chan_it != channels.end()) {
                Channel& channel = chan_it->second;

                channel.members.erase(old_nick);
                channel.members.insert(new_nick);

                if (channel.operators.find(old_nick) != channel.operators.end()) {
                    channel.operators.erase(old_nick);
                    channel.operators.insert(new_nick);
                }

                for (std::set<std::string>::iterator m_it = channel.members.begin(); m_it != channel.members.end(); ++m_it) {
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
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;

        if (!client.authenticated) return;

        if (!client.nick.empty() && !client.user.empty() && !client.registered) {
            client.registered = true;
            send_reply(fd, "001", ":Welcome to the IRC Network " + client.nick);
            send_reply(fd, "002", ":Your host is IRCServer");
            send_reply(fd, "003", ":This server was created recently");
            send_reply(fd, "004", "IRCServer 1.0 o o");
        }
    }

    // JOIN: ordre des vérifs standardisé -> +l, puis +i (toujours), puis +k
    void handle_join(int fd, const std::string& channel_name, const std::string& key = "") {
        if (channel_name.empty() || channel_name[0] != '#') {
            send_reply(fd, "403", channel_name + " :No such channel");
            return;
        }
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;

        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        bool new_channel = false;
        if (ch_it == channels.end()) {
            channels[channel_name] = Channel(channel_name);
            ch_it = channels.find(channel_name);
            new_channel = true;
        }
        Channel& channel = ch_it->second;

        if (channel.members.find(client.nick) != channel.members.end()) return;

        bool has_invite = (client.invited_channels.find(channel_name) != client.invited_channels.end());

        // 1) Limite (+l)
        if (channel.user_limit > 0 && (int)channel.members.size() >= channel.user_limit) {
            send_reply(fd, "471", channel_name + " :Cannot join channel (+l)");
            return;
        }

        // 2) Invite-only (+i) — doit être respecté même si la clé est correcte
        if (channel.invite_only && !has_invite) {
            send_reply(fd, "473", channel_name + " :Cannot join channel (+i)");
            return;
        }

        // 3) Clé (+k)
        if (!channel.key.empty() && key != channel.key) {
            send_reply(fd, "475", channel_name + " :Cannot join channel (+k)");
            return;
        }

        if (has_invite) client.invited_channels.erase(channel_name);

        if (new_channel) channel.operators.insert(client.nick);

        channel.members.insert(client.nick);
        client.channels.insert(channel_name);

        std::string join_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " JOIN " + channel_name;
        send_to_channel(channel_name, join_msg);

        if (!channel.topic.empty())
            send_reply(fd, "332", channel_name + " :" + channel.topic);
        else
            send_reply(fd, "331", channel_name + " :No topic is set");

        send_names(fd, channel_name);
    }

    void handle_part(int fd, const std::string& channel_name, const std::string& reason) {
        leave_channel(fd, channel_name, reason, false);
    }

    void leave_channel(int fd, const std::string& channel_name, const std::string& reason, bool exclude_self) {
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;

        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) { send_reply(fd, "403", channel_name + " :No such channel"); return; }

        Channel& channel = ch_it->second;
        if (channel.members.find(client.nick) == channel.members.end()) { send_reply(fd, "442", channel_name + " :You're not on that channel"); return; }

        std::string part_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " PART " + channel_name;
        if (!reason.empty()) part_msg += " :" + reason;

        if (exclude_self) send_to_channel_except(channel_name, part_msg, fd);
        else send_to_channel(channel_name, part_msg);

        channel.members.erase(client.nick);
        channel.operators.erase(client.nick);
        client.channels.erase(channel_name);

        if (channel.members.empty()) channels.erase(channel_name);
    }

    void handle_privmsg(int fd, const std::string& target, const std::string& message) {
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;

        if (!target.empty() && target[0] == '#') {
            std::map<std::string, Channel>::iterator ch_it = channels.find(target);
            if (ch_it == channels.end()) { send_reply(fd, "403", target + " :No such channel"); return; }

            Channel& channel = ch_it->second;
            if (channel.members.find(client.nick) == channel.members.end()) { send_reply(fd, "442", target + " :You're not on that channel"); return; }

            std::string msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " PRIVMSG " + target + " :" + message;
            send_to_channel_except(target, msg, fd);
        } else {
            std::map<std::string, int>::iterator it = nick_to_fd.find(target);
            if (it == nick_to_fd.end()) { send_reply(fd, "401", target + " :No such nick/channel"); return; }
            std::string msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " PRIVMSG " + target + " :" + message;
            send_raw(it->second, msg);
        }
    }

    void handle_quit(int fd, const std::string& reason) {
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;

        std::string quit_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " QUIT :" + reason;

        std::set<int> notified;
        for (std::set<std::string>::iterator ch_it = client.channels.begin(); ch_it != client.channels.end(); ++ch_it) {
            std::map<std::string, Channel>::iterator chan_it = channels.find(*ch_it);
            if (chan_it != channels.end()) {
                Channel& channel = chan_it->second;
                for (std::set<std::string>::iterator m_it = channel.members.begin(); m_it != channel.members.end(); ++m_it) {
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

        disconnect_client(fd, false);
    }

    void handle_kick(int fd, const std::string& channel_name, const std::string& target_nick, const std::string& reason) {
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;

        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) { send_reply(fd, "403", channel_name + " :No such channel"); return; }
        Channel& channel = ch_it->second;

        if (channel.members.find(client.nick) == channel.members.end()) { send_reply(fd, "442", channel_name + " :You're not on that channel"); return; }
        if (channel.operators.find(client.nick) == channel.operators.end()) { send_reply(fd, "482", channel_name + " :You're not channel operator"); return; }
        if (channel.members.find(target_nick) == channel.members.end()) { send_reply(fd, "441", target_nick + " " + channel_name + " :They aren't on that channel"); return; }

        std::string kick_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " KICK " + channel_name + " " + target_nick + " :" + reason;
        send_to_channel(channel_name, kick_msg);

        channel.members.erase(target_nick);
        channel.operators.erase(target_nick);

        std::map<std::string, int>::iterator target_it = nick_to_fd.find(target_nick);
        if (target_it != nick_to_fd.end()) {
            Client* tgt = get_client(target_it->second);
            if (tgt) tgt->channels.erase(channel_name);
        }

        if (channel.members.empty()) channels.erase(channel_name);
    }

    void handle_invite(int fd, const std::string& target_nick, const std::string& channel_name) {
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;

        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) { send_reply(fd, "403", channel_name + " :No such channel"); return; }
        Channel& channel = ch_it->second;

        if (channel.members.find(client.nick) == channel.members.end()) { send_reply(fd, "442", channel_name + " :You're not on that channel"); return; }
        if (channel.operators.find(client.nick) == channel.operators.end()) { send_reply(fd, "482", channel_name + " :You're not channel operator"); return; }

        std::map<std::string, int>::iterator target_it = nick_to_fd.find(target_nick);
        if (target_it == nick_to_fd.end()) { send_reply(fd, "401", target_nick + " :No such nick/channel"); return; }
        if (channel.members.find(target_nick) != channel.members.end()) { send_reply(fd, "443", target_nick + " " + channel_name + " :is already on channel"); return; }

        Client* tgt = get_client(target_it->second);
        if (!tgt) return;
        tgt->invited_channels.insert(channel_name);

        std::string invite_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " INVITE " + target_nick + " " + channel_name;
        send_raw(target_it->second, invite_msg);
        send_reply(fd, "341", target_nick + " " + channel_name);
    }

    void handle_topic(int fd, const std::string& channel_name, const std::string& new_topic) {
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;

        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) { send_reply(fd, "403", channel_name + " :No such channel"); return; }
        Channel& channel = ch_it->second;

        if (channel.members.find(client.nick) == channel.members.end()) { send_reply(fd, "442", channel_name + " :You're not on that channel"); return; }

        if (new_topic.empty()) {
            if (channel.topic.empty()) send_reply(fd, "331", channel_name + " :No topic is set");
            else send_reply(fd, "332", channel_name + " :" + channel.topic);
        } else {
            if (channel.topic_protected && channel.operators.find(client.nick) == channel.operators.end()) {
                send_reply(fd, "482", channel_name + " :You're not channel operator");
                return;
            }
            channel.topic = new_topic;
            std::string topic_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " TOPIC " + channel_name + " :" + new_topic;
            send_to_channel(channel_name, topic_msg);
        }
    }

    void handle_mode(int fd, const std::vector<std::string>& params) {
        Client* pclient = get_client(fd);
        if (!pclient) return;
        Client& client = *pclient;
        std::string target = params[0];

        if (!target.empty() && target[0] == '#') {
            std::map<std::string, Channel>::iterator ch_it = channels.find(target);
            if (ch_it == channels.end()) { send_reply(fd, "403", target + " :No such channel"); return; }
            Channel& channel = ch_it->second;

            if (channel.members.find(client.nick) == channel.members.end()) { send_reply(fd, "442", target + " :You're not on that channel"); return; }

            if (params.size() == 1) {
                std::string modes = "+";
                if (channel.invite_only) modes += "i";
                if (channel.topic_protected) modes += "t";
                if (!channel.key.empty()) modes += "k";
                if (channel.user_limit > 0) modes += "l";
                send_reply(fd, "324", target + " " + modes);
                return;
            }

            if (channel.operators.find(client.nick) == channel.operators.end()) { send_reply(fd, "482", target + " :You're not channel operator"); return; }

            std::string mode_string = params[1];
            bool adding = true;
            std::string changes = "";
            std::string change_params = "";
            int param_index = 2;

            for (size_t i = 0; i < mode_string.length(); i++) {
                char mode = mode_string[i];
                if (mode == '+') { adding = true; continue; }
                if (mode == '-') { adding = false; continue; }

                bool mode_changed = false;
                std::string mode_param = "";

                switch (mode) {
                    case 'i':
                        if (channel.invite_only != adding) { channel.invite_only = adding; mode_changed = true; }
                        break;
                    case 't':
                        if (channel.topic_protected != adding) { channel.topic_protected = adding; mode_changed = true; }
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
                            if (!channel.key.empty()) { channel.key = ""; mode_changed = true; }
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
                            if (channel.user_limit > 0) { channel.user_limit = 0; mode_changed = true; }
                        }
                        break;
                    case 'o':
                        if (param_index < (int)params.size()) {
                            std::string target_nick = params[param_index];
                            param_index++;
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

            if (!changes.empty()) {
                std::string mode_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " MODE " + target + " " + changes;
                if (!change_params.empty()) mode_msg += " " + change_params;
                send_to_channel(target, mode_msg);
            }
        }
    }

    void send_names(int fd, const std::string& channel_name) {
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) return;
        Channel& channel = ch_it->second;

        std::string names = "";
        for (std::set<std::string>::iterator it = channel.members.begin(); it != channel.members.end(); ++it) {
            if (!names.empty()) names += " ";
            if (channel.operators.find(*it) != channel.operators.end()) names += "@";
            names += *it;
        }

        send_reply(fd, "353", "= " + channel_name + " :" + names);
        send_reply(fd, "366", channel_name + " :End of /NAMES list");
    }

    void send_to_channel(const std::string& channel_name, const std::string& message) {
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) return;
        Channel& channel = ch_it->second;
        std::string line = message + "\r\n";

        for (std::set<std::string>::iterator it = channel.members.begin(); it != channel.members.end(); ++it) {
            std::map<std::string, int>::iterator nick_it = nick_to_fd.find(*it);
            if (nick_it != nick_to_fd.end()) queue_send(nick_it->second, line);
        }
    }

    void send_to_channel_except(const std::string& channel_name, const std::string& message, int except_fd) {
        std::map<std::string, Channel>::iterator ch_it = channels.find(channel_name);
        if (ch_it == channels.end()) return;
        Channel& channel = ch_it->second;
        std::string line = message + "\r\n";

        for (std::set<std::string>::iterator it = channel.members.begin(); it != channel.members.end(); ++it) {
            std::map<std::string, int>::iterator nick_it = nick_to_fd.find(*it);
            if (nick_it != nick_to_fd.end() && nick_it->second != except_fd) queue_send(nick_it->second, line);
        }
    }

    // Numériques au format RFC/tests : :IRCServer <code> <nick|*> <params...>
    void send_reply(int fd, const std::string& code, const std::string& message) {
        Client* c = get_client(fd);
        if (!c) return;
        const std::string target = c->nick.empty() ? "*" : c->nick;
        std::string reply = ":IRCServer " + code + " " + target + " " + message + "\r\n";
        queue_send(fd, reply);
    }

    void send_raw(int fd, const std::string& message) {
        if (!get_client(fd)) return;
        std::string msg = message + "\r\n";
        queue_send(fd, msg);
    }
};

bool IRCServer::shutdown_requested = false;

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <port> <password>" << std::endl;
        return 1;
    }

    signal(SIGINT,  IRCServer::signal_handler);
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
