# EOS for L^

Steam認証・ロビー検索によるマッチング・P2P通信を提供するL^ネイティブ拡張。ゲーム状態の同期やロールバックはゲーム側で実装する。

[English](README.md)

## 必要なもの

- **拡張ABI 2**に対応したL^ホスト。今回追加した終了フックを含むlhat/lhat-loveをビルドする。旧ABI 1のホストではロードできない。
- ホストと一致するL^のソースcheckout（既定は`../lhat`）。CMakeがlhat本体だけを使うツールをビルドし、`lhat/version.h`も生成する。利用先と同じL^の版・ABI設定でビルドする。
- 別途取得したEOS SDK。**1.19.2.1 / Windows x64**で確認。`EOSSDK/SDK/Include`、`Lib`、`Bin`が存在する形で展開する。
- CMake 3.25以上、C/C++17コンパイラ、Python 3。LÔVEはサンプルの実行にのみ必要で、ビルド・テストには不要。

SDK本体・Steamworks SDK・認証情報・チケット・ビルド成果物はリポジトリに同梱しない。EOS SDKへはリンクするが、別コピーのlhatランタイムはリンクしない。

## ビルド

```powershell
.\scripts\build.ps1 -Test
```

DLLと、lhat本体だけをリンクする小さなツール`eos_lhat_host`をビルドする。このツールからEOSの署名表を出力して埋め込み、DLLを再ビルドする。`build/Release/eos_lhat.dll`をフル版・VM専用版の両方で使える。バインドやL^の更新後には作り直す。

配置が異なる場合は`-Lhat`、`-Sdk`、`-Build`を指定する。`-Test`はネイティブテストに加えてL^の統合テストも実行し、`LHAT_WITH_FRONTEND=OFF`のVM専用テストホストも自動ビルドする。Linux/macOS向けCMake設定もあるが、現段階では未ビルド・未検証。詳しいCMakeコマンドは英語版READMEを参照。`BUILD_TESTING=OFF`でテスト用DLLを除外できる。`cmake --install build --config Release --prefix dist`のインストール対象は本物の拡張DLLだけ。Windowsの配布物は **`eos_lhat.dll`のみ**。`.lib`、`.exp`、ツール、`eos_lhat_mock.dll`は配布しない。mockはEOSサービスへ接続せずに動作を再現する自動テスト用。利用者はEOSランタイムを別途用意する。

## GitHub Actions

`.github/workflows/release.yml`は`windows-2022`でWindows x64のReleaseをビルドする。LÔVEは不要。mainへのpushと手動実行で、ネイティブテスト・フル版／VM専用版の統合テスト・Programのrestartテストまで実行する。成功時のArtifact名は`eos-lhat-windows-x64`で、中身は`eos_lhat.dll`だけ。

`v*`タグをpushすると、同じ検証後にGitHub Releaseを作成し、DLLを添付する。タグのworkflowを再実行すると同じタグのDLLを差し替える。mainや手動実行ではReleaseを公開しない。GitHubが自動生成するソースアーカイブはDLLの添付ファイルとは別。

依存先は`lhat.rev`と`eos-sdk.rev`の40桁コミットIDで固定している。更新時は各リポジトリへpush済みのコミットを指定する。生成したDLLのL^の版・ABI設定は利用先のホストと一致させる。

SDKは`SAM-tak/eos-sdk-private`から`EOSSDK`へcheckoutする。**eos-lhatリポジトリ側**のSettings → Secrets and variables → Actionsに、Repository secretとして`EOS_SDK_READ_TOKEN`を登録する。トークンはSDKリポジトリだけを対象にしたfine-grained PATで、`Contents: Read-only`を付与する。SDKリポジトリ内には`SDK/Include`・`SDK/Lib`・`SDK/Bin`を配置する。pull requestでは実行せず、SDK・mock DLL・インポートライブラリ・ツールはアップロードしない。

## エディタ用の型情報

今回の `--extension` 対応を含むフル版lhat-cliなら、ゲームを起動せずEOSの定義をJSONへ追加できる。eos-lhatディレクトリから実行する例:

```powershell
$env:PATH = "$PWD\EOSSDK\SDK\Bin;$env:PATH"
..\lhat\build\msvc-release\lhat.exe --dump-host-api lhat-host.json --extension .\build\Release\eos_lhat.dll
```

このJSONにはCLIの標準ライブラリとEOSが含まれる。LÔVEのAPIも含める場合は、同じSDKの `PATH` 設定で、今回の対応を含むlovecを使う:

```powershell
..\lhat-love\build\love\Release\lovec.exe --dump-host-api lhat-host.json --extension .\build\Release\eos_lhat.dll
```

複数のDLLは `--extension` を繰り返して指定する。拡張を列挙したゲームを指定する `lovec --dump-host-api lhat-host.json path/to/game` も使える。明示的なDLL指定は非fused実行ファイルの型情報ダンプ専用。

## Steam認証の準備

EOS Developer PortalでProduct・Sandbox・Deploymentを作成し、SteamのIdentity Providerと、Connect/Lobby/P2Pの操作を許可するゲームクライアント用ポリシーを設定する。Steam App IDとチケットのIdentityも一致させる。

ゲームのSteam連携側で`ISteamUser::GetAuthTicketForWebApi("epiconlineservices")`を呼び、`GetTicketForWebApiResponse_t`の成功を待ち、返されたバイト列を16進文字列へ変換する。その文字列を`client.loginSteam(ticketHex)`へ渡す。IdentityはEOS側設定と一致させる。詳細は[Steamworks API](https://partner.steamgames.com/doc/api/ISteamUser#GetAuthTicketForWebApi)とSDKの`eos_common.h`内の`EOS_ECT_STEAM_SESSION_TICKET`を参照。

**チケット取得自体はこの拡張の担当外。** Steamの起動やSteamworksバインドは含まない。チケットの寿命はSteamworksの契約に従って管理する。`authExpired`通知を受けたら新しいチケットを取得し、同じClientで`loginSteam`を再実行する。ロビーは維持する。

初回ログインで`EOS_InvalidUser`になった場合は`EOS_Connect_CreateUser`へ進む。既存アカウントとのリンク機能は未実装。別アカウントへ切り替える場合はClientを作り直す。

開発用には`loginDevice(displayName)`も利用できる。同じ端末のDevice IDは同じユーザーに対応しうるため、実際の2人対戦には別端末・別アカウントを使う。

## LÔVEで試す

ゲームの`native/`にDLLを置き、`extensions.txt`へ次を記載する。

```text
native/eos_lhat
```

EOSのランタイムDLLもOSから解決可能な場所に必要。Windowsでは`EOSSDK-Win64-Shipping.dll`を`lovec.exe`やfusedゲーム実行ファイルの隣に置く。開発時はSDKの`Bin`を`PATH`に追加してもよい。任意のサブディレクトリにある拡張DLLの隣へ置くだけでは足りない。

`examples/love-p2p`は2人で番号付き入力パケットを送り合うサンプル。

1. `build/Release/eos_lhat.dll`を`examples/love-p2p/native/`へコピーする。
2. `config.example.lton`を`config.lton`へコピーし、EOSの設定を埋める。両端末でDeployment・socketName・bucketを揃え、書き込み可能なcacheDirectoryを指定する。
3. 一方を`host = true^`、もう一方を`false^`にする。
4. 各端末の別々のSteamアカウントでチケットを取得し、`steam-ticket.txt`に16進文字列を書く。**BOM・改行・空白は入れない。**
5. ホスト側を先に起動。もう一方はbucketでロビーを検索して参加する。0.5秒ごとにパケットを送り、受信内容を画面に表示する。
6. Sで再検索、Rで更新したチケットファイルを読み直して再認証、Escで退出して終了する。退出は最大5秒待つ。

```powershell
$env:PATH = "$PWD\EOSSDK\SDK\Bin;$env:PATH"
..\lhat-love\build\love\Release\lovec.exe --no-error-screen examples/love-p2p
```

チケットファイルは開発用の受け渡し手段。製品ではSteam連携側から直接渡す。

## API

```text
eos.create({ productId, sandboxId, deploymentId, clientId, clientSecret,
             cacheDirectory, socketName }) -> Client | Error
```

設定値はすべてstring。socketNameはEOSが許可する1～32文字。Clientは1つのPlatform・1人のProduct User・最大1つの参加ロビーを所有する。全Clientを同じスレッドで操作する。L^の別machineへのClient転送は許可しない。

| Clientのメソッド | 結果・用途 |
| --- | --- |
| `tick()` | `nil | Error`。毎フレーム呼んで非同期処理を進める |
| `poll()` | 通知レコード、空ならnil、失敗ならError |
| `loginSteam(hexTicket)` | リクエストIDまたはError。認証更新にも使う |
| `loginDevice(displayName)` | リクエストIDまたはError。開発用認証 |
| `createLobby(bucket, capacity)` | リクエストIDまたはError。公開ロビー、2～64人、ホスト移行有効 |
| `search(bucket, limit)` | リクエストIDまたはError。bucket一致、空き1以上、最大1～100件 |
| `results()` | 検索完了後のロビー配列、またはError |
| `join(lobbyId)` / `leave()` | リクエストIDまたはError。ID参加を許可したロビーが対象 |
| `userId()` | Product User ID文字列、未ログインは空、失敗ならError |
| `lobby()` | 現在のロビーレコード、またはError |
| `members()` | 自分を含むProduct User IDの配列、またはError |
| `send(peerId, data, channel, reliability)` | `nil | Error`。dataはバイナリ文字列、最大1170バイト、channelは0～255 |
| `receive()` | `{peerId, data, channel}`、空ならnil、失敗ならError |
| `disconnect(peerId)` | `nil | Error`。その後sendすれば再接続しうる |
| `relay(mode)` | `nil | Error`。`eos.Relay.never` / `allow`（SDK既定）/ `always` |
| `close()` | `nil | Error`。何度呼んでもよく、未完了操作とPlatformを破棄する |

送信の信頼性は`eos.Reliability.unreliable` / `reliableUnordered` / `reliableOrdered`。
ロビーレコードは`{id:string, owner:string, bucket:string, capacity:number, available:number}`。
配列添字は0起点。検索結果はスナップショットで、次の検索開始時に消える。

非同期操作は即座にリクエストIDを返す。対応する通知の`request`がそのIDになり、`ok`と`code`で成否を確認する。自発通知のrequestは0。通知には常に`lobbyId`と`peerId`も含まれ、該当しない場合は空文字列になる。`kind`は`eos.EventKind`のenum。

- 完了: `login`、`search`、`created`、`joined`、`left`。
- 変化: `memberJoined`、`memberLeft`、`ownerChanged`、`lobbyClosed`、`authExpired`、`loggedOut`、`peerClosed`。
- 診断: `overflow`、`internalError`。overflowのcodeは捨てた通知数。発生時は参加者一覧と未完了操作を再確認する。

完了通知のcodeはEOSの結果名。参加者変化とP2P切断ではSDKのstatus/reason数値文字列。同期的な失敗はmessageを持つ`eos.Error`。同時実行できる認証・ロビー変更・検索はそれぞれ1件まで。

毎フレームpollで通知を取り出す。キューは256件まで。receiveは別socketや退出済みユーザーのパケットを捨て、1回に最大64件を処理する。ゲーム側でも1フレームの受信回数に上限を設ける。

接続要求の受理・送信・受信は同じロビーの相手だけに制限する。退出やkickではP2P接続を閉じる。ホスト移行時のゲーム状態移送、ゲーム上の権限確認、不正対策はゲーム側の担当。send成功や信頼送信の成功は、相手のゲームが処理済みであることを意味しない。

## 終了と検証範囲

正常退出はleave後もtickを続け、完了してからcloseする。GC/disposeは後始末の保険であり、正常なロビー退出を保証しない。close後は未完了操作の通知は届かない。

Client再作成やLÔVE restartではEOS SDKの初期化状態を保持する。ABI 2の終了フックで、全Program・レジストリを破棄した後、DLLを解放する直前に`EOS_Shutdown`を1回だけ呼ぶ。別のホストが既にEOSを初期化している場合は拒否する。既存Platformを借りる連携は未実装。

テストは実SDKのロード・オフラインでのPlatform再作成、通常版/VM版のバインド、模擬SDKでの2クライアント通信・認証更新・参加者制限・通知上限・後始末、サンプルのコンパイルを対象とする。**実Steam認証・EOSサービス上のマッチング・NAT越え/リレーでの2台通信は未検証**。有効なEOS設定と2つの実アカウント/端末が必要で、模擬テストでは確認できない。

実績・フレンド・招待・ボイス・カスタムロビー属性・自動マッチングキュー・Steamチケット取得・アカウントリンク・ゲーム状態同期は初版の範囲外。

統合テストはLÔVEを使わず、フル版・VM専用版のL^で実行する。restartテストでは同じプロセス内でProgramとVMを3回作り直し、EOSの初期化・終了が各1回で、全3個のPlatformが解放されることを確認する。`examples/love-p2p`はそのまま残し、このテストの対象から外している。
