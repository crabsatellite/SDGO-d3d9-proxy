#include "config.h"

static bool ReadBool(const char* path, const char* section, const char* key, bool def)
{
    char buf[16];
    GetPrivateProfileStringA(section, key, def ? "1" : "0", buf, sizeof(buf), path);
    return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y' || buf[0] == 'Y';
}

static int ReadInt(const char* path, const char* section, const char* key, int def)
{
    return GetPrivateProfileIntA(section, key, def, path);
}

void ProxyConfig::Load(const char* iniPath)
{
    enableLog        = ReadBool(iniPath, "General", "EnableLog", enableLog);
    forceAnisotropic = ReadBool(iniPath, "Quality", "ForceAnisotropic", forceAnisotropic);
    anisotropicLevel = ReadInt(iniPath, "Quality", "AnisotropicLevel", anisotropicLevel);
    forceTrilinear   = ReadBool(iniPath, "Quality", "ForceTrilinear", forceTrilinear);
}
