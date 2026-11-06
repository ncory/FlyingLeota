// Just enough networking for the simulator: a UDP socket for SCMP and a tiny HTTP/1.1
// server for the viewer page. POSIX sockets, single-threaded, non-blocking.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <poll.h>

namespace leota_sim {

struct Address {
    sockaddr_in addr{};
    bool set = false;
    std::string text() const;
    bool operator==(const Address& o) const;
};

class UdpSocket {
public:
    ~UdpSocket();
    bool open(const std::string& host, int port, std::string* error);
    int port() const { return port_; }
    int fd() const { return fd_; }
    // Reads one datagram if there is one.
    bool receive(std::string& data, Address& from);
    void send(const std::string& data, const Address& to);

private:
    int fd_ = -1, port_ = 0;
};

struct HttpRequest {
    std::string method, path, body;
};
struct HttpResponse {
    int status = 200;
    std::string type = "text/plain; charset=utf-8", body;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    ~HttpServer();
    bool open(const std::string& host, int port, std::string* error);
    int port() const { return port_; }
    // Adds this server's sockets to a poll set, and handles whatever is ready.
    void add_to(std::vector<pollfd>& fds) const;
    void serve(const std::vector<pollfd>& fds, const Handler& handler);

private:
    struct Conn { std::string in; };
    void finish(int fd, Conn& c, const Handler& handler);
    int fd_ = -1, port_ = 0;
    std::map<int, Conn> conns_;
};

}  // namespace leota_sim
