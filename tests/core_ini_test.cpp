#include "../core/ini_string.h"
#include <cstdio>

int main()
{
    wchar_t temporary[MAX_PATH]{}, file[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH,temporary) || !GetTempFileNameW(temporary,L"sgi",0,file))
        return 1;
    int failures=0;
    const size_t lengths[]={0,1,2046,2047,2048,4095,8192,16384};
    for (size_t length:lengths) {
        const std::wstring value(length,L'x');
        if (!WritePrivateProfileStringW(L"mod",L"requires",value.c_str(),file) ||
            sdgo::ReadIniString(file,L"mod",L"requires",L"missing")!=value) {
            std::printf("FAIL: complete INI value length %zu\n",length);++failures;
        }
    }
    if (sdgo::ReadIniString(file,L"mod",L"absent",L"fallback")!=L"fallback")++failures;
    DeleteFileW(file);
    std::printf("Windows INI complete-value regression: %d failures\n",failures);
    return failures ? 1 : 0;
}
