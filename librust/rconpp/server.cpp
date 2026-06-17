#include <mutex>
#include <csignal>
#include <algorithm>
#include "server.h"
#include "utilities.h"

rconpp::rcon_server::rcon_server(const std::string_view addr, const int _port, const std::string_view pass) : address(addr), port(_port), password(pass) {
}

rconpp::rcon_server::~rcon_server() {
    if (on_log) {
       on_log("RCON server is shutting down.");
    }

    online = false;
    terminating.notify_all();

    for(const auto& client : connected_clients) {
       disconnect_client(client.first, false);
    }

#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
    if (accept_connections_runner.joinable()) {
       accept_connections_runner.join();
    }
}

bool rconpp::rcon_server::startup_server() {
#ifdef _WIN32
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
       on_log("WSAStartup failed. Error: " + std::to_string(result));
       return false;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

#ifdef _WIN32
    if (sock == INVALID_SOCKET) {
#else
    if (sock == -1) {
#endif
       const last_error err = get_last_error();
       on_log("Failed to open socket [Error code: " + std::to_string(err.error_code) + "]!");
       return false;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(port);

    int allow = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&allow), sizeof(allow));

    int status = bind(sock, reinterpret_cast<const sockaddr*>(&server), sizeof(server));
    if (status == -1) return false;

    status = listen(sock, SOMAXCONN);
    if (status == -1) return false;

    return true;
}

void rconpp::rcon_server::disconnect_client(const SOCKET_TYPE client_socket, const bool remove_after /*= true*/) {
#ifdef _WIN32
    closesocket(client_socket);
#else
    close(client_socket);
#endif

    if (connected_clients.find(client_socket) == connected_clients.end()) {
       on_log("Client [Socket: " + std::to_string(client_socket) + "] does not appear to be a connected client.");
       return;
    }

    connected_client& client = connected_clients.at(client_socket);
    client.connected = false;
    client.authenticated = false;
    
    on_log("Client [" + std::string(inet_ntoa(client.sock_info.sin_addr)) + ":" + std::to_string(ntohs(client.sock_info.sin_port)) + " | Socket: " + std::to_string(client_socket) + "] has been disconnected from the server.");

    if (remove_after) {
       {
          while (!request_handlers_mutex.try_lock()) {
             std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          request_handlers.erase(client_socket);
          request_handlers_mutex.unlock();
       }
       remove_client(client_socket);
    }
}

void rconpp::rcon_server::broadcast_log(const std::string& log) {
    if (log.empty() || !online) return;
    const size_t MAX_BODY_LEN = 4000;

    std::lock_guard<std::mutex> lock(connected_clients_mutex);
    for (auto& pair : connected_clients) {
        auto& client = pair.second;
        if (client.authenticated) {
            size_t offset = 0;
            while (offset < log.length()) {
                size_t chunkLen = (std::min)((size_t)MAX_BODY_LEN, log.length() - offset);
                std::string chunk = log.substr(offset, chunkLen);

                packet p1 = form_packet(chunk, 0, SERVERDATA_CONSOLE_LOG);
                packet p2 = form_packet(chunk, client.auth_id, SERVERDATA_RESPONSE_VALUE);
                
                std::vector<char> combined;
                combined.reserve(p1.length + p2.length);
                combined.insert(combined.end(), p1.data.begin(), p1.data.end());
                combined.insert(combined.end(), p2.data.begin(), p2.data.end());
                
                send(client.socket, combined.data(), combined.size(), MSG_NOSIGNAL);
                offset += chunkLen;
            }
        }
    }
}

void rconpp::rcon_server::read_packet(connected_client& client) {
    const int packet_size = read_packet_size(client.socket);

    if (packet_size == -1) {
       const last_error err = get_last_error();
       on_log("Failed to read packet size from Client [" + std::string(inet_ntoa(client.sock_info.sin_addr)) + ":" + std::to_string(ntohs(client.sock_info.sin_port)) + " | Error code: " + std::to_string(err.error_code) + "]!");
       if (err.type_of_error == DISCONNECTED) client.last_heartbeat = 0;
       return;
    }

    if (packet_size < MIN_PACKET_SIZE) {
       on_log("Packet size too small: " + std::to_string(packet_size));
       return;
    }

    std::vector<char> buffer{};
    buffer.resize(packet_size);

    if (recv(client.socket, buffer.data(), packet_size, MSG_NOSIGNAL) == -1) {
       const last_error err = get_last_error();
       on_log("Failed to receive the full packet from Client [" + std::string(inet_ntoa(client.sock_info.sin_addr)) + ":" + std::to_string(ntohs(client.sock_info.sin_port)) + " | Error code: " + std::to_string(err.error_code) + "]!");
       if (err.type_of_error == DISCONNECTED) client.last_heartbeat = 0;
       return;
    }

    client.last_heartbeat = time(nullptr);

    std::string packet_data(&buffer[8], &buffer[buffer.size()-2]);
    int id = bit32_to_int(buffer);
    int type = type_to_int(buffer);

    packet packet_to_send{};

    if (!client.authenticated) {
       on_log("Client not authenticated, handling authentication.");
       
       if (packet_data == password) {
          packet p1 = form_packet("", id, SERVERDATA_RESPONSE_VALUE);
          packet p2 = form_packet("", id, SERVERDATA_AUTH_RESPONSE);
          
          std::vector<char> combined;
          combined.reserve(p1.length + p2.length);
          combined.insert(combined.end(), p1.data.begin(), p1.data.end());
          combined.insert(combined.end(), p2.data.begin(), p2.data.end());
          send(client.socket, combined.data(), combined.size(), MSG_NOSIGNAL);

          client.authenticated = true;
          client.auth_id = id;
          on_log("Client [" + std::string(inet_ntoa(client.sock_info.sin_addr)) + ":" + std::to_string(ntohs(client.sock_info.sin_port)) + "] has authenticated successfully!");
       } else {
          packet p1 = form_packet("", id, SERVERDATA_RESPONSE_VALUE);
          packet p2 = form_packet("", -1, SERVERDATA_AUTH_RESPONSE);
          
          std::vector<char> combined;
          combined.reserve(p1.length + p2.length);
          combined.insert(combined.end(), p1.data.begin(), p1.data.end());
          combined.insert(combined.end(), p2.data.begin(), p2.data.end());
          send(client.socket, combined.data(), combined.size(), MSG_NOSIGNAL);

          std::string client_ip = inet_ntoa(client.sock_info.sin_addr);
          on_log("RCON connection from " + client_ip + " was unsuccessful - ignoring them for 60 seconds");
          
          {
              std::lock_guard<std::mutex> lock(blocked_ips_mutex);
              blocked_ips[client_ip] = time(nullptr);
          }

          disconnect_client(client.socket);
       }
    } else {
       if (type != SERVERDATA_EXECCOMMAND) {
          packet_to_send = form_packet("Invalid packet type (" + std::to_string(type) + "). Double check your packets.", id, SERVERDATA_RESPONSE_VALUE);
          on_log("Invalid packet type (" + std::to_string(type) + ") sent by [" + inet_ntoa(client.sock_info.sin_addr) + ":" + std::to_string(ntohs(client.sock_info.sin_port)) + "]. Asking client to double check their packets.");
          send(client.socket, packet_to_send.data.data(), packet_to_send.length, MSG_NOSIGNAL);
       } else {
          on_log("Client [" + std::string(inet_ntoa(client.sock_info.sin_addr)) + ":" + std::to_string(ntohs(client.sock_info.sin_port)) + "] has asked to execute the command: \"" + packet_data + "\"");
          if (!on_command) {
             on_log("You have not set any response for on_command! The server will default to a blank response.");
             packet_to_send = form_packet("", id, SERVERDATA_RESPONSE_VALUE);
             send(client.socket, packet_to_send.data.data(), packet_to_send.length, MSG_NOSIGNAL);
          } else {
             client_command command{};
             command.command = packet_data;
             command.client = client;

             std::string text_to_send = on_command(command);
             on_log("Sending reply to client [" + std::string(inet_ntoa(client.sock_info.sin_addr)) + ":" + std::to_string(ntohs(client.sock_info.sin_port)) + "].");

             if (text_to_send.empty()) {
                 packet p1 = form_packet("", 0, SERVERDATA_CONSOLE_LOG);
                 packet p2 = form_packet("", id, SERVERDATA_RESPONSE_VALUE);
                 
                 std::vector<char> combined;
                 combined.reserve(p1.length + p2.length);
                 combined.insert(combined.end(), p1.data.begin(), p1.data.end());
                 combined.insert(combined.end(), p2.data.begin(), p2.data.end());
                 send(client.socket, combined.data(), combined.size(), MSG_NOSIGNAL);
                 return;
             }

             size_t offset = 0;
             const size_t MAX_BODY_LEN = 4000;
             while (offset < text_to_send.length()) {
                 size_t chunkLen = (std::min)(MAX_BODY_LEN, text_to_send.length() - offset);
                 std::string chunk = text_to_send.substr(offset, chunkLen);

                 packet p1 = form_packet(chunk, 0, SERVERDATA_CONSOLE_LOG);
                 packet p2 = form_packet(chunk, id, SERVERDATA_RESPONSE_VALUE);
                 
                 std::vector<char> combined;
                 combined.reserve(p1.length + p2.length);
                 combined.insert(combined.end(), p1.data.begin(), p1.data.end());
                 combined.insert(combined.end(), p2.data.begin(), p2.data.end());

                 if (send(client.socket, combined.data(), combined.size(), MSG_NOSIGNAL) < 0) {
                     const last_error err = get_last_error();
                     on_log("Failed to send a chunked packet to Client | Error code: " + std::to_string(err.error_code) + "!");
                     if (err.type_of_error == DISCONNECTED) client.last_heartbeat = 0;
                     return;
                 }
                 offset += chunkLen;
             }
          }
       }
    }
}

bool rconpp::rcon_server::send_heartbeat(connected_client& client) {
    on_log("Sending heartbeat to Client [" + std::string(inet_ntoa(client.sock_info.sin_addr)) + ":" + std::to_string(ntohs(client.sock_info.sin_port)) + "]");

    packet p1 = form_packet("", 0, SERVERDATA_CONSOLE_LOG);
    packet p2 = form_packet("", client.auth_id, SERVERDATA_RESPONSE_VALUE);
    
    std::vector<char> combined;
    combined.reserve(p1.length + p2.length);
    combined.insert(combined.end(), p1.data.begin(), p1.data.end());
    combined.insert(combined.end(), p2.data.begin(), p2.data.end());

    if (send(client.socket, combined.data(), combined.size(), MSG_NOSIGNAL) < 0) {
       const last_error err = get_last_error();
       on_log("Failed to send a heartbeat to Client [" + std::string(inet_ntoa(client.sock_info.sin_addr)) + ":" + std::to_string(ntohs(client.sock_info.sin_port)) + " | Error code: " + std::to_string(err.error_code) + "]!");
       return false;
    }

    client.last_heartbeat = time(nullptr);
    return true;
}

void rconpp::rcon_server::client_process_loop(connected_client& client) {
    while (client.connected) {
       read_packet(client);

       const time_t current_time = time(nullptr);

       if (client.last_heartbeat == 0 || current_time - client.last_heartbeat >= HEARTBEAT_TIME) {
          if (!send_heartbeat(client)) {
             on_log("Client [" + std::string(inet_ntoa(client.sock_info.sin_addr)) + ":" + std::to_string(ntohs(client.sock_info.sin_port)) + " | Socket: " + std::to_string(client.socket) + "] is now being disconnected.");
             disconnect_client(client.socket);
             client.connected = false;
          }
       }

       std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void rconpp::rcon_server::start(bool return_after) {
    auto block_calling_thread = [this]() {
       std::mutex thread_mutex;
       std::unique_lock thread_lock(thread_mutex);
       this->terminating.wait(thread_lock);
    };

    if (port > 65535) {
       on_log("Invalid port! The port can't exceed 65535!");
       return;
    }

    on_log("Attempting to startup an RCON server...");

    if (!startup_server()) {
       on_log("RCON server is aborting as it failed to initiate server.");
       return;
    }

    online = true;
    on_log("Server is now listening, initiating runners...");

    accept_connections_runner = std::thread([this]() {
       while (online) {
          sockaddr_in client_info{};
          socklen_t client_len = sizeof(client_info);
          SOCKET_TYPE client_socket = accept(sock, reinterpret_cast<sockaddr*>(&client_info), &client_len);

          if (client_socket == INVALID_SOCKET) {
             const last_error err = get_last_error();
             on_log("A new client attempted to join but failed [Error code: " + std::to_string(err.error_code) + "]!");
             continue;
          }

          std::string client_ip = inet_ntoa(client_info.sin_addr);
          {
              std::lock_guard<std::mutex> lock(blocked_ips_mutex);
              auto it = blocked_ips.find(client_ip);
              if (it != blocked_ips.end()) {
                  if (time(nullptr) - it->second < 60) {
                      on_log("Ignoring RCON connection from " + client_ip + " (too soon)");
#ifdef _WIN32
                      closesocket(client_socket);
#else
                      close(client_socket);
#endif
                      continue;
                  } else {
                      blocked_ips.erase(it);
                  }
              }
          }

          on_log("Client [" + client_ip + ":" + std::to_string(ntohs(client_info.sin_port)) + " | Socket: " + std::to_string(client_socket) + "] is connecting to the server.");

          connected_client client{};
          client.sock_info = client_info;
          client.socket = client_socket;
          client.connected = true;
          client.last_heartbeat = time(nullptr);

          add_client(client_socket, client);

          std::thread client_thread(&rcon_server::client_process_loop, this, std::ref(connected_clients.at(client_socket)));

          while (!request_handlers_mutex.try_lock()) {
             std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }

          request_handlers.insert({ client_socket, std::move(client_thread) });
          request_handlers.at(client_socket).detach();
          request_handlers_mutex.unlock();

          on_log("Client [" + std::string(inet_ntoa(client_info.sin_addr)) + ":" + std::to_string(ntohs(client_info.sin_port)) + " | Socket: " + std::to_string(client_socket) + "] has successfully connected to the server, asking for authentication.");
       }
    });

    accept_connections_runner.detach();
    on_log("Server is now ready!");

    if (!return_after) {
       block_calling_thread();
    }
}