/*
  ==============================================================================

    PairedDevices.cpp

  ==============================================================================
*/

#include "PairedDevices.h"
#include <iostream>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <dpapi.h>
 #pragma comment(lib, "crypt32.lib")
#endif

namespace
{
    juce::String fromStd (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
}

PairedDevices::PairedDevices (const juce::File& f) : file (f)
{
    load();
}

juce::File PairedDevices::getDefaultFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("ExplicitlyDesktop")
               .getChildFile ("devices.json");
}

//==============================================================================
bool PairedDevices::protectKey (const StreamCrypto::Bytes& key, juce::String& base64Out)
{
   #if JUCE_WINDOWS
    DATA_BLOB in { (DWORD) key.size(), const_cast<BYTE*> (key.data()) };
    DATA_BLOB out {};
    if (! CryptProtectData (&in, L"Explicitly Desktop phone key", nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;

    base64Out = juce::Base64::toBase64 (out.pbData, out.cbData);
    LocalFree (out.pbData);
    return true;
   #else
    juce::ignoreUnused (key, base64Out);
    return false;
   #endif
}

bool PairedDevices::unprotectKey (const juce::String& base64, StreamCrypto::Bytes& keyOut)
{
   #if JUCE_WINDOWS
    juce::MemoryOutputStream decoded;
    if (! juce::Base64::convertFromBase64 (decoded, base64))
        return false;

    DATA_BLOB in { (DWORD) decoded.getDataSize(), (BYTE*) const_cast<void*> (decoded.getData()) };
    DATA_BLOB out {};
    if (! CryptUnprotectData (&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;

    keyOut.assign (out.pbData, out.pbData + out.cbData);
    SecureZeroMemory (out.pbData, out.cbData);
    LocalFree (out.pbData);
    return true;
   #else
    juce::ignoreUnused (base64, keyOut);
    return false;
   #endif
}

//==============================================================================
void PairedDevices::load()
{
    std::lock_guard<std::mutex> sl (lock);
    devices.clear();

    if (file.existsAsFile())
    {
        auto json = juce::JSON::parse (file.loadFileAsString());
        hostId = json.getProperty ("hostId", {}).toString().toStdString();

        if (auto* list = json.getProperty ("devices", {}).getArray())
        {
            for (auto& d : *list)
            {
                Device device;
                device.info.deviceId = d.getProperty ("deviceId", {}).toString().toStdString();
                device.info.name = d.getProperty ("name", {}).toString().toStdString();
                device.info.paired = d.getProperty ("paired", {}).toString();
                device.info.lastSeen = d.getProperty ("lastSeen", {}).toString();

                bool haveKey = false;
                if (d.hasProperty ("keyProtected"))
                    haveKey = unprotectKey (d.getProperty ("keyProtected", {}).toString(), device.key);
                else if (d.hasProperty ("key"))
                    haveKey = StreamCrypto::fromHex (d.getProperty ("key", {}).toString().toStdString(), device.key);

                // A key DPAPI can't open (another Windows user, or a copied file) can't
                // verify anything: that phone has to pair again
                if (! haveKey || device.key.size() != 32 || device.info.deviceId.empty())
                {
                    std::cout << "[Phone] Skipping unreadable device entry " << device.info.deviceId << std::endl;
                    continue;
                }
                devices.push_back (std::move (device));
            }
        }
    }

    if (hostId.empty())
    {
        hostId = StreamCrypto::toHex (StreamCrypto::randomBytes (16));
        saveLocked();
    }
}

bool PairedDevices::saveLocked() const
{
    juce::Array<juce::var> list;
    for (auto& d : devices)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("deviceId", fromStd (d.info.deviceId));
        obj->setProperty ("name", fromStd (d.info.name));

        juce::String protectedKey;
        if (protectKey (d.key, protectedKey))
            obj->setProperty ("keyProtected", protectedKey);
        else
            obj->setProperty ("key", fromStd (StreamCrypto::toHex (d.key)));   // Not Windows: tests only

        obj->setProperty ("paired", d.info.paired);
        obj->setProperty ("lastSeen", d.info.lastSeen);
        list.add (juce::var (obj));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("hostId", fromStd (hostId));
    root->setProperty ("devices", list);

    file.getParentDirectory().createDirectory();
    if (! file.replaceWithText (juce::JSON::toString (juce::var (root))))
    {
        std::cout << "[Phone] Could not write " << file.getFullPathName() << std::endl;
        return false;
    }
    return true;
}

//==============================================================================
std::string PairedDevices::getHostId() const
{
    std::lock_guard<std::mutex> sl (lock);
    return hostId;
}

bool PairedDevices::addDevice (const std::string& deviceId, const std::string& name, const StreamCrypto::Bytes& key)
{
    std::lock_guard<std::mutex> sl (lock);

    auto now = juce::Time::getCurrentTime().toISO8601 (true);
    for (auto it = devices.begin(); it != devices.end(); ++it)
    {
        if (it->info.deviceId == deviceId)
        {
            devices.erase (it);
            break;
        }
    }

    Device device;
    device.info.deviceId = deviceId;
    device.info.name = name;
    device.info.paired = now;
    device.info.lastSeen = now;
    device.key = key;
    devices.push_back (std::move (device));
    return saveLocked();
}

bool PairedDevices::removeDevice (const std::string& deviceId)
{
    std::lock_guard<std::mutex> sl (lock);
    for (auto it = devices.begin(); it != devices.end(); ++it)
    {
        if (it->info.deviceId == deviceId)
        {
            std::fill (it->key.begin(), it->key.end(), (uint8_t) 0);
            devices.erase (it);
            saveLocked();
            return true;
        }
    }
    return false;
}

bool PairedDevices::getDevice (const std::string& deviceId, StreamCrypto::Bytes& key, std::string& name) const
{
    std::lock_guard<std::mutex> sl (lock);
    for (auto& d : devices)
    {
        if (d.info.deviceId == deviceId)
        {
            key = d.key;
            name = d.info.name;
            return true;
        }
    }
    return false;
}

void PairedDevices::touch (const std::string& deviceId)
{
    std::lock_guard<std::mutex> sl (lock);
    auto nowMs = juce::Time::currentTimeMillis();

    for (auto& d : devices)
    {
        if (d.info.deviceId == deviceId)
        {
            d.info.lastSeen = juce::Time::getCurrentTime().toISO8601 (true);
            if (nowMs - d.lastSavedSeenMs > 60 * 1000)
            {
                d.lastSavedSeenMs = nowMs;
                saveLocked();
            }
            return;
        }
    }
}

std::vector<PairedDevices::Info> PairedDevices::getDevices() const
{
    std::lock_guard<std::mutex> sl (lock);
    std::vector<Info> list;
    for (auto& d : devices)
        list.push_back (d.info);
    return list;
}
