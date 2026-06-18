# Fougerite_LibRust_x64

**Author:** DreTaX

x64 reimplementation of Facepunch's `librust` native library for Rust Legacy dedicated servers.

---

## Background

Rust Legacy's dedicated server shipped with a native Win32 DLL called `librust.dll` - an x86 library that handled Steamworks integration (authentication, server listings, VAC) and RCON over TCP. The original was closed-source and built against an older Steamworks SDK (v128).

With 64-bit operating environments becoming the standard and x86 support being phased out, the goal of this project is to reverse engineer the full public API surface of `librust` and rebuild it as a proper x64 native DLL. This lets Rust Legacy's dedicated server (`assembly-csharp`) run on a 64-bit process without needing legacy x86 compatibility layers.

The Steamworks SDK has been upgraded from **v1.28 to v1.64** (latest) as part of this rebuild.

A longer-term goal of the project is to allow proper Steam emulation - specifically, faking a free-to-play game entry so that Rust Legacy servers can appear in Steam's server browser automatically without needing a paid license on the backend.

---

## How Rust Legacy Uses `librust`

The game's managed code (`assembly-csharp`, compiled Unity C#) calls into `librust` via P/Invoke (`[DllImport("librust")]`). There are five classes in assembly-csharp that import from this library:

### `LibRust : MonoBehaviour`

The main lifecycle bridge. On `Awake`, it attaches to or allocates a console window, registers the Unity log callback (`CaptureLog`) so all `Debug.Log` output flows through to `ConsoleLog`, and calls `Initialize` with the full command-line argument array.

On `Update` (called every frame on Unity's main thread), it does three things in order:

1. Calls `Cycle()` which pumps the Steam game server callback queue.
2. Calls `Console_Input()` - if a command was typed in the server console, runs it via `ConsoleSystem.Run`.
3. Calls `Console_Closing()` - if the native side signals a close request (CTRL+C, CTRL+BREAK, or the X button on the console window), runs `quit` and destroys the MonoBehaviour.

On `OnDestroy`, it calls `Hooks.ServerShutdown()` then `Shutdown()` to cleanly tear down the native side.

`CaptureLog` is the Unity log callback. Every `Debug.Log`, warning, error, and exception fired anywhere in the game passes through it and gets forwarded to `ConsoleLog` in the DLL, which handles colored console output and RCON capture.

```csharp
[DllImport("librust")] public static extern int Initialize(string[] args, int numargs);
[DllImport("librust")] public static extern void Shutdown();
[DllImport("librust")] public static extern void Cycle();
[DllImport("librust")] public static extern bool Console_Closing();
[DllImport("librust")] public static extern IntPtr Console_Input();
[DllImport("librust")] public static extern void ConsoleLog(string log, string trace, int type);
```

### `Rust.Steam.Server`

Handles all Steam game server logic. `Init()` calls `Steam_ServerStartup` with the listen port and protocol version (1069). If that fails, the process quits. On success it allocates and pins the two managed delegates (`funcUserAuth`, `funcUserGroup`) via `GCHandle` to prevent garbage collection, then registers them as callbacks. It also fetches the server's SteamID and public IP immediately after init.

`StartUserAuth` pins the auth ticket byte array and calls `SteamServer_BeginAuthSession`. The result string is checked against `"ok"` to decide whether to accept the connection.

`OnPlayerCountChanged` pushes current player count, server name, map, and tags to Steam (which updates the server browser listing) and also updates the console window title.

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

The main server console command class. Contains `Console_AllowClose`, which the game calls during its shutdown sequence to unblock the native side's console control handler and allow the process to exit.

```csharp
[DllImport("librust")] public static extern void Console_AllowClose();
```

### `Rust.Utility.FreezeMonitor`

Two stubs. The original x86 build may have had a watchdog thread that detected server freezes; in this implementation they are no-ops that satisfy the import.

```csharp
[DllImport("librust")] private static extern void FreezeMonitor_On();
[DllImport("librust")] private static extern void FreezeMonitor_Off();
```

### `RCon`

Sets up the RCON callback pipeline. `Setup()` allocates and pins two managed delegates via `GCHandle` - one for password auth, one for command dispatch - and passes them to `RCON_SetupCallbacks`. The native side wires these into the rconpp TCP server. `RunCommand` executes incoming RCON commands synchronously via `ConsoleSystem.Run` on whatever thread calls it (the rconpp client thread in practice - Loom is not used).

```csharp
[DllImport("librust")] public static extern void RCON_SetupCallbacks(rconFuncAuth auth, rconFuncCommand command);
```

---

## Exported API Coverage

Every function imported by assembly-csharp has been implemented. Full list:

| Export | Status | Notes |
|---|---|---|
| `Initialize` | ✅ | Console setup, WSA init, cfg parsing, arg parsing, starts input thread |
| `Shutdown` | ✅ | Stops input thread, destroys RCON server, WSA cleanup |
| `Cycle` | ✅ | Pumps Steam callbacks and processes any pending RCON command on the main thread |
| `Console_Closing` | ✅ | Returns close request flag; only true after `Steam_ServerStartup` succeeds |
| `Console_AllowClose` | ✅ | Grants the blocked close handler permission to exit |
| `Console_Input` | ✅ | Pops next command string from the input queue |
| `ConsoleLog` | ✅ | Colored console output; captures into RCON buffer when a command is executing |
| `SetTitleOfConsole` | ✅ | UTF-8 → wide char, calls `SetConsoleTitleW` |
| `FreezeMonitor_On` | ✅ (stub) | No-op |
| `FreezeMonitor_Off` | ✅ (stub) | No-op |
| `RCON_SetupCallbacks` | ✅ | Functional via custom Python RCON client; see note below |
| `Steam_ServerStartup` | ✅ | `SteamGameServer_Init` with `eServerModeAuthenticationAndSecure` |
| `Steam_ServerShutdown` | ✅ | Cleans up callbacks, calls `SteamGameServer_Shutdown` |
| `Steam_UpdateServer` | ✅ | Pushes max players, map, name, and tags to Steam |
| `SteamServer_SetCallback_UserAuth` | ✅ | Stores the managed delegate pointer |
| `SteamServer_SetCallback_UserGroup` | ✅ | Stores the managed delegate pointer |
| `SteamServer_BeginAuthSession` | ✅ | Calls `BeginAuthSession`, returns string status |
| `SteamServer_UserGroupStatus` | ✅ | Calls `RequestUserGroupStatus` |
| `SteamServer_UserLeave` | ✅ | Calls `EndAuthSession` |
| `SteamServer_GetSteamID` | ✅ | Returns server SteamID as uint64 |
| `SteamServer_GetPublicIP` | ✅ | Returns public IP as uint32 |

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

## RCON

RCON uses a customized build of [rconpp](https://codeberg.org/Jaskowicz/rconpp) (included in the repo) over Source RCON TCP. Several fixes were made to rconpp to work correctly:

- Heartbeat packets (`id=-1`) are completely disabled - the original librust never sent them and they corrupt the client's packet stream.
- `recv` is looped on the packet body until all bytes are read, handling TCP fragmentation.
- Clean disconnect (`recv` returning 0) is properly detected and triggers `disconnect_client`.
- RCON commands are executed on Unity's main thread via `Cycle()` using a condition variable, ensuring `Debug.Log` fires the registered log callback synchronously and output is captured correctly.

RustAdmin is not compatible with this implementation. Use `legacy_rcon.py` or you can build your own with librust_rcontest.
I found It a waste of time to try to make RustAdmin work.

---

## Custom RCON Client (Python)

Due to RustAdmin's non-standard internal state machine, a dedicated standalone RCON client (`legacy_rcon.py`) is provided. It has a graphical UI, parses `status` output into a player table, and streams console output live.

### Requirements

- Python 3.x
- Dependencies:

```
pip install customtkinter pyinstaller
```

### Compiling to a Standalone Executable

To package the tool into a portable, standalone Windows `.exe` application:

```
pyinstaller --noconsole --onefile legacy_rcon.py
```

Output is in the `dist` directory.

---

## Dependencies

- [Steamworks SDK 1.64](https://partner.steamgames.com/) - `steam/steam_gameserver.h`
- [rconpp](https://github.com/Miku-UI/rconpp) - TCP RCON server library (customized, included in repo)
- Windows SDK
- MSVC x64 build toolchain

---

## Build

Compile as a 64-bit DLL (`x64`) targeting Windows. Link against `steam_api64.lib` from the Steamworks SDK. The output must be named `librust.dll` and placed in the Rust Legacy dedicated server Plugins directory.