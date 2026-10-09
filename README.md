# EOS for L^

A native L^ extension for Steam-authenticated lobby matchmaking and P2P multiplayer.
The binding uses the EOS SDK's Connect, Lobby and P2P interfaces. It does not implement game-state replication or rollback.

[日本語](README.ja.md)

## Requirements

- An L^ host supporting **native extension ABI 2**, including the module shutdown hook. Build the accompanying lhat/lhat-love changes before using this extension; the earlier ABI 1 release cannot load it.
- The matching L^ source headers and generated `lhat/version.h` from that host build.
- Epic Online Services SDK, downloaded separately. Tested against **1.19.2.1, Windows x64**. Unpack so `EOSSDK/SDK/Include`, `Lib` and `Bin` exist, or set `EOS_SDK_ROOT`.
- CMake 3.20+, C++17 compiler, Python 3, and a full LÔVE executable to generate signatures.

The repository contains no EOS SDK or Steamworks SDK. SDK installation directories, local credentials, tickets and build artifacts are ignored by Git. The extension links the EOS SDK but **does not link a second lhat runtime**.

## Build

```powershell
.\scripts\build.ps1 -Test
python tests/test_lhat.py
```

The script first builds the DLL, asks the full host for its registered signatures, embeds them, and rebuilds. The resulting `build/Release/eos_lhat.dll` supports both full and VM-only hosts. Re-run after changing bindings or upgrading L^. Override `-Lovec`, `-Lhat`, `-LhatGenerated`, `-Sdk`, or `-Build` for other layouts.

Equivalent CMake commands (platform SDK libraries must be available):

```sh
cmake -S . -B build -DEOS_SDK_ROOT=/path/to/SDK \
  -DLHAT_ROOT=/path/to/lhat -DLHAT_GENERATED_INCLUDE=/path/to/host/lhat/include
cmake --build build --config Release
python scripts/embed-signatures.py --lovec /path/to/full/love --sdk /path/to/SDK
ctest --test-dir build -C Release --output-on-failure
python tests/test_lhat.py --lovec /path/to/full/love --vm /path/to/vm/love
```

Linux/macOS build paths are provided but have not been built or tested here. Set `BUILD_TESTING=OFF` for a production-only build. `cmake --install` installs only the real extension, never the mock library or the SDK.

## Editor type information

With the full lhat CLI containing `--extension` support, run from this repository:

```powershell
$env:PATH = "$PWD\EOSSDK\SDK\Bin;$env:PATH"
..\lhat\build\msvc-release\lhat.exe --dump-host-api lhat-host.json --extension .\build\Release\eos_lhat.dll
```

This includes the CLI's standard library and EOS definitions without running a game.
To include LÔVE definitions as well, use the updated lovec with the same SDK `PATH`:

```powershell
..\lhat-love\build\love\Release\lovec.exe --dump-host-api lhat-host.json --extension .\build\Release\eos_lhat.dll
```

Repeat `--extension` for additional libraries. Alternatively, specify a game with EOS listed in its `extensions.txt`: `lovec --dump-host-api lhat-host.json path/to/game`. Explicit extensions are only accepted for API dumps from non-fused executables.

## Steam authentication

Configure your EOS product, sandbox, deployment, Steam identity provider and a client policy permitting Connect, Lobby and P2P operations. Use a **game-client** policy. Set the Steam App ID and ticket identity in the EOS developer portal for your product.

Your Steam integration obtains a ticket using `ISteamUser::GetAuthTicketForWebApi("epiconlineservices")`, waits for the successful `GetTicketForWebApiResponse_t` callback, and hex-encodes the returned bytes. Pass that string to `client.loginSteam(ticketHex)`. The identity string must match the EOS provider configuration. This is a Web API session ticket, not the older `GetAuthSessionTicket` ticket. See the [Steamworks API](https://partner.steamgames.com/doc/api/ISteamUser#GetAuthTicketForWebApi) and the `EOS_ECT_STEAM_SESSION_TICKET` documentation in your SDK's `eos_common.h`.

**Ticket acquisition is outside this extension.** It neither starts Steam nor distributes Steamworks SDK code. Keep the ticket valid according to the Steamworks lifecycle. On `authExpired`, obtain a fresh ticket and call `loginSteam` again on the same Client; lobby membership is retained. A first login returning `EOS_InvalidUser` automatically calls `EOS_Connect_CreateUser`. Account linking and switching between accounts are not implemented; create a new Client to change accounts.

`loginDevice(displayName)` is available for development without Steam. It is not a substitute for Steam identity in a shipped Steam game. Two clients on the same device can map to the same Device ID; use separate devices/accounts for real two-player tests.

## LÔVE setup and example

Place the extension in the game's `native` directory, and list it in `extensions.txt`:

```text
native/eos_lhat
```

The EOS runtime library must be discoverable by the OS. On Windows put `EOSSDK-Win64-Shipping.dll` beside `lovec.exe` / your fused game executable, or put the SDK `Bin` directory on `PATH` for local development. Placing it next to an extension in an arbitrary subdirectory alone is not sufficient. Deploy the appropriate EOS runtime separately under Epic's SDK terms.

`examples/love-p2p` is a two-player input-packet probe:

1. Copy `build/Release/eos_lhat.dll` to `examples/love-p2p/native/`.
2. Copy `config.example.lton` to `config.lton` and fill the EOS settings. Use the same deployment, socket name and bucket on both computers. Give each a writable cache directory.
3. Set `host = true^` on one computer and `false^` on the other.
4. Obtain a Steam Web API ticket separately on each computer, using different Steam accounts. Write the hex string to `steam-ticket.txt` **without BOM, newline or spaces**.
5. Start the host first, then the other client. The first creates a public two-player lobby; the second searches by bucket and joins. Each sends a numbered input packet every 0.5 seconds and displays received packets.
6. S searches again; R reads a fresh ticket from the file and refreshes login; Esc leaves the lobby before quitting (with a five-second fallback).

```powershell
$env:PATH = "$PWD\EOSSDK\SDK\Bin;$env:PATH"
..\lhat-love\build\love\Release\lovec.exe --no-error-screen examples/love-p2p
```

The ticket file is a development bridge. A released game should pass tickets directly from its Steam integration.

## API

```text
eos.create({ productId, sandboxId, deploymentId, clientId, clientSecret,
             cacheDirectory, socketName }) -> Client | Error
```

All configuration fields are strings. `socketName` is 1–32 EOS socket-name characters. One Client owns one platform, one local Product User ID and at most one joined lobby. All clients must run on the same thread. Client values cannot be transferred to L^ worker machines.

| Client method | Result / purpose |
| --- | --- |
| `tick()` | `nil | Error`; call once per frame to drive EOS callbacks |
| `poll()` | Event record, `nil` when empty, or Error |
| `loginSteam(hexTicket)` | Request ID or Error; also refreshes credentials |
| `loginDevice(displayName)` | Request ID or Error; development login |
| `createLobby(bucket, capacity)` | Request ID or Error; public lobby, capacity 2–64, host migration enabled |
| `search(bucket, limit)` | Request ID or Error; bucket equality and at least one free slot, limit 1–100 |
| `results()` | Array of lobby records or Error; available after search completion |
| `join(lobbyId)` | Request ID or Error; lobbies must allow joining by ID |
| `leave()` | Request ID or Error |
| `userId()` | Product User ID string; empty before login, or Error |
| `lobby()` | Current lobby record or Error |
| `members()` | Array of Product User ID strings (including self), or Error |
| `send(peerId, data, channel, reliability)` | `nil | Error`; binary-safe string, maximum 1170 bytes, channel 0–255 |
| `receive()` | `{peerId, data, channel}`, `nil` when empty, or Error |
| `disconnect(peerId)` | `nil | Error`; a subsequent send may reconnect |
| `relay(mode)` | `nil | Error`; `eos.Relay.never`, `.allow` (SDK default), `.always` |
| `close()` | `nil | Error`; idempotent, cancels outstanding operations and releases this platform |

Reliability values: `eos.Reliability.unreliable`, `.reliableUnordered`, `.reliableOrdered`.
Lobby records: `{id:string, owner:string, bucket:string, capacity:number, available:number}`.
Arrays use zero-based L^ indexes. Search results are snapshots and reset at the next search.

Operations return a request ID immediately. Their Event has the matching `request`, `ok`, and EOS result name in `code`; unsolicited notifications have request 0. Events also contain `lobbyId` and `peerId` strings (empty when inapplicable). `kind` is an `eos.EventKind`:

- Completions: `login`, `search`, `created`, `joined`, `left`.
- Notifications: `memberJoined`, `memberLeft`, `ownerChanged`, `lobbyClosed`, `authExpired`, `loggedOut`, `peerClosed`.
- Diagnostics: `overflow`, `internalError`. Overflow reports the number of dropped notifications in `code`. Resynchronize membership and pending operations if it occurs.

Membership and peer-close notifications use their numeric SDK status/reason in `code`. Synchronous validation/SDK failures return `eos.Error` with a message. Only one login, one lobby mutation and one search may be outstanding at a time. Search and lobby operations are otherwise independent.

Poll all events each frame. The notification queue is bounded to 256 records. `receive` discards packets for other sockets/former members, processing at most 64 queued packets per call. Limit receive calls per frame in games exposed to sustained traffic.

Only current lobby peers can be sent to or accepted; disconnecting/kicking members closes their P2P connection. This is membership filtering, not game authority or anti-cheat. Host migration changes the owner notification; the game must migrate its own simulation state. Neither `send` success nor reliable delivery means the remote game consumed a packet.

## Lifetime and testing

Call `leave()` and keep ticking until completion for graceful departure, then `close()`. GC/disposal is a fallback, not an orderly lobby exit. Closing a Client cancels pending completions; no events are delivered afterward. The EOS SDK stays initialized across Client recreation and LÔVE restart. The ABI 2 shutdown hook calls `EOS_Shutdown` once, after all programs/registry callbacks are destroyed, before the extension is unloaded. A host that already owns EOS is deliberately rejected; borrowed-platform integration is not implemented.

Tests cover real SDK loading, offline platform creation/recreation, full/VM registration, and a deterministic fake backend for login continuation, token refresh, two-player lobby search/join, binary P2P packets, membership filtering, queue overflow and cleanup. The sample is also compiled by the test. **Real Steam login, EOS backend matchmaking and NAT/relay connectivity have not been verified**: they require a configured EOS deployment and two real accounts/devices. Mock tests do not validate those services.

Achievements, friends, invites, voice, custom lobby attributes, automatic matchmaking queues, Steam ticket acquisition, account linking and game-state synchronization are outside this first implementation.
