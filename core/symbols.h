#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sdgo
{
    // One entry of sdgo/symbols/<host_key>.ini.
    //   kind=code  callable or detourable function; `expect` must match its first bytes
    //   kind=site  any other code bytes a mod depends on; `expect` must match
    //   kind=slot  pointer-sized slot (vtable entry); it must currently hold base+`target_rva`
    //   kind=data  plain address, verified only when `expect` is given
    struct Symbol
    {
        std::string name;
        std::string kind;
        uintptr_t address = 0;
        std::vector<int> expect; // byte values, -1 for a "??" wildcard
        uintptr_t expectedTarget = 0;
        bool verified = false;
        std::string problem;
    };

    class SymbolTable
    {
    public:
        // Loads and verifies every entry against the running host. Returns false when the
        // file is absent or was written for a different build.
        bool Load(const std::wstring& path, HMODULE host, const char* hostKey);
        const Symbol* Find(const char* name) const;
        size_t Count() const { return m_symbols.size(); }
        size_t VerifiedCount() const;

    private:
        std::vector<Symbol> m_symbols;
    };
}
