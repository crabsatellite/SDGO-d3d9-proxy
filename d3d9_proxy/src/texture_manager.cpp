#include "texture_manager.h"
#include "config.h"
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

// ============================================================
// CRC32 (ISO 3309 / ITU-T V.42)
// ============================================================

static uint32_t s_crc32Table[256];
static bool s_crc32Init = false;

static void InitCRC32()
{
    if (s_crc32Init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
        s_crc32Table[i] = c;
    }
    s_crc32Init = true;
}

static uint32_t CRC32(const void* data, size_t len)
{
    InitCRC32();
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++)
        crc = s_crc32Table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

// ============================================================
// DDS file format structures
// ============================================================

#ifndef MAKEFOURCC
#define MAKEFOURCC(a,b,c,d) ((DWORD)(a)|((DWORD)(b)<<8)|((DWORD)(c)<<16)|((DWORD)(d)<<24))
#endif

#define DDS_MAGIC 0x20534444  // "DDS "

struct DDS_PIXELFORMAT {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwABitMask;
};

#define DDPF_FOURCC    0x4
#define DDPF_RGB       0x40
#define DDPF_ALPHAPIXELS 0x1

struct DDS_HEADER {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwHeight;
    DWORD dwWidth;
    DWORD dwPitchOrLinearSize;
    DWORD dwDepth;
    DWORD dwMipMapCount;
    DWORD dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    DWORD dwCaps;
    DWORD dwCaps2;
    DWORD dwCaps3;
    DWORD dwCaps4;
    DWORD dwReserved2;
};

// ============================================================
// Normal-map pixel shader (HLSL ps_3_0)
// ============================================================

// Replaces fixed-function pixel pipeline when a normal map is available.
// s0 = diffuse texture, s1 = tangent-space normal map
// c0 = (strength, lightDirX, lightDirY, lightDirZ)
//
// Since SDGO uses fixed-function vertex pipeline, COLOR0 contains the
// interpolated vertex diffuse color (includes pre-computed lighting).
// We modulate it with a detail factor derived from the normal map.
static const char g_szNormalMap[] = R"(
sampler2D DiffuseSampler : register(s0);
sampler2D NormalSampler  : register(s1);
float4 Params : register(c0);  // x=strength, y=lightX, z=lightY, w=lightZ

float4 main(float2 uv : TEXCOORD0, float4 vcolor : COLOR0) : COLOR0
{
    float4 diffuse = tex2D(DiffuseSampler, uv);

    // Decode tangent-space normal from [0,1] → [-1,1]
    float3 normal = tex2D(NormalSampler, uv).rgb * 2.0 - 1.0;

    // Scale XY by strength, recompute Z for unit length
    normal.xy *= Params.x;
    normal.z = sqrt(max(0.001, 1.0 - dot(normal.xy, normal.xy)));
    normal = normalize(normal);

    // Light direction in tangent space (configurable, default top-right-front)
    float3 lightDir = normalize(float3(Params.y, Params.z, Params.w));

    // Half-Lambert for soft, non-harsh shading
    float NdotL = dot(normal, lightDir);
    float lighting = NdotL * 0.4 + 0.6;  // remap [-1,1] → [0.2, 1.0]

    // Blend: 1.0 = no change, strength controls how much normal map affects output
    float detail = lerp(1.0, lighting, Params.x);

    // Output: texture * vertex color * normal detail
    float4 result;
    result.rgb = diffuse.rgb * vcolor.rgb * detail;
    result.a = diffuse.a * vcolor.a;  // preserve alpha for alpha test/blend
    return result;
}
)";

// Alpha-test variant: same but outputs diffuse alpha unmodified by vertex color
// (some SDGO objects use alpha test with texture alpha only)
static const char g_szNormalMapAlpha[] = R"(
sampler2D DiffuseSampler : register(s0);
sampler2D NormalSampler  : register(s1);
float4 Params : register(c0);

float4 main(float2 uv : TEXCOORD0, float4 vcolor : COLOR0) : COLOR0
{
    float4 diffuse = tex2D(DiffuseSampler, uv);
    float3 normal = tex2D(NormalSampler, uv).rgb * 2.0 - 1.0;
    normal.xy *= Params.x;
    normal.z = sqrt(max(0.001, 1.0 - dot(normal.xy, normal.xy)));
    normal = normalize(normal);

    float3 lightDir = normalize(float3(Params.y, Params.z, Params.w));
    float NdotL = dot(normal, lightDir);
    float lighting = NdotL * 0.4 + 0.6;
    float detail = lerp(1.0, lighting, Params.x);

    float4 result;
    result.rgb = diffuse.rgb * vcolor.rgb * detail;
    result.a = diffuse.a;  // alpha from texture only
    return result;
}
)";

// ============================================================
// Implementation
// ============================================================

bool TextureManager::Init(IDirect3DDevice9* pDevice, const char* baseDir)
{
    if (m_initialized) return true;

    m_pDevice = pDevice;
    m_baseDir = baseDir;

    // Ensure trailing slash/backslash
    if (!m_baseDir.empty() && m_baseDir.back() != '\\' && m_baseDir.back() != '/')
        m_baseDir += '\\';

    // Load hashmap
    std::string hashmapPath = m_baseDir + "_normalmap.txt";
    if (!LoadHashmap(hashmapPath.c_str())) {
        if (g_Config.enableLog) {
            FILE* f = fopen("d3d9_proxy.log", "a");
            if (f) { fprintf(f, "TextureManager: No hashmap at %s\n", hashmapPath.c_str()); fclose(f); }
        }
        return false;
    }

    // Compile shaders
    m_pNormalShader = CompilePS(g_szNormalMap, "main");
    m_pNormalAlphaShader = CompilePS(g_szNormalMapAlpha, "main");

    if (!m_pNormalShader) {
        if (g_Config.enableLog) {
            FILE* f = fopen("d3d9_proxy.log", "a");
            if (f) { fprintf(f, "TextureManager: Failed to compile normal map shader\n"); fclose(f); }
        }
        return false;
    }

    m_initialized = true;

    if (g_Config.enableLog) {
        FILE* f = fopen("d3d9_proxy.log", "a");
        if (f) {
            fprintf(f, "TextureManager: Loaded %zu hashmap entries, shaders OK\n", m_hashToFile.size());
            fclose(f);
        }
    }
    return true;
}

void TextureManager::Release()
{
    m_pCurrentNormal = nullptr;

    for (auto& [hash, pTex] : m_loadedNormals) {
        if (pTex) pTex->Release();
    }
    m_loadedNormals.clear();
    m_texHashCache.clear();
    m_hashToFile.clear();

    if (m_pNormalShader) { m_pNormalShader->Release(); m_pNormalShader = nullptr; }
    if (m_pNormalAlphaShader) { m_pNormalAlphaShader->Release(); m_pNormalAlphaShader = nullptr; }

    m_initialized = false;
}

void TextureManager::OnLostDevice()
{
    // Normal map textures are D3DPOOL_MANAGED, survive Reset
    // Shaders survive Reset too
    // Just clear current state
    m_pCurrentNormal = nullptr;
}

void TextureManager::OnResetDevice(IDirect3DDevice9* pDevice)
{
    m_pDevice = pDevice;
}

bool TextureManager::LoadHashmap(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;

    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        // Parse: <hex CRC32> <filename>
        uint32_t hash;
        char filename[256];
        if (sscanf(line, "%x %255s", &hash, filename) == 2) {
            m_hashToFile[hash] = filename;
            count++;
        }
    }
    fclose(f);
    return count > 0;
}

uint32_t TextureManager::HashTexture(IDirect3DTexture9* pTex)
{
    if (!pTex) return HASH_NO_MATCH;

    // Check cache first
    auto it = m_texHashCache.find(pTex);
    if (it != m_texHashCache.end())
        return it->second;

    // Get texture description
    D3DSURFACE_DESC desc;
    if (FAILED(pTex->GetLevelDesc(0, &desc))) {
        m_texHashCache[pTex] = HASH_NO_MATCH;
        return HASH_NO_MATCH;
    }

    // Only hash MANAGED/SYSTEMMEM textures (can be locked for reading)
    if (desc.Pool != D3DPOOL_MANAGED && desc.Pool != D3DPOOL_SYSTEMMEM) {
        m_texHashCache[pTex] = HASH_NO_MATCH;
        return HASH_NO_MATCH;
    }

    // Lock level 0 read-only
    D3DLOCKED_RECT locked;
    HRESULT hr = pTex->LockRect(0, &locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        m_texHashCache[pTex] = HASH_NO_MATCH;
        return HASH_NO_MATCH;
    }

    // Compute CRC32 of pixel data
    // For DXT formats, the data is contiguous (pitch = block row size)
    // For uncompressed, we hash row by row using the pitch
    uint32_t hash = HASH_NO_MATCH;

    bool isDXT = (desc.Format == D3DFMT_DXT1 || desc.Format == D3DFMT_DXT2 ||
                  desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT4 ||
                  desc.Format == D3DFMT_DXT5);

    if (isDXT) {
        // DXT: contiguous block data
        UINT blockW = (desc.Width + 3) / 4;
        UINT blockH = (desc.Height + 3) / 4;
        UINT blockSize = (desc.Format == D3DFMT_DXT1) ? 8 : 16;
        UINT totalSize = blockW * blockH * blockSize;

        // Hash first 4KB or full texture, whichever is smaller
        // (full hash is too slow for large textures on first use)
        UINT hashSize = (totalSize < 4096) ? totalSize : 4096;
        hash = CRC32(locked.pBits, hashSize);

        // Mix in dimensions to reduce collisions from partial hash
        uint32_t dimKey = (desc.Width << 16) | desc.Height;
        hash ^= CRC32(&dimKey, sizeof(dimKey));
    } else {
        // Uncompressed: hash first N rows
        UINT bytesPerPixel = 4;  // assume A8R8G8B8 or similar 32-bit
        if (desc.Format == D3DFMT_R5G6B5 || desc.Format == D3DFMT_A1R5G5B5 ||
            desc.Format == D3DFMT_X1R5G5B5 || desc.Format == D3DFMT_A4R4G4B4)
            bytesPerPixel = 2;

        UINT rowBytes = desc.Width * bytesPerPixel;
        UINT rowsToHash = (desc.Height < 8) ? desc.Height : 8;
        uint32_t runningCrc = 0xFFFFFFFF;

        InitCRC32();
        const uint8_t* rowPtr = (const uint8_t*)locked.pBits;
        for (UINT row = 0; row < rowsToHash; row++) {
            for (UINT b = 0; b < rowBytes; b++)
                runningCrc = s_crc32Table[(runningCrc ^ rowPtr[b]) & 0xFF] ^ (runningCrc >> 8);
            rowPtr += locked.Pitch;
        }
        hash = runningCrc ^ 0xFFFFFFFF;

        uint32_t dimKey = (desc.Width << 16) | desc.Height;
        hash ^= CRC32(&dimKey, sizeof(dimKey));
    }

    pTex->UnlockRect(0);
    m_texHashCache[pTex] = hash;
    return hash;
}

void TextureManager::OnSetDiffuseTexture(IDirect3DTexture9* pTex)
{
    m_pCurrentNormal = nullptr;

    if (!m_initialized || !pTex)
        return;

    uint32_t hash = HashTexture(pTex);
    if (hash == HASH_NO_MATCH)
        return;

    // Check if we have a normal map for this hash
    auto fileIt = m_hashToFile.find(hash);
    if (fileIt == m_hashToFile.end())
        return;

    // Check if already loaded
    auto loadedIt = m_loadedNormals.find(hash);
    if (loadedIt != m_loadedNormals.end()) {
        m_pCurrentNormal = loadedIt->second;
        return;
    }

    // Lazy-load the DDS file
    std::string fullPath = m_baseDir + fileIt->second;
    IDirect3DTexture9* pNormal = LoadDDS(fullPath.c_str());
    m_loadedNormals[hash] = pNormal;  // cache even if null (avoid retry)
    m_pCurrentNormal = pNormal;

    if (g_Config.enableLog && pNormal) {
        FILE* f = fopen("d3d9_proxy.log", "a");
        if (f) {
            fprintf(f, "TextureManager: Loaded normal map %s (hash %08X)\n",
                    fileIt->second.c_str(), hash);
            fclose(f);
        }
    }
}

IDirect3DTexture9* TextureManager::LoadDDS(const char* path)
{
    if (!m_pDevice || !path) return nullptr;

    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;

    // Read magic
    DWORD magic;
    if (fread(&magic, 4, 1, f) != 1 || magic != DDS_MAGIC) {
        fclose(f);
        return nullptr;
    }

    // Read header
    DDS_HEADER header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        return nullptr;
    }

    UINT width = header.dwWidth;
    UINT height = header.dwHeight;
    UINT mipCount = header.dwMipMapCount;
    if (mipCount == 0) mipCount = 1;

    // Determine D3D format
    D3DFORMAT format = D3DFMT_UNKNOWN;
    UINT blockSize = 0;
    bool isCompressed = false;

    if (header.ddspf.dwFlags & DDPF_FOURCC) {
        isCompressed = true;
        switch (header.ddspf.dwFourCC) {
        case MAKEFOURCC('D','X','T','1'):
            format = D3DFMT_DXT1; blockSize = 8; break;
        case MAKEFOURCC('D','X','T','3'):
            format = D3DFMT_DXT3; blockSize = 16; break;
        case MAKEFOURCC('D','X','T','5'):
            format = D3DFMT_DXT5; blockSize = 16; break;
        default:
            fclose(f); return nullptr;
        }
    } else if (header.ddspf.dwFlags & DDPF_RGB) {
        if (header.ddspf.dwRGBBitCount == 32) {
            if (header.ddspf.dwABitMask == 0xFF000000)
                format = D3DFMT_A8R8G8B8;
            else
                format = D3DFMT_X8R8G8B8;
        } else if (header.ddspf.dwRGBBitCount == 24) {
            // 24-bit RGB not directly supported by D3D9, would need conversion
            fclose(f); return nullptr;
        }
    }

    if (format == D3DFMT_UNKNOWN) {
        fclose(f);
        return nullptr;
    }

    // Create texture in MANAGED pool (survives Reset, can be locked)
    IDirect3DTexture9* pTex = nullptr;
    HRESULT hr = m_pDevice->CreateTexture(width, height, mipCount, 0,
        format, D3DPOOL_MANAGED, &pTex, nullptr);
    if (FAILED(hr)) {
        fclose(f);
        return nullptr;
    }

    // Fill each mip level
    for (UINT mip = 0; mip < mipCount; mip++) {
        UINT mipW = width >> mip;   if (mipW < 1) mipW = 1;
        UINT mipH = height >> mip;  if (mipH < 1) mipH = 1;

        D3DLOCKED_RECT locked;
        hr = pTex->LockRect(mip, &locked, nullptr, 0);
        if (FAILED(hr)) break;

        if (isCompressed) {
            UINT blockW = (mipW + 3) / 4;
            UINT blockH = (mipH + 3) / 4;
            UINT rowBytes = blockW * blockSize;
            UINT totalBytes = rowBytes * blockH;

            // DDS stores compressed data contiguously, D3D pitch may differ
            if ((UINT)locked.Pitch == rowBytes) {
                fread(locked.pBits, 1, totalBytes, f);
            } else {
                uint8_t* dst = (uint8_t*)locked.pBits;
                for (UINT row = 0; row < blockH; row++) {
                    fread(dst, 1, rowBytes, f);
                    dst += locked.Pitch;
                }
            }
        } else {
            UINT bytesPerPixel = header.ddspf.dwRGBBitCount / 8;
            UINT srcRowBytes = mipW * bytesPerPixel;
            uint8_t* dst = (uint8_t*)locked.pBits;
            for (UINT row = 0; row < mipH; row++) {
                fread(dst, 1, srcRowBytes, f);
                dst += locked.Pitch;
            }
        }

        pTex->UnlockRect(mip);
    }

    fclose(f);
    return pTex;
}

IDirect3DPixelShader9* TextureManager::CompilePS(const char* source, const char* entry)
{
    if (!m_pDevice) return nullptr;

    ID3DBlob* pCode = nullptr;
    ID3DBlob* pErrors = nullptr;

    HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
        entry, "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pCode, &pErrors);

    if (FAILED(hr)) {
        if (g_Config.enableLog) {
            FILE* f = fopen("d3d9_proxy.log", "a");
            if (f) {
                fprintf(f, "TextureManager shader compile failed: 0x%08lX\n", hr);
                if (pErrors)
                    fprintf(f, "  %s\n", (const char*)pErrors->GetBufferPointer());
                fclose(f);
            }
        }
        if (pErrors) pErrors->Release();
        return nullptr;
    }

    IDirect3DPixelShader9* pShader = nullptr;
    hr = m_pDevice->CreatePixelShader((const DWORD*)pCode->GetBufferPointer(), &pShader);
    pCode->Release();
    if (pErrors) pErrors->Release();

    return SUCCEEDED(hr) ? pShader : nullptr;
}
