# Fougerite_LibRust_x64

**Author:** DreTaX

x64 reimplementation of Facepunch's `librust` native library for Rust Legacy dedicated servers.

---

## Background

Rust Legacy's dedicated server shipped with a native Win32 DLL called `librust.dll` - an x86 library that handled Steamworks integration (authentication, server listings, VAC) and RCON over TCP. The original was closed-source and built against an older Steamworks SDK (v128).

With 64-bit operating environments becoming the standard and x86 support being phased out, the goal of this project is to reverse engineer the full public API surface of `librust` and rebuild it as a proper x64 native DLL. This lets Rust Legacy's dedicated server (`assembly-csharp`) run on a 64-bit process without needing legacy x86 compatibility layers.

The Steamworks SDK has been upgraded from **v1.28 to v1.29a** as part of this rebuild, with potential for further upgrades down the line.

A longer-term goal of the project is to allow proper Steam emulation - specifically, faking a free-to-play game entry so that Rust Legacy servers can appear in Steam's server browser automatically without needing a paid license on the backend.

---

## How Rust Legacy Uses `librust`

The game's managed code (`assembly-csharp`, compiled Unity C#) calls into `librust` via P/Invoke (`[DllImport("librust")]`). There are four classes in assembly-csharp that import from this library:

### `LibRust : MonoBehaviour`

The main lifecycle bridge. On `Awake`, it attaches to the console, registers the Unity log callback, and calls `Initialize` with the command-line args. On `Update`, it polls `Cycle` (which pumps Steam callbacks), reads queued console input via `Console_Input`, and checks `Console_Closing` to handle graceful shutdown. `OnDestroy` calls `Shutdown`.

```csharp
[DllImport("librust")] public static extern int Initialize(string[] args, int numargs);
[DllImport("librust")] public static extern void Shutdown();
[DllImport("librust")] public static extern void Cycle();
[DllImport("librust")] public static extern bool Console_Closing();
[DllImport("librust")] public static extern IntPtr Console_Input();
[DllImport("librust")] public static extern void ConsoleLog(string log, string trace, int type);
```

### `Rust.Steam.Server`

Handles all Steam game server logic: startup/shutdown, player authentication sessions, group membership queries, server info broadcasting to Steam, and fetching the server's public SteamID and IP.

```csharp
[DllImport("librust")] public static extern bool Steam_ServerStartup(int port, int protocol);
[DllImport("librust")] public static extern void Steam_ServerShutdown();
[DllImport("librust")] public static extern void Steam_UpdateServer(int maxplayers, int icurrentplayers, string strServerName, string strMapName, string strTags);
[DllImport("librust")] public static extern IntPtr SteamServer_BeginAuthSession(IntPtr pData, int iDataSize, ulong iUserID);
[DllImport("librust")] public static extern void SteamServer_UserLeave(ulong iUserID);
[DllImport("librust")] public static extern bool SteamServer_UserGroupStatus(ulong iUserID, ulong iGroupID);
[DllImport("librust")] public static extern void SteamServer_SetCallback_UserAuth(funcUserAuth fnc);
[DllImport("librust")] public static extern void SteamServer_SetCallback_UserGroup(funcUserGroup fnc);
[DllImport("librust")] public static extern ulong SteamServer_GetSteamID();
[DllImport("librust")] public static extern uint SteamServer_GetPublicIP();
[DllImport("librust")] public static extern void SetTitleOfConsole(string log);
```

### `global : ConsoleSystem`

Contains the `Console_AllowClose` import, used when the game gracefully acknowledges a console close event (CTRL+C / CTRL+CLOSE) signalled from the native side.

```csharp
[DllImport("librust")] public static extern void Console_AllowClose();
```

### `Rust.Utility.FreezeMonitor`

Two stubs that the game calls to enable/disable a freeze watchdog. In the original x86 build these may have had an actual implementation; currently they are no-ops.

```csharp
[DllImport("librust")] private static extern void FreezeMonitor_On();
[DllImport("librust")] private static extern void FreezeMonitor_Off();
```

### `RCon`

Sets up the RCON callback pipeline. The game passes in two delegates - one for password authentication, one for command dispatch - and the native side wires them into an rconpp TCP server.

```csharp
[DllImport("librust")] public static extern void RCON_SetupCallbacks(rconFuncAuth auth, rconFuncCommand command);
```

---

## Exported API Coverage

Every function imported by assembly-csharp has been implemented. Full list:

| Export | Status | Notes                                                                  |
|---|---|------------------------------------------------------------------------|
| `Initialize` | ✅ | Console setup, WSA init, cfg parsing, arg parsing, starts input thread |
| `Shutdown` | ✅ | Stops input thread, destroys RCON server, WSA cleanup                  |
| `Cycle` | ✅ | Calls `SteamGameServer_RunCallbacks()`                                 |
| `Console_Closing` | ✅ | Returns close request flag                                             |
| `Console_AllowClose` | ✅ | Grants the blocked close handler permission to exit                    |
| `Console_Input` | ✅ | Pops next command string from the input queue                          |
| `ConsoleLog` | ✅ | Colored console output; captures into RCON buffer when active          |
| `SetTitleOfConsole` | ✅ | UTF-8 → wide char, calls `SetConsoleTitleW`                            |
| `FreezeMonitor_On` | ✅ (stub) | No-op, satisfies the import                                            |
| `FreezeMonitor_Off` | ✅ (stub) | No-op, satisfies the import                                            |
| `RCON_SetupCallbacks` | ✅ ⚠️ | Old RCON tools may not work with it                                    |
| `Steam_ServerStartup` | ✅ | `SteamGameServer_Init` with `eServerModeAuthenticationAndSecure`       |
| `Steam_ServerShutdown` | ✅ | Cleans up callbacks, calls `SteamGameServer_Shutdown`                  |
| `Steam_UpdateServer` | ✅ | Pushes max players, map, name, and tags to Steam                       |
| `SteamServer_SetCallback_UserAuth` | ✅ | Stores the managed delegate pointer                                    |
| `SteamServer_SetCallback_UserGroup` | ✅ | Stores the managed delegate pointer                                    |
| `SteamServer_BeginAuthSession` | ✅ | Calls `BeginAuthSession`, returns string status                        |
| `SteamServer_UserGroupStatus` | ✅ | Calls `RequestUserGroupStatus`                                         |
| `SteamServer_UserLeave` | ✅ | Calls `EndAuthSession`                                                 |
| `SteamServer_GetSteamID` | ✅ | Returns server SteamID as uint64                                       |
| `SteamServer_GetPublicIP` | ✅ | Returns public IP as uint32                                            |

All 21 exports are covered.

---

## Configuration

`Initialize` accepts `-port` and `-cfg` on the command line.

When `-cfg <filename>` is passed, the file is resolved relative to the executable directory and parsed. Supported keys:

```
server.port  29015
rcon.port    29016
rcon.password "yourpassword"
```

Lines starting with `#` are treated as comments. Values may be optionally quoted.

If `rcon.port` is not specified, it defaults to `server.port + 1`.

---

## Known Issues

### RCON - Work In Progress

The RCON implementation (`RCON_SetupCallbacks` / `rconpp`) had to be customized, I couldn't get it to work with RustAdmin.
Feel free to take the simple RustLegacyRCON python script from the repo, and adapt it to your needs.
You could also try fixing the compatibility issue. I found it a waste of time.

---

## Dependencies

- [Steamworks SDK 1.29a](https://partner.steamgames.com/) - `steam/steam_gameserver.h`
- [rconpp](https://github.com/Miku-UI/rconpp) - TCP RCON server library, kind of customized, available in the repo...
- Windows SDK (Winsock2, Win32 console APIs)
- MSVC x64 build toolchain

---

## Build

Compile as a 64-bit DLL (`x64`) targeting Windows. Link against `steam_api64.lib` / `steamclient64.lib` from the Steamworks SDK and the rconpp static library. The output must be named `librust.dll` and placed in the Rust Legacy dedicated server directory.

---

## Status

Rust Legacy dedicated server runs properly under a 64-bit process with this library. Steam authentication, VAC, server listings, group status queries, and console I/O all function correctly. RCON is partially working but output response is unreliable pending the threading fix.

---

## Custom RCON Client (Python)

Due to RustAdmin's brittle and non-standard internal state machine, maintaining direct compatibility with it is counterproductive. RustAdmin expects a raw, unbuffered stream of interleaved TCP packet pairings across multiple engine threads, which breaks in our modern x64 multithreaded pipeline.

To solve this, a dedicated standalone RCON administration client tool (`legacy_rcon.py`) is provided from me. It features a base graphical UI, properly parses player entries from the `status` block into a table, and streams the background console feedback live without locking or crashing the server thread.

### Requirements

- Python 3.x
- Required dependencies can be installed via pip:
```
pip install customtkinter pyinstaller
```

### Compiling to a Standalone Executable

To package the tool into a portable, standalone Windows `.exe` application:

```
pyinstaller --noconsole --onefile legacy_rcon.py
```

Once the compilation finishes, you can find the executable inside the generated `dist` directory.