#include "client.h"
#include <iostream>
#include <functional>
#include <cstdlib>
namespace fake {
extern int initializations, shutdowns, releases, accepts, closes;
extern bool invalidUser, failSearch, persistentAuth;
extern int authLogins, portalLogins, idTokens;
void expire(EOS_ProductUserId);
void request(EOS_ProductUserId, EOS_ProductUserId, std::string);
} // namespace fake
using namespace eos_lhat;
int checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
void refused(std::function<void()> f) {
    try {
        f();
        check(false, "Expected Failure");
    } catch (const Failure &) {
        check(true, "refused");
    }
}
Event wait(Client &c, EventKind kind) {
    for (int tick = 0; tick < 8; ++tick) {
        c.tick();
        while (auto e = c.poll())
            if (e->kind == kind)
                return *e;
    }
    throw Failure("Missing completion event");
}
int main() {
    Config config{"product", "sandbox", "deployment", "client", "secret", "cache", "test"};
    auto bad = config;
    bad.socketName = "bad/socket";
    refused([&] { Client c(bad); });
    {
        Client a(config), b(config);
        check(fake::initializations == 1, "Shared EOS initialization");
        refused([&] { a.createLobby("duel", 2); });
        refused([&] { a.loginSteam("not-hex"); });
        fake::invalidUser = true;
        auto request = a.loginSteam("aabb");
        refused([&] { a.loginSteam("aabb"); });
        check(a.userId().empty(), "Login deferred until tick");
        auto e = wait(a, EventKind::Login);
        check(e.ok && e.request == request, "CreateUser continuation preserves request");
        b.loginSteam("ccdd");
        check(wait(b, EventKind::Login).ok, "Second login");
        std::thread other([&] { refused([&] { a.tick(); }); });
        other.join();
        request = a.createLobby("duel", 2);
        refused([&] { a.join("room0"); });
        check(wait(a, EventKind::Created).request == request, "Create completion");
        check(a.lobby().available == 1, "Lobby capacity");
        fake::failSearch = true;
        b.search("duel", 10);
        check(!wait(b, EventKind::Search).ok, "Search error propagated");
        b.search("duel", 10);
        refused([&] { b.search("duel", 10); });
        refused([&] { b.results(); });
        check(wait(b, EventKind::Search).ok && b.results().size() == 1, "Bucket search");
        b.join(b.results()[0].id);
        check(wait(b, EventKind::Joined).ok, "Join by id");
        check(wait(a, EventKind::MemberJoined).peerId == b.userId(), "Membership notification");
        check(a.members().size() == 2, "Member list");
        const auto ap = EOS_ProductUserId_FromString(a.userId().c_str()),
                   bp = EOS_ProductUserId_FromString(b.userId().c_str());
        fake::request(ap, bp, "wrong");
        a.tick();
        check(fake::accepts == 0, "Wrong socket refused");
        fake::request(ap, bp, "test");
        a.tick();
        check(fake::accepts == 1, "Lobby peer accepted");
        std::string bytes("\0\xff\x01\0", 4);
        a.send(b.userId(), bytes, 7, EOS_EPacketReliability::EOS_PR_ReliableOrdered);
        auto packet = b.receive();
        check(packet && packet->data == bytes && packet->channel == 7 && packet->peer == a.userId(),
              "Binary packet preserved");
        b.send(a.userId(), "reply", 0, EOS_EPacketReliability::EOS_PR_UnreliableUnordered);
        check(a.receive()->data == "reply", "Bidirectional packet");
        check(!a.receive(), "Empty receive");
        refused([&] {
            a.send(b.userId(), std::string(1171, 'x'), 0,
                   EOS_EPacketReliability::EOS_PR_ReliableOrdered);
        });
        fake::expire(ap);
        check(wait(a, EventKind::AuthExpired).kind == EventKind::AuthExpired, "Auth expiry");
        a.loginSteam("eeff");
        check(wait(a, EventKind::Login).ok && a.members().size() == 2,
              "Token refresh preserves lobby");
        b.leave();
        check(wait(b, EventKind::Left).ok, "Leave");
        wait(a, EventKind::MemberLeft);
        refused(
            [&] { a.send(b.userId(), "x", 0, EOS_EPacketReliability::EOS_PR_ReliableOrdered); });
        fake::request(ap, bp, "test");
        a.tick();
        check(fake::accepts == 1, "Former member refused");
        for (int i = 0; i < 300; ++i)
            fake::expire(ap);
        a.tick();
        check(a.poll()->kind == EventKind::Overflow, "Bounded notification queue");
        b.close();
        b.close();
        refused([&] { b.tick(); });
        check(fake::shutdowns == 0, "Other client retains runtime");
        a.search("duel", 10);
        a.close();
        check(fake::shutdowns == 0, "Runtime retained across restart");
    }
    {
        Client c(config);
        c.loginDevice("Developer");
        check(wait(c, EventKind::Login).ok, "Existing Device ID login");
    }
    {
        Client c(config);
        refused([&] { c.loginEpicExchange(""); });
        c.loginEpic(false);
        check(!wait(c, EventKind::Login).ok && fake::portalLogins == 0,
              "Silent Epic login fails without a stored token");
        auto request = c.loginEpic(true);
        refused([&] { c.loginEpic(true); });
        auto e = wait(c, EventKind::Login);
        check(e.ok && e.request == request && fake::portalLogins == 1 && fake::persistentAuth,
              "Account Portal fallback");
        refused([&] { c.forgetEpic(); });
        auto logins = fake::authLogins;
        fake::expire(EOS_ProductUserId_FromString(c.userId().c_str()));
        wait(c, EventKind::AuthExpired);
        c.loginEpic(false);
        check(wait(c, EventKind::Login).ok && fake::authLogins == logins && fake::idTokens == 2,
              "Connect refresh reuses the Epic session");
    }
    {
        Client c(config);
        c.loginEpic(false);
        check(wait(c, EventKind::Login).ok && fake::portalLogins == 1, "Persistent Epic login");
    }
    {
        Client c(config);
        auto request = c.forgetEpic();
        auto e = wait(c, EventKind::EpicForgotten);
        check(e.ok && e.request == request && !fake::persistentAuth, "Forget persistent Epic login");
        c.loginEpicExchange("expired");
        check(!wait(c, EventKind::Login).ok, "Exchange code error propagated");
        c.loginEpicExchange("code");
        check(wait(c, EventKind::Login).ok, "Exchange code login");
    }
    {
        Client c(config);
        c.loginEpicDeveloper("localhost:6547", "dev");
        check(wait(c, EventKind::Login).ok, "Developer Auth Tool login");
    }
    check(fake::initializations == 1 && fake::shutdowns == 0 && fake::releases == 7,
          "Restart retains SDK but releases platforms");
    shutdownRuntime();
    shutdownRuntime();
    check(fake::shutdowns == 1, "SDK shut down exactly once");
    refused([&] { Client c(config); });
    std::cout << checks << " checks passed\n";
}
