/*
  ==============================================================================

    PairedDevices.h

    Phones allowed to use phone streaming, kept in
    %APPDATA%\ExplicitlyDesktop\devices.json (docs/STREAMING_PROTOCOL.md section 6):

      {"hostId","devices":[{"deviceId","name","keyProtected":"<b64>","paired","lastSeen"}]}

    The device key K has to stay on the PC to check request MACs. On Windows
    it is encrypted with DPAPI (CryptProtectData, current user); other
    platforms (tests only) store it as "key":"<hex>".

    Also holds the nonce replay cache (5 minutes). Thread safe.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <mutex>
#include <string>
#include <vector>
#include "StreamCrypto.h"

class PairedDevices
{
public:
    struct Info
    {
        std::string deviceId, name;
        juce::String paired, lastSeen;   // ISO 8601
    };

    /** Loads the file (or starts empty). A hostId is created and saved once. */
    explicit PairedDevices (const juce::File& file = getDefaultFile());

    static juce::File getDefaultFile();

    std::string getHostId() const;

    /** Adds (or replaces) a device and saves the file. */
    bool addDevice (const std::string& deviceId, const std::string& name, const StreamCrypto::Bytes& key);

    /** Revoke/unpair: removes it and saves. False if it wasn't there. */
    bool removeDevice (const std::string& deviceId);

    /** False for an unknown (or revoked) device. */
    bool getDevice (const std::string& deviceId, StreamCrypto::Bytes& key, std::string& name) const;

    /** Updates lastSeen (saved at most once a minute per device). */
    void touch (const std::string& deviceId);

    std::vector<Info> getDevices() const;

    StreamCrypto::NonceCache& getNonceCache() { return nonces; }

    /** DPAPI on Windows; false elsewhere (the caller then stores hex). */
    static bool protectKey (const StreamCrypto::Bytes& key, juce::String& base64Out);
    static bool unprotectKey (const juce::String& base64, StreamCrypto::Bytes& keyOut);

private:
    struct Device
    {
        Info info;
        StreamCrypto::Bytes key;
        juce::int64 lastSavedSeenMs = 0;
    };

    void load();
    bool saveLocked() const;

    juce::File file;
    mutable std::mutex lock;
    std::string hostId;
    std::vector<Device> devices;
    StreamCrypto::NonceCache nonces { 5 * 60 * 1000 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PairedDevices)
};
