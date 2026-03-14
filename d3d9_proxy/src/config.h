#pragma once
#include <windows.h>

struct ProxyConfig
{
    // General
    bool enableLog = false;

    // Quality enhancements (proxy-level, always active)
    bool forceAnisotropic = true;
    int  anisotropicLevel = 16;
    bool forceTrilinear = true;

    void Load(const char* iniPath);
};

extern ProxyConfig g_Config;
