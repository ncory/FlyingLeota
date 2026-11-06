#include "net.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace leota_sim {

namespace {

bool make_address(const std::string& host, int port, sockaddr_in& a) {
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    return inet_pton(AF_INET, host.c_str(), &a.sin_addr) == 1;
}

void non_blocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

int bound_port(int fd) {
    sockaddr_in a{};
    socklen_t len = sizeof a;
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    return ntohs(a.sin_port);
}

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
    }
    return "Error";
}

}  // namespace

std::string Address::text() const {
    char buf[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

bool Address::operator==(const Address& o) const {
    return set == o.set && addr.sin_addr.s_addr == o.addr.sin_addr.s_addr &&
           addr.sin_port == o.addr.sin_port;
}

UdpSocket::~UdpSocket() {
    if (fd_ >= 0) close(fd_);
}

bool UdpSocket::open(const std::string& host, int port, std::string* error) {
    sockaddr_in a;
    if (!make_address(host, port, a)) {
        *error = "not an IPv4 address: " + host;
        return false;
    }
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        *error = std::string("can't use UDP port ") + std::to_string(port) + ": " + std::strerror(errno);
        return false;
    }
    non_blocking(fd_);
    port_ = bound_port(fd_);
    return true;
}

bool UdpSocket::receive(std::string& data, Address& from) {
    char buf[65536];
    socklen_t len = sizeof from.addr;
    const ssize_t n = recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from.addr), &len);
    if (n < 0) return false;
    from.set = true;
    data.assign(buf, static_cast<size_t>(n));
    return true;
}

void UdpSocket::send(const std::string& data, const Address& to) {
    if (!to.set) return;
    sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&to.addr), sizeof to.addr);
}

HttpServer::~HttpServer() {
    for (auto& [fd, c] : conns_) close(fd);
    if (fd_ >= 0) close(fd_);
}

bool HttpServer::open(const std::string& host, int port, std::string* error) {
    sockaddr_in a;
    if (!make_address(host, port, a)) {
        *error = "not an IPv4 address: " + host;
        return false;
    }
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    const int yes = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd_, 16) != 0) {
        *error = std::string("can't use TCP port ") + std::to_string(port) + ": " + std::strerror(errno);
        return false;
    }
    non_blocking(fd_);
    port_ = bound_port(fd_);
    return true;
}

void HttpServer::add_to(std::vector<pollfd>& fds) const {
    if (fd_ < 0) return;
    fds.push_back({fd_, POLLIN, 0});
    for (const auto& [fd, c] : conns_) fds.push_back({fd, POLLIN, 0});
}

void HttpServer::serve(const std::vector<pollfd>& fds, const Handler& handler) {
    for (const pollfd& p : fds) {
        if (!(p.revents & (POLLIN | POLLHUP | POLLERR))) continue;
        if (p.fd == fd_) {
            int c;
            while ((c = accept(fd_, nullptr, nullptr)) >= 0) {
                non_blocking(c);
                conns_[c] = {};
            }
            continue;
        }
        auto it = conns_.find(p.fd);
        if (it == conns_.end()) continue;
        char buf[8192];
        const ssize_t n = read(p.fd, buf, sizeof buf);
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(p.fd);
                conns_.erase(it);
            }
            continue;
        }
        it->second.in.append(buf, static_cast<size_t>(n));
        finish(p.fd, it->second, handler);
    }
}

void HttpServer::finish(int fd, Conn& c, const Handler& handler) {
    const size_t end = c.in.find("\r\n\r\n");
    HttpResponse res;
    HttpRequest req;
    bool ready = false;
    if (c.in.size() > 1 << 20) {
        res.status = 413;
        ready = true;
    } else if (end != std::string::npos) {
        const std::string head = c.in.substr(0, end);
        size_t length = 0;
        for (size_t at = 0; at < head.size();) {
            size_t eol = head.find("\r\n", at);
            if (eol == std::string::npos) eol = head.size();
            std::string line = head.substr(at, eol - at);
            for (char& ch : line) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (line.rfind("content-length:", 0) == 0) length = std::strtoul(line.c_str() + 15, nullptr, 10);
            at = eol + 2;
        }
        if (c.in.size() < end + 4 + length) return;  // the body isn't all here yet
        const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
        req.method = head.substr(0, sp1);
        req.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
        req.body = c.in.substr(end + 4, length);
        res = handler(req);
        ready = true;
    }
    if (!ready) return;
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) +
                      "\r\nContent-Type: " + res.type + "\r\nContent-Length: " +
                      std::to_string(res.body.size()) +
                      "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + res.body;
    for (size_t sent = 0; sent < out.size();) {  // small replies: a blocking loop is fine
        const ssize_t n = write(fd, out.data() + sent, out.size() - sent);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd w{fd, POLLOUT, 0};
                poll(&w, 1, 100);
                continue;
            }
            break;
        }
        sent += static_cast<size_t>(n);
    }
    close(fd);
    conns_.erase(fd);
}

}  // namespace leota_sim
