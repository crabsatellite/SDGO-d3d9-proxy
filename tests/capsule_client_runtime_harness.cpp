#include <cstdio>
#include <string>
#include <unordered_map>

#include "capsule_runtime.h"

namespace
{
    bool CheckChecksumGate()
    {
        std::unordered_map<void*, std::wstring> markers;
        void* first = reinterpret_cast<void*>(1);
        void* second = reinterpret_cast<void*>(2);
        const std::wstring gdt0033 = L"gdrs\\0033.gdt";
        const std::wstring gdt0101 = L"gdrs\\0101.gdt";
        const std::wstring unapproved = L"gdrs\\9999.gdt";

        markers[first] = gdt0033;
        if (!capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, &gdt0033) ||
            !markers.empty() ||
            capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, &gdt0033))
            return false;

        markers[first] = gdt0033;
        if (capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, &gdt0101) ||
            !markers.empty())
            return false;

        markers[first] = gdt0033;
        if (capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, nullptr) ||
            !markers.empty())
            return false;

        markers[first] = unapproved;
        if (capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, &unapproved) ||
            !markers.empty())
            return false;
        const std::wstring ui = L"uirs\\1161.uir";
        markers[first] = ui;
        if (capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, &ui) ||
            !markers.empty())
            return false;

        markers[first] = gdt0033;
        if (capsule_catalog::ConsumeOverlayGdtChecksum(markers, second, &gdt0033) ||
            markers.size() != 1)
            return false;
        return true;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2 || argc > 3) {
        std::puts("usage: capsule_client_runtime_harness <bundle> [rejected-bundle]");
        return 2;
    }
    capsule_catalog::Bundle bundle;
    std::string error;
    if (!capsule_catalog::LoadBundle(argv[1], bundle, error)) {
        std::printf("FAIL %s\n", error.c_str());
        return 1;
    }
    if (!CheckChecksumGate()) {
        std::puts("CHECKSUM_GATE 0");
        return 4;
    }
    std::puts("CHECKSUM_GATE 1");
    std::printf("OK %s %u\n", bundle.catalogRevision.c_str(),
                static_cast<unsigned>(bundle.entries.size()));
    for (const wchar_t* name : {L"gdrs\\0033.gdt", L"gdrs\\0101.gdt",
                                L"gdrs\\0154.gdt", L"gdrs\\0155.gdt",
                                L"uirs\\1161.uir",
                                L"sprs\\60000.spr", L"txrs\\60000.txr"}) {
        std::printf("FIND %ls %d\n", name, bundle.Find(name) != nullptr ? 1 : 0);
    }
    if (argc == 3) {
        const std::string revision = bundle.catalogRevision;
        const size_t entries = bundle.entries.size();
        error.clear();
        if (capsule_catalog::LoadBundle(argv[2], bundle, error)) {
            std::puts("SECOND_UNEXPECTED_OK");
            return 3;
        }
        std::printf("SECOND_FAIL %s\n", error.c_str());
        std::printf("PRESERVED %d\n",
                    bundle.catalogRevision == revision && bundle.entries.size() == entries ? 1 : 0);
    }
    return 0;
}
