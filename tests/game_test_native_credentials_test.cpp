// Offline native-call ABI contract. Never starts a client or authenticates.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "../mods/game_test_client/native_actions.h"

namespace {
uint8_t network[0x80]{};
uint8_t app[0x500]{};
uint8_t* profile = nullptr;
int peer = 0, hashes = 0, sends = 0, constructors = 0, failures = 0;
char capturedUser[32]{}, capturedHash[40]{};
const char* plainHash = "fedcba9876543210fedcba9876543210";
void Check(bool value, const char* name) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}
uint8_t* __cdecl App() { return app; }
uint8_t* __cdecl Network() { return network; }
char* __cdecl Md5(const void*, char* output, int) {
    ++hashes; memcpy(output, plainHash, 33); return output;
}
void* __fastcall Packet(void* self, void*, const char* user, const char* hash) {
    ++constructors;
    strcpy_s(capturedUser, user); strcpy_s(capturedHash, hash);
    memset(self, 0, 0x4D); *static_cast<uint16_t*>(self) = 0x2329;
    return self;
}
int __fastcall Send(void*, void*, void* stream, const void* data, int size, char flags) {
    ++sends;
    Check(stream == &peer && size == 0x4D && flags == 0 &&
        *static_cast<const uint16_t*>(data) == 0x2329, "normal login transport ABI");
    return size;
}
void __cdecl Unused() {}
int __fastcall Dispatch(void*, void*, int, int, int, int, void*) { return 73; }
void* Symbol(SdgoMod*, const char* name) {
    if (!strcmp(name, "game.app")) return reinterpret_cast<void*>(&App);
    if (!strcmp(name, "flow.network")) return reinterpret_cast<void*>(&Network);
    if (!strcmp(name, "flow.password_md5_hex")) return reinterpret_cast<void*>(&Md5);
    if (!strcmp(name, "flow.login_packet_ctor")) return reinterpret_cast<void*>(&Packet);
    if (!strcmp(name, "flow.network_send")) return reinterpret_cast<void*>(&Send);
    if (!strcmp(name, "flow.login_service_dispatch")) return reinterpret_cast<void*>(&Dispatch);
    if (!strcmp(name, "flow.profile_pointer")) return &profile;
    return reinterpret_cast<void*>(&Unused);
}
int Hook(SdgoMod* mod, const char* name, void*, void** original) {
    *original = Symbol(mod, name); return *original != nullptr;
}
}

int main() {
    *reinterpret_cast<void**>(network + 0x14) = &peer;
    SdgoApi api{}; api.size = sizeof(api); api.version = SDGO_API_VERSION;
    api.Symbol = &Symbol; api.HookCode = &Hook;
    NativeActions actions;
    NativeActions::Config config{"existing", "short1", "offline", 1, 0};
    auto* mod = reinterpret_cast<SdgoMod*>(1);
    Check(actions.Bind(&api, mod, config), "plaintext account binds");
    Check(actions.Login(), "plaintext login sends");
    Check(hashes == 1 && constructors == 1 && sends == 1, "plaintext uses original hasher/constructor/sender once");
    Check(!strcmp(capturedHash, plainHash), "native hash reaches original packet constructor");
    const char* imported = "0123456789abcdef0123456789abcdef";
    config.password = imported; config.password_is_md5 = true;
    Check(actions.Bind(&api, mod, config), "explicit imported credential binds");
    Check(actions.Login(), "imported credential still uses native authentication request");
    Check(hashes == 1 && constructors == 2 && sends == 2, "imported credential is not hashed a second time");
    Check(!strcmp(capturedUser, "existing") && !strcmp(capturedHash, imported), "existing identity and credential are preserved exactly");
    const char* invalidDigests[]{"short", "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", "0123456789abcdef0123456789abcdef0"};
    for (const char* invalid : invalidDigests) {
        config.password = invalid;
        Check(!actions.Bind(&api, mod, config), "malformed imported digest is rejected");
        Check(!actions.Login(), "malformed credential never reaches transport");
    }
    Check(hashes == 1 && constructors == 2 && sends == 2, "invalid credentials cause no native request");
    std::printf("OFFLINE native credential failures: %d\n", failures);
    return failures ? 1 : 0;
}
