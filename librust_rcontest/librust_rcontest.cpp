#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <iomanip>

#pragma comment(lib, "ws2_32.lib")

const char* SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 29016;
const char* RCON_PASSWORD = "rconpassword";

void HexDump(const unsigned char* data, int size)
{
    for (int i = 0; i < size; ++i)
    {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
        if ((i + 1) % 16 == 0) std::cout << "\n";
    }
    std::cout << std::dec << "\n";
}

void SendPacket(SOCKET sock, int id, int type, const std::string& body)
{
    int bodyLen = body.length();
    int packetSize = 10 + bodyLen;

    std::vector<char> out(4 + packetSize, 0);
    *(int*)&out[0] = packetSize;
    *(int*)&out[4] = id;
    *(int*)&out[8] = type;
    if (bodyLen > 0)
    {
        memcpy(&out[12], body.c_str(), bodyLen);
    }

    send(sock, out.data(), out.size(), 0);

    std::cout << "\n[SENT] ID: " << id << " | Type: " << type << " | Body: " << body << "\n";
    std::cout << "[SENT HEX]\n";
    HexDump((unsigned char*)out.data(), out.size());
    std::cout << "----------------------------------------\n";
}

void ReaderThread(SOCKET sock)
{
    while (true)
    {
        int size = 0;
        int bytes = recv(sock, (char*)&size, 4, 0);
        if (bytes <= 0)
        {
            std::cout << "\n[DISCONNECTED] Server closed the connection.\n";
            exit(0);
        }

        std::vector<char> buf(size);
        int totalRead = 0;
        while (totalRead < size)
        {
            int r = recv(sock, buf.data() + totalRead, size - totalRead, 0);
            if (r <= 0) exit(0);
            totalRead += r;
        }

        int id = *(int*)&buf[0];
        int type = *(int*)&buf[4];
        std::string body(&buf[8]);

        std::cout << "\n\n=== PACKET RECEIVED ===\n";
        std::cout << "Reported Size: " << size << "\n";
        std::cout << "ID:   " << id << "\n";
        std::cout << "Type: " << type << "\n";
        std::cout << "Body: " << body << "\n";
        std::cout << "Raw Hex (including size header):\n";

        unsigned char* sizePtr = (unsigned char*)&size;
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)sizePtr[0] << " "
            << std::setw(2) << std::setfill('0') << (int)sizePtr[1] << " "
            << std::setw(2) << std::setfill('0') << (int)sizePtr[2] << " "
            << std::setw(2) << std::setfill('0') << (int)sizePtr[3] << " ";

        HexDump((unsigned char*)buf.data(), size);
        std::cout << "=======================\n> ";
    }
}

int main()
{
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        std::cout << "Failed to create socket.\n";
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &serverAddr.sin_addr);

    std::cout << "Connecting to " << SERVER_IP << ":" << SERVER_PORT << "...\n";
    if (connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        std::cout << "Connection failed.\n";
        return 1;
    }
    std::cout << "Connected.\n";

    std::thread reader(ReaderThread, sock);
    reader.detach();

    // Send initial Auth packet
    int currentId = 1;
    SendPacket(sock, currentId++, 3, RCON_PASSWORD);

    // Command loop
    std::string input;
    while (true)
    {
        std::cout << "> ";
        std::getline(std::cin, input);
        if (input.empty()) continue;

        // Command packet (Type 2)
        SendPacket(sock, currentId++, 2, input);
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
