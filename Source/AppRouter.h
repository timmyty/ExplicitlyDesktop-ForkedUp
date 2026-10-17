/*
  ==============================================================================

    AppRouter.h

    Sends chosen apps (Spotify, browsers...) to VB-CABLE while the filter is
    processing, and back to the normal Windows output when it stops or the app
    closes, so their sound keeps working without Explicitly Desktop.

    Uses Windows' per-app output setting (the same one as Settings > System >
    Sound > Volume mixer). Windows remembers it per program, and it can only be
    changed through a running process of that program, so the router checks
    every few seconds and fixes any matching process it hasn't handled yet.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <map>

class AppRouter
{
public:
    AppRouter();
    ~AppRouter();

    /** Programs to route, by exe name (case-insensitive), e.g. "Spotify.exe". */
    void setApps (const juce::StringArray& exeNames);
    const juce::StringArray& getApps() const { return apps; }

    /** true: route the apps to "CABLE Input"; false: back to the Windows default.
        Call periodically (cheap); only processes not yet set are touched. */
    void update (bool toCable);

    /** Send every running app back to the default output now (on stop/exit). */
    void restoreAll();

    /** Names of routed apps that weren't running at the last restore. Windows
        keeps those on the cable until the app runs while this one can fix it. */
    juce::StringArray getUnrestored() const { return unrestored; }

private:
    bool setProcessOutput (unsigned long pid, bool toCable);
    juce::String cableEndpointId();

    juce::StringArray apps;
    std::map<unsigned long, bool> handled;     // pid -> state applied
    bool currentTarget = false;
    juce::String cableId;
    juce::StringArray unrestored;
    juce::StringArray everRouted;              // Exe names routed this session
};
