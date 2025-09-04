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

// -------------------- Client / Channel --------------------
struct Client {
    int fd;
    std::string nick;
    std::string user;
    std::string real;
    std::string hostname;
    bool authenticated;
    bool registered;

    std::string inbuf;              // recv buffer
    std::string outbuf;             // send queue
    bool closing_after_flush;       // flag to close after flush

    std::set<std::string> channels;
    std::set<std::string> invited_channels;

    Client() : fd(-1), authenticated(false), registered(false),
               closing_after_flush(false) {}
    Client(int f) : fd(f), authenticated(false), registered(false),
                    closing_after_flush(false) {}
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

    Channel(const std::string& n) : name(n),
        invite_only(false), topic_protected(true), user_limit(0) {}
};

// -------------------- IRC Server --------------------
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
    ~IRCServer() { cleanup(); }

    static void signal_handler(int sig) {
        if (sig == SIGINT || sig == SIGTERM) {
            std::cout << "\nReceived signal, shutting down gracefully...\n";
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
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        int flags = fcntl(server_fd, F_GETFL, 0);
        fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

        if (port.empty()) { std::cerr << "Port cannot be empty\n"; exit(1); }
        int port_num = atoi(port.c_str());
        if (port_num < 0 || port_num > 65535) { std::cerr << "Bad port\n"; exit(1); }

        sockaddr_in addr; memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_num);

        if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) == -1) { perror("bind"); exit(1); }

        socklen_t alen = sizeof(addr);
        getsockname(server_fd, (sockaddr*)&addr, &alen);
        int actual_port = ntohs(addr.sin_port);

        if (listen(server_fd, SOMAXCONN) == -1) { perror("listen"); exit(1); }

        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) { perror("epoll_create1"); exit(1); }

        epoll_event ev; ev.events = EPOLLIN; ev.data.fd = server_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
            perror("epoll_ctl"); exit(1);
        }

        std::cout << "IRC Server listening on port " << actual_port << std::endl;
    }

    void run() {
        epoll_event events[MAX_EVENTS];
        while (!shutdown_requested) {
            int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
            if (nfds == -1) {
                if (errno == EINTR) continue;
                perror("epoll_wait"); break;
            }
            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;
                uint32_t ev = events[i].events;

                if (fd == server_fd) {
                    accept_connection();
                    continue;
                }

                if (ev & (EPOLLERR | EPOLLHUP)) {
                    disconnect_client(fd, /*silent=*/false);
                    continue;
                }

                if (ev & EPOLLIN) handle_client_read(fd);
                if (ev & EPOLLOUT) flush_client(fd);
            }
        }
        std::cout << "Server loop ended, cleaning up..." << std::endl;
        cleanup();
    }

private:
    // ---------- EPOLL helpers ----------
    void mod_epoll_events(int fd, bool want_out) {
        epoll_event ev;
        ev.data.fd = fd;
        ev.events = EPOLLIN | EPOLLET;
        if (want_out) ev.events |= EPOLLOUT;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }

    void add_epoll_client(int fd) {
        epoll_event ev;
        ev.data.fd = fd;
        ev.events = EPOLLIN | EPOLLET;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }

    // ---------- Accept ----------
    void accept_connection() {
        for (;;) {
            sockaddr_in ca; socklen_t cl = sizeof(ca);
            int cfd = accept(server_fd, (sockaddr*)&ca, &cl);
            if (cfd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                perror("accept"); break;
            }

            int flags = fcntl(cfd, F_GETFL, 0);
            fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

            add_epoll_client(cfd);

            clients[cfd] = Client(cfd);
            clients[cfd].hostname = inet_ntoa(ca.sin_addr);

            std::cout << "New connection from " << clients[cfd].hostname << std::endl;
        }
    }

    // ---------- Read (ET: drain) ----------
    void handle_client_read(int fd) {
        std::map<int, Client>::iterator it = clients.find(fd);
        if (it == clients.end()) return;
        Client& c = it->second;

        char buf[BUFFER_SIZE];
        for (;;) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                c.inbuf.append(buf, buf + n);
                for (;;) {
                    size_t nl = c.inbuf.find('\n');
                    if (nl == std::string::npos) break;
                    std::string line = c.inbuf.substr(0, nl);
                    if (!line.empty() && line[line.size()-1] == '\r') line.erase(line.size()-1);
                    c.inbuf.erase(0, nl + 1);
                    process_message(fd, line);
                    if (clients.find(fd) == clients.end()) return; // client disparu
                }
            } else if (n == 0) {
                disconnect_client(fd, /*silent=*/false);
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break; // drain fini
                disconnect_client(fd, /*silent=*/false);
                return;
            }
        }
        if (c.inbuf.size() > (1u<<20)) c.inbuf.clear(); // garde-fou
    }

    // ---------- Write queue ----------
    void queue_send(int fd, const std::string& data) {
        std::map<int, Client>::iterator it = clients.find(fd);
        if (it == clients.end()) return;
        Client& c = it->second;

        if (c.outbuf.empty()) {
            ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
            if (n == (ssize_t)data.size()) return; // tout envoyé
            if (n >= 0 && n < (ssize_t)data.size()) {
                c.outbuf.append(data.data() + n, data.size() - n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                c.outbuf.append(data);
            } else {
                // erreur fatale (EPIPE ignoré par SIGPIPE/IGN)
                disconnect_client(fd, /*silent=*/false);
                return;
            }
            mod_epoll_events(fd, true); // besoin d'EPOLLOUT
        } else {
            c.outbuf.append(data);
            mod_epoll_events(fd, true);
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
                mod_epoll_events(fd, true);
                return;
            } else {
                disconnect_client(fd, /*silent=*/false);
                return;
            }
        }

        mod_epoll_events(fd, false);      // plus rien à envoyer
        if (c.closing_after_flush) {
            disconnect_client(fd, /*silent=*/true);
        }
    }

    // ---------- Disconnect ----------
    // silent=true => enlève des salons sans PART (utilisé pour QUIT)
    void disconnect_client(int fd, bool silent) {
        std::map<int, Client>::iterator it = clients.find(fd);
        if (it == clients.end()) return;
        Client client = it->second; // copie (on va erase)

        // retrait de tous les salons
        std::set<std::string> chans = client.channels;
        for (std::set<std::string>::iterator ch = chans.begin(); ch != chans.end(); ++ch) {
            remove_from_channel_silent(client.nick, *ch, silent);
        }

        if (!client.nick.empty()) nick_to_fd.erase(client.nick);

        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        clients.erase(it);

        std::cout << "Client " << client.hostname << " disconnected\n";
    }

    void remove_from_channel_silent(const std::string& nick, const std::string& chan, bool silent) {
        (void)silent; // silencieux par conception ici
        std::map<std::string, Channel>::iterator it = channels.find(chan);
        if (it == channels.end()) return;
        Channel& c = it->second;

        if (c.members.erase(nick)) {
            c.operators.erase(nick);
            if (c.members.empty()) channels.erase(chan);
        }
    }

    // ---------- Message parsing ----------
    void process_message(int fd, const std::string& line) {
        if (line.empty()) return;
        std::istringstream iss(line);
        std::string command; iss >> command;
        std::transform(command.begin(), command.end(), command.begin(), ::toupper);

        std::vector<std::string> params;
        std::string rest; std::getline(iss, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0,1);

        if (!rest.empty()) {
            if (rest[0] == ':') {
                params.push_back(rest.substr(1));
            } else {
                std::istringstream ps(rest);
                std::string p;
                while (ps >> p) {
                    if (!p.empty() && p[0] == ':') {
                        std::string trailing; std::getline(ps, trailing);
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
            if (params.empty()) { send_reply(fd, "461", "PASS :Not enough parameters"); return; }
            if (params[0] == password) client.authenticated = true;
            else { send_reply(fd, "464", ":Password incorrect"); disconnect_client(fd, false); }
        }
        else if (cmd == "NICK") {
            if (params.empty()) { send_reply(fd, "431", ":No nickname given"); return; }
            handle_nick(fd, params[0]);
        }
        else if (cmd == "USER") {
            if (params.size() < 4) { send_reply(fd, "461", "USER :Not enough parameters"); return; }
            client.user  = params[0];
            client.real  = params[3];
            check_registration(fd);
        }
        else if (cmd == "JOIN") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.empty()) { send_reply(fd, "461", "JOIN :Not enough parameters"); return; }
            std::vector<std::string> chans, keys;
            split_csv(params[0], chans);
            if (params.size() > 1) split_csv(params[1], keys);
            for (size_t i = 0; i < chans.size(); ++i) {
                std::string k = (i < keys.size()) ? keys[i] : "";
                handle_join(fd, chans[i], k);
            }
        }
        else if (cmd == "PART") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.empty()) { send_reply(fd, "461", "PART :Not enough parameters"); return; }
            std::string reason = (params.size() > 1) ? params[1] : "";
            handle_part(fd, params[0], reason);
        }
        else if (cmd == "PRIVMSG") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.size() < 2) { send_reply(fd, "461", "PRIVMSG :Not enough parameters"); return; }
            handle_privmsg(fd, params[0], params[1]);
        }
        else if (cmd == "QUIT") {
            std::string reason = params.empty() ? "Client Quit" : params[0];
            handle_quit(fd, reason);
        }
        else if (cmd == "PING") {
            if (!params.empty()) send_raw(fd, "PONG :" + params[0]);
            else send_raw(fd, "PONG :IRCServer");
        }
        else if (cmd == "KICK") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.size() < 2) { send_reply(fd, "461", "KICK :Not enough parameters"); return; }
            handle_kick(fd, params[0], params[1], (params.size() > 2) ? params[2] : client.nick);
        }
        else if (cmd == "INVITE") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.size() < 2) { send_reply(fd, "461", "INVITE :Not enough parameters"); return; }
            handle_invite(fd, params[0], params[1]);
        }
        else if (cmd == "TOPIC") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.empty()) { send_reply(fd, "461", "TOPIC :Not enough parameters"); return; }
            handle_topic(fd, params[0], (params.size() > 1) ? params[1] : "");
        }
        else if (cmd == "MODE") {
            if (!client.registered) { send_reply(fd, "451", ":You have not registered"); return; }
            if (params.empty()) { send_reply(fd, "461", "MODE :Not enough parameters"); return; }
            handle_mode(fd, params);
        }
    }

    // ---------- Helpers ----------
    static void split_csv(const std::string& s, std::vector<std::string>& out) {
        size_t start = 0, end;
        while ((end = s.find(',', start)) != std::string::npos) {
            out.push_back(s.substr(start, end - start));
            start = end + 1;
        }
        if (start <= s.size()) out.push_back(s.substr(start));
    }

    void handle_nick(int fd, const std::string& nick) {
        Client& client = clients[fd];

        if (nick.length() > MAX_NICK_LEN) { send_reply(fd, "432", nick + " :Erroneous nickname"); return; }
        if (nick_to_fd.find(nick) != nick_to_fd.end()) { send_reply(fd, "433", nick + " :Nickname is already in use"); return; }

        std::string old = client.nick;
        bool was_reg = client.registered;

        if (!client.nick.empty()) nick_to_fd.erase(client.nick);
        client.nick = nick;
        nick_to_fd[nick] = fd;

        if (was_reg && !old.empty()) handle_nick_change(fd, old, nick);
        check_registration(fd);
    }

    void handle_nick_change(int fd, const std::string& old_nick, const std::string& new_nick) {
        Client& client = clients[fd];
        std::set<int> notified;

        std::string nick_msg = ":" + old_nick + "!" + client.user + "@" + client.hostname + " NICK :" + new_nick;
        send_raw(fd, nick_msg); notified.insert(fd);

        for (std::set<std::string>::iterator ch = client.channels.begin(); ch != client.channels.end(); ++ch) {
            std::map<std::string, Channel>::iterator it = channels.find(*ch);
            if (it == channels.end()) continue;
            Channel& c = it->second;

            c.members.erase(old_nick); c.members.insert(new_nick);
            if (c.operators.erase(old_nick)) c.operators.insert(new_nick);

            for (std::set<std::string>::iterator m = c.members.begin(); m != c.members.end(); ++m) {
                std::map<std::string,int>::iterator ni = nick_to_fd.find(*m);
                if (ni != nick_to_fd.end() && !notified.count(ni->second)) {
                    send_raw(ni->second, nick_msg);
                    notified.insert(ni->second);
                }
            }
        }
    }

    void check_registration(int fd) {
        Client& c = clients[fd];
        if (!c.authenticated) { send_reply(fd, "464", ":Password incorrect"); return; }

        if (!c.nick.empty() && !c.user.empty() && !c.registered) {
            c.registered = true;
            send_reply(fd, "001", ":Welcome to the IRC Network " + c.nick);
            send_reply(fd, "002", ":Your host is IRCServer");
            send_reply(fd, "003", ":This server was created recently");
            send_reply(fd, "004", "IRCServer 1.0 o o");
        }
    }

    void handle_join(int fd, const std::string& channel_name, const std::string& key = "") {
        if (channel_name.empty() || channel_name[0] != '#') { send_reply(fd, "403", channel_name + " :No such channel"); return; }
        Client& client = clients[fd];

        std::map<std::string, Channel>::iterator it = channels.find(channel_name);
        bool new_channel = false;
        if (it == channels.end()) {
            it = channels.insert(std::make_pair(channel_name, Channel(channel_name))).first;
            new_channel = true;
        }
        Channel& ch = it->second;

        if (ch.members.count(client.nick)) return;

        if (!ch.key.empty() && ch.key != key) { send_reply(fd, "475", channel_name + " :Cannot join channel (+k)"); return; }
        if (ch.invite_only && !client.invited_channels.count(channel_name)) {
            send_reply(fd, "473", channel_name + " :Cannot join channel (+i)"); return;
        }
        if (ch.user_limit > 0 && (int)ch.members.size() >= ch.user_limit) {
            send_reply(fd, "471", channel_name + " :Cannot join channel (+l)"); return;
        }
        client.invited_channels.erase(channel_name);

        if (new_channel) ch.operators.insert(client.nick);

        ch.members.insert(client.nick);
        client.channels.insert(channel_name);

        std::string join_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " JOIN " + channel_name;
        send_to_channel(channel_name, join_msg);

        if (!ch.topic.empty()) send_reply(fd, "332", channel_name + " :" + ch.topic);
        else send_reply(fd, "331", channel_name + " :No topic is set");

        send_names(fd, channel_name);
    }

    void handle_part(int fd, const std::string& channel_name, const std::string& reason) {
        Client& client = clients[fd];
        std::map<std::string, Channel>::iterator it = channels.find(channel_name);
        if (it == channels.end()) { send_reply(fd, "403", channel_name + " :No such channel"); return; }

        Channel& ch = it->second;
        if (!ch.members.count(client.nick)) { send_reply(fd, "442", channel_name + " :You're not on that channel"); return; }

        std::string part_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " PART " + channel_name;
        if (!reason.empty()) part_msg += " :" + reason;
        send_to_channel(channel_name, part_msg);

        ch.members.erase(client.nick);
        ch.operators.erase(client.nick);
        client.channels.erase(channel_name);
        if (ch.members.empty()) channels.erase(channel_name);
    }

    void handle_privmsg(int fd, const std::string& target, const std::string& message) {
        Client& client = clients[fd];
        if (!target.empty() && target[0] == '#') {
            std::map<std::string, Channel>::iterator it = channels.find(target);
            if (it == channels.end()) { send_reply(fd, "403", target + " :No such channel"); return; }
            Channel& ch = it->second;
            if (!ch.members.count(client.nick)) { send_reply(fd, "442", target + " :You're not on that channel"); return; }
            std::string msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " PRIVMSG " + target + " :" + message;
            send_to_channel_except(target, msg, fd);
        } else {
            std::map<std::string,int>::iterator it = nick_to_fd.find(target);
            if (it == nick_to_fd.end()) { send_reply(fd, "401", target + " :No such nick/channel"); return; }
            std::string msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " PRIVMSG " + target + " :" + message;
            send_raw(it->second, msg);
        }
    }

    void handle_quit(int fd, const std::string& reason) {
        Client& client = clients[fd];
        std::string quit_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname + " QUIT :" + reason;

        std::set<int> notified;
        for (std::set<std::string>::iterator ch = client.channels.begin(); ch != client.channels.end(); ++ch) {
            std::map<std::string, Channel>::iterator it = channels.find(*ch);
            if (it == channels.end()) continue;
            Channel& c = it->second;
            for (std::set<std::string>::iterator m = c.members.begin(); m != c.members.end(); ++m) {
                std::map<std::string,int>::iterator ni = nick_to_fd.find(*m);
                if (ni != nick_to_fd.end() && ni->second != fd && !notified.count(ni->second)) {
                    send_raw(ni->second, quit_msg);
                    notified.insert(ni->second);
                }
            }
        }
        // suppression silencieuse des salons (pas de PART)
        disconnect_client(fd, /*silent=*/true);
    }

    void handle_kick(int fd, const std::string& channel_name, const std::string& target_nick, const std::string& reason) {
        Client& client = clients[fd];
        std::map<std::string, Channel>::iterator it = channels.find(channel_name);
        if (it == channels.end()) { send_reply(fd, "403", channel_name + " :No such channel"); return; }
        Channel& ch = it->second;

        if (!ch.members.count(client.nick)) { send_reply(fd, "442", channel_name + " :You're not on that channel"); return; }
        if (!ch.operators.count(client.nick)) { send_reply(fd, "482", channel_name + " :You're not channel operator"); return; }
        if (!ch.members.count(target_nick)) { send_reply(fd, "441", target_nick + " " + channel_name + " :They aren't on that channel"); return; }

        std::string kick_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname +
                               " KICK " + channel_name + " " + target_nick + " :" + reason;
        send_to_channel(channel_name, kick_msg);

        ch.members.erase(target_nick);
        ch.operators.erase(target_nick);

        std::map<std::string,int>::iterator ti = nick_to_fd.find(target_nick);
        if (ti != nick_to_fd.end()) clients[ti->second].channels.erase(channel_name);

        if (ch.members.empty()) channels.erase(channel_name);
    }

    void handle_invite(int fd, const std::string& target_nick, const std::string& channel_name) {
        Client& client = clients[fd];
        std::map<std::string, Channel>::iterator it = channels.find(channel_name);
        if (it == channels.end()) { send_reply(fd, "403", channel_name + " :No such channel"); return; }
        Channel& ch = it->second;

        if (!ch.members.count(client.nick)) { send_reply(fd, "442", channel_name + " :You're not on that channel"); return; }
        if (!ch.operators.count(client.nick)) { send_reply(fd, "482", channel_name + " :You're not channel operator"); return; }

        std::map<std::string,int>::iterator ti = nick_to_fd.find(target_nick);
        if (ti == nick_to_fd.end()) { send_reply(fd, "401", target_nick + " :No such nick/channel"); return; }
        if (ch.members.count(target_nick)) { send_reply(fd, "443", target_nick + " " + channel_name + " :is already on channel"); return; }

        clients[ti->second].invited_channels.insert(channel_name);

        std::string invite_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname +
                                 " INVITE " + target_nick + " " + channel_name;
        send_raw(ti->second, invite_msg);
        send_reply(fd, "341", target_nick + " " + channel_name);
    }

    void handle_topic(int fd, const std::string& channel_name, const std::string& new_topic) {
        Client& client = clients[fd];
        std::map<std::string, Channel>::iterator it = channels.find(channel_name);
        if (it == channels.end()) { send_reply(fd, "403", channel_name + " :No such channel"); return; }
        Channel& ch = it->second;

        if (!ch.members.count(client.nick)) { send_reply(fd, "442", channel_name + " :You're not on that channel"); return; }

        if (new_topic.empty()) {
            if (ch.topic.empty()) send_reply(fd, "331", channel_name + " :No topic is set");
            else send_reply(fd, "332", channel_name + " :" + ch.topic);
        } else {
            if (ch.topic_protected && !ch.operators.count(client.nick)) {
                send_reply(fd, "482", channel_name + " :You're not channel operator"); return;
            }
            ch.topic = new_topic;
            std::string topic_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname +
                                    " TOPIC " + channel_name + " :" + new_topic;
            send_to_channel(channel_name, topic_msg);
        }
    }

    void handle_mode(int fd, const std::vector<std::string>& params) {
        Client& client = clients[fd];
        std::string target = params[0];
        if (target.empty() || target[0] != '#') return;

        std::map<std::string, Channel>::iterator it = channels.find(target);
        if (it == channels.end()) { send_reply(fd, "403", target + " :No such channel"); return; }
        Channel& ch = it->second;

        if (!ch.members.count(client.nick)) { send_reply(fd, "442", target + " :You're not on that channel"); return; }

        if (params.size() == 1) {
            std::string modes = "+";
            if (ch.invite_only) modes += "i";
            if (ch.topic_protected) modes += "t";
            if (!ch.key.empty()) modes += "k";
            if (ch.user_limit > 0) modes += "l";
            send_reply(fd, "324", target + " " + modes);
            return;
        }

        if (!ch.operators.count(client.nick)) { send_reply(fd, "482", target + " :You're not channel operator"); return; }

        std::string mode_string = params[1];
        bool adding = true;
        std::string changes, change_params;
        int idx = 2;

        for (size_t i = 0; i < mode_string.size(); ++i) {
            char m = mode_string[i];
            if (m == '+') { adding = true; continue; }
            if (m == '-') { adding = false; continue; }

            bool changed = false; std::string p;

            switch (m) {
                case 'i': if (ch.invite_only != adding) { ch.invite_only = adding; changed = true; } break;
                case 't': if (ch.topic_protected != adding) { ch.topic_protected = adding; changed = true; } break;
                case 'k':
                    if (adding) {
                        if (idx < (int)params.size()) { ch.key = params[idx]; p = params[idx]; ++idx; changed = true; }
                    } else {
                        if (!ch.key.empty()) { ch.key.clear(); changed = true; }
                    }
                    break;
                case 'l':
                    if (adding) {
                        if (idx < (int)params.size()) {
                            int lim = atoi(params[idx].c_str());
                            if (lim > 0) { ch.user_limit = lim; p = params[idx]; ++idx; changed = true; }
                        }
                    } else {
                        if (ch.user_limit > 0) { ch.user_limit = 0; changed = true; }
                    }
                    break;
                case 'o':
                    if (idx < (int)params.size()) {
                        p = params[idx++]; // target nick
                        if (ch.members.count(p)) {
                            if (adding) { if (!ch.operators.count(p)) { ch.operators.insert(p); changed = true; } }
                            else { if (ch.operators.erase(p)) { changed = true; } }
                        }
                    }
                    break;
                default:
                    send_reply(fd, "472", std::string(1, m) + " :is unknown mode char to me");
                    break;
            }

            if (changed) {
                changes += (adding ? "+" : "-"); changes += m;
                if (!p.empty()) { if (!change_params.empty()) change_params += " "; change_params += p; }
            }
        }

        if (!changes.empty()) {
            std::string mode_msg = ":" + client.nick + "!" + client.user + "@" + client.hostname +
                                   " MODE " + target + " " + changes;
            if (!change_params.empty()) mode_msg += " " + change_params;
            send_to_channel(target, mode_msg);
        }
    }

    // ---------- Replies / Broadcast ----------
    void send_names(int fd, const std::string& channel_name) {
        std::map<std::string, Channel>::iterator it = channels.find(channel_name);
        if (it == channels.end()) return;
        Channel& ch = it->second;

        std::string names;
        for (std::set<std::string>::iterator m = ch.members.begin(); m != ch.members.end(); ++m) {
            if (!names.empty()) names += " ";
            if (ch.operators.count(*m)) names += "@";
            names += *m;
        }
        send_reply(fd, "353", "= " + channel_name + " :" + names);
        send_reply(fd, "366", channel_name + " :End of /NAMES list");
    }

    void send_to_channel(const std::string& channel_name, const std::string& message) {
        std::map<std::string, Channel>::iterator it = channels.find(channel_name);
        if (it == channels.end()) return;
        Channel& ch = it->second;
        std::string line = message + "\r\n";
        for (std::set<std::string>::iterator m = ch.members.begin(); m != ch.members.end(); ++m) {
            std::map<std::string,int>::iterator ni = nick_to_fd.find(*m);
            if (ni != nick_to_fd.end()) queue_send(ni->second, line);
        }
    }

    void send_to_channel_except(const std::string& channel_name, const std::string& message, int except_fd) {
        std::map<std::string, Channel>::iterator it = channels.find(channel_name);
        if (it == channels.end()) return;
        Channel& ch = it->second;
        std::string line = message + "\r\n";
        for (std::set<std::string>::iterator m = ch.members.begin(); m != ch.members.end(); ++m) {
            std::map<std::string,int>::iterator ni = nick_to_fd.find(*m);
            if (ni != nick_to_fd.end() && ni->second != except_fd) queue_send(ni->second, line);
        }
    }

    void send_reply(int fd, const std::string& code, const std::string& message) {
        Client& c = clients[fd];
        std::string nick = c.nick.empty() ? "*" : c.nick;
        std::string line = ":IRCServer " + code + " " + nick + " " + message + "\r\n";
        queue_send(fd, line);
    }

    void send_raw(int fd, const std::string& message) {
        std::string line = message + "\r\n";
        queue_send(fd, line);
    }
};

// Static
bool IRCServer::shutdown_requested = false;

// -------------------- main --------------------
int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <port> <password>\n";
        return 1;
    }
    signal(SIGINT,  IRCServer::signal_handler);
    signal(SIGTERM, IRCServer::signal_handler);
    signal(SIGPIPE, SIG_IGN);

    try {
        IRCServer s(argv[1], argv[2]);
        s.run();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
