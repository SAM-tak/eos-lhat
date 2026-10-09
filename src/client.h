#pragma once
#include <eos_sdk.h>
#include <eos_connect.h>
#include <eos_lobby.h>
#include <eos_p2p.h>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace eos_lhat {
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Config {
    std::string productId, sandboxId, deploymentId, clientId, clientSecret;
    std::string cacheDirectory, socketName;
};
enum class EventKind {
    Login,
    Search,
    Created,
    Joined,
    Left,
    MemberJoined,
    MemberLeft,
    OwnerChanged,
    LobbyClosed,
    AuthExpired,
    LoggedOut,
    PeerClosed,
    Overflow,
    InternalError
};
struct Event {
    EventKind kind;
    uint64_t request = 0;
    bool ok = true;
    std::string code, lobbyId, peerId;
};
struct LobbyInfo {
    std::string id, owner, bucket;
    uint32_t capacity = 0, available = 0;
};
struct Packet {
    std::string peer, data;
    uint8_t channel = 0;
};

class Client {
  public:
    explicit Client(const Config &config);
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    void close();
    void tick();
    std::optional<Event> poll();
    uint64_t loginSteam(const std::string &ticket);
    uint64_t loginDevice(const std::string &name);
    uint64_t createLobby(const std::string &bucket, uint32_t capacity);
    uint64_t search(const std::string &bucket, uint32_t limit);
    uint64_t join(const std::string &id);
    uint64_t leave();
    std::vector<LobbyInfo> results() const;
    std::vector<std::string> members() const;
    LobbyInfo lobby() const;
    std::string userId() const;
    void send(const std::string &peer, const std::string &bytes, uint8_t channel,
              EOS_EPacketReliability reliability);
    std::optional<Packet> receive();
    void disconnect(const std::string &peer);
    void relay(EOS_ERelayControl mode);

  private:
    Config config_;
    std::thread::id thread_;
    EOS_HPlatform platform_ = nullptr;
    EOS_HConnect connect_ = nullptr;
    EOS_HLobby lobbies_ = nullptr;
    EOS_HP2P p2p_ = nullptr;
    EOS_ProductUserId user_ = nullptr;
    EOS_P2P_SocketId socket_{};
    EOS_HLobbySearch search_ = nullptr;
    EOS_NotificationId authNotify_ = 0, statusNotify_ = 0, memberNotify_ = 0, requestNotify_ = 0,
                       closedNotify_ = 0;
    bool runtime_ = false, closing_ = false;
    uint64_t nextRequest_ = 1, loginRequest_ = 0, lobbyRequest_ = 0, searchRequest_ = 0,
             dropped_ = 0;
    std::string lobbyId_, loginName_;
    std::vector<LobbyInfo> results_;
    std::deque<Event> events_;
    void check(bool loggedIn = false) const;
    void idleLobby(bool mustBeJoined) const;
    void emit(Event event);
    void finishLogin(EOS_EResult code, EOS_ProductUserId user);
    void connectLogin(EOS_EExternalCredentialType type, const char *token);
    void installPeerNotifications();
    void removePeerNotifications();
    void closePeers();
    bool isMember(EOS_ProductUserId peer) const;
    EOS_HLobbyDetails details() const;
    static LobbyInfo info(EOS_HLobbyDetails details);
    template <class F> static void callback(void *data, F action) noexcept {
        auto &self = *static_cast<Client *>(data);
        if (self.closing_)
            return;
        try {
            action(self);
        } catch (const std::exception &e) {
            try {
                self.emit({EventKind::InternalError, 0, false, e.what()});
            } catch (...) {
                ++self.dropped_;
            }
        } catch (...) {
            ++self.dropped_;
        }
    }
};
std::string userText(EOS_ProductUserId user);
void require(EOS_EResult result, const char *operation);
// Called once by the host after all extension users and callbacks are gone.
void shutdownRuntime() noexcept;
} // namespace eos_lhat
