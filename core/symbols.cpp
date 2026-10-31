#include "symbols.h"

#include <cstdlib>
#include <cstring>

#include "log.h"
#include "sdgo/sdgo_api.h"

namespace
{
    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        out.reserve(text.size());
        for (wchar_t c : text)
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        return out;
    }

    std::wstring Value(const std::wstring& path, const wchar_t* section, const wchar_t* key)
    {
        wchar_t buffer[1024] = {};
        GetPrivateProfileStringW(section, key, L"", buffer, 1024, path.c_str());
        return buffer;
    }

    bool ParseNumber(const std::wstring& text, uintptr_t& out)
    {
        if (text.empty())
            return false;
        wchar_t* end = nullptr;
        unsigned long value = wcstoul(text.c_str(), &end, 0);
        if (!end || *end != L'\0')
            return false;
        out = value;
        return true;
    }

    bool ParseBytes(const std::wstring& text, std::vector<int>& out)
    {
        out.clear();
        size_t i = 0;
        while (i < text.size()) {
            if (text[i] == L' ' || text[i] == L'\t') {
                ++i;
                continue;
            }
            if (i + 1 >= text.size())
                return false;
            std::wstring token = text.substr(i, 2);
            if (token == L"??") {
                out.push_back(-1);
            } else {
                wchar_t* end = nullptr;
                unsigned long value = wcstoul(token.c_str(), &end, 16);
                if (!end || *end != L'\0')
                    return false;
                out.push_back(static_cast<int>(value));
            }
            i += 2;
        }
        return !out.empty();
    }

    bool Readable(const void* address, size_t bytes)
    {
        MEMORY_BASIC_INFORMATION info = {};
        if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT)
            return false;
        if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return false;
        auto end = reinterpret_cast<uintptr_t>(address) + bytes;
        return end <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    }

    bool BytesMatch(uintptr_t address, const std::vector<int>& expect, std::string& problem)
    {
        if (!Readable(reinterpret_cast<void*>(address), expect.size())) {
            problem = "address not readable";
            return false;
        }
        auto bytes = reinterpret_cast<const uint8_t*>(address);
        for (size_t i = 0; i < expect.size(); ++i) {
            if (expect[i] >= 0 && bytes[i] != expect[i]) {
                char text[96];
                sprintf_s(text, "byte +%u is %02X, expected %02X", static_cast<unsigned>(i), bytes[i], expect[i]);
                problem = text;
                return false;
            }
        }
        return true;
    }
}

bool sdgo::SymbolTable::Load(const std::wstring& path, HMODULE host, const char* hostKey)
{
    m_symbols.clear();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log::Writef("core", SDGO_LOG_WARN, "no symbol file for host %s", hostKey);
        return false;
    }

    auto base = reinterpret_cast<uintptr_t>(host);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uintptr_t timestamp = 0, sizeOfImage = 0;
    if (!ParseNumber(Value(path, L"host", L"timestamp"), timestamp) ||
        !ParseNumber(Value(path, L"host", L"size_of_image"), sizeOfImage) ||
        timestamp != nt->FileHeader.TimeDateStamp ||
        sizeOfImage != nt->OptionalHeader.SizeOfImage) {
        log::Writef("core", SDGO_LOG_ERROR, "symbol file [host] does not describe %s; ignoring it", hostKey);
        return false;
    }

    std::vector<wchar_t> names(64 * 1024);
    DWORD length = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), path.c_str());
    for (const wchar_t* section = names.data(); section < names.data() + length && *section; section += wcslen(section) + 1) {
        if (_wcsicmp(section, L"host") == 0)
            continue;

        Symbol symbol;
        symbol.name = Narrow(section);
        symbol.kind = Narrow(Value(path, section, L"kind"));
        uintptr_t rva = 0;
        if (!ParseNumber(Value(path, section, L"rva"), rva) || rva >= sizeOfImage) {
            symbol.problem = "bad rva";
        } else {
            symbol.address = base + rva;
            std::wstring expect = Value(path, section, L"expect");
            if (!expect.empty() && !ParseBytes(expect, symbol.expect))
                symbol.problem = "bad expect bytes";
        }

        if (symbol.problem.empty()) {
            if (symbol.kind == "code" || symbol.kind == "site") {
                if (symbol.expect.size() < 5)
                    symbol.problem = "code and site symbols need at least 5 expected bytes";
                else
                    symbol.verified = BytesMatch(symbol.address, symbol.expect, symbol.problem);
            } else if (symbol.kind == "slot") {
                uintptr_t targetRva = 0;
                if (!ParseNumber(Value(path, section, L"target_rva"), targetRva)) {
                    symbol.problem = "slot needs target_rva";
                } else if (!Readable(reinterpret_cast<void*>(symbol.address), sizeof(void*))) {
                    symbol.problem = "slot not readable";
                } else {
                    symbol.expectedTarget = base + targetRva;
                    uintptr_t current = *reinterpret_cast<uintptr_t*>(symbol.address);
                    symbol.verified = current == symbol.expectedTarget;
                    if (!symbol.verified) {
                        char text[96];
                        sprintf_s(text, "slot holds %08X, expected %08X", static_cast<unsigned>(current),
                                  static_cast<unsigned>(symbol.expectedTarget));
                        symbol.problem = text;
                    }
                }
            } else if (symbol.kind == "data") {
                symbol.verified = symbol.expect.empty() || BytesMatch(symbol.address, symbol.expect, symbol.problem);
            } else {
                symbol.problem = "unknown kind";
            }
        }

        if (symbol.verified)
            log::Writef("core", SDGO_LOG_DEBUG, "symbol %s ok at %08X", symbol.name.c_str(), static_cast<unsigned>(symbol.address));
        else
            log::Writef("core", SDGO_LOG_WARN, "symbol %s rejected: %s", symbol.name.c_str(), symbol.problem.c_str());
        m_symbols.push_back(std::move(symbol));
    }
    return true;
}

const sdgo::Symbol* sdgo::SymbolTable::Find(const char* name) const
{
    for (const Symbol& symbol : m_symbols) {
        if (symbol.name == name)
            return &symbol;
    }
    return nullptr;
}

size_t sdgo::SymbolTable::VerifiedCount() const
{
    size_t count = 0;
    for (const Symbol& symbol : m_symbols)
        count += symbol.verified ? 1 : 0;
    return count;
}
