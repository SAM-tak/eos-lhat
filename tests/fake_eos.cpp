// Deterministic EOS test double. No SDK implementation or network service is bundled.
#include "client.h"
#include <eos_init.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>

using R = EOS_EResult;
struct EOS_ProductUserIdDetails {
    std::string id;
};
struct Wire {
    EOS_ProductUserId peer;
    std::string socket, data;
    uint8_t channel;
};
struct FakePlatform {
    EOS_ProductUserId user;
    std::deque<std::function<void()>> work;
    std::deque<Wire> packets;
    void *authContext = nullptr, *statusContext = nullptr, *memberContext = nullptr,
         *requestContext = nullptr, *closedContext = nullptr;
    EOS_Connect_OnAuthExpirationCallback auth = nullptr;
    EOS_Connect_OnLoginStatusChangedCallback status = nullptr;
    EOS_Lobby_OnLobbyMemberStatusReceivedCallback member = nullptr;
    EOS_P2P_OnIncomingConnectionRequestCallback request = nullptr;
    EOS_P2P_OnRemoteConnectionClosedCallback closed = nullptr;
    std::set<EOS_ProductUserId> accepted;
};
struct Room {
    std::string id, bucket;
    uint32_t capacity;
    std::vector<EOS_ProductUserId> members;
};
struct Search {
    FakePlatform *platform;
    std::string bucket;
    uint32_t limit;
    std::vector<std::string> results;
};
namespace fake {
std::map<std::string, std::unique_ptr<EOS_ProductUserIdDetails>> users;
std::vector<FakePlatform *> platforms;
std::map<std::string, Room> rooms;
int initializations = 0, shutdowns = 0, releases = 0, accepts = 0, closes = 0;
bool invalidUser = false, failSearch = false;
int userSerial = 0;
template <class H> FakePlatform *p(H h) {
    return reinterpret_cast<FakePlatform *>(h);
}
EOS_ProductUserId user(std::string id) {
    auto &u = users[id];
    if (!u)
        u = std::make_unique<EOS_ProductUserIdDetails>(EOS_ProductUserIdDetails{id});
    return u.get();
}
FakePlatform *platform(EOS_ProductUserId user) {
    for (auto *p : platforms)
        if (p->user == user)
            return p;
    return nullptr;
}
void member(Room &r, EOS_ProductUserId who, EOS_ELobbyMemberStatus status) {
    for (auto *p : platforms)
        if (p->member) {
            auto id = r.id;
            p->work.push_back([p, id, who, status] {
                if (p->member) {
                    EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo i{p->memberContext, id.c_str(),
                                                                      who, status};
                    p->member(&i);
                }
            });
        }
}
void expire(EOS_ProductUserId who) {
    auto *p = platform(who);
    p->work.push_back([p] {
        EOS_Connect_AuthExpirationCallbackInfo i{p->authContext, p->user};
        p->auth(&i);
    });
}
void request(EOS_ProductUserId local, EOS_ProductUserId remote, std::string socket) {
    auto *p = platform(local);
    p->work.push_back([p, remote, socket] {
        EOS_P2P_SocketId s{};
        s.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
        std::strcpy(s.SocketName, socket.c_str());
        EOS_P2P_OnIncomingConnectionRequestInfo i{p->requestContext, p->user, remote, &s};
        if (p->request)
            p->request(&i);
    });
}
} // namespace fake
EOS_EResult EOS_CALL EOS_Initialize(const EOS_InitializeOptions *) {
    ++fake::initializations;
    return R::EOS_Success;
}
EOS_EResult EOS_CALL EOS_Shutdown() {
    ++fake::shutdowns;
    if (const char *path = std::getenv("EOS_LHAT_TEST_LIFETIME")) {
        std::ofstream log(path, std::ios::app);
        log << fake::initializations << " " << fake::shutdowns << " " << fake::platforms.size()
            << " " << fake::releases << "\n";
    }
    return R::EOS_Success;
}
EOS_HPlatform EOS_CALL EOS_Platform_Create(const EOS_Platform_Options *) {
    auto *p = new FakePlatform{};
    p->user = fake::user("player" + std::to_string(++fake::userSerial));
    fake::platforms.push_back(p);
    return reinterpret_cast<EOS_HPlatform>(p);
}
void EOS_CALL EOS_Platform_Release(EOS_HPlatform h) {
    auto *p = fake::p(h);
    fake::platforms.erase(std::find(fake::platforms.begin(), fake::platforms.end(), p));
    ++fake::releases;
    delete p;
}
void EOS_CALL EOS_Platform_Tick(EOS_HPlatform h) {
    auto *p = fake::p(h);
    auto work = std::move(p->work);
    p->work.clear();
    for (auto &f : work)
        f();
}
EOS_HConnect EOS_CALL EOS_Platform_GetConnectInterface(EOS_HPlatform h) {
    return reinterpret_cast<EOS_HConnect>(h);
}
EOS_HLobby EOS_CALL EOS_Platform_GetLobbyInterface(EOS_HPlatform h) {
    return reinterpret_cast<EOS_HLobby>(h);
}
EOS_HP2P EOS_CALL EOS_Platform_GetP2PInterface(EOS_HPlatform h) {
    return reinterpret_cast<EOS_HP2P>(h);
}
const char *EOS_CALL EOS_EResult_ToString(EOS_EResult r) {
    return r == R::EOS_Success ? "EOS_Success" : "EOS_TestFailure";
}
EOS_EResult EOS_CALL EOS_ProductUserId_ToString(EOS_ProductUserId u, char *out, int32_t *size) {
    if (*size <= int(u->id.size()))
        return R::EOS_LimitExceeded;
    std::strcpy(out, u->id.c_str());
    *size = int32_t(u->id.size() + 1);
    return R::EOS_Success;
}
EOS_ProductUserId EOS_CALL EOS_ProductUserId_FromString(const char *s) {
    auto i = fake::users.find(s);
    return i == fake::users.end() ? nullptr : i->second.get();
}
void EOS_CALL EOS_Connect_Login(EOS_HConnect h, const EOS_Connect_LoginOptions *o, void *data,
                                EOS_Connect_OnLoginCallback cb) {
    auto *p = fake::p(h);
    auto type = o->Credentials->Type;
    if (type != EOS_EExternalCredentialType::EOS_ECT_STEAM_SESSION_TICKET &&
        type != EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN)
        std::abort();
    p->work.push_back([p, data, cb] {
        EOS_Connect_LoginCallbackInfo i{};
        i.ClientData = data;
        i.LocalUserId = p->user;
        i.ResultCode = fake::invalidUser ? R::EOS_InvalidUser : R::EOS_Success;
        i.ContinuanceToken = reinterpret_cast<EOS_ContinuanceToken>(p);
        fake::invalidUser = false;
        cb(&i);
    });
}
void EOS_CALL EOS_Connect_CreateUser(EOS_HConnect h, const EOS_Connect_CreateUserOptions *,
                                     void *data, EOS_Connect_OnCreateUserCallback cb) {
    auto *p = fake::p(h);
    p->work.push_back([p, data, cb] {
        EOS_Connect_CreateUserCallbackInfo i{R::EOS_Success, data, p->user};
        cb(&i);
    });
}
void EOS_CALL EOS_Connect_CreateDeviceId(EOS_HConnect h, const EOS_Connect_CreateDeviceIdOptions *,
                                         void *data, EOS_Connect_OnCreateDeviceIdCallback cb) {
    fake::p(h)->work.push_back([data, cb] {
        EOS_Connect_CreateDeviceIdCallbackInfo i{R::EOS_DuplicateNotAllowed, data};
        cb(&i);
    });
}
// Explicit definitions keep the test double checked against the downloaded headers.
EOS_NotificationId EOS_CALL EOS_Connect_AddNotifyAuthExpiration(
    EOS_HConnect h, const EOS_Connect_AddNotifyAuthExpirationOptions *, void *d,
    EOS_Connect_OnAuthExpirationCallback cb) {
    auto *p = fake::p(h);
    p->auth = cb;
    p->authContext = d;
    return 1;
}
EOS_NotificationId EOS_CALL EOS_Connect_AddNotifyLoginStatusChanged(
    EOS_HConnect h, const EOS_Connect_AddNotifyLoginStatusChangedOptions *, void *d,
    EOS_Connect_OnLoginStatusChangedCallback cb) {
    auto *p = fake::p(h);
    p->status = cb;
    p->statusContext = d;
    return 2;
}
EOS_NotificationId EOS_CALL EOS_Lobby_AddNotifyLobbyMemberStatusReceived(
    EOS_HLobby h, const EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions *, void *d,
    EOS_Lobby_OnLobbyMemberStatusReceivedCallback cb) {
    auto *p = fake::p(h);
    p->member = cb;
    p->memberContext = d;
    return 3;
}
EOS_NotificationId EOS_CALL EOS_P2P_AddNotifyPeerConnectionRequest(
    EOS_HP2P h, const EOS_P2P_AddNotifyPeerConnectionRequestOptions *, void *d,
    EOS_P2P_OnIncomingConnectionRequestCallback cb) {
    auto *p = fake::p(h);
    p->request = cb;
    p->requestContext = d;
    return 4;
}
EOS_NotificationId EOS_CALL EOS_P2P_AddNotifyPeerConnectionClosed(
    EOS_HP2P h, const EOS_P2P_AddNotifyPeerConnectionClosedOptions *, void *d,
    EOS_P2P_OnRemoteConnectionClosedCallback cb) {
    auto *p = fake::p(h);
    p->closed = cb;
    p->closedContext = d;
    return 5;
}
void EOS_CALL EOS_Connect_RemoveNotifyAuthExpiration(EOS_HConnect h, EOS_NotificationId) {
    fake::p(h)->auth = nullptr;
}
void EOS_CALL EOS_Connect_RemoveNotifyLoginStatusChanged(EOS_HConnect h, EOS_NotificationId) {
    fake::p(h)->status = nullptr;
}
void EOS_CALL EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived(EOS_HLobby h, EOS_NotificationId) {
    fake::p(h)->member = nullptr;
}
void EOS_CALL EOS_P2P_RemoveNotifyPeerConnectionRequest(EOS_HP2P h, EOS_NotificationId) {
    fake::p(h)->request = nullptr;
}
void EOS_CALL EOS_P2P_RemoveNotifyPeerConnectionClosed(EOS_HP2P h, EOS_NotificationId) {
    fake::p(h)->closed = nullptr;
}
void EOS_CALL EOS_Lobby_CreateLobby(EOS_HLobby h, const EOS_Lobby_CreateLobbyOptions *o, void *d,
                                    EOS_Lobby_OnCreateLobbyCallback cb) {
    auto *p = fake::p(h);
    Room r{"room" + std::to_string(fake::rooms.size()),
           o->BucketId,
           o->MaxLobbyMembers,
           {o->LocalUserId}};
    p->work.push_back([r, d, cb] {
        fake::rooms[r.id] = r;
        EOS_Lobby_CreateLobbyCallbackInfo i{R::EOS_Success, d, r.id.c_str()};
        cb(&i);
    });
}
void EOS_CALL EOS_Lobby_JoinLobbyById(EOS_HLobby h, const EOS_Lobby_JoinLobbyByIdOptions *o,
                                      void *d, EOS_Lobby_OnJoinLobbyByIdCallback cb) {
    auto *p = fake::p(h);
    std::string id = o->LobbyId;
    p->work.push_back([p, id, d, cb] {
        auto it = fake::rooms.find(id);
        auto result = R::EOS_NotFound;
        if (it != fake::rooms.end() && it->second.members.size() < it->second.capacity) {
            it->second.members.push_back(p->user);
            result = R::EOS_Success;
            fake::member(it->second, p->user, EOS_ELobbyMemberStatus::EOS_LMS_JOINED);
        }
        EOS_Lobby_JoinLobbyByIdCallbackInfo i{result, d, id.c_str()};
        cb(&i);
    });
}
void EOS_CALL EOS_Lobby_LeaveLobby(EOS_HLobby h, const EOS_Lobby_LeaveLobbyOptions *o, void *d,
                                   EOS_Lobby_OnLeaveLobbyCallback cb) {
    auto *p = fake::p(h);
    std::string id = o->LobbyId;
    p->work.push_back([p, id, d, cb] {
        auto &r = fake::rooms.at(id);
        r.members.erase(std::remove(r.members.begin(), r.members.end(), p->user), r.members.end());
        fake::member(r, p->user, EOS_ELobbyMemberStatus::EOS_LMS_LEFT);
        EOS_Lobby_LeaveLobbyCallbackInfo i{R::EOS_Success, d, id.c_str()};
        cb(&i);
    });
}
EOS_EResult EOS_CALL EOS_Lobby_CreateLobbySearch(EOS_HLobby h,
                                                 const EOS_Lobby_CreateLobbySearchOptions *o,
                                                 EOS_HLobbySearch *out) {
    *out = reinterpret_cast<EOS_HLobbySearch>(new Search{fake::p(h), "", o->MaxResults, {}});
    return R::EOS_Success;
}
EOS_EResult EOS_CALL EOS_LobbySearch_SetParameter(EOS_HLobbySearch h,
                                                  const EOS_LobbySearch_SetParameterOptions *o) {
    if (o->Parameter->ValueType == EOS_ELobbyAttributeType::EOS_AT_STRING)
        reinterpret_cast<Search *>(h)->bucket = o->Parameter->Value.AsUtf8;
    return R::EOS_Success;
}
void EOS_CALL EOS_LobbySearch_Find(EOS_HLobbySearch h, const EOS_LobbySearch_FindOptions *, void *d,
                                   EOS_LobbySearch_OnFindCallback cb) {
    auto *s = reinterpret_cast<Search *>(h);
    s->platform->work.push_back([s, d, cb] {
        for (auto &[id, r] : fake::rooms)
            if (r.bucket == s->bucket && s->results.size() < s->limit)
                s->results.push_back(id);
        EOS_LobbySearch_FindCallbackInfo i{fake::failSearch ? R::EOS_NoConnection : R::EOS_Success,
                                           d};
        fake::failSearch = false;
        cb(&i);
    });
}
uint32_t EOS_CALL EOS_LobbySearch_GetSearchResultCount(
    EOS_HLobbySearch h, const EOS_LobbySearch_GetSearchResultCountOptions *) {
    return uint32_t(reinterpret_cast<Search *>(h)->results.size());
}
EOS_EResult EOS_CALL EOS_LobbySearch_CopySearchResultByIndex(
    EOS_HLobbySearch h, const EOS_LobbySearch_CopySearchResultByIndexOptions *o,
    EOS_HLobbyDetails *out) {
    auto *s = reinterpret_cast<Search *>(h);
    *out =
        reinterpret_cast<EOS_HLobbyDetails>(new Room(fake::rooms.at(s->results.at(o->LobbyIndex))));
    return R::EOS_Success;
}
void EOS_CALL EOS_LobbySearch_Release(EOS_HLobbySearch h) {
    delete reinterpret_cast<Search *>(h);
}
EOS_EResult EOS_CALL EOS_Lobby_CopyLobbyDetailsHandle(
    EOS_HLobby, const EOS_Lobby_CopyLobbyDetailsHandleOptions *o, EOS_HLobbyDetails *out) {
    auto i = fake::rooms.find(o->LobbyId);
    if (i == fake::rooms.end())
        return R::EOS_NotFound;
    *out = reinterpret_cast<EOS_HLobbyDetails>(new Room(i->second));
    return R::EOS_Success;
}
void EOS_CALL EOS_LobbyDetails_Release(EOS_HLobbyDetails h) {
    delete reinterpret_cast<Room *>(h);
}
EOS_EResult EOS_CALL EOS_LobbyDetails_CopyInfo(EOS_HLobbyDetails h,
                                               const EOS_LobbyDetails_CopyInfoOptions *,
                                               EOS_LobbyDetails_Info **out) {
    auto *r = reinterpret_cast<Room *>(h);
    auto *i = new EOS_LobbyDetails_Info{};
    i->LobbyId = r->id.c_str();
    i->BucketId = r->bucket.c_str();
    i->LobbyOwnerUserId = r->members.empty() ? nullptr : r->members[0];
    i->MaxMembers = r->capacity;
    i->AvailableSlots = r->capacity - uint32_t(r->members.size());
    *out = i;
    return R::EOS_Success;
}
void EOS_CALL EOS_LobbyDetails_Info_Release(EOS_LobbyDetails_Info *i) {
    delete i;
}
uint32_t EOS_CALL EOS_LobbyDetails_GetMemberCount(EOS_HLobbyDetails h,
                                                  const EOS_LobbyDetails_GetMemberCountOptions *) {
    return uint32_t(reinterpret_cast<Room *>(h)->members.size());
}
EOS_ProductUserId EOS_CALL EOS_LobbyDetails_GetMemberByIndex(
    EOS_HLobbyDetails h, const EOS_LobbyDetails_GetMemberByIndexOptions *o) {
    return reinterpret_cast<Room *>(h)->members.at(o->MemberIndex);
}
EOS_EResult EOS_CALL EOS_P2P_SendPacket(EOS_HP2P, const EOS_P2P_SendPacketOptions *o) {
    auto *p = fake::platform(o->RemoteUserId);
    if (!p)
        return R::EOS_NotFound;
    p->packets.push_back({o->LocalUserId, o->SocketId->SocketName,
                          std::string(static_cast<const char *>(o->Data), o->DataLengthBytes),
                          o->Channel});
    return R::EOS_Success;
}
EOS_EResult EOS_CALL EOS_P2P_ReceivePacket(EOS_HP2P h, const EOS_P2P_ReceivePacketOptions *o,
                                           EOS_ProductUserId *peer, EOS_P2P_SocketId *socket,
                                           uint8_t *channel, void *out, uint32_t *size) {
    auto *p = fake::p(h);
    if (p->packets.empty())
        return R::EOS_NotFound;
    auto w = p->packets.front();
    p->packets.pop_front();
    if (w.data.size() > o->MaxDataSizeBytes)
        return R::EOS_LimitExceeded;
    *peer = w.peer;
    *channel = w.channel;
    *size = uint32_t(w.data.size());
    std::strcpy(socket->SocketName, w.socket.c_str());
    std::memcpy(out, w.data.data(), w.data.size());
    return R::EOS_Success;
}
EOS_EResult EOS_CALL EOS_P2P_AcceptConnection(EOS_HP2P h,
                                              const EOS_P2P_AcceptConnectionOptions *o) {
    ++fake::accepts;
    fake::p(h)->accepted.insert(o->RemoteUserId);
    return R::EOS_Success;
}
EOS_EResult EOS_CALL EOS_P2P_CloseConnection(EOS_HP2P h, const EOS_P2P_CloseConnectionOptions *o) {
    ++fake::closes;
    fake::p(h)->accepted.erase(o->RemoteUserId);
    return R::EOS_Success;
}
EOS_EResult EOS_CALL EOS_P2P_CloseConnections(EOS_HP2P h, const EOS_P2P_CloseConnectionsOptions *) {
    ++fake::closes;
    fake::p(h)->accepted.clear();
    fake::p(h)->packets.clear();
    return R::EOS_Success;
}
EOS_EResult EOS_CALL EOS_P2P_SetRelayControl(EOS_HP2P, const EOS_P2P_SetRelayControlOptions *) {
    return R::EOS_Success;
}
