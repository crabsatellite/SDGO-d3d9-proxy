// Optional local probe. Reads explicitly supplied, hash-pinned game images;
// executes only their result-field emitter/parser slices in this isolated host.
// It never starts GOnline/BattleServer, connects to a server or writes a wallet.
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "mods/battle_rewards/result_presentation.h"

static_assert(sizeof(void*) == 4, "native result probe requires Win32");
namespace {
int failures = 0;
unsigned checks = 0;
void Check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", label); }
}
template<class T> T Get(const BYTE* base, ptrdiff_t offset) {
    T value{}; std::memcpy(&value, base + offset, sizeof(value)); return value;
}
template<class T> void Put(BYTE* base, ptrdiff_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

std::string Sha256(const std::vector<BYTE>& bytes) {
    std::array<BYTE, 32> digest{};
    if (bytes.size() > (std::numeric_limits<ULONG>::max)())
        throw std::runtime_error("invalid hash input");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider unavailable");
    const NTSTATUS status = BCryptHash(algorithm, nullptr, 0,
        const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest.data(),
        static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("SHA256 failed");
    std::string text;
    for (BYTE value : digest) {
        text += "0123456789abcdef"[value >> 4];
        text += "0123456789abcdef"[value & 15];
    }
    return text;
}

struct Image {
    std::vector<BYTE> raw;
    explicit Image(const wchar_t* path, const char* expected) {
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        raw.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (!input.eof() && input.fail()) throw std::runtime_error("image read failed");
        if (Sha256(raw) != expected) throw std::runtime_error("unsupported image SHA256");
    }
    std::vector<BYTE> Slice(uint32_t va, size_t length) const {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(raw.data() + dos->e_lfanew);
        const uint32_t rva = va - nt->OptionalHeader.ImageBase;
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (rva < section[i].VirtualAddress) continue;
            const size_t local = rva - section[i].VirtualAddress;
            const size_t offset = section[i].PointerToRawData + local;
            if (local + length <= section[i].SizeOfRawData && offset + length <= raw.size())
                return {raw.begin() + offset, raw.begin() + offset + length};
        }
        throw std::runtime_error("native slice outside file");
    }
};

struct Patch { size_t offset; const void* target; };
struct Code {
    BYTE* memory = nullptr;
    Code(const Image& image, uint32_t address, size_t length,
         std::initializer_list<Patch> patches = {}, bool append_ret = false) {
        const auto bytes = image.Slice(address, length);
        memory = static_cast<BYTE*>(VirtualAlloc(nullptr, length + 1,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!memory) throw std::runtime_error("native slice allocation failed");
        std::memcpy(memory, bytes.data(), length);
        for (const auto& patch : patches) {
            if (patch.offset + 5 > length || memory[patch.offset] != 0xE8)
                throw std::runtime_error("native call anchor mismatch");
            const uint32_t relative = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(patch.target) -
                reinterpret_cast<uintptr_t>(memory + patch.offset + 5));
            std::memcpy(memory + patch.offset + 1, &relative, 4);
        }
        if (append_ret) memory[length] = 0xC3;
        DWORD previous = 0;
        if (!VirtualProtect(memory, length + 1, PAGE_EXECUTE_READ, &previous))
            throw std::runtime_error("native slice protection failed");
        FlushInstructionCache(GetCurrentProcess(), memory, length + 1);
    }
    Code(const Code&) = delete;
    Code& operator=(const Code&) = delete;
    ~Code() { if (memory) VirtualFree(memory, 0, MEM_RELEASE); }
};

void __fastcall BaseConstructor(void*, void*) {}
std::array<BYTE, 0x200> wallet{};
void* __cdecl CurrentWallet() { return wallet.data(); }
std::array<uint32_t, 2> displayed{};
unsigned displayed_count = 0;
void __fastcall DisplayNumber(void*, void*, uint32_t value) {
    if (displayed_count < displayed.size()) displayed[displayed_count] = value;
    ++displayed_count;
}

__declspec(naked) void __cdecl RunEmitter(void*, BYTE*) {
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov eax, [ebp + 8]
        mov edx, [ebp + 12]
        push ebp
        mov ebp, edx
        call eax
        pop ebp
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

struct Contract {
    uint32_t emitter;
    ptrdiff_t rank, packet, total, base;
    std::array<ptrdiff_t, 4> bonuses;
};
// Independent trace from the exact supported server packet writers. Each
// 0xBC-byte slice copies five display rows and total into its native result.
constexpr std::array<Contract, 10> contracts = {{
    {0x48D78E,-0x914,-0x740,-0x28,-0x758,{{-0x10,-0x76C,-0x744,-0x87C}}},
    {0x48F68B,-0x980,-0x730,-0x20,-0x744,{{-0x08,-0x754,-0x734,-0x834}}},
    {0x4915D4,-0x980,-0x730,-0x20,-0x744,{{-0x08,-0x754,-0x734,-0x834}}},
    {0x49344C,-0x984,-0x738,-0x24,-0x74C,{{-0x08,-0x75C,-0x73C,-0x83C}}},
    {0x495461,-0x994,-0x738,-0x24,-0x74C,{{-0x08,-0x760,-0x73C,-0x844}}},
    {0x497668,-0x990,-0x738,-0x20,-0x74C,{{-0x08,-0x75C,-0x73C,-0x83C}}},
    {0x4995B0,-0x990,-0x738,-0x20,-0x74C,{{-0x08,-0x75C,-0x73C,-0x83C}}},
    {0x49B5A3,-0x980,-0x730,-0x20,-0x744,{{-0x08,-0x754,-0x734,-0x834}}},
    {0x49D68B,-0x984,-0x730,-0x20,-0x744,{{-0x08,-0x754,-0x734,-0x834}}},
    {0x49F69A,-0x8D8,-0x738,-0x20,-0x74C,{{-0x08,-0x75C,-0x73C,-0x840}}},
}};
using ClientFn = void(__thiscall*)(void*, const void*);

void Verify(const Image& server, const Code& event_code, const Code& ui_code,
            unsigned site, uint16_t amount, bool repaired) {
    std::array<BYTE, 0xD00> storage{};
    BYTE* frame = storage.data() + 0xC80;
    const auto& c = contracts[site];
    Put(frame, c.rank, uint32_t{0});
    Put(frame, c.base, uint16_t{16});
    for (ptrdiff_t offset : c.bonuses) Put(frame, offset, uint16_t{0});
    Put(frame, c.total, amount);
    const auto old_frame = storage;
    if (repaired) {
        Check(sdgo::battle_rewards::WriteNativeGResult(frame, site, amount), "publish consistent G fields");
        for (size_t i = 0; i < storage.size(); ++i) {
            const ptrdiff_t relative = static_cast<ptrdiff_t>(i) - 0xC80;
            bool permitted = relative == c.total || relative == c.total + 1 ||
                             relative == c.base || relative == c.base + 1;
            for (ptrdiff_t offset : c.bonuses)
                permitted = permitted || relative == offset || relative == offset + 1;
            Check(permitted || storage[i] == old_frame[i], "unrelated frame bytes preserved");
        }
    }
    Code emitter(server, c.emitter, 0xBC, {}, true);
    RunEmitter(emitter.memory, frame);
    const BYTE* packet = frame + c.packet;
    Check(Get<uint16_t>(packet, 0x18) == amount, "native server result total is correct");
    std::array<BYTE, 0x80> event{};
    std::array<BYTE, 0x180> ui{};
    reinterpret_cast<ClientFn>(event_code.memory)(event.data(), packet);
    displayed_count = 0;
    reinterpret_cast<ClientFn>(ui_code.memory)(ui.data(), event.data());
    Check(Get<uint32_t>(ui.data(), 0xA4) == amount, "native skip-animation total is correct");
    const uint32_t expected_display = repaired ? amount : 16u;
    Check(Get<uint32_t>(ui.data(), 0x90) == expected_display, "native initial GB display");
    Check(displayed_count == 2 && displayed[0] == expected_display, "native number setter receives expected GB");
    uint32_t animated = Get<uint32_t>(ui.data(), 0x90);
    for (unsigned row = 0; row < 4; ++row) {
        if (ui[0x54 + row]) animated += Get<uint16_t>(ui.data(), 0x58 + 2 * row);
    }
    Check(animated == expected_display, "normal-animation GB follows display rows");
    Check(repaired ? animated == amount : animated != amount, "legacy mismatch or repaired agreement");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::puts("Usage: probe GOnline.exe BattleServer.exe"); return 2; }
    try {
        Image client(argv[1], "e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92");
        Image server(argv[2], "f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4");
        Code event(client, 0x4D0630, 0x90, {{6, reinterpret_cast<void*>(&BaseConstructor)}});
        Code bonus(client, 0x4E2F20, 0x66);
        Code ui(client, 0x4E3D60, 0x6F,
            {{0x2D, bonus.memory}, {0x40, reinterpret_cast<void*>(&DisplayNumber)},
             {0x4F, reinterpret_cast<void*>(&CurrentWallet)}, {0x64, reinterpret_cast<void*>(&DisplayNumber)}});
        for (unsigned site = 0; site < contracts.size(); ++site) {
            Verify(server, event, ui, site, 22000, false);
            for (uint16_t amount : {uint16_t{0}, uint16_t{4871}, uint16_t{10655}, uint16_t{22000}, uint16_t{65535}})
                Verify(server, event, ui, site, amount, true);
        }
        std::puts("Legacy fixture: total 22000 / display 16 reproduced for all 10 native emitters.");
        std::puts("Repaired: initial display, animated row sum and skip total agree for all 10 emitters.");
        Check(!sdgo::battle_rewards::WriteNativeGResult(nullptr, 0, 1), "null frame rejected");
        std::array<BYTE, 16> untouched{};
        Check(!sdgo::battle_rewards::WriteNativeGResult(untouched.data(), 10, 1), "invalid site rejected");
    } catch (const std::exception& error) {
        std::printf("PROBE ERROR: %s\n", error.what()); return 2;
    }
    std::printf("%u checks, %d failures\n", checks, failures);
    std::puts("Boundary: native packet/UI parsing executed; no rendered HUD or authenticated match.");
    return failures ? 1 : 0;
}
