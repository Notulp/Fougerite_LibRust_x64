#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <iostream>
#include <string>
#include <string_view>   
#include <cstdint>
#include <queue>
#include <mutex>
#include <thread>
#include <conio.h>
#include <fstream>
#include <sstream>
#include <algorithm>

#include "steam/steam_gameserver.h"

#include "rconpp/rcon.h"

#define EXPORT extern "C" __declspec(dllexport)

typedef void (__stdcall*funcUserAuth)(uint64_t iUserID, const char* strStatus);
typedef void (__stdcall*funcUserGroup)(uint64_t iUserID, uint64_t iGroupID, const char* strStatus);
typedef bool (__stdcall*rconFuncAuth)(const char* strPassword);
typedef void (__stdcall*rconFuncCommand)(int iID, const char* strCommand);

funcUserAuth g_UserAuth = nullptr;
funcUserGroup g_UserGroup = nullptr;
rconFuncAuth g_RconAuth = nullptr;
rconFuncCommand g_RconCommand = nullptr;

std::queue<std::string> g_InputQueue;
std::mutex g_InputMutex;
std::mutex g_ConsoleMutex; 
std::thread g_InputThread;
std::string g_CurrentInput; 

rconpp::rcon_server* g_RconServer = nullptr;
int g_GamePort = 29015; 
int g_RconPort = 0;
std::string g_RconPassword = "testing";
std::string g_RconCaptureBuffer;
bool g_IsCapturingRcon = false;
std::mutex g_RconCaptureMutex;

bool g_IsRunning = false;
bool g_WantToClose = false;
bool g_AllowCloseGranted = false;
char g_ReturnBuffer[4096] = { 0 };

class CSteamCallbacks
{
public:
    CSteamCallbacks() :
        m_CallbackAuthTicketResponse(this, &CSteamCallbacks::OnAuthTicketResponse),
        m_CallbackClientGroupStatus(this, &CSteamCallbacks::OnClientGroupStatus)
    {
    }

    STEAM_GAMESERVER_CALLBACK(CSteamCallbacks, OnAuthTicketResponse, ValidateAuthTicketResponse_t, m_CallbackAuthTicketResponse);
    STEAM_GAMESERVER_CALLBACK(CSteamCallbacks, OnClientGroupStatus, GSClientGroupStatus_t, m_CallbackClientGroupStatus);
};

CSteamCallbacks* g_pCallbacks = nullptr;

void CSteamCallbacks::OnAuthTicketResponse(ValidateAuthTicketResponse_t* pParam)
{
    if (g_UserAuth)
    {
        const char* status = "failed";
        switch (pParam->m_eAuthSessionResponse)
        {
        case k_EAuthSessionResponseOK: status = "ok"; break;
        case k_EAuthSessionResponseUserNotConnectedToSteam: status = "not connected to steam"; break;
        case k_EAuthSessionResponseNoLicenseOrExpired: status = "no license"; break;
        case k_EAuthSessionResponseVACBanned: status = "vac banned"; break;
        case k_EAuthSessionResponseLoggedInElseWhere: status = "logged in elsewhere"; break;
        case k_EAuthSessionResponseVACCheckTimedOut: status = "vac timeout"; break;
        case k_EAuthSessionResponseAuthTicketCanceled: status = "canceled"; break;
        case k_EAuthSessionResponseAuthTicketInvalidAlreadyUsed: status = "invalid ticket"; break;
        case k_EAuthSessionResponseAuthTicketInvalid: status = "invalid ticket"; break;
        }
        g_UserAuth(pParam->m_SteamID.ConvertToUint64(), status);
    }
}

void CSteamCallbacks::OnClientGroupStatus(GSClientGroupStatus_t* pParam)
{
    if (g_UserGroup)
    {
        const char* status = pParam->m_bMember ? "member" : "not member";
        g_UserGroup(pParam->m_SteamIDUser.ConvertToUint64(), pParam->m_SteamIDGroup.ConvertToUint64(), status);
    }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    switch (ctrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        g_WantToClose = true;
        while (!g_AllowCloseGranted)
        {
            Sleep(50);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

void ConsoleInputWorker()
{
    while (g_IsRunning)
    {
        if (_kbhit())
        {
            std::lock_guard<std::mutex> lock(g_ConsoleMutex);
            int c = _getch();

            if (c == 0 || c == 224)
            {
                _getch();
                continue;
            }

            if (c == '\r') 
            {
                if (!g_CurrentInput.empty())
                {
                    std::lock_guard<std::mutex> qLock(g_InputMutex);
                    g_InputQueue.push(g_CurrentInput);
                }
                std::cout << "\r" << std::string(g_CurrentInput.length() + 2, ' ') << "\r";
                g_CurrentInput.clear();
            }
            else if (c == '\b') 
            {
                if (!g_CurrentInput.empty())
                {
                    g_CurrentInput.pop_back();
                    std::cout << "\b \b";
                }
            }
            else if (c >= 32 && c <= 126) 
            {
                g_CurrentInput.push_back((char)c);
                std::cout << (char)c;
            }
        }
        else
        {
            Sleep(10);
        }
    }
}

void CleanString(std::string& str) 
{
    str.erase(str.find_last_not_of(" \t\r\n") + 1);
    str.erase(0, str.find_first_not_of(" \t\r\n"));
}

void ParseCfgFile(const std::string& filePath) 
{
    std::ifstream file(filePath);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) 
    {
        CleanString(line);
        if (line.empty() || line[0] == '#') continue;

        std::stringstream ss(line);
        std::string key, val;
        ss >> key;
        
        std::size_t pos = line.find(key);
        if (pos != std::string::npos) 
        {
            val = line.substr(pos + key.length());
        }
        
        CleanString(val);
        if (!val.empty() && val.front() == '\"') val.erase(0, 1);
        if (!val.empty() && val.back() == '\"') val.pop_back();
        
        if (key == "server.port") 
        {
            int p = atoi(val.c_str());
            if (p > 0) g_GamePort = p;
            std::cout << "[LibRust x64] Game Port set to " << g_GamePort << "\n";
        } 
        else if (key == "rcon.port") 
        {
            int p = atoi(val.c_str());
            if (p > 0) g_RconPort = p;
            std::cout << "[LibRust x64] RCON Port set to " << g_RconPort << "\n";
        } 
        else if (key == "rcon.password") 
        {
            if (!val.empty()) g_RconPassword = val;
            std::cout << "[LibRust x64] RCON Password found " << "\n";
        }
    }
}

EXPORT int Initialize(char** args, int numargs)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
    {
        AllocConsole();
    }
    
    freopen_s((FILE**)stdin, "CONIN$", "r", stdin);
    freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
    
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    std::string cfgPath = "";
    for (int i = 0; i < numargs; ++i)
    {
        if (strcmp(args[i], "-port") == 0 && (i + 1) < numargs)
        {
            g_GamePort = atoi(args[i + 1]);
        }
        if (strcmp(args[i], "-cfg") == 0 && (i + 1) < numargs)
        {
            cfgPath = args[i + 1];
        }
    }

    if (!cfgPath.empty()) 
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir = exePath;
        std::size_t lastSlash = exeDir.find_last_of("\\/");
        if (lastSlash != std::string::npos) 
        {
            exeDir = exeDir.substr(0, lastSlash + 1);
        }
        std::string fullPath = exeDir + cfgPath;
        ParseCfgFile(fullPath);
    }

    if (g_RconPort == 0) 
    {
        g_RconPort = g_GamePort + 1;
        std::cout << "[LibRust x64] RCON Port not specified, defaulting to " << g_RconPort << "\n";
    }
    else
    {
        std::cout << "[LibRust x64] RCON Port set to " << g_RconPort << "\n";
    }

    g_IsRunning = true;
    g_InputThread = std::thread(ConsoleInputWorker);

    std::cout << "[LibRust x64] Dedicated Native System Online.\n";
    return 1;
}

EXPORT void Shutdown()
{
    g_IsRunning = false;
    if (g_InputThread.joinable())
    {
        g_InputThread.detach(); 
    }

    if (g_RconServer)
    {
        delete g_RconServer; 
        g_RconServer = nullptr;
    }
    WSACleanup();
}

EXPORT void Cycle()
{
    SteamGameServer_RunCallbacks();
}

EXPORT bool Console_Closing()
{
    return g_WantToClose;
}

EXPORT void Console_AllowClose()
{
    g_AllowCloseGranted = true;
}

EXPORT const char* Console_Input()
{
    std::lock_guard<std::mutex> lock(g_InputMutex);
    if (g_InputQueue.empty())
    {
        return nullptr;
    }

    std::string command = g_InputQueue.front();
    g_InputQueue.pop();

    memset(g_ReturnBuffer, 0, sizeof(g_ReturnBuffer));
    strncpy_s(g_ReturnBuffer, command.c_str(), _TRUNCATE);

    return g_ReturnBuffer;
}

EXPORT void ConsoleLog(const char* log, const char* trace, int type)
{
    if (!log) return;

    if (g_IsCapturingRcon)
    {
        std::lock_guard<std::mutex> lock(g_RconCaptureMutex);
        g_RconCaptureBuffer += log;
        g_RconCaptureBuffer += "\n";
    }

    std::lock_guard<std::mutex> lock(g_ConsoleMutex);
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    std::cout << "\r" << std::string(g_CurrentInput.length() + 2, ' ') << "\r";

    WORD defaultColor = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    WORD color = defaultColor;

    if (type == 0 || type == 1 || type == 2 || type == 4) 
    {
        color = FOREGROUND_RED | FOREGROUND_INTENSITY; 
    }

    SetConsoleTextAttribute(hConsole, color);
    std::cout << log << "\n";

    if ((type == 0 || type == 4) && trace && strlen(trace) > 0)
    {
        std::cout << trace << "\n";
    }

    SetConsoleTextAttribute(hConsole, defaultColor);

    if (!g_CurrentInput.empty())
    {
        std::cout << g_CurrentInput;
    }
    std::cout.flush();
}

EXPORT void RCON_SetupCallbacks(rconFuncAuth auth, rconFuncCommand command)
{
    g_RconAuth = auth;
    g_RconCommand = command;

    if (g_RconServer) return;

    g_RconServer = new rconpp::rcon_server("0.0.0.0", g_RconPort, g_RconPassword);

    std::cout << "[LibRust x64] rconpp instance created. Socket Listening: " << (g_RconServer->online ? "SUCCESS" : "FAILED") << "\n";
    if (!g_RconServer->online)
    {
        std::cout << "[LibRust x64] Socket Bind Error Code: " << WSAGetLastError() << "\n";
    }

    g_RconServer->on_command = [](const rconpp::client_command& cmd) -> std::string {
        if (g_RconCommand)
        {
            std::lock_guard<std::mutex> lock(g_RconCaptureMutex);
            g_RconCaptureBuffer.clear();
            g_IsCapturingRcon = true;
            std::cout << "[LibRust x64] RCON Command: " << cmd.command << "\n";

            g_RconCommand(0, cmd.command.c_str());

            g_IsCapturingRcon = false;
            
            if (g_RconCaptureBuffer.empty())
            {
                return "Command executed successfully.\n";
            }
            return g_RconCaptureBuffer;
        }
        return "Engine core command pipeline unavailable.\n";
    };

    std::cout << "[LibRust x64] rconpp pipeline mapped on TCP port " << g_RconPort << "\n";
}

EXPORT void FreezeMonitor_On() {}
EXPORT void FreezeMonitor_Off() {}

EXPORT void SetTitleOfConsole(const char* log)
{
    if (log) 
    {
        int len = MultiByteToWideChar(CP_UTF8, 0, log, -1, NULL, 0);
        wchar_t* wstr = new wchar_t[len];
        MultiByteToWideChar(CP_UTF8, 0, log, -1, wstr, len);
        SetConsoleTitleW(wstr);
        delete[] wstr;
    }
}

EXPORT bool Steam_ServerStartup(int port, int protocol)
{
    char versionString[32];
    sprintf_s(versionString, "%d", protocol);
    
    bool result = SteamGameServer_Init(0, 8766, port, g_RconPort, eServerModeAuthenticationAndSecure, versionString);
    if (result)
    {
        g_pCallbacks = new CSteamCallbacks();
        SteamGameServer()->SetModDir("rust");
        SteamGameServer()->SetProduct("rust");
        SteamGameServer()->SetGameDescription("Rust Legacy x64");
        SteamGameServer()->LogOnAnonymous();
        std::cout << "[LibRust x64] SteamGameServer Initialized with Protocol " << versionString << " successfully.\n";
    }
    else
    {
        std::cout << "[LibRust x64] SteamGameServer Initialization FAILED.\n";
    }
    return result;
}

EXPORT void Steam_ServerShutdown()
{
    if (g_pCallbacks)
    {
        delete g_pCallbacks;
        g_pCallbacks = nullptr;
    }
    SteamGameServer_Shutdown();
}

EXPORT void Steam_UpdateServer(int maxplayers, int icurrentplayers, const char* strServerName, const char* strMapName, const char* strTags)
{
    if (SteamGameServer())
    {
        SteamGameServer()->SetMaxPlayerCount(maxplayers);
        SteamGameServer()->SetMapName(strMapName);
        SteamGameServer()->SetServerName(strServerName);
        SteamGameServer()->SetGameTags(strTags);
    }
}

EXPORT void SteamServer_SetCallback_UserAuth(funcUserAuth fnc)
{
    g_UserAuth = fnc;
}

EXPORT void SteamServer_SetCallback_UserGroup(funcUserGroup fnc)
{
    g_UserGroup = fnc;
}

EXPORT const char* SteamServer_BeginAuthSession(void* pData, int iDataSize, uint64_t iUserID)
{
    if (!SteamGameServer()) return "failed";

    CSteamID steamID(iUserID);
    EBeginAuthSessionResult res = SteamGameServer()->BeginAuthSession(pData, iDataSize, steamID);

    switch (res)
    {
    case k_EBeginAuthSessionResultOK: return "ok";
    case k_EBeginAuthSessionResultInvalidTicket: return "invalid ticket";
    case k_EBeginAuthSessionResultDuplicateRequest: return "duplicate request";
    case k_EBeginAuthSessionResultInvalidVersion: return "invalid version";
    case k_EBeginAuthSessionResultGameMismatch: return "game mismatch";
    case k_EBeginAuthSessionResultExpiredTicket: return "expired ticket";
    }
    return "failed";
}

EXPORT bool SteamServer_UserGroupStatus(uint64_t iUserID, uint64_t iGroupID)
{
    if (SteamGameServer())
    {
        return SteamGameServer()->RequestUserGroupStatus(CSteamID(iUserID), CSteamID(iGroupID));
    }
    return false;
}

EXPORT void SteamServer_UserLeave(uint64_t iUserID)
{
    if (SteamGameServer())
    {
        SteamGameServer()->EndAuthSession(CSteamID(iUserID));
    }
}

EXPORT uint64_t SteamServer_GetSteamID()
{
    if (SteamGameServer())
    {
        return SteamGameServer()->GetSteamID().ConvertToUint64();
    }
    return 0;
}

EXPORT uint32_t SteamServer_GetPublicIP()
{
    if (SteamGameServer())
    {
        return SteamGameServer()->GetPublicIP();
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    return TRUE;
}