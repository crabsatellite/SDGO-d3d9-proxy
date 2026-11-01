#pragma once
#include <windows.h>
#include <cstdint>
#include "sdgo/sdgo_api.h"

// The shipped Miles driver first opens DirectSound and crashes in its failure
// cleanup on a non-input desktop. Its real AIL_open_digital_driver routine
// tests flag bit 0 before changing preference 15 (DIG_USE_WAVEOUT). Preserve
// that preference and use its native WaveOut path; never synthesize a driver.
// Master gain is per returned Miles driver, not the Windows/system volume.
class QuietAudio {
public:
    using OpenFn=void*(WINAPI*)(uint32_t,int,int,uint32_t);
    using PreferenceFn=int(WINAPI*)(uint32_t,int);
    using VolumeFn=void(WINAPI*)(void*,float);
    using ReadVolumeFn=float(WINAPI*)(void*);
    struct Bindings {PreferenceFn preference;VolumeFn volume;ReadVolumeFn read_volume;};
    struct Observation {uint32_t active,opened,muted,failed;};
    bool Install(const SdgoApi* api,SdgoMod* mod,const Bindings* supplied=nullptr) {
        if (!api || !api->HookImport || (active_ && active_!=this))return false;
        api_=api;mod_=mod;
        if (supplied) bindings_=*supplied;
        else {
            HMODULE miles=GetModuleHandleW(L"Mss32.dll");
            if (!miles)return false;
            bindings_={reinterpret_cast<PreferenceFn>(GetProcAddress(miles,"_AIL_set_preference@8")),
                reinterpret_cast<VolumeFn>(GetProcAddress(miles,"_AIL_set_digital_master_volume_level@8")),
                reinterpret_cast<ReadVolumeFn>(GetProcAddress(miles,"_AIL_digital_master_volume_level@4"))};
        }
        if (!bindings_.preference || !bindings_.volume || !bindings_.read_volume)return false;
        active_=this;
        if (!api->HookImport(mod,"Mss32.dll","_AIL_open_digital_driver@16",reinterpret_cast<void*>(&Open),
                            reinterpret_cast<void**>(&original_)) || !original_) {active_=nullptr;return false;}
        installed_=true;return true;
    }
    Observation State() const {return {installed_ ? 1u:0u,Read(opened_),Read(muted_),Read(failed_)};}
private:
    static uint32_t Read(volatile LONG& value) {return static_cast<uint32_t>(InterlockedCompareExchange(&value,0,0));}
    static void* WINAPI Open(uint32_t rate,int bits,int channels,uint32_t flags) {
        QuietAudio* self=active_;
        if (!self || !self->original_)return nullptr;
        self->bindings_.preference(15,1);
        if (self->api_->Log) self->api_->Log(self->mod_,SDGO_LOG_INFO,"quiet Miles native WaveOut open: rate=%u bits=%d channels=%d flags=%u",rate,bits,channels,flags|1u);
        void* driver=self->original_(rate,bits,channels,flags|1u);
        if (self->api_->Log) self->api_->Log(self->mod_,SDGO_LOG_INFO,"quiet Miles native driver returned %p",driver);
        if (!driver) {InterlockedIncrement(&self->failed_);return nullptr;}
        InterlockedIncrement(&self->opened_);
        self->bindings_.volume(driver,0.0f);
        if (self->bindings_.read_volume(driver)==0.0f)InterlockedIncrement(&self->muted_);
        else InterlockedIncrement(&self->failed_);
        return driver;
    }
    inline static QuietAudio* active_=nullptr;
    Bindings bindings_{};
    const SdgoApi* api_=nullptr;
    SdgoMod* mod_=nullptr;
    OpenFn original_=nullptr;
    bool installed_=false;
    mutable volatile LONG opened_=0,muted_=0,failed_=0;
};
