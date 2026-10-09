#include "client.h"
#include <lhat/extension.h>
#if LHAT_EXTENSION_ABI != 2
#error "EOS for L^ requires native extension ABI 2 with a shutdown callback"
#endif
#include <cmath>
#include <limits>
#include <new>
#include "eos_signatures.h"

namespace {
using namespace eos_lhat;
const LhatExtensionAPI *api;
const LhatHostDataTag *clientTag;
const LhatErrorKind *errorKind;
const char *const eventNames[] = {"login",       "search",       "created",    "joined",
                                  "left",        "memberJoined", "memberLeft", "ownerChanged",
                                  "lobbyClosed", "authExpired",  "loggedOut",  "peerClosed",
                                  "overflow",    "internalError"};
std::string string(LhatValue v) {
    if (!lhat_is_object_kind(v, LHAT_OBJECT_STRING))
        throw Failure("Expected a string");
    auto *s = reinterpret_cast<LhatString *>(lhat_as_object(v));
    return {s->text, s->length};
}
uint32_t integer(LhatValue v, uint32_t max) {
    if (!lhat_is_number(v))
        throw Failure("Expected a number");
    auto n = lhat_number_as_real(v);
    if (!std::isfinite(n) || n < 0 || n > max || std::floor(n) != n)
        throw Failure("Integer out of range");
    return uint32_t(n);
}
int choice(LhatValue v, const char *const *names, size_t count) {
    if (!lhat_is_object_kind(v, LHAT_OBJECT_ENUMERATOR))
        throw Failure("Expected an enum");
    auto *e = reinterpret_cast<LhatEnumerator *>(lhat_as_object(v));
    for (size_t i = 0; i < count; ++i)
        if (std::string(e->name->text, e->name->length) == names[i])
            return int(i);
    throw Failure("Unknown enum member");
}
const char *const reliabilityNames[] = {"unreliable", "reliableUnordered", "reliableOrdered"};
const char *const relayNames[] = {"never", "allow", "always"};

Client &client(LhatValue v) {
    auto *c = static_cast<Client *>(api->lhat_hostdata_pointer(v, clientTag));
    if (!c)
        throw Failure("Invalid EOS client");
    return *c;
}
LhatValue str(LhatMachine *m, const std::string &s) {
    LhatValue v;
    if (!api->lhat_machine_make_string(m, s.data(), s.size(), &v))
        throw std::bad_alloc();
    return v;
}
LhatValue table(LhatMachine *m) {
    LhatValue v;
    if (!api->lhat_machine_make_table(m, &v))
        throw std::bad_alloc();
    return v;
}
void put(LhatMachine *m, LhatValue t, LhatValue key, LhatValue value) {
    bool refused = false;
    if (!api->lhat_machine_table_set(m, reinterpret_cast<LhatTable *>(lhat_as_object(t)), key,
                                     value, &refused) ||
        refused)
        throw std::bad_alloc();
}
void field(LhatMachine *m, LhatValue t, const char *key, LhatValue value) {
    put(m, t, str(m, key), value);
}
LhatValue lobbyValue(LhatMachine *m, const LobbyInfo &i) {
    auto t = table(m);
    field(m, t, "id", str(m, i.id));
    field(m, t, "owner", str(m, i.owner));
    field(m, t, "bucket", str(m, i.bucket));
    field(m, t, "capacity", lhat_integer(i.capacity));
    field(m, t, "available", lhat_integer(i.available));
    return t;
}
LhatValue eventValue(LhatMachine *m, const Event &e) {
    auto t = table(m);
    LhatValue kind;
    if (!api->lhat_machine_registered(m, "eos", nullptr, "EventKind", &kind))
        throw Failure("Event enum unavailable");
    kind = api->lhat_table_get_bytes(reinterpret_cast<LhatEnum *>(lhat_as_object(kind))->members,
                                     eventNames[int(e.kind)],
                                     std::char_traits<char>::length(eventNames[int(e.kind)]));
    field(m, t, "kind", kind);
    field(m, t, "request", lhat_integer(e.request));
    field(m, t, "ok", lhat_bool(e.ok));
    field(m, t, "code", str(m, e.code));
    field(m, t, "lobbyId", str(m, e.lobbyId));
    field(m, t, "peerId", str(m, e.peerId));
    return t;
}
using Call = LhatValue (*)(LhatMachine *, const LhatValue *);
template <Call call>
void guard(LhatMachine *m, void *, const LhatValue *a, size_t, LhatValue *out,
           int *count) noexcept {
    try {
        out[0] = call(m, a);
        *count = 1;
    } catch (const std::exception &e) {
        if (api->lhat_machine_make_error(m, errorKind, e.what(), lhat_nil(), out))
            *count = 1;
        else
            api->lhat_machine_panic_text(m, "Could not allocate eos.Error");
    } catch (...) {
        api->lhat_machine_panic_text(m, "Unexpected native EOS exception");
    }
}
LhatValue create(LhatMachine *m, const LhatValue *a) {
    if (!lhat_is_object_kind(a[0], LHAT_OBJECT_TABLE))
        throw Failure("Expected a configuration table");
    auto *t = reinterpret_cast<LhatTable *>(lhat_as_object(a[0]));
    auto get = [&](const char *key, size_t n) {
        return string(api->lhat_table_get_bytes(t, key, n));
    };
    Config c{get("productId", 9),  get("sandboxId", 9),     get("deploymentId", 12),
             get("clientId", 8),   get("clientSecret", 12), get("cacheDirectory", 14),
             get("socketName", 10)};
    auto instance = std::make_unique<Client>(c);
    LhatValue v;
    if (!api->lhat_machine_make_hostdata(m, clientTag, instance.get(), &v))
        throw std::bad_alloc();
    instance.release();
    return v;
}
LhatValue tick(LhatMachine *, const LhatValue *a) {
    client(a[0]).tick();
    return lhat_nil();
}
LhatValue close(LhatMachine *, const LhatValue *a) {
    client(a[0]).close();
    return lhat_nil();
}
LhatValue poll(LhatMachine *m, const LhatValue *a) {
    auto e = client(a[0]).poll();
    return e ? eventValue(m, *e) : lhat_nil();
}
LhatValue loginSteam(LhatMachine *, const LhatValue *a) {
    return lhat_integer(client(a[0]).loginSteam(string(a[1])));
}
LhatValue loginDevice(LhatMachine *, const LhatValue *a) {
    return lhat_integer(client(a[0]).loginDevice(string(a[1])));
}
LhatValue createLobby(LhatMachine *, const LhatValue *a) {
    return lhat_integer(client(a[0]).createLobby(string(a[1]), integer(a[2], 64)));
}
LhatValue search(LhatMachine *, const LhatValue *a) {
    return lhat_integer(client(a[0]).search(string(a[1]), integer(a[2], 100)));
}
LhatValue join(LhatMachine *, const LhatValue *a) {
    return lhat_integer(client(a[0]).join(string(a[1])));
}
LhatValue leave(LhatMachine *, const LhatValue *a) {
    return lhat_integer(client(a[0]).leave());
}
LhatValue userId(LhatMachine *m, const LhatValue *a) {
    return str(m, client(a[0]).userId());
}
LhatValue lobby(LhatMachine *m, const LhatValue *a) {
    return lobbyValue(m, client(a[0]).lobby());
}
LhatValue results(LhatMachine *m, const LhatValue *a) {
    auto list = client(a[0]).results();
    auto t = table(m);
    for (size_t i = 0; i < list.size(); ++i)
        put(m, t, lhat_integer(i), lobbyValue(m, list[i]));
    return t;
}
LhatValue members(LhatMachine *m, const LhatValue *a) {
    auto list = client(a[0]).members();
    auto t = table(m);
    for (size_t i = 0; i < list.size(); ++i)
        put(m, t, lhat_integer(i), str(m, list[i]));
    return t;
}
LhatValue send(LhatMachine *, const LhatValue *a) {
    client(a[0]).send(string(a[1]), string(a[2]), uint8_t(integer(a[3], 255)),
                      EOS_EPacketReliability(choice(a[4], reliabilityNames, 3)));
    return lhat_nil();
}
LhatValue receive(LhatMachine *m, const LhatValue *a) {
    auto p = client(a[0]).receive();
    if (!p)
        return lhat_nil();
    auto t = table(m);
    field(m, t, "peerId", str(m, p->peer));
    field(m, t, "data", str(m, p->data));
    field(m, t, "channel", lhat_integer(p->channel));
    return t;
}
LhatValue disconnect(LhatMachine *, const LhatValue *a) {
    client(a[0]).disconnect(string(a[1]));
    return lhat_nil();
}
LhatValue relay(LhatMachine *, const LhatValue *a) {
    client(a[0]).relay(EOS_ERelayControl(choice(a[1], relayNames, 3)));
    return lhat_nil();
}
void dispose(LhatMachine *, void *, const LhatValue *a, size_t, LhatValue *, int *) noexcept {
    delete static_cast<Client *>(api->lhat_hostdata_pointer(a[0], clientTag));
}
#define LOBBY "t^{id:string^,owner:string^,bucket:string^,capacity:number^,available:number^}"
const char *install(const LhatExtensionAPI *host, LhatProgram *program, uint32_t phase, void **) {
    if (host->abi_version != LHAT_EXTENSION_ABI || host->struct_size < sizeof(*host))
        return "Incompatible L^ host API";
    api = host;
    if (phase == LHAT_EXTENSION_TYPES) {
        clientTag = api->lhat_register_hostdata_type(program, "eos", "Client");
        const char *errors[] = {"Failed"};
        if (!clientTag ||
            !api->lhat_register_error_kind(program, "eos", "Error", errors, 1, &errorKind,
                                           nullptr) ||
            !api->lhat_register_enum(program, "eos", nullptr, "EventKind", eventNames,
                                     sizeof(eventNames) / sizeof(*eventNames)) ||
            !api->lhat_register_enum(program, "eos", nullptr, "Reliability", reliabilityNames, 3) ||
            !api->lhat_register_enum(program, "eos", nullptr, "Relay", relayNames, 3))
            return "Could not register EOS types";
        return nullptr;
    }
    if (!api->lhat_register_func(program, "eos", "create",
                                 "p^t^{productId:string^,sandboxId:string^,deploymentId:string^,"
                                 "clientId:string^,clientSecret:string^,cacheDirectory:string^,"
                                 "socketName:string^} -> eos.Client|eos.Error;",
                                 guard<create>, nullptr))
        return "Could not register eos.create";
#define MEMBER(name, sig)                                                                          \
    if (!api->lhat_register_member(program, "eos", "Client", #name, sig, guard<name>, nullptr))    \
    return "Could not register Client." #name
    MEMBER(tick, "p^self^ -> nil^|eos.Error;");
    MEMBER(close, "p^self^ -> nil^|eos.Error;");
    MEMBER(poll, "p^self^ -> "
                 "t^{kind:eos.EventKind,request:number^,ok:bool^,code:string^,lobbyId:string^,"
                 "peerId:string^}|nil^|eos.Error;");
    MEMBER(loginSteam, "p^self^,string^ -> number^|eos.Error;");
    MEMBER(loginDevice, "p^self^,string^ -> number^|eos.Error;");
    MEMBER(createLobby, "p^self^,string^,number^ -> number^|eos.Error;");
    MEMBER(search, "p^self^,string^,number^ -> number^|eos.Error;");
    MEMBER(join, "p^self^,string^ -> number^|eos.Error;");
    MEMBER(leave, "p^self^ -> number^|eos.Error;");
    MEMBER(userId, "f^self^ -> string^|eos.Error;");
    MEMBER(lobby, "f^self^ -> " LOBBY "|eos.Error;");
    MEMBER(results, "f^self^ -> t^{" LOBBY "[]}|eos.Error;");
    MEMBER(members, "f^self^ -> t^{string^[]}|eos.Error;");
    MEMBER(send, "p^self^,string^,string^,number^,eos.Reliability -> nil^|eos.Error;");
    MEMBER(receive, "p^self^ -> t^{peerId:string^,data:string^,channel:number^}|nil^|eos.Error;");
    MEMBER(disconnect, "p^self^,string^ -> nil^|eos.Error;");
    MEMBER(relay, "p^self^,eos.Relay -> nil^|eos.Error;");
#undef MEMBER
    return api->lhat_register_member(program, "eos", "Client", "dispose", "p^self^;", dispose,
                                     nullptr)
               ? nullptr
               : "Could not register Client.dispose";
}
} // namespace
LHAT_EXTENSION_EXPORT const LhatExtension *lhat_extension_v2(void) {
    static const LhatExtension extension{
        LHAT_EXTENSION_ABI, sizeof(LhatExtension), LHAT_VERSION, sizeof(LhatValue), "eos",
        eos_signatures,     eos_signatures_length, install,      shutdownRuntime};
    return &extension;
}
