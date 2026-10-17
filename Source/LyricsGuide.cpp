/*
  ==============================================================================

    LyricsGuide.cpp

  ==============================================================================
*/

#include "LyricsGuide.h"
#include <algorithm>
#include <iostream>
#include <regex>
#include <sstream>

namespace
{
    constexpr float matchThreshold = 0.75f;   // Word similarity counted as "same word"
    constexpr float gapPenalty = 0.8f;
    constexpr float mismatchPenalty = 1.0f;

    std::string trim (const std::string& s)
    {
        auto b = s.find_first_not_of (" \t\r\n-");
        auto e = s.find_last_not_of (" \t\r\n-");
        return b == std::string::npos ? std::string() : s.substr (b, e - b + 1);
    }

    std::string cleanTitle (std::string t)
    {
        // Drop "(Official Video)", "[Lyrics]", "ft. Someone" and similar decoration
        t = std::regex_replace (t, std::regex ("\\([^)]*\\)|\\[[^\\]]*\\]"), "");
        t = std::regex_replace (t, std::regex ("\\s(ft\\.?|feat\\.?|featuring)\\s.*$", std::regex::icase), "");
        t = std::regex_replace (t, std::regex ("\\b(official\\s+)?(music\\s+)?(video|audio|lyrics?|visualizer|hd|4k)\\b\\s*$", std::regex::icase), "");
        return trim (t);
    }

    std::string cleanArtist (std::string a)
    {
        a = std::regex_replace (a, std::regex ("\\s*-\\s*Topic$|VEVO$|\\s+Official$", std::regex::icase), "");
        return trim (a);
    }
}

LyricsGuide::LyricsGuide (const ProfanityFilter& filter)
    : profanityFilter (filter)
{
    worker = std::thread ([this] { workerLoop(); });
}

LyricsGuide::~LyricsGuide()
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        stopWorker = true;
    }
    wake.notify_all();
    if (worker.joinable())
        worker.join();
}

std::string LyricsGuide::normalizeWord (const std::string& word)
{
    std::string out;
    for (unsigned char c : word)
        if (c < 128 && std::isalnum (c))
            out += (char) std::tolower (c);
    return out;
}

//==============================================================================
void LyricsGuide::setSong (const std::string& artist, const std::string& title)
{
    if (title.empty())
        return;

    auto key = normalizeWord (artist) + "|" + normalizeWord (title);

    {
        std::lock_guard<std::mutex> lock (mutex);
        if (key == currentKey)
            return;

        currentKey = key;
        currentSong = artist.empty() ? title : artist + " - " + title;
        following = false;
        lastPosition = -1;

        if (loadedKey != key)
        {
            lyrics.clear();
            source.clear();
        }

        std::cout << "[LyricsGuide] Now playing: " << currentSong << std::endl;

        if (loadedKey == key || notFound.count (key) > 0)
            return;

        pendingArtist = artist;
        pendingTitle = title;
        pendingKey = key;
        pending = true;
    }
    wake.notify_all();
}

LyricsGuide::Status LyricsGuide::getStatus() const
{
    std::lock_guard<std::mutex> lock (mutex);
    Status s;
    s.song = currentSong;
    s.source = source;
    s.searching = searching;
    s.hasLyrics = ! lyrics.empty() && loadedKey == currentKey;
    s.following = following && s.hasLyrics;
    return s;
}

//==============================================================================
juce::File LyricsGuide::cacheFileFor (const std::string& songKey) const
{
    auto name = juce::String (songKey).replaceCharacter ('|', '_');
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("ExplicitlyDesktop").getChildFile ("lyrics")
               .getChildFile (juce::File::createLegalFileName (name) + ".txt");
}

std::vector<LyricsGuide::Candidate> LyricsGuide::candidatesFor (const std::string& artist, const std::string& title) const
{
    std::vector<Candidate> out;
    auto t = cleanTitle (title);

    // YouTube titles are usually "Artist - Song"; the "artist" is the channel
    std::smatch m;
    if (std::regex_match (t, m, std::regex ("(.+?)\\s+[-\xE2\x80\x93\xE2\x80\x94]+\\s+(.+)")))
        out.push_back ({ cleanArtist (m[1].str()), cleanTitle (m[2].str()) });

    out.push_back ({ cleanArtist (artist), t });
    return out;
}

std::string LyricsGuide::fetchFromLrclib (const Candidate& c) const
{
    juce::URL url ("https://lrclib.net/api/search");
    url = url.withParameter ("track_name", juce::String (c.title));
    if (! c.artist.empty())
        url = url.withParameter ("artist_name", juce::String (c.artist));

    auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs (8000)
                       .withNumRedirectsToFollow (3)
                       .withExtraHeaders ("User-Agent: ExplicitlyDesktop/1.0 (personal profanity filter)");

    auto stream = url.createInputStream (options);
    if (stream == nullptr)
        return {};

    auto json = juce::JSON::parse (stream->readEntireStreamAsString());
    if (auto* results = json.getArray())
    {
        for (auto& r : *results)
        {
            if ((bool) r["instrumental"])
                continue;
            auto plain = r["plainLyrics"].toString();
            if (plain.isNotEmpty())
                return plain.toStdString();
        }
    }
    return {};
}

void LyricsGuide::setLyricsText (const std::string& songKey, const std::string& text, const std::string& src)
{
    std::vector<LyricWord> words;
    bool censoredCopy = false;
    std::istringstream lines (text);
    std::string line;
    while (std::getline (lines, line))
    {
        line = std::regex_replace (line, std::regex ("\\[\\d+:\\d+(\\.\\d+)?\\]"), "");  // LRC times
        auto t = trim (line);
        if (t.empty() || t[0] == '#' || (t.front() == '[' && t.back() == ']'))         // [Chorus]
            continue;

        std::istringstream ws (t);
        std::string w;
        while (ws >> w)
        {
            // Starred words ("f**k") are censored profanity in a clean copy of the lyrics
            bool starred = w.find ('*') != std::string::npos;
            auto n = normalizeWord (w);
            if (! n.empty() || starred)
                words.push_back ({ n.empty() ? std::string ("*") : n, starred || profanityFilter.isProfane (n) });
            censoredCopy = censoredCopy || starred;
        }
    }

    // Two-word profanity ("mother" + "fucker" style) flags both words
    for (size_t i = 0; i + 1 < words.size(); ++i)
        if (profanityFilter.isProfane (words[i].text + words[i + 1].text))
            words[i].profane = words[i + 1].profane = true;

    std::lock_guard<std::mutex> lock (mutex);
    if (songKey != currentKey)
        return;

    lyrics = std::move (words);
    lyricsCensored = censoredCopy;
    loadedKey = songKey;
    source = src;
    lastPosition = -1;

    auto profaneCount = std::count_if (lyrics.begin(), lyrics.end(), [] (auto& w) { return w.profane; });
    std::cout << "[LyricsGuide] Lyrics ready from " << src << ": " << lyrics.size()
              << " words, " << profaneCount << " flagged" << std::endl;
}

void LyricsGuide::workerLoop()
{
    for (;;)
    {
        std::string artist, title, key;
        {
            std::unique_lock<std::mutex> lock (mutex);
            wake.wait (lock, [this] { return stopWorker || pending; });
            if (stopWorker)
                return;
            artist = pendingArtist;
            title = pendingTitle;
            key = pendingKey;
            pending = false;
        }

        // Local copy first, so each song is only looked up online once
        auto cache = cacheFileFor (key);
        if (cache.existsAsFile())
        {
            setLyricsText (key, cache.loadFileAsString().toStdString(), "local copy");
            continue;
        }

        { std::lock_guard<std::mutex> lock (mutex); searching = true; }

        std::string text, src;
        for (auto& c : candidatesFor (artist, title))
        {
            if (c.title.empty())
                continue;
            text = fetchFromLrclib (c);
            if (! text.empty()) { src = "LRCLIB"; break; }
        }
        if (text.empty())
        {
            for (auto& c : candidatesFor (artist, title))
            {
                if (c.artist.empty() || c.title.empty())
                    continue;
                text = LyricsAlignment::fetchLyricsFromOvh (c.artist, c.title).lyrics;
                if (! text.empty()) { src = "lyrics.ovh"; break; }
            }
        }

        { std::lock_guard<std::mutex> lock (mutex); searching = false; }

        if (text.empty())
        {
            std::cout << "[LyricsGuide] No lyrics found for " << artist << " - " << title << std::endl;
            std::lock_guard<std::mutex> lock (mutex);
            notFound.insert (key);
            continue;
        }

        cache.getParentDirectory().createDirectory();
        cache.replaceWithText (juce::String ("# source: ") + src + "\n" + juce::String (text));
        setLyricsText (key, text, src);
    }
}

//==============================================================================
float LyricsGuide::wordSimilarity (const std::string& a, const std::string& b)
{
    if (a == b)
        return 1.0f;
    if (a.empty() || b.empty())
        return 0.0f;

    // Levenshtein ratio, with a boost for same-sounding words ("sit"/"set")
    float ratio = LyricsAlignment::calculateSimilarity (a, b);
    if (a[0] == b[0] && LyricsAlignment::soundexEncode (a) == LyricsAlignment::soundexEncode (b))
        ratio = std::max (ratio, matchThreshold);
    return ratio;
}

LyricsGuide::Decision LyricsGuide::analyze (const std::vector<WordSegment>& whisperWords, double chunkSeconds)
{
    Decision d;
    d.lyricsSayClean.assign (whisperWords.size(), false);

    std::lock_guard<std::mutex> lock (mutex);
    if (lyrics.empty() || loadedKey != currentKey || whisperWords.size() < 2)
    {
        if (! whisperWords.empty())
            following = false;
        return d;
    }

    std::vector<std::string> w;
    for (auto& ws : whisperWords)
        w.push_back (normalizeWord (ws.word));

    const int n = (int) w.size();
    const int m = (int) lyrics.size();

    // Smith-Waterman local alignment of the chunk's words against the whole lyrics
    std::vector<std::vector<float>> H (n + 1, std::vector<float> (m + 1, 0.0f));
    std::vector<std::vector<float>> sim (n, std::vector<float> (m, 0.0f));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j)
            sim[i][j] = wordSimilarity (w[i], lyrics[j].text);

    float best = 0.0f;
    int bi = 0, bj = 0;
    int expected = lastPosition >= 0 ? lastPosition + n : -1;

    for (int i = 1; i <= n; ++i)
    {
        for (int j = 1; j <= m; ++j)
        {
            float s = sim[i - 1][j - 1];
            float diag = H[i - 1][j - 1] + (s >= matchThreshold ? 2.0f * s : -mismatchPenalty);
            float h = std::max ({ 0.0f, diag, H[i - 1][j] - gapPenalty, H[i][j - 1] - gapPenalty });
            H[i][j] = h;

            // Repeated choruses tie: prefer the copy nearest where we expect to be
            bool better = h > best + 1e-4f
                       || (std::abs (h - best) <= 1e-4f && expected >= 0
                           && std::abs (j - expected) < std::abs (bj - expected));
            if (better) { best = h; bi = i; bj = j; }
        }
    }

    // Trace back: pairs (whisper i, lyrics j) or gaps
    struct Op { int i; int j; };   // -1 marks a gap on that side
    std::vector<Op> ops;
    int i = bi, j = bj;
    while (i > 0 && j > 0 && H[i][j] > 0.0f)
    {
        float s = sim[i - 1][j - 1];
        float diag = H[i - 1][j - 1] + (s >= matchThreshold ? 2.0f * s : -mismatchPenalty);
        if (std::abs (H[i][j] - diag) < 1e-4f)              { ops.push_back ({ i - 1, j - 1 }); --i; --j; }
        else if (std::abs (H[i][j] - (H[i - 1][j] - gapPenalty)) < 1e-4f) { ops.push_back ({ i - 1, -1 }); --i; }
        else                                               { ops.push_back ({ -1, j - 1 }); --j; }
    }
    std::reverse (ops.begin(), ops.end());

    int matched = 0, spanWhisper = 0;
    for (auto& op : ops)
    {
        if (op.i >= 0) ++spanWhisper;
        if (op.i >= 0 && op.j >= 0 && sim[op.i][op.j] >= matchThreshold) ++matched;
    }

    bool strong = matched >= 3 && matched >= 0.6 * spanWhisper;
    bool continuing = following && expected >= 0 && std::abs (bj - expected) <= 12
                      && matched >= 2 && matched == spanWhisper;
    d.aligned = strong || continuing;
    following = d.aligned;

    if (! d.aligned)
        return d;

    lastPosition = bj;

    auto lyricProfane = [this] (int jj) { return jj >= 0 && jj < (int) lyrics.size() && lyrics[jj].profane; };

    for (size_t k = 0; k < ops.size(); ++k)
    {
        auto op = ops[k];

        if (op.i >= 0 && op.j >= 0)
        {
            // Whisper word paired with a lyrics word. Only trust "clean" when the two
            // sound alike (a mishearing like "sit"/"shit"), not when an edited copy of
            // the lyrics swapped the word out, and never from a censored copy
            d.lyricsSayClean[op.i] = ! lyricProfane (op.j) && ! lyricsCensored
                                     && sim[op.i][op.j] >= 0.5f;

            bool whisperProfane = profanityFilter.isProfane (w[op.i]);
            if (lyricProfane (op.j) && ! whisperProfane)
            {
                d.extraMutes.push_back ({ whisperWords[op.i].start, whisperWords[op.i].end });
                d.extraMuteWords.push_back (lyrics[op.j].text + " (heard \"" + w[op.i] + "\")");
            }
        }
        else if (op.i >= 0)
        {
            // Whisper heard a word the lyrics don't have here. Could be an ad-lib or
            // a word a clean copy of the lyrics left out, so Whisper still decides
        }
        else if (lyricProfane (op.j))
        {
            // Lyrics word Whisper missed: time it between the neighbouring words
            double prevEnd = 0.0, nextStart = chunkSeconds;
            for (int b = (int) k - 1; b >= 0; --b)
                if (ops[b].i >= 0) { prevEnd = whisperWords[ops[b].i].end; break; }
            for (size_t f = k + 1; f < ops.size(); ++f)
                if (ops[f].i >= 0) { nextStart = whisperWords[ops[f].i].start; break; }

            if (nextStart - prevEnd < 0.3)
            {
                double mid = 0.5 * (prevEnd + nextStart);
                prevEnd = mid - 0.2;
                nextStart = mid + 0.2;
            }
            d.extraMutes.push_back ({ std::max (0.0, prevEnd), nextStart });
            d.extraMuteWords.push_back (lyrics[op.j].text + " (not heard)");
        }
    }

    std::cout << "[LyricsGuide] Aligned " << matched << "/" << n << " words at lyrics word "
              << (bj - (int) ops.size() + 1) << "-" << bj << std::endl;
    return d;
}
