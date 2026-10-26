#include "client.h"
#include <eos_init.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <mutex>
#if defined(_WIN32) && defined(EOS_LHAT_SDK_DLL)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace eos_lhat {
namespace {
#if defined(_WIN32) && defined(EOS_LHAT_SDK_DLL)
// The SDK is delay-loaded. Load the copy beside this extension first, so hosts
// need not put it on their own search path; the delay-load helper then binds to
// the already loaded module. Fall back to the default search order (e.g. PATH).
void loadSdk() {
    static const HMODULE sdk = [] {
        const std::wstring name(EOS_LHAT_SDK_DLL, EOS_LHAT_SDK_DLL + std::strlen(EOS_LHAT_SDK_DLL));
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&loadSdk), &self)) {
            std::wstring path(MAX_PATH, L'\0');
            for (;;) {
                const DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
                if (n == 0) {
                    path.clear();
                    break;
                }
                if (n < path.size()) {
                    path.resize(n);
                    break;
                }
                path.resize(path.size() * 2);
            }
            if (!path.empty()) {
                const auto beside = std::filesystem::path(path).replace_filename(name);
                if (HMODULE m = LoadLibraryExW(beside.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
                    return m;
            }
        }
        return LoadLibraryW(name.c_str());
    }();
    if (!sdk)
        throw Failure("Could not load " EOS_LHAT_SDK_DLL "; place it beside the eos_lhat library");
}
#endif
std::mutex runtimeMutex;
size_t runtimeUsers = 0;
bool initialized = false, terminated = false;
std::thread::id runtimeThread;
void acquireRuntime() {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    if (terminated)
        throw Failure("EOS SDK has already shut down");
    if (initialized && runtimeThread != std::this_thread::get_id())
        throw Failure("All EOS clients must run on the same host thread");
    if (!initialized) {
#if defined(_WIN32) && defined(EOS_LHAT_SDK_DLL)
        loadSdk();
#endif
        EOS_InitializeOptions o{};
        o.ApiVersion = EOS_INITIALIZE_API_LATEST;
        o.ProductName = "eos-lhat";
        o.ProductVersion = "0.1.5";
        // An externally owned SDK needs a separate, explicit integration contract.
        require(EOS_Initialize(&o), "EOS_Initialize");
        initialized = true;
        runtimeThread = std::this_thread::get_id();
    }
    ++runtimeUsers;
}
void releaseRuntime() {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    --runtimeUsers;
}
void text(const std::string &s, const char *name, size_t max = 256) {
    if (s.empty() || s.size() > max || s.find('\0') != std::string::npos)
        throw Failure(std::string("Invalid ") + name);
}
struct Details {
    EOS_HLobbyDetails value;
    ~Details() {
        if (value)
            EOS_LobbyDetails_Release(value);
    }
};
} // namespace
void shutdownRuntime() noexcept {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    if (initialized && !terminated && runtimeUsers == 0) {
        EOS_Shutdown();
        terminated = true;
    }
}
void require(EOS_EResult r, const char *operation) {
    if (r != EOS_EResult::EOS_Success)
        throw Failure(std::string(operation) + ": " + EOS_EResult_ToString(r));
}
std::string userText(EOS_ProductUserId user) {
    if (!user)
        return {};
    char data[EOS_PRODUCTUSERID_MAX_LENGTH + 1];
    int32_t size = sizeof(data);
    require(EOS_ProductUserId_ToString(user, data, &size), "ProductUserId");
    return data;
}
Client::Client(const Config &c) : config_(c), thread_(std::this_thread::get_id()) {
    text(c.productId, "productId", 64);
    text(c.sandboxId, "sandboxId", 64);
    text(c.deploymentId, "deploymentId", 64);
    text(c.clientId, "clientId", 64);
    text(c.clientSecret, "clientSecret", 64);
    text(c.cacheDirectory, "cacheDirectory", 4096);
    text(c.socketName, "socketName", 32);
    if (c.socketName.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_ +=.") !=
        std::string::npos)
        throw Failure("Invalid socketName characters");
    auto cache = std::filesystem::absolute(std::filesystem::u8path(c.cacheDirectory));
    std::filesystem::create_directories(cache);
    config_.cacheDirectory = cache.u8string();
    socket_.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
    std::memcpy(socket_.SocketName, c.socketName.c_str(), c.socketName.size() + 1);
    acquireRuntime();
    runtime_ = true;
    try {
        EOS_Platform_Options o{};
        o.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
        o.ProductId = config_.productId.c_str();
        o.SandboxId = config_.sandboxId.c_str();
        o.DeploymentId = config_.deploymentId.c_str();
        o.ClientCredentials = {config_.clientId.c_str(), config_.clientSecret.c_str()};
        o.CacheDirectory = config_.cacheDirectory.c_str();
        o.Flags = EOS_PF_DISABLE_OVERLAY;
        o.TickBudgetInMilliseconds = 2;
        platform_ = EOS_Platform_Create(&o);
        if (!platform_)
            throw Failure("EOS_Platform_Create failed");
        auth_ = EOS_Platform_GetAuthInterface(platform_);
        connect_ = EOS_Platform_GetConnectInterface(platform_);
        lobbies_ = EOS_Platform_GetLobbyInterface(platform_);
        p2p_ = EOS_Platform_GetP2PInterface(platform_);
        if (!auth_ || !connect_ || !lobbies_ || !p2p_)
            throw Failure("EOS interface unavailable");
        EOS_Connect_AddNotifyAuthExpirationOptions auth{
            EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST};
        authNotify_ = EOS_Connect_AddNotifyAuthExpiration(
            connect_, &auth, this, [](const EOS_Connect_AuthExpirationCallbackInfo *i) {
                callback(i->ClientData, [&](Client &c) {
                    if (i->LocalUserId == c.user_)
                        c.emit(
                            {EventKind::AuthExpired, 0, true, "", c.lobbyId_, userText(c.user_)});
                });
            });
        EOS_Connect_AddNotifyLoginStatusChangedOptions status{
            EOS_CONNECT_ADDNOTIFYLOGINSTATUSCHANGED_API_LATEST};
        statusNotify_ = EOS_Connect_AddNotifyLoginStatusChanged(
            connect_, &status, this, [](const EOS_Connect_LoginStatusChangedCallbackInfo *i) {
                callback(i->ClientData, [&](Client &c) {
                    if (i->LocalUserId == c.user_ &&
                        i->CurrentStatus == EOS_ELoginStatus::EOS_LS_NotLoggedIn) {
                        c.closePeers();
                        c.removePeerNotifications();
                        c.user_ = nullptr;
                        c.lobbyId_.clear();
                        c.emit({EventKind::LoggedOut});
                    }
                });
            });
        EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions member{
            EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST};
        memberNotify_ = EOS_Lobby_AddNotifyLobbyMemberStatusReceived(
            lobbies_, &member, this, [](const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo *i) {
                callback(i->ClientData, [&](Client &c) {
                    if (c.lobbyId_ != i->LobbyId)
                        return;
                    auto kind = i->CurrentStatus == EOS_ELobbyMemberStatus::EOS_LMS_JOINED
                                    ? EventKind::MemberJoined
                                : i->CurrentStatus == EOS_ELobbyMemberStatus::EOS_LMS_PROMOTED
                                    ? EventKind::OwnerChanged
                                    : EventKind::MemberLeft;
                    if (kind == EventKind::MemberLeft) {
                        if (i->TargetUserId == c.user_) {
                            c.closePeers();
                            c.lobbyId_.clear();
                            kind = EventKind::LobbyClosed;
                        } else {
                            EOS_P2P_CloseConnectionOptions o{EOS_P2P_CLOSECONNECTION_API_LATEST,
                                                             c.user_, i->TargetUserId, &c.socket_};
                            EOS_P2P_CloseConnection(c.p2p_, &o);
                        }
                    }
                    c.emit({kind, 0, true, std::to_string(int(i->CurrentStatus)), i->LobbyId,
                            userText(i->TargetUserId)});
                });
            });
        if (!authNotify_ || !statusNotify_ || !memberNotify_)
            throw Failure("EOS notification registration failed");
    } catch (...) {
        close();
        throw;
    }
}
Client::~Client() {
    close();
}
void Client::check(bool loggedIn) const {
    if (thread_ != std::this_thread::get_id())
        throw Failure("EOS client used from another thread");
    if (!platform_ || closing_)
        throw Failure("EOS client is closed");
    if (loggedIn && !user_)
        throw Failure("Connect login required");
}
void Client::close() {
    if (closing_)
        return;
    closing_ = true;
    if (search_) {
        EOS_LobbySearch_Release(search_);
        search_ = nullptr;
    }
    if (platform_) {
        closePeers();
        removePeerNotifications();
        if (authNotify_)
            EOS_Connect_RemoveNotifyAuthExpiration(connect_, authNotify_);
        if (statusNotify_)
            EOS_Connect_RemoveNotifyLoginStatusChanged(connect_, statusNotify_);
        if (memberNotify_)
            EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived(lobbies_, memberNotify_);
        // Context stays alive while the platform cancels pending callbacks.
        EOS_Platform_Release(platform_);
        platform_ = nullptr;
    }
    events_.clear();
    results_.clear();
    user_ = nullptr;
    epic_ = nullptr;
    lobbyId_.clear();
    if (runtime_) {
        releaseRuntime();
        runtime_ = false;
    }
}
void Client::tick() {
    check();
    EOS_Platform_Tick(platform_);
}
void Client::emit(Event event) {
    if (events_.size() >= 256) {
        ++dropped_;
        return;
    }
    events_.push_back(std::move(event));
}
std::optional<Event> Client::poll() {
    check();
    if (dropped_) {
        Event e{EventKind::Overflow, 0, false, std::to_string(dropped_)};
        dropped_ = 0;
        return e;
    }
    if (events_.empty())
        return {};
    Event e = std::move(events_.front());
    events_.pop_front();
    return e;
}
void Client::finishLogin(EOS_EResult code, EOS_ProductUserId user) {
    auto request = loginRequest_;
    loginRequest_ = 0;
    if (code == EOS_EResult::EOS_Success) {
        if (user_ && user_ != user) {
            emit({EventKind::Login, request, false, "Account switch requires a new client"});
            return;
        }
        user_ = user;
        try {
            installPeerNotifications();
        } catch (...) {
            user_ = nullptr;
            removePeerNotifications();
            emit({EventKind::Login, request, false, "Peer notification registration failed"});
            return;
        }
    }
    emit({EventKind::Login, request, code == EOS_EResult::EOS_Success, EOS_EResult_ToString(code),
          "", userText(user_)});
}
void Client::connectLogin(EOS_EExternalCredentialType type, const char *token) {
    EOS_Connect_Credentials credentials{EOS_CONNECT_CREDENTIALS_API_LATEST, token, type};
    EOS_Connect_UserLoginInfo info{EOS_CONNECT_USERLOGININFO_API_LATEST, loginName_.c_str(),
                                   nullptr};
    EOS_Connect_LoginOptions o{
        EOS_CONNECT_LOGIN_API_LATEST, &credentials,
        type == EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN ? &info : nullptr};
    EOS_Connect_Login(connect_, &o, this, [](const EOS_Connect_LoginCallbackInfo *i) {
        callback(i->ClientData, [&](Client &c) {
            if (i->ResultCode == EOS_EResult::EOS_InvalidUser && i->ContinuanceToken && !c.user_) {
                EOS_Connect_CreateUserOptions o{EOS_CONNECT_CREATEUSER_API_LATEST,
                                                i->ContinuanceToken};
                EOS_Connect_CreateUser(
                    c.connect_, &o, &c, [](const EOS_Connect_CreateUserCallbackInfo *j) {
                        callback(j->ClientData,
                                 [&](Client &d) { d.finishLogin(j->ResultCode, j->LocalUserId); });
                    });
            } else
                c.finishLogin(i->ResultCode, i->LocalUserId);
        });
    });
}
uint64_t Client::loginSteam(const std::string &ticket) {
    check();
    text(ticket, "Steam hex session ticket", 16384);
    if (ticket.size() % 2 ||
        ticket.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        throw Failure("Steam ticket must be an even-length hexadecimal string");
    if (loginRequest_)
        throw Failure("Login already pending");
    auto request = loginRequest_ = nextRequest_++;
    connectLogin(EOS_EExternalCredentialType::EOS_ECT_STEAM_SESSION_TICKET, ticket.c_str());
    return request;
}
uint64_t Client::loginDevice(const std::string &name) {
    check();
    text(name, "display name", 32);
    if (loginRequest_)
        throw Failure("Login already pending");
    if (user_ && !deviceLogin_)
        throw Failure("Device login cannot switch an authenticated account");
    loginName_ = name;
    auto request = loginRequest_ = nextRequest_++;
    if (user_) {
        // Refresh Connect credentials without recreating the Device ID or lobby.
        connectLogin(EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN, nullptr);
        return request;
    }
    deviceLogin_ = true;
    EOS_Connect_CreateDeviceIdOptions o{EOS_CONNECT_CREATEDEVICEID_API_LATEST, "eos-lhat desktop"};
    EOS_Connect_CreateDeviceId(
        connect_, &o, this, [](const EOS_Connect_CreateDeviceIdCallbackInfo *i) {
            callback(i->ClientData, [&](Client &c) {
                if (i->ResultCode == EOS_EResult::EOS_Success ||
                    i->ResultCode == EOS_EResult::EOS_DuplicateNotAllowed)
                    c.connectLogin(EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN,
                                   nullptr);
                else
                    c.finishLogin(i->ResultCode, nullptr);
            });
        });
    return request;
}
uint64_t Client::beginEpic() {
    if (loginRequest_)
        throw Failure("Login already pending");
    auto request = loginRequest_ = nextRequest_++;
    // A signed-in Epic account only needs a fresh ID token, e.g. after authExpired.
    if (epic_ && EOS_Auth_GetLoginStatus(auth_, epic_) == EOS_ELoginStatus::EOS_LS_LoggedIn)
        connectEpic();
    else
        epic_ = nullptr;
    return request;
}
uint64_t Client::loginEpic(bool interactive) {
    check();
    auto request = beginEpic();
    if (!epic_)
        authLogin(EOS_ELoginCredentialType::EOS_LCT_PersistentAuth, nullptr, nullptr, interactive);
    return request;
}
uint64_t Client::loginEpicExchange(const std::string &code) {
    check();
    text(code, "exchange code", 1024);
    auto request = beginEpic();
    if (!epic_)
        authLogin(EOS_ELoginCredentialType::EOS_LCT_ExchangeCode, nullptr, code.c_str(), false);
    return request;
}
uint64_t Client::loginEpicDeveloper(const std::string &host, const std::string &name) {
    check();
    text(host, "developer auth host");
    text(name, "developer credential name");
    auto request = beginEpic();
    if (!epic_)
        authLogin(EOS_ELoginCredentialType::EOS_LCT_Developer, host.c_str(), name.c_str(), false);
    return request;
}
void Client::authLogin(EOS_ELoginCredentialType type, const char *id, const char *token,
                       bool fallback) {
    EOS_Auth_Credentials credentials{};
    credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
    credentials.Id = id;
    credentials.Token = token;
    credentials.Type = type;
    EOS_Auth_LoginOptions o{};
    o.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
    o.Credentials = &credentials;
    // Basic Profile is the one permission every Epic Account Services client must enable.
    o.ScopeFlags = EOS_EAuthScopeFlags::EOS_AS_BasicProfile;
    // Automatic login must stay silent; only the portal may show a login UI.
    if (type == EOS_ELoginCredentialType::EOS_LCT_PersistentAuth)
        o.LoginFlags = EOS_LF_NO_USER_INTERFACE;
    epicFallback_ = fallback;
    EOS_Auth_Login(auth_, &o, this, [](const EOS_Auth_LoginCallbackInfo *i) {
        callback(i->ClientData, [&](Client &c) {
            auto fallback = c.epicFallback_;
            c.epicFallback_ = false;
            if (i->ResultCode == EOS_EResult::EOS_Success) {
                c.epic_ = i->LocalUserId;
                c.displayName_.clear();
                auto users = EOS_Platform_GetUserInfoInterface(c.platform_);
                EOS_UserInfo_QueryUserInfoOptions query{EOS_USERINFO_QUERYUSERINFO_API_LATEST,
                                                       c.epic_, c.epic_};
                EOS_UserInfo_QueryUserInfo(users, &query, &c,
                    [](const EOS_UserInfo_QueryUserInfoCallbackInfo *result) {
                        callback(result->ClientData, [&](Client &self) {
                            if (result->ResultCode == EOS_EResult::EOS_Success) {
                                EOS_UserInfo_CopyUserInfoOptions options{
                                    EOS_USERINFO_COPYUSERINFO_API_LATEST, self.epic_, self.epic_};
                                EOS_UserInfo *info = nullptr;
                                auto handle = EOS_Platform_GetUserInfoInterface(self.platform_);
                                if (EOS_UserInfo_CopyUserInfo(handle, &options, &info) ==
                                        EOS_EResult::EOS_Success && info) {
                                    if (info->DisplayName)
                                        self.displayName_ = info->DisplayName;
                                    EOS_UserInfo_Release(info);
                                }
                            }
                            // Profile lookup failure must not prevent authentication.
                            self.connectEpic();
                        });
                    });
            } else if (fallback && i->ResultCode != EOS_EResult::EOS_NoConnection &&
                       i->ResultCode != EOS_EResult::EOS_TimedOut &&
                       i->ResultCode != EOS_EResult::EOS_Canceled)
                c.authLogin(EOS_ELoginCredentialType::EOS_LCT_AccountPortal, nullptr, nullptr,
                            false);
            else
                c.finishLogin(i->ResultCode, nullptr);
        });
    });
}
void Client::connectEpic() {
    EOS_Auth_CopyIdTokenOptions o{EOS_AUTH_COPYIDTOKEN_API_LATEST, epic_};
    EOS_Auth_IdToken *raw = nullptr;
    auto r = EOS_Auth_CopyIdToken(auth_, &o, &raw);
    if (r != EOS_EResult::EOS_Success) {
        finishLogin(r, nullptr);
        return;
    }
    std::unique_ptr<EOS_Auth_IdToken, decltype(&EOS_Auth_IdToken_Release)> token(
        raw, EOS_Auth_IdToken_Release);
    connectLogin(EOS_EExternalCredentialType::EOS_ECT_EPIC_ID_TOKEN, token->JsonWebToken);
}
std::string Client::displayName() const {
    check();
    return displayName_;
}
uint64_t Client::forgetEpic() {
    check();
    if (user_ || epic_ || loginRequest_)
        throw Failure("Forgetting the Epic login requires a new, idle client");
    auto request = loginRequest_ = nextRequest_++;
    EOS_Auth_DeletePersistentAuthOptions o{EOS_AUTH_DELETEPERSISTENTAUTH_API_LATEST, nullptr};
    EOS_Auth_DeletePersistentAuth(
        auth_, &o, this, [](const EOS_Auth_DeletePersistentAuthCallbackInfo *i) {
            callback(i->ClientData, [&](Client &c) {
                auto request = c.loginRequest_;
                c.loginRequest_ = 0;
                c.emit({EventKind::EpicForgotten, request, i->ResultCode == EOS_EResult::EOS_Success,
                        EOS_EResult_ToString(i->ResultCode)});
            });
        });
    return request;
}
void Client::idleLobby(bool joined) const {
    check(true);
    if (lobbyRequest_)
        throw Failure("Lobby operation already pending");
    if (joined == lobbyId_.empty())
        throw Failure(joined ? "Join a lobby first" : "Leave the current lobby first");
}
uint64_t Client::createLobby(const std::string &bucket, uint32_t capacity) {
    idleLobby(false);
    text(bucket, "bucket", 60);
    if (capacity < 2 || capacity > 64)
        throw Failure("Lobby capacity must be 2..64");
    EOS_Lobby_CreateLobbyOptions o{};
    o.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
    o.LocalUserId = user_;
    o.MaxLobbyMembers = capacity;
    o.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
    o.BucketId = bucket.c_str();
    o.bEnableJoinById = EOS_TRUE;
    auto request = lobbyRequest_ = nextRequest_++;
    EOS_Lobby_CreateLobby(lobbies_, &o, this, [](const EOS_Lobby_CreateLobbyCallbackInfo *i) {
        callback(i->ClientData, [&](Client &c) {
            auto request = c.lobbyRequest_;
            c.lobbyRequest_ = 0;
            if (i->ResultCode == EOS_EResult::EOS_Success)
                c.lobbyId_ = i->LobbyId;
            c.emit({EventKind::Created, request, i->ResultCode == EOS_EResult::EOS_Success,
                    EOS_EResult_ToString(i->ResultCode), c.lobbyId_});
        });
    });
    return request;
}
uint64_t Client::join(const std::string &id) {
    idleLobby(false);
    text(id, "lobby id");
    EOS_Lobby_JoinLobbyByIdOptions o{};
    o.ApiVersion = EOS_LOBBY_JOINLOBBYBYID_API_LATEST;
    o.LocalUserId = user_;
    o.LobbyId = id.c_str();
    auto request = lobbyRequest_ = nextRequest_++;
    EOS_Lobby_JoinLobbyById(lobbies_, &o, this, [](const EOS_Lobby_JoinLobbyByIdCallbackInfo *i) {
        callback(i->ClientData, [&](Client &c) {
            auto request = c.lobbyRequest_;
            c.lobbyRequest_ = 0;
            if (i->ResultCode == EOS_EResult::EOS_Success)
                c.lobbyId_ = i->LobbyId;
            c.emit({EventKind::Joined, request, i->ResultCode == EOS_EResult::EOS_Success,
                    EOS_EResult_ToString(i->ResultCode), c.lobbyId_});
        });
    });
    return request;
}
uint64_t Client::leave() {
    idleLobby(true);
    EOS_Lobby_LeaveLobbyOptions o{EOS_LOBBY_LEAVELOBBY_API_LATEST, user_, lobbyId_.c_str()};
    auto request = lobbyRequest_ = nextRequest_++;
    EOS_Lobby_LeaveLobby(lobbies_, &o, this, [](const EOS_Lobby_LeaveLobbyCallbackInfo *i) {
        callback(i->ClientData, [&](Client &c) {
            auto request = c.lobbyRequest_;
            c.lobbyRequest_ = 0;
            if (i->ResultCode == EOS_EResult::EOS_Success) {
                c.closePeers();
                c.lobbyId_.clear();
            }
            c.emit({EventKind::Left, request, i->ResultCode == EOS_EResult::EOS_Success,
                    EOS_EResult_ToString(i->ResultCode), i->LobbyId ? i->LobbyId : ""});
        });
    });
    return request;
}
LobbyInfo Client::info(EOS_HLobbyDetails details) {
    EOS_LobbyDetails_CopyInfoOptions o{EOS_LOBBYDETAILS_COPYINFO_API_LATEST};
    EOS_LobbyDetails_Info *raw = nullptr;
    require(EOS_LobbyDetails_CopyInfo(details, &o, &raw), "Lobby info");
    std::unique_ptr<EOS_LobbyDetails_Info, decltype(&EOS_LobbyDetails_Info_Release)> holder(
        raw, EOS_LobbyDetails_Info_Release);
    LobbyInfo result{raw->LobbyId, userText(raw->LobbyOwnerUserId), raw->BucketId ? raw->BucketId : "",
                     raw->MaxMembers, raw->AvailableSlots};
    for (auto key : {"vp_name", "vp_locked"}) {
        EOS_LobbyDetails_CopyAttributeByKeyOptions attrOptions{EOS_LOBBYDETAILS_COPYATTRIBUTEBYKEY_API_LATEST, key};
        EOS_Lobby_Attribute *attr = nullptr;
        if (EOS_LobbyDetails_CopyAttributeByKey(details, &attrOptions, &attr) == EOS_EResult::EOS_Success && attr) {
            if (attr->Data && std::string(key) == "vp_name" && attr->Data->ValueType == EOS_EAttributeType::EOS_AT_STRING)
                result.name = attr->Data->Value.AsUtf8 ? attr->Data->Value.AsUtf8 : "";
            if (attr->Data && std::string(key) == "vp_locked" && attr->Data->ValueType == EOS_EAttributeType::EOS_AT_BOOLEAN)
                result.locked = attr->Data->Value.AsBool == EOS_TRUE;
            EOS_Lobby_Attribute_Release(attr);
        }
    }
    return result;
}
uint64_t Client::setRoom(const std::string &name, bool locked) {
    check(true);
    text(name, "room name", 96);
    if (lobbyId_.empty() || lobby().owner != userId()) throw Failure("Only the lobby owner can set the room name");
    if (roomRequest_) throw Failure("Room update already pending");
    EOS_HLobbyModification mod = nullptr;
    EOS_Lobby_UpdateLobbyModificationOptions options{EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST, user_, lobbyId_.c_str()};
    require(EOS_Lobby_UpdateLobbyModification(lobbies_, &options, &mod), "Modify room");
    try {
        EOS_Lobby_AttributeData data{};
        data.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
        data.Key = "vp_name";
        data.ValueType = EOS_EAttributeType::EOS_AT_STRING;
        data.Value.AsUtf8 = name.c_str();
        EOS_LobbyModification_AddAttributeOptions add{EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST, &data, EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC};
        require(EOS_LobbyModification_AddAttribute(mod, &add), "Set room name");
        data.Key = "vp_locked";
        data.ValueType = EOS_EAttributeType::EOS_AT_BOOLEAN;
        data.Value.AsBool = locked ? EOS_TRUE : EOS_FALSE;
        require(EOS_LobbyModification_AddAttribute(mod, &add), "Set room lock flag");
        auto request = roomRequest_ = nextRequest_++;
        EOS_Lobby_UpdateLobbyOptions update{EOS_LOBBY_UPDATELOBBY_API_LATEST, mod};
        EOS_Lobby_UpdateLobby(lobbies_, &update, this, [](const EOS_Lobby_UpdateLobbyCallbackInfo *i) {
            callback(i->ClientData, [&](Client &c) {
                auto request = c.roomRequest_;
                c.roomRequest_ = 0;
                c.emit({EventKind::RoomUpdated, request, i->ResultCode == EOS_EResult::EOS_Success,
                        EOS_EResult_ToString(i->ResultCode), i->LobbyId ? i->LobbyId : ""});
            });
        });
        EOS_LobbyModification_Release(mod);
        return request;
    } catch (...) { EOS_LobbyModification_Release(mod); throw; }
}
void Client::kick(const std::string &peer) {
    check(true);
    auto who = EOS_ProductUserId_FromString(peer.c_str());
    if (!who || who == user_ || lobby().owner != userId()) throw Failure("Only the owner can kick another member");
    EOS_Lobby_KickMemberOptions o{EOS_LOBBY_KICKMEMBER_API_LATEST, lobbyId_.c_str(), user_, who};
    EOS_Lobby_KickMember(lobbies_, &o, this, [](const EOS_Lobby_KickMemberCallbackInfo *i) {
        callback(i->ClientData, [&](Client &c) {
            c.emit({EventKind::Kicked, 0, i->ResultCode == EOS_EResult::EOS_Success, EOS_EResult_ToString(i->ResultCode)});
        });
    });
}
uint64_t Client::search(const std::string &bucket, uint32_t limit) {
    check(true);
    text(bucket, "bucket", 60);
    if (searchRequest_)
        throw Failure("Search already pending");
    if (limit < 1 || limit > 100)
        throw Failure("Search limit must be 1..100");
    if (search_) {
        EOS_LobbySearch_Release(search_);
        search_ = nullptr;
    }
    results_.clear();
    EOS_Lobby_CreateLobbySearchOptions o{EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST, limit};
    require(EOS_Lobby_CreateLobbySearch(lobbies_, &o, &search_), "Create search");
    EOS_Lobby_AttributeData attribute{};
    attribute.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
    attribute.Key = EOS_LOBBY_SEARCH_BUCKET_ID;
    attribute.ValueType = EOS_EAttributeType::EOS_AT_STRING;
    attribute.Value.AsUtf8 = bucket.c_str();
    EOS_LobbySearch_SetParameterOptions param{EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST, &attribute,
                                              EOS_EComparisonOp::EOS_CO_EQUAL};
    require(EOS_LobbySearch_SetParameter(search_, &param), "Search bucket");
    attribute.Key = EOS_LOBBY_SEARCH_MINSLOTSAVAILABLE;
    attribute.ValueType = EOS_ELobbyAttributeType::EOS_AT_INT64;
    attribute.Value.AsInt64 = 1;
    param.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
    require(EOS_LobbySearch_SetParameter(search_, &param), "Search available slots");
    EOS_LobbySearch_FindOptions find{EOS_LOBBYSEARCH_FIND_API_LATEST, user_};
    auto request = searchRequest_ = nextRequest_++;
    EOS_LobbySearch_Find(search_, &find, this, [](const EOS_LobbySearch_FindCallbackInfo *i) {
        callback(i->ClientData, [&](Client &c) {
            auto request = c.searchRequest_;
            c.searchRequest_ = 0;
            auto result = i->ResultCode;
            if (result == EOS_EResult::EOS_Success) {
                try {
                    EOS_LobbySearch_GetSearchResultCountOptions count{
                        EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST};
                    auto size = EOS_LobbySearch_GetSearchResultCount(c.search_, &count);
                    for (uint32_t n = 0; n < size; ++n) {
                        EOS_LobbySearch_CopySearchResultByIndexOptions o{
                            EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST, n};
                        Details d{nullptr};
                        require(EOS_LobbySearch_CopySearchResultByIndex(c.search_, &o, &d.value),
                                "Search result");
                        c.results_.push_back(info(d.value));
                    }
                } catch (...) {
                    c.results_.clear();
                    result = EOS_EResult::EOS_UnexpectedError;
                }
            }
            c.emit({EventKind::Search, request, result == EOS_EResult::EOS_Success,
                    EOS_EResult_ToString(result)});
        });
    });
    return request;
}
std::vector<LobbyInfo> Client::results() const {
    check(true);
    if (searchRequest_)
        throw Failure("Search still pending");
    return results_;
}
EOS_HLobbyDetails Client::details() const {
    check(true);
    if (lobbyId_.empty())
        throw Failure("Join a lobby first");
    EOS_Lobby_CopyLobbyDetailsHandleOptions o{EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST,
                                              lobbyId_.c_str(), user_};
    EOS_HLobbyDetails d = nullptr;
    require(EOS_Lobby_CopyLobbyDetailsHandle(lobbies_, &o, &d), "Lobby details");
    return d;
}
LobbyInfo Client::lobby() const {
    Details d{details()};
    return info(d.value);
}
std::vector<std::string> Client::members() const {
    Details d{details()};
    EOS_LobbyDetails_GetMemberCountOptions count{EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST};
    auto size = EOS_LobbyDetails_GetMemberCount(d.value, &count);
    std::vector<std::string> out;
    for (uint32_t n = 0; n < size; ++n) {
        EOS_LobbyDetails_GetMemberByIndexOptions o{EOS_LOBBYDETAILS_GETMEMBERBYINDEX_API_LATEST, n};
        out.push_back(userText(EOS_LobbyDetails_GetMemberByIndex(d.value, &o)));
    }
    return out;
}
bool Client::isMember(EOS_ProductUserId peer) const {
    if (!peer || peer == user_ || lobbyId_.empty())
        return false;
    auto list = members();
    auto id = userText(peer);
    return std::find(list.begin(), list.end(), id) != list.end();
}
std::string Client::userId() const {
    check();
    return userText(user_);
}
void Client::installPeerNotifications() {
    if (requestNotify_ && closedNotify_)
        return;
    EOS_P2P_AddNotifyPeerConnectionRequestOptions request{
        EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST, user_, &socket_};
    requestNotify_ = EOS_P2P_AddNotifyPeerConnectionRequest(
        p2p_, &request, this, [](const EOS_P2P_OnIncomingConnectionRequestInfo *i) {
            callback(i->ClientData, [&](Client &c) {
                if (i->LocalUserId != c.user_ || !i->SocketId ||
                    std::strcmp(i->SocketId->SocketName, c.socket_.SocketName))
                    return;
                if (!c.isMember(i->RemoteUserId))
                    return;
                EOS_P2P_AcceptConnectionOptions o{EOS_P2P_ACCEPTCONNECTION_API_LATEST, c.user_,
                                                  i->RemoteUserId, &c.socket_};
                require(EOS_P2P_AcceptConnection(c.p2p_, &o), "Accept peer");
            });
        });
    EOS_P2P_AddNotifyPeerConnectionClosedOptions closed{
        EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST, user_, &socket_};
    closedNotify_ = EOS_P2P_AddNotifyPeerConnectionClosed(
        p2p_, &closed, this, [](const EOS_P2P_OnRemoteConnectionClosedInfo *i) {
            callback(i->ClientData, [&](Client &c) {
                c.emit({EventKind::PeerClosed, 0, true, std::to_string(int(i->Reason)), c.lobbyId_,
                        userText(i->RemoteUserId)});
            });
        });
    if (!requestNotify_ || !closedNotify_)
        throw Failure("P2P notifications unavailable");
}
void Client::removePeerNotifications() {
    if (requestNotify_)
        EOS_P2P_RemoveNotifyPeerConnectionRequest(p2p_, requestNotify_);
    if (closedNotify_)
        EOS_P2P_RemoveNotifyPeerConnectionClosed(p2p_, closedNotify_);
    requestNotify_ = closedNotify_ = 0;
}
void Client::closePeers() {
    if (!user_ || !p2p_)
        return;
    EOS_P2P_CloseConnectionsOptions o{EOS_P2P_CLOSECONNECTIONS_API_LATEST, user_, &socket_};
    EOS_P2P_CloseConnections(p2p_, &o);
}
void Client::send(const std::string &peer, std::string_view bytes, uint8_t channel,
                  EOS_EPacketReliability reliability) {
    check(true);
    text(peer, "peer id", EOS_PRODUCTUSERID_MAX_LENGTH);
    if (bytes.size() > EOS_P2P_MAX_PACKET_SIZE)
        throw Failure("Packet exceeds EOS_P2P_MAX_PACKET_SIZE (1170)");
    if (reliability < EOS_EPacketReliability::EOS_PR_UnreliableUnordered ||
        reliability > EOS_EPacketReliability::EOS_PR_ReliableOrdered)
        throw Failure("Invalid reliability");
    auto remote = EOS_ProductUserId_FromString(peer.c_str());
    if (!isMember(remote))
        throw Failure("Peer is not another member of this lobby");
    EOS_P2P_SendPacketOptions o{};
    o.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
    o.LocalUserId = user_;
    o.RemoteUserId = remote;
    o.SocketId = &socket_;
    o.Channel = channel;
    o.Data = bytes.data();
    o.DataLengthBytes = uint32_t(bytes.size());
    o.bAllowDelayedDelivery = EOS_TRUE;
    o.Reliability = reliability;
    require(EOS_P2P_SendPacket(p2p_, &o), "Send packet");
}
std::optional<Packet> Client::receive() {
    std::array<char, EOS_P2P_MAX_PACKET_SIZE> data{};
    auto r = receiveInto(data.data(), uint32_t(data.size()));
    if (!r)
        return {};
    return Packet{std::move(r->peer), std::string(data.data(), r->size), r->channel};
}
std::optional<Received> Client::receiveInto(char *out, uint32_t capacity) {
    check(true);
    if (capacity < EOS_P2P_MAX_PACKET_SIZE)
        throw Failure("Receive buffer is smaller than EOS_P2P_MAX_PACKET_SIZE");
    // Bound work when unrelated sockets or former members flood the SDK queue.
    for (int n = 0; n < 64; ++n) {
        EOS_P2P_ReceivePacketOptions o{EOS_P2P_RECEIVEPACKET_API_LATEST, user_,
                                       EOS_P2P_MAX_PACKET_SIZE, nullptr};
        EOS_ProductUserId peer = nullptr;
        EOS_P2P_SocketId socket{};
        socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
        uint8_t channel = 0;
        uint32_t size = 0;
        auto r = EOS_P2P_ReceivePacket(p2p_, &o, &peer, &socket, &channel, out, &size);
        if (r == EOS_EResult::EOS_NotFound)
            return {};
        require(r, "Receive packet");
        if (std::strcmp(socket.SocketName, socket_.SocketName) || !isMember(peer))
            continue;
        return Received{userText(peer), channel, size};
    }
    return {};
}
void Client::disconnect(const std::string &peer) {
    check(true);
    text(peer, "peer id", EOS_PRODUCTUSERID_MAX_LENGTH);
    auto remote = EOS_ProductUserId_FromString(peer.c_str());
    if (!remote)
        throw Failure("Invalid peer id");
    EOS_P2P_CloseConnectionOptions o{EOS_P2P_CLOSECONNECTION_API_LATEST, user_, remote, &socket_};
    require(EOS_P2P_CloseConnection(p2p_, &o), "Close peer");
}
void Client::relay(EOS_ERelayControl mode) {
    check();
    if (mode < EOS_ERelayControl::EOS_RC_NoRelays || mode > EOS_ERelayControl::EOS_RC_ForceRelays)
        throw Failure("Invalid relay mode: " + std::to_string(int(mode)));
    EOS_P2P_SetRelayControlOptions o{EOS_P2P_SETRELAYCONTROL_API_LATEST, mode};
    require(EOS_P2P_SetRelayControl(p2p_, &o), "Relay control");
}
} // namespace eos_lhat
