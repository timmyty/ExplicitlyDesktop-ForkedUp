/*
  ==============================================================================

    WindowsMediaInfo.cpp
    Created: 20 Dec 2024
    Author: Explicitly Audio Systems

    Implementation of Windows Media Control integration.

  ==============================================================================
*/

#include "WindowsMediaInfo.h"
#include <iostream>

WindowsMediaInfo::WindowsMediaInfo()
{
}

WindowsMediaInfo::~WindowsMediaInfo()
{
#ifdef _WIN32
    if (sessionManager != nullptr && sessionChangedToken)
    {
        sessionManager.SessionsChanged(sessionChangedToken);
    }
#endif
}

bool WindowsMediaInfo::initialize()
{
#ifdef _WIN32
    try
    {
        std::cout << "[MediaInfo] Initializing WinRT apartment..." << std::endl;
        
        // Initialize WinRT (catch if already initialized)
        try
        {
            winrt::init_apartment();
            std::cout << "[MediaInfo] WinRT apartment initialized" << std::endl;
        }
        catch (const winrt::hresult_error& e)
        {
            // Apartment already initialized is OK
            std::cout << "[MediaInfo] WinRT apartment already initialized (OK)" << std::endl;
        }
        
        std::cout << "[MediaInfo] Requesting media session manager..." << std::endl;
        
        // Get the media session manager
        auto asyncOp = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        sessionManager = asyncOp.get();
        
        if (sessionManager == nullptr)
        {
            std::cout << "[MediaInfo] Failed to get session manager (nullptr)" << std::endl;
            return false;
        }
        
        std::cout << "[MediaInfo] Session manager obtained successfully" << std::endl;
        
        // Register for session changes
        sessionChangedToken = sessionManager.SessionsChanged([this](auto&&, auto&&) {
            onMediaPropertiesChanged();
        });
        
        std::cout << "[MediaInfo] Windows Media Control initialized successfully" << std::endl;
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        std::cout << "[MediaInfo] WinRT ERROR: 0x" << std::hex << e.code() << " - " << winrt::to_string(e.message()) << std::endl;
        return false;
    }
    catch (const std::exception& e)
    {
        std::cout << "[MediaInfo] ERROR: " << e.what() << std::endl;
        return false;
    }
    catch (...)
    {
        std::cout << "[MediaInfo] Unknown ERROR during initialization" << std::endl;
        return false;
    }
#else
    std::cout << "[MediaInfo] Windows Media Control not available on this platform" << std::endl;
    return false;
#endif
}

WindowsMediaInfo::MediaInfo WindowsMediaInfo::getCurrentMedia()
{
    MediaInfo info;
    
#ifdef _WIN32
    try
    {
        if (sessionManager == nullptr)
            return info;
        
        // Prefer whichever app is actually playing (e.g. YouTube while Spotify
        // sits paused); fall back to Windows' current session
        auto currentSession = sessionManager.GetCurrentSession();
        for (auto const& session : sessionManager.GetSessions())
        {
            if (session.GetPlaybackInfo().PlaybackStatus()
                == winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
            {
                currentSession = session;
                break;
            }
        }
        
        if (currentSession == nullptr)
            return info;
        
        // Get playback info
        auto playbackInfo = currentSession.GetPlaybackInfo();
        info.isPlaying = (playbackInfo.PlaybackStatus() == winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
        
        // Get media properties
        auto asyncProps = currentSession.TryGetMediaPropertiesAsync();
        auto props = asyncProps.get();
        
        if (props != nullptr)
        {
            info.artist = winrt::to_string(props.Artist());
            info.title = winrt::to_string(props.Title());
            info.album = winrt::to_string(props.AlbumTitle());
        }
        
    }
    catch (const std::exception& e)
    {
        std::cout << "[MediaInfo] ERROR getting media info: " << e.what() << std::endl;
    }
#endif
    
    return info;
}

void WindowsMediaInfo::setMediaChangedCallback(std::function<void(const MediaInfo&)> callback)
{
    mediaCallback = callback;
}

void WindowsMediaInfo::onMediaPropertiesChanged()
{
    auto info = getCurrentMedia();
    
    if (mediaCallback && !info.title.empty())
    {
        mediaCallback(info);
    }
}
