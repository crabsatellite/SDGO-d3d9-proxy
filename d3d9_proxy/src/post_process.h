#pragma once
#include <d3d9.h>

class ModManager;

// Post-processing resource manager.
// Owns render targets and handles state save/restore.
// All actual effects are mod DLLs.
class PostProcessor
{
public:
    PostProcessor(IDirect3DDevice9* pDevice);
    ~PostProcessor();

    // Run all mod post-passes with proper state management
    void Apply(IDirect3DDevice9* pDevice, ModManager* modManager);

    void SetDepthTexture(IDirect3DTexture9* pDepthTex) { m_pDepthTexture = pDepthTex; }
    void SetProjParams(float nearZ, float farZ) { m_nearZ = nearZ; m_farZ = farZ; }
    void OnLostDevice();
    void OnResetDevice(IDirect3DDevice9* pDevice);

    // Accessors for mod manager
    IDirect3DTexture9* GetDepthTexture() const { return m_pDepthTexture; }
    float GetNearZ() const { return m_nearZ; }
    float GetFarZ() const { return m_farZ; }
    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }

private:
    bool CreateResources(IDirect3DDevice9* pDevice);
    void ReleaseResources();

    // Full-res render targets (ping-pong)
    IDirect3DTexture9*  m_pSceneTexture = nullptr;
    IDirect3DSurface9*  m_pSceneSurface = nullptr;
    IDirect3DTexture9*  m_pTempTexture = nullptr;
    IDirect3DSurface9*  m_pTempSurface = nullptr;

    // Depth texture (owned by device proxy, not released here)
    IDirect3DTexture9*  m_pDepthTexture = nullptr;

    float m_nearZ = 0.1f;
    float m_farZ = 1000.0f;

    UINT m_width = 0;
    UINT m_height = 0;
    bool m_initialized = false;
};
