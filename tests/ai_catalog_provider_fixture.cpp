#include <cstdint>
// An isolated provider contract fixture; never staged into a game package.
extern "C" __declspec(dllexport) int __cdecl SdgoAiCatalogPoolV1(int level,uint32_t* ids,uint32_t capacity) {
    if(level==2) return 0;
    if(level==3) return -1;
    if(level==4) return 3001;
    if(level!=1 || capacity<2 || !ids) return -1;
    ids[0]=700;ids[1]=701;return 2;
}
