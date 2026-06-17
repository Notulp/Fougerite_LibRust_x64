#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <fcntl.h>
#include <string>
#include <functional>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <mutex>
#include "utilities.h"

namespace rconpp {

struct connected_client {
    sockaddr_in sock_info{};
    SOCKET_TYPE socket{0};

    bool connected{false};
    bool authenticated{false};
    int32_t auth_id{0};
    uint8_t authentication_attempts{0};

    time_t last_heartbeat{0};
};

struct client_command {
    connected_client client;
    std::string command{};
};

class RCONPP_EXPORT rcon_server {
    std::string address{};
    int port{0};
    std::string password{};

    SOCKET_TYPE sock{INVALID_SOCKET};

    std::thread accept_connections_runner;

    std::mutex connected_clients_mutex;
    std::mutex request_handlers_mutex;
    
    std::unordered_map<std::string, time_t> blocked_ips{};
    std::mutex blocked_ips_mutex;

public:
    bool online{false};

    std::function<std::string(const client_command& command)> on_command = [](const client_command&){ return ""; };

    std::function<void(const std::string_view log)> on_log = [](const std::string_view){};

    std::condition_variable terminating;

    std::unordered_map<SOCKET_TYPE, connected_client> connected_clients{};

    std::unordered_map<SOCKET_TYPE, std::thread> request_handlers{};

    rcon_server(std::string_view addr, int _port, std::string_view pass);

    ~rcon_server();

    void start(bool return_after);
    void broadcast_log(const std::string& log);
    void disconnect_client(SOCKET_TYPE client_socket, bool remove_after = true);

private:
    bool startup_server();
    void read_packet(connected_client& client);
    bool send_heartbeat(connected_client& client);
    void client_process_loop(connected_client& client);

    void add_client(const SOCKET_TYPE client_socket, connected_client& client) {
       while (!connected_clients_mutex.try_lock()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
       }

       connected_clients.insert({ client_socket, client });

       connected_clients_mutex.unlock();
    }

    void remove_client(const SOCKET_TYPE client_socket) {
       while (!connected_clients_mutex.try_lock()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
       }

       connected_clients.erase(client_socket);

       connected_clients_mutex.unlock();
    };
};

} // namespace rconpp