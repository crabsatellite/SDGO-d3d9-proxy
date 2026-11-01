#include "ai_test_support.h"
#include "sdgo/ai_catalog_provider.h"
#include <filesystem>
namespace {
std::wstring directory;
const wchar_t* Directory(SdgoMod*) { return directory.c_str(); }
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    const auto path=std::filesystem::absolute(argv[1]);
    directory=(path.parent_path().parent_path()/L"pvp_ai").wstring();
    SdgoApi api=ai_test::api;api.ModDirectory=&Directory;
    std::vector<uint32_t> pool{123};
    SCENARIO("optional catalog is untouched when not installed");
    CHECK(!sdgo::AiCatalogPool(&api,nullptr,1,pool));CHECK(pool==std::vector<uint32_t>{123});
    const HMODULE module=LoadLibraryW(path.c_str());CHECK(module!=nullptr);
    if(module) {
        SCENARIO("active provider is authoritative, including empty and refused pools");
        CHECK(sdgo::AiCatalogPool(&api,nullptr,1,pool));CHECK(pool==std::vector<uint32_t>({700,701}));
        for(int level:{2,3,4}) {
            pool={123};CHECK(sdgo::AiCatalogPool(&api,nullptr,level,pool));CHECK(pool.empty());
        }
        FreeLibrary(module);
        CHECK(!sdgo::AiCatalogPool(&api,nullptr,1,pool));
    }
    return ai_test::Finish();
}
