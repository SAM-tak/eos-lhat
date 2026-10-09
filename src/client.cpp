#include "client.h"
#include <eos_init.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace eos_lhat {
namespace {
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
        EOS_InitializeOptions o{};
        o.ApiVersion = EOS_INITIALIZE_API_LATEST;
        o.ProductName = "eos-lhat";
        o.ProductVersion = "0.1.0";
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
        connect_ = EOS_Platform_GetConnectInterface(platform_);
        lobbies_ = EOS_Platform_GetLobbyInterface(platform_);
        p2p_ = EOS_Platform_GetP2PInterface(platform_);
        if (!connect_ || !lobbies_ || !p2p_)
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
    if (user_ || loginRequest_)
        throw Failure("Device login requires a new, idle client");
    loginName_ = name;
    auto request = loginRequest_ = nextRequest_++;
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
    return {raw->LobbyId, userText(raw->LobbyOwnerUserId), raw->BucketId ? raw->BucketId : "",
            raw->MaxMembers, raw->AvailableSlots};
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
void Client::send(const std::string &peer, const std::string &bytes, uint8_t channel,
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
    check(true);
    // Bound work when unrelated sockets or former members flood the SDK queue.
    for (int n = 0; n < 64; ++n) {
        std::array<char, EOS_P2P_MAX_PACKET_SIZE> data{};
        EOS_P2P_ReceivePacketOptions o{EOS_P2P_RECEIVEPACKET_API_LATEST, user_,
                                       uint32_t(data.size()), nullptr};
        EOS_ProductUserId peer = nullptr;
        EOS_P2P_SocketId socket{};
        socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
        uint8_t channel = 0;
        uint32_t size = 0;
        auto r = EOS_P2P_ReceivePacket(p2p_, &o, &peer, &socket, &channel, data.data(), &size);
        if (r == EOS_EResult::EOS_NotFound)
            return {};
        require(r, "Receive packet");
        if (std::strcmp(socket.SocketName, socket_.SocketName) || !isMember(peer))
            continue;
        return Packet{userText(peer), std::string(data.data(), size), channel};
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
