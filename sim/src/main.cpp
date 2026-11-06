// leota-sim: the Flying Leota controller, simulated on a PC.
//
// Speaks SCMP on UDP (default port 7720) exactly as the real controller will, so Show
// Controller can connect to it as an `scmp` device. Serves a viewer on HTTP (default
// port 8090): the rig in 3-D, the controller's own screen controls, and buttons that
// make things go wrong.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app.hpp"
#include "net.hpp"

using namespace leota_sim;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void usage() {
    std::puts(
        "leota-sim: the Flying Leota controller, simulated.\n"
        "\n"
        "  --host ADDR        address to listen on (default 0.0.0.0)\n"
        "  --port N           SCMP UDP port (default 7720; 0 picks one)\n"
        "  --http N           viewer HTTP port (default 8090; 0 picks one; -1 for none)\n"
        "  --two-axis         fly in a fixed depth plane\n"
        "  --wind M/S         starting wind speed (default 2)\n"
        "  --gusty 0..1       how much the wind varies (default 0.3)\n"
        "  --no-push          the wind doesn't push the ball\n"
        "  --pulley-error MM  pulley A is really this far off in x and y (default 0)\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") opt.host = next();
        else if (a == "--port") opt.scmp_port = std::atoi(next());
        else if (a == "--http") opt.http_port = std::atoi(next());
        else if (a == "--two-axis") opt.two_axis = true;
        else if (a == "--wind") opt.wind = std::atof(next());
        else if (a == "--gusty") opt.gusty = std::atof(next());
        else if (a == "--no-push") opt.push = false;
        else if (a == "--pulley-error") opt.pulley_error = std::atof(next());
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    std::string error;
    UdpSocket udp;
    if (!udp.open(opt.host, opt.scmp_port, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    opt.scmp_port = udp.port();
    HttpServer http;
    if (opt.http_port >= 0 && !http.open(opt.host, opt.http_port, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    SimApp app(opt);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("SCMP on UDP port %d\n", udp.port());
    if (opt.http_port >= 0) std::printf("Viewer on http://localhost:%d/\n", http.port());
    std::printf("ready\n");
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    auto clock = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() + 1.0;
    };
    std::vector<pollfd> fds;
    while (!g_stop) {
        fds.clear();
        fds.push_back({udp.fd(), POLLIN, 0});
        http.add_to(fds);
        poll(fds.data(), fds.size(), 1);  // wakes at least every millisecond
        double now = clock();
        std::string msg;
        Address from;
        while (udp.receive(msg, from)) {
            const std::string reply = app.datagram(msg, from, now);
            if (!reply.empty()) udp.send(reply, from);
        }
        http.serve(fds, [&](const HttpRequest& req) { return app.http(req, now); });
        now = clock();
        const std::string status = app.step(now);
        if (!status.empty()) udp.send(status, app.status_to());
    }
    std::puts("stopped");
    return 0;
}
