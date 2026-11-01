#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace capsule_catalog
{
    struct NativeFileInfo
    {
        std::array<uint32_t, 9> words = {};
    };

    struct OverlayEntry
    {
        NativeFileInfo info;
    };

    struct Bundle
    {
        std::wstring zpkPath;
        std::string catalogRevision;
        uint32_t chunkSize = 0;
        uint32_t fixedCount = 0;
        uint32_t coinCount = 0;
        uint32_t customCount = 0;
        uint32_t candidateCount = 0;
        std::unordered_map<std::wstring, OverlayEntry> entries;

        const OverlayEntry* Find(const std::wstring& nativePath) const;
    };

    bool IsOverlayGdtPath(const std::wstring& path);

    // Consumes a tracked stream marker exactly once. A mismatched or missing
    // native path still removes the marker so pointer reuse cannot authorize a
    // later checksum call.
    bool ConsumeOverlayGdtChecksum(
        std::unordered_map<void*, std::wstring>& trackedStreams,
        void* stream,
        const std::wstring* nativePath);

    // Loads a complete compiler output bundle from runtime.ini. Every declared
    // GDT and both archive files are hashed before any result is published.
    // On failure, out is left unchanged so the caller can use the stock pack.
    bool LoadBundle(const std::wstring& modDirectory, Bundle& out, std::string& error);
}
