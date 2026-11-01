#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "../mods/hangar_capacity/capacity_policy.h"

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 2; // expected database, account, lock milliseconds, output
    try {
        std::size_t used = 0;
        const std::wstring number(argv[2]);
        const auto account = std::stoll(number, &used);
        if (used != number.size()) return 2;
        const std::wstring wait(argv[3]);
        const auto timeout = std::stoi(wait, &used);
        if (used != wait.size()) return 2;
        const auto sql = sdgo::hangar_capacity::MakeEnsureSql(argv[1], account, timeout);
        if (!sql) return 2;
        const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, sql->data(),
            static_cast<int>(sql->size()), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) return 2;
        std::string text(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, sql->data(), static_cast<int>(sql->size()),
                text.data(), bytes, nullptr, nullptr) != bytes) return 2;
        std::ofstream output(std::filesystem::path(argv[4]), std::ios::binary | std::ios::trunc);
        output.write(text.data(), text.size());
        return output ? 0 : 1;
    } catch (...) { return 2; }
}
