#pragma once
#include <d3d9.h>
#include <d3dcompiler.h>
#include <unordered_map>
#include <string>

// Manages per-texture normal map injection for PBR-style rendering.
//
// Flow:
//   1. On startup, loads a hashmap file mapping CRC32 → DDS filename
//   2. When SetTexture(0, pTex) is called, hashes the texture (cached)
//   3. If a matching normal map exists, lazy-loads the DDS file
//   4. Before Draw* calls, the device proxy checks GetCurrentNormalMap()
//      and injects the normal map shader + texture on stage 1
//
// Hashmap file format (_normalmap.txt):
//   # comment lines start with #
//   <hex CRC32> <relative DDS path>
//   Example:
//   A3F1B2C4 normals/tex_body_01.dds
//   ...

class TextureManager
{
public:
    bool Init(IDirect3DDevice9* pDevice, const char* baseDir);
    void Release();

    // Called from SetTexture(0, pTex) — identifies texture and finds normal map
    void OnSetDiffuseTexture(IDirect3DTexture9* pTex);

    // Returns normal map for current diffuse texture (or nullptr)
    IDirect3DTexture9* GetCurrentNormalMap() const { return m_pCurrentNormal; }

    // Compiled pixel shader for normal-mapped fixed-function replacement
    IDirect3DPixelShader9* GetNormalMapShader() const { return m_pNormalShader; }

    // Compiled pixel shader for normal-mapped + vertex color (alpha test path)
    IDirect3DPixelShader9* GetNormalMapAlphaShader() const { return m_pNormalAlphaShader; }

    void OnLostDevice();
    void OnResetDevice(IDirect3DDevice9* pDevice);

private:
    uint32_t HashTexture(IDirect3DTexture9* pTex);
    bool LoadHashmap(const char* path);
    IDirect3DTexture9* LoadDDS(const char* path);
    IDirect3DPixelShader9* CompilePS(const char* source, const char* entry);

    IDirect3DDevice9* m_pDevice = nullptr;
    std::string m_baseDir;

    // CRC32 → relative DDS path
    std::unordered_map<uint32_t, std::string> m_hashToFile;

    // CRC32 → loaded normal map texture (lazy-loaded)
    std::unordered_map<uint32_t, IDirect3DTexture9*> m_loadedNormals;

    // Texture pointer → CRC32 (avoids re-hashing each frame)
    std::unordered_map<IDirect3DTexture9*, uint32_t> m_texHashCache;

    // Special sentinel: hash was computed but no normal map exists
    static constexpr uint32_t HASH_NO_MATCH = 0;

    IDirect3DTexture9* m_pCurrentNormal = nullptr;

    // Shaders
    IDirect3DPixelShader9* m_pNormalShader = nullptr;
    IDirect3DPixelShader9* m_pNormalAlphaShader = nullptr;

    bool m_initialized = false;
};
