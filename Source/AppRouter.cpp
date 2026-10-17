/*
  ==============================================================================

    AppRouter.cpp

  ==============================================================================
*/

#include "AppRouter.h"
#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    // Undocumented Windows 10 21H2+/11 interface behind the Volume mixer's
    // per-app output setting (the same one EarTrumpet uses)
    MIDL_INTERFACE ("ab3d4648-e242-459f-b02f-541c70306324")
    IAudioPolicyConfigFactory : public IInspectable
    {
        virtual HRESULT STDMETHODCALLTYPE f1() = 0;  virtual HRESULT STDMETHODCALLTYPE f2() = 0;
        virtual HRESULT STDMETHODCALLTYPE f3() = 0;  virtual HRESULT STDMETHODCALLTYPE f4() = 0;
        virtual HRESULT STDMETHODCALLTYPE f5() = 0;  virtual HRESULT STDMETHODCALLTYPE f6() = 0;
        virtual HRESULT STDMETHODCALLTYPE f7() = 0;  virtual HRESULT STDMETHODCALLTYPE f8() = 0;
        virtual HRESULT STDMETHODCALLTYPE f9() = 0;  virtual HRESULT STDMETHODCALLTYPE f10() = 0;
        virtual HRESULT STDMETHODCALLTYPE f11() = 0; virtual HRESULT STDMETHODCALLTYPE f12() = 0;
        virtual HRESULT STDMETHODCALLTYPE f13() = 0; virtual HRESULT STDMETHODCALLTYPE f14() = 0;
        virtual HRESULT STDMETHODCALLTYPE f15() = 0; virtual HRESULT STDMETHODCALLTYPE f16() = 0;
        virtual HRESULT STDMETHODCALLTYPE f17() = 0; virtual HRESULT STDMETHODCALLTYPE f18() = 0;
        virtual HRESULT STDMETHODCALLTYPE f19() = 0;
        virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint (UINT32 processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint (UINT32 processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
        virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
    };

    IAudioPolicyConfigFactory* policyFactory()
    {
        static IAudioPolicyConfigFactory* factory = nullptr;
        if (factory == nullptr)
        {
            RoInitialize (RO_INIT_MULTITHREADED);   // Fine if already initialised
            const wchar_t* cls = L"Windows.Media.Internal.AudioPolicyConfig";
            HSTRING name = nullptr;
            if (SUCCEEDED (WindowsCreateString (cls, (UINT32) wcslen (cls), &name)))
            {
                RoGetActivationFactory (name, __uuidof (IAudioPolicyConfigFactory), (void**) &factory);
                WindowsDeleteString (name);
            }
        }
        return factory;
    }
}
#endif

AppRouter::AppRouter()
{
    setApps ({ "Spotify.exe", "firefox.exe", "chrome.exe", "brave.exe" });
}

AppRouter::~AppRouter()
{
    restoreAll();
}

void AppRouter::setApps (const juce::StringArray& exeNames)
{
    apps = exeNames;
    handled.clear();   // Re-apply to everything on the next update
}

juce::String AppRouter::cableEndpointId()
{
#ifdef _WIN32
    if (cableId.isNotEmpty())
        return cableId;

    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator), (void**) &enumerator)))
        return {};

    IMMDeviceCollection* devices = nullptr;
    if (SUCCEEDED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devices)))
    {
        UINT count = 0;
        devices->GetCount (&count);
        for (UINT i = 0; i < count && cableId.isEmpty(); ++i)
        {
            IMMDevice* device = nullptr;
            if (FAILED (devices->Item (i, &device)))
                continue;

            IPropertyStore* props = nullptr;
            if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &props)))
            {
                PROPVARIANT name;
                PropVariantInit (&name);
                if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR
                    && juce::String (name.pwszVal).startsWithIgnoreCase ("CABLE Input"))
                {
                    LPWSTR id = nullptr;
                    if (SUCCEEDED (device->GetId (&id)))
                    {
                        cableId = juce::String (id);
                        CoTaskMemFree (id);
                    }
                }
                PropVariantClear (&name);
                props->Release();
            }
            device->Release();
        }
        devices->Release();
    }
    enumerator->Release();
#endif
    return cableId;
}

bool AppRouter::setProcessOutput (unsigned long pid, bool toCable)
{
#ifdef _WIN32
    auto* factory = policyFactory();
    if (factory == nullptr)
        return false;

    HSTRING device = nullptr;
    if (toCable)
    {
        auto id = cableEndpointId();
        if (id.isEmpty())
            return false;
        // Device interface path form that the policy store expects
        auto full = ("\\\\?\\SWD#MMDEVAPI#" + id + "#{e6327cad-dcec-4949-ae8a-991e976a79d2}").toWideCharPointer();
        WindowsCreateString (full, (UINT32) wcslen (full), &device);
    }

    // Windows only accepts this on the app's audio process; helpers fail
    bool ok = SUCCEEDED (factory->SetPersistedDefaultAudioEndpoint ((UINT32) pid, eRender, eConsole, device))
           && SUCCEEDED (factory->SetPersistedDefaultAudioEndpoint ((UINT32) pid, eRender, eMultimedia, device));

    if (device != nullptr)
        WindowsDeleteString (device);
    return ok;
#else
    juce::ignoreUnused (pid, toCable);
    return false;
#endif
}

void AppRouter::update (bool toCable)
{
#ifdef _WIN32
    if (toCable != currentTarget)
    {
        currentTarget = toCable;
        handled.clear();
    }

    HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    std::map<unsigned long, bool> stillRunning;
    PROCESSENTRY32W entry { sizeof (entry) };
    for (BOOL more = Process32FirstW (snapshot, &entry); more; more = Process32NextW (snapshot, &entry))
    {
        juce::String exe (entry.szExeFile);
        if (! apps.contains (exe, true))
            continue;

        auto pid = (unsigned long) entry.th32ProcessID;
        auto it = handled.find (pid);
        if (it != handled.end())
        {
            stillRunning[pid] = it->second;
            continue;
        }

        // Most of a browser's processes refuse; only the audio one matters, and
        // Windows applies the setting to the whole program
        bool ok = setProcessOutput (pid, toCable);
        stillRunning[pid] = ok;
        if (ok)
        {
            std::cout << "[AppRouter] " << exe << " -> " << (toCable ? "CABLE Input" : "default output") << std::endl;
            if (toCable)
                everRouted.addIfNotAlreadyThere (exe);
            else
                unrestored.removeString (exe, true);
        }
    }
    CloseHandle (snapshot);
    handled = stillRunning;   // Forget processes that exited
#else
    juce::ignoreUnused (toCable);
#endif
}

void AppRouter::restoreAll()
{
    // Anything routed this session that isn't running can't be reset right now
    unrestored = everRouted;
    currentTarget = true;   // Force a fresh pass
    update (false);
    for (auto& name : unrestored)
        std::cout << "[AppRouter] " << name << " not running; it stays on the cable until it runs while the app is open" << std::endl;
}
