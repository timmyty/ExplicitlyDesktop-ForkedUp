/*
  ==============================================================================

    LyricsGuide.h

    Uses the playing song's lyrics to check Whisper's profanity calls.

    The song comes from Windows' "now playing" info (Spotify, browsers).
    Lyrics are looked up online (LRCLIB, then lyrics.ovh) on a background
    thread and kept as a local copy under %APPDATA%\ExplicitlyDesktop\lyrics,
    so a song is only looked up once.

    For each transcribed chunk, Whisper's words are aligned to the lyrics with
    a fuzzy local sequence alignment. When the alignment is solid:
      - a profane word Whisper heard where the lyrics have a similar-sounding
        clean word is not muted (avoids unnecessary muting), and
      - a profane lyrics word Whisper misheard or missed is muted, timed from
        the Whisper words aligned around it.
    When there are no lyrics or the alignment is weak, Whisper alone decides.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include "LyricsAlignment.h"
#include "ProfanityFilter.h"

#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

class LyricsGuide
{
public:
    explicit LyricsGuide (const ProfanityFilter& filter);
    ~LyricsGuide();

    /** Tell the guide what is playing. Cheap to call repeatedly; any thread. */
    void setSong (const std::string& artist, const std::string& title);

    struct Decision
    {
        bool aligned = false;
        // Per Whisper word: true when the lyrics confidently say this word is clean
        std::vector<bool> lyricsSayClean;
        // Profane lyrics words Whisper didn't flag: (start, end) seconds in the chunk.
        // Misheard words carry Whisper's word times (corrected like any Whisper hit);
        // words Whisper missed are placed in real time already (extraMuteIsFinal)
        std::vector<std::pair<double, double>> extraMutes;
        std::vector<bool> extraMuteIsFinal;
        std::vector<std::string> extraMuteWords;
        // Lyrics line(s) this chunk matched, profanity masked, for display
        std::string lyricsLines;
    };

    /** Called on the transcription thread for each chunk. */
    Decision analyze (const std::vector<WordSegment>& whisperWords, double chunkSeconds, double startBias);

    struct Status
    {
        std::string song;       // "Artist - Title" being followed
        std::string source;     // "LRCLIB", "lyrics.ovh", "local copy", or ""
        bool searching = false;
        bool hasLyrics = false;
        bool following = false; // Last chunk aligned to the lyrics
    };
    Status getStatus() const;

    /** Text normalisation shared with the matcher: lowercase ASCII letters/digits. */
    static std::string normalizeWord (const std::string& word);

private:
    struct LyricWord
    {
        std::string text;
        bool profane = false;
        int line = 0;           // Index into lyricLines
    };

    struct Candidate { std::string artist, title; };

    void workerLoop();
    std::vector<Candidate> candidatesFor (const std::string& artist, const std::string& title) const;
    std::string fetchFromLrclib (const Candidate& c) const;
    juce::File cacheFileFor (const std::string& songKey) const;
    void setLyricsText (const std::string& songKey, const std::string& text, const std::string& source);

    static float wordSimilarity (const std::string& a, const std::string& b);

    const ProfanityFilter& profanityFilter;

    mutable std::mutex mutex;
    std::string currentKey;                 // Normalised "artist|title" requested
    std::string currentSong;                // Display name
    std::string loadedKey;                  // Song the lyrics below belong to
    std::vector<LyricWord> lyrics;
    std::vector<std::string> lyricLines;    // Display lines, profanity masked
    bool lyricsCensored = false;            // Copy has starred-out words, so it can't clear Whisper
    std::string source;
    bool searching = false;
    bool following = false;
    int lastPosition = -1;
    std::set<std::string> notFound;         // Songs already looked up without success

    // Background lookup
    std::thread worker;
    std::condition_variable wake;
    bool stopWorker = false;
    std::string pendingArtist, pendingTitle, pendingKey;
    bool pending = false;

    JUCE_DECLARE_NON_COPYABLE (LyricsGuide)
};
