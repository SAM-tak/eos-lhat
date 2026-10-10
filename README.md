# Epic Online Service (EOS) for L^

A native L^ extension for lobby matchmaking and P2P multiplayer, authenticated with Steam or an Epic Games account.
The binding uses the EOS SDK's Auth, Connect, Lobby and P2P interfaces. It does not implement game-state replication or rollback.

[日本語](README.ja.md)

## Requirements

- An L^ host supporting **native extension ABI 2**, including the module shutdown hook. Build the accompanying lhat/lhat-love changes before using this extension; the earlier ABI 1 release cannot load it.
- The matching L^ source checkout (by default `../lhat`). CMake builds a small tooling host against this checkout and generates `lhat/version.h`; use the same L^ version and ABI settings as the target application.
- Epic Online Services SDK, downloaded separately. Tested against **1.19.2.1, Windows x64**. Unpack so `EOSSDK/SDK/Include`, `Lib` and `Bin` exist, or set `EOS_SDK_ROOT`.
- CMake 3.25+, C/C++17 compilers and Python 3. LÔVE is only needed to run the example, not to build or test the extension.

The repository contains no EOS SDK or Steamworks SDK. SDK installation directories, local credentials, tickets and build artifacts are ignored by Git. The extension links the EOS SDK but **does not link a second lhat runtime**.

## Build

```powershell
.\scripts\build.ps1 -Test
```

The script builds the DLL and `eos_lhat_host`, a small tool linked only to the L^ core. It uses this tool to generate EOS signatures, embeds them, and rebuilds the DLL. The resulting `build/Release/eos_lhat.dll` supports both full and VM-only hosts. Re-run after changing bindings or upgrading L^. Override `-Lhat`, `-Sdk`, or `-Build` for other layouts. `-Test` runs the native tests and the L^ integration suite, which builds a second tooling host with `LHAT_WITH_FRONTEND=OFF`.

Equivalent CMake commands (platform SDK libraries must be available):

```sh
cmake -S . -B build -DEOS_SDK_ROOT=/path/to/SDK \
  -DLHAT_ROOT=/path/to/lhat -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
python scripts/embed-signatures.py --sdk /path/to/SDK
ctest --test-dir build -C Release --output-on-failure
python tests/test_lhat.py --lhat /path/to/lhat --sdk /path/to/SDK
```

Linux and macOS use the CMake/Python commands above. Set `BUILD_TESTING=OFF` for a production-only build. `cmake --install build --config Release --prefix dist` installs only the real extension. On Windows, the release artifact is **`eos_lhat.dll` only**; `.lib`, `.exp`, tooling executables and `eos_lhat_mock.dll` are not distributed. The mock replaces EOS for deterministic tests and cannot connect to real EOS services. Users supply the EOS runtime separately. For Linux/macOS deployment, put `libEOSSDK-Linux-Shipping.so` / `libEOSSDK-Mac-Shipping.dylib` next to the installed extension; its runtime search path is `$ORIGIN` / `@loader_path`. macOS builds are universal by default; pass `-DEOS_LHAT_MACOS_UNIVERSAL=OFF` for the host CPU only. Build/test tools also set `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH` when using the SDK directory directly.

## GitHub Actions

`.github/workflows/release.yml` builds these Release configurations without LÔVE:

| Platform | Runner | Artifact | Library inside |
| --- | --- | --- | --- |
| Windows x64 | `windows-2022` | `eos-lhat-windows-x64` | `eos_lhat.dll` |
| Linux x64 | `ubuntu-22.04` | `eos-lhat-linux-x64` | `eos_lhat.so` |
| macOS Apple Silicon | `macos-15` | `eos-lhat-macos-arm64` | `eos_lhat.dylib` (universal) |
| macOS Intel | `macos-15-intel` | `eos-lhat-macos-x64` | `eos_lhat.dylib` (universal) |

Pushes to `main` and manual runs execute native tests plus full/VM-only L^ integration tests, including Program restarts, on every platform. The installed library is then relocated and tested again with the EOS runtime beside it. Each artifact contains only the extension library.

Pushing a `v*` tag publishes the libraries only after all four builds and tests pass. Release assets are `eos_lhat.dll`, `eos_lhat.so` and `eos_lhat.dylib`, so `native/eos_lhat` in `extensions.txt` works unchanged on every platform. The macOS library is a universal binary (arm64 and x86_64), like the EOS SDK's dylib; both macOS jobs load and test it natively. Re-running the tagged workflow replaces those assets. Main-branch and manual runs do not publish Releases. GitHub's automatic source archives are separate from these binary assets.

Dependencies are pinned by the 40-character commit IDs in `lhat.rev` and `eos-sdk.rev`. Update these files when upgrading L^ or the SDK, using commits already pushed to the corresponding repositories. The generated extension must match the consuming host's L^ version and ABI configuration.

The workflow checks out `SAM-tak/eos-sdk-private` into `EOSSDK`. Configure **`EOS_SDK_READ_TOKEN` in the eos-lhat repository's Actions repository secrets**, using a fine-grained token with `Contents: Read-only` access to that SDK repository. The SDK repository must contain `SDK/Include`, `SDK/Lib`, and `SDK/Bin`. The workflow does not run on pull requests and does not upload SDK files, mock DLLs, import libraries or tooling executables.

## Editor type information

With the full lhat CLI containing `--extension` support, run from this repository:

```powershell
$env:PATH = "$PWD\EOSSDK\SDK\Bin;$env:PATH"
..\lhat\build\release\lhat.exe --dump-host-api lhat-host.json --extension .\build\Release\eos_lhat.dll
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

## Epic Games account authentication

In the Developer Portal, create an Epic Account Services application with the **Basic Profile** permission, link it to the product's client, and add the **Epic Games** identity provider to the deployment. The extension requests only Basic Profile, signs in with EOS Auth, then logs in to Connect with the account's ID token. A Product User is created on first login, as with Steam.

- `loginEpic(false)` signs in silently with the refresh token the SDK stored on this device; it fails when none is stored or the user must interact.
- `loginEpic(true)` does the same, then falls back to the Account Portal login UI unless the failure was a network error, timeout or cancellation. A successful portal login stores the refresh token for the next run.
- `loginEpicExchange(code)` consumes the one-time exchange code the Epic Games Launcher passes as `-AUTH_PASSWORD=<code>` (with `-AUTH_TYPE=exchangecode`). Parse the command line in the game; use the code promptly, once.
- `loginEpicDeveloper(host, credentialName)` uses the SDK's Developer Authentication Tool, e.g. `loginEpicDeveloper("localhost:6547", "Player1")`. Development only.
- `forgetEpic()` deletes the stored refresh token so the next interactive login shows the portal (for "switch account"). It requires a new client that has not logged in; the result is an `epicForgotten` event.

While the Epic account remains signed in, every `loginEpic*` call only copies a fresh ID token and repeats the Connect login. On `authExpired`, call `loginEpic(false)` on the same client; the lobby is kept.

The overlay is disabled (`EOS_PF_DISABLE_OVERLAY`). On Windows, the SDK documents that Account Portal login requires starting the game through the EOS Bootstrapper with the EOS redistributable service installed; Epic Games Store builds normally use the exchange code instead. **Real Epic logins, including the portal's behaviour without the overlay, have not been verified**; the tests use a fake SDK.

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
| `tick()` | `nil \| Error`; call once per frame to drive EOS callbacks |
| `poll()` | Event record, `nil` when empty, or Error |
| `loginSteam(hexTicket)` | Request ID or Error; also refreshes credentials |
| `loginDevice(displayName)` | Request ID or Error; development login |
| `loginEpic(interactive)` | Request ID or Error; persistent Epic login, optionally falling back to the Account Portal |
| `loginEpicExchange(code)` | Request ID or Error; Epic Games Launcher exchange code |
| `loginEpicDeveloper(host, credentialName)` | Request ID or Error; Developer Authentication Tool |
| `forgetEpic()` | Request ID or Error; deletes the stored Epic refresh token |
| `createLobby(bucket, capacity)` | Request ID or Error; public lobby, capacity 2–64, host migration enabled |
| `search(bucket, limit)` | Request ID or Error; bucket equality and at least one free slot, limit 1–100 |
| `results()` | Array of lobby records or Error; available after search completion |
| `join(lobbyId)` | Request ID or Error; lobbies must allow joining by ID |
| `leave()` | Request ID or Error |
| `userId()` | Product User ID string; empty before login, or Error |
| `lobby()` | Current lobby record or Error |
| `members()` | Array of Product User ID strings (including self), or Error |
| `send(peerId, data, channel, reliability)` | `nil \| Error`; `data` is a binary-safe `string^` or a `std.binary.Bytes` (two overloads), maximum 1170 bytes, channel 0–255 |
| `receive()` | `{peerId, data, channel}`, `nil` when empty, or Error |
| `receiveInto(bytes)` | `(bytes \| nil, peerId, channel) \| Error`; overwrites a reused `std.binary.Bytes` without allocating a record; `nil, "", 0` when empty |
| `disconnect(peerId)` | `nil \| Error`; a subsequent send may reconnect |
| `relay(mode)` | `nil \| Error`; `eos.Relay.never`, `.allow` (SDK default), `.always` |
| `close()` | `nil \| Error`; idempotent, cancels outstanding operations and releases this platform |

The `Bytes` overloads of `send` and `receiveInto` exist only when the host registers `std.binary` before loading extensions; otherwise the client has the `string^` methods alone. Using a disposed `Bytes` panics, as in `std.binary`.

Reliability values: `eos.Reliability.unreliable`, `.reliableUnordered`, `.reliableOrdered`.
Lobby records: `{id:string, owner:string, bucket:string, capacity:number, available:number}`.
Arrays use zero-based L^ indexes. Search results are snapshots and reset at the next search.

Operations return a request ID immediately. Their Event has the matching `request`, `ok`, and EOS result name in `code`; unsolicited notifications have request 0. Events also contain `lobbyId` and `peerId` strings (empty when inapplicable). `kind` is an `eos.EventKind`:

- Completions: `login`, `search`, `created`, `joined`, `left`, `epicForgotten`.
- Notifications: `memberJoined`, `memberLeft`, `ownerChanged`, `lobbyClosed`, `authExpired`, `loggedOut`, `peerClosed`.
- Diagnostics: `overflow`, `internalError`. Overflow reports the number of dropped notifications in `code`. Resynchronize membership and pending operations if it occurs.

Membership and peer-close notifications use their numeric SDK status/reason in `code`. Synchronous validation/SDK failures return `eos.Error` with a message. Only one login (including `forgetEpic`), one lobby mutation and one search may be outstanding at a time. Search and lobby operations are otherwise independent.

Poll all events each frame. The notification queue is bounded to 256 records. `receive` discards packets for other sockets/former members, processing at most 64 queued packets per call. Limit receive calls per frame in games exposed to sustained traffic.

Only current lobby peers can be sent to or accepted; disconnecting/kicking members closes their P2P connection. This is membership filtering, not game authority or anti-cheat. Host migration changes the owner notification; the game must migrate its own simulation state. Neither `send` success nor reliable delivery means the remote game consumed a packet.

## Lifetime and testing

Call `leave()` and keep ticking until completion for graceful departure, then `close()`. GC/disposal is a fallback, not an orderly lobby exit. Closing a Client cancels pending completions; no events are delivered afterward. The EOS SDK stays initialized across Client recreation and LÔVE restart. The ABI 2 shutdown hook calls `EOS_Shutdown` once, after all programs/registry callbacks are destroyed, before the extension is unloaded. A host that already owns EOS is deliberately rejected; borrowed-platform integration is not implemented.

Tests cover real SDK loading, offline platform creation/recreation, full/VM registration, and a deterministic fake backend for login continuation, token refresh, Epic persistent/portal/exchange/developer login, two-player lobby search/join, binary P2P packets, membership filtering, queue overflow and cleanup. The restart test destroys and recreates the entire L^ Program and VM three times in one process while retaining the extension. It checks one SDK initialization/shutdown and the release of all three platforms. The LÔVE example is kept separately and is not part of this L^-only suite. **Real Steam login, EOS backend matchmaking and NAT/relay connectivity have not been verified**: they require a configured EOS deployment and two real accounts/devices. Mock tests do not validate those services.

Achievements, friends, invites, voice, custom lobby attributes, automatic matchmaking queues, Steam ticket acquisition, account linking and game-state synchronization are outside this first implementation.
