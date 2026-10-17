/*
  ==============================================================================

    LyricsGuide.cpp

  ==============================================================================
*/

#include "LyricsGuide.h"
#include <algorithm>
#include <iostream>
#include <regex>
#include <cstring>
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
        {
            // Same song: retry a failed lookup once its wait is over
            auto it = notFound.find (key);
            bool retry = loadedKey != key && ! searching && ! pending && it != notFound.end()
                         && juce::Time::getMillisecondCounterHiRes() >= it->second;
            if (! retry)
                return;
            notFound.erase (it);
            pendingArtist = artist; pendingTitle = title; pendingKey = key; pending = true;
            wake.notify_all();
            return;
        }

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

        if (loadedKey == key)
            return;
        auto nf = notFound.find (key);
        if (nf != notFound.end() && juce::Time::getMillisecondCounterHiRes() < nf->second)
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
    auto add = [&out] (std::string a, std::string t)
    {
        a = cleanArtist (a);
        t = cleanTitle (t);
        if (t.empty())
            return;
        for (auto& c : out)
            if (c.artist == a && c.title == t)
                return;
        out.push_back ({ a, t });
    };

    // Spotify-style suffixes: "Song - Acoustic", "Song - From 'Film' / Soundtrack
    // Version", "Song - Remastered 2011", "Song - Live at ..."
    auto baseTitle = std::regex_replace (title,
        std::regex ("\\s+[-\xE2\x80\x93]\\s+(acoustic|live|remaster|remastered|radio edit|single|mono|stereo|explicit|clean|from |bonus|demo|version|edit|mix|remix|\\d{4} remaster).*$",
                    std::regex::icase), "");

    // Several artists ("A, B & C", "A feat. B"): try the first one too
    auto firstArtist = std::regex_replace (artist, std::regex ("\\s*(,|&|\\bfeat\\.?|\\bft\\.?|\\bx\\b|\\bwith\\b).*$", std::regex::icase), "");

    add (artist, baseTitle);
    add (firstArtist, baseTitle);
    if (baseTitle != title)
        add (artist, title);

    // YouTube titles are usually "Artist - Song", and the "artist" is the channel
    std::smatch m;
    auto yt = cleanTitle (title);
    if (std::regex_match (yt, m, std::regex ("(.+?)\\s+[-\xE2\x80\x93\xE2\x80\x94]+\\s+(.+)")))
        add (m[1].str(), m[2].str());

    return out;
}

LyricsGuide::Lookup LyricsGuide::fetchFromLrclib (const Candidate& c, bool fuzzy) const
{
    juce::URL url ("https://lrclib.net/api/search");
    if (fuzzy)
        url = url.withParameter ("q", juce::String (c.artist + " " + c.title));
    else
    {
        url = url.withParameter ("track_name", juce::String (c.title));
        if (! c.artist.empty())
            url = url.withParameter ("artist_name", juce::String (c.artist));
    }

    auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs (8000)
                       .withNumRedirectsToFollow (3)
                       .withExtraHeaders ("User-Agent: ExplicitlyDesktop/1.0 (personal profanity filter)");

    auto stream = url.createInputStream (options);
    if (stream == nullptr)
        return { Lookup::Error, {} };

    auto json = juce::JSON::parse (stream->readEntireStreamAsString());
    auto* results = json.getArray();
    if (results == nullptr)
        return { Lookup::Error, {} };   // e.g. {"name":"ServerOverloaded"}: try again later

    // Only accept a result for the same song: the artist must match, and the
    // title must be close (fuzzy search returns all sorts)
    auto wantArtist = normalizeWord (c.artist);
    auto wantTitle = normalizeWord (c.title);
    for (auto& r : *results)
    {
        if ((bool) r["instrumental"])
            continue;
        auto plain = r["plainLyrics"].toString();
        if (plain.isEmpty())
            continue;

        auto gotArtist = normalizeWord (r["artistName"].toString().toStdString());
        auto gotTitle = normalizeWord (r["trackName"].toString().toStdString());
        bool artistOk = wantArtist.empty() || gotArtist.find (wantArtist) != std::string::npos
                        || wantArtist.find (gotArtist) != std::string::npos
                        || LyricsAlignment::calculateSimilarity (gotArtist, wantArtist) >= 0.7f;
        bool titleOk = gotTitle.find (wantTitle) != std::string::npos || wantTitle.find (gotTitle) != std::string::npos
                       || LyricsAlignment::calculateSimilarity (gotTitle, wantTitle) >= 0.6f;
        if (artistOk && titleOk)
            return { Lookup::Found, plain.toStdString() };
    }
    return { Lookup::NotFound, {} };
}

void LyricsGuide::setLyricsText (const std::string& songKey, const std::string& text, const std::string& src)
{
    std::vector<LyricWord> words;
    std::vector<std::vector<std::string>> lineTokens;   // Raw tokens per display line
    bool censoredCopy = false;
    std::istringstream lines (text);
    std::string line;
    while (std::getline (lines, line))
    {
        line = std::regex_replace (line, std::regex ("\\[\\d+:\\d+(\\.\\d+)?\\]"), "");  // LRC times
        auto t = trim (line);
        if (t.empty() || t[0] == '#' || (t.front() == '[' && t.back() == ']'))         // [Chorus]
            continue;

        lineTokens.emplace_back();
        std::istringstream ws (t);
        std::string w;
        while (ws >> w)
        {
            lineTokens.back().push_back (w);
            // Starred words ("f**k") are censored profanity in a clean copy of the lyrics
            bool starred = w.find ('*') != std::string::npos;
            auto n = normalizeWord (w);
            if (! n.empty() || starred)
                words.push_back ({ n.empty() ? std::string ("*") : n, starred || profanityFilter.isProfane (n),
                                   (int) lineTokens.size() - 1 });
            censoredCopy = censoredCopy || starred;
        }
    }

    // Two-word profanity ("mother" + "fucker" style) flags both words
    for (size_t i = 0; i + 1 < words.size(); ++i)
        if (profanityFilter.isProfane (words[i].text + words[i + 1].text))
            words[i].profane = words[i + 1].profane = true;

    // Display lines with profane words masked ("d***"), using the flags above
    std::vector<std::string> displayLines;
    size_t wi = 0;
    for (auto& tokens : lineTokens)
    {
        std::string line;
        for (auto& tok : tokens)
        {
            bool counted = ! normalizeWord (tok).empty() || tok.find ('*') != std::string::npos;
            bool profane = counted && wi < words.size() && words[wi].profane;
            if (counted) ++wi;

            std::string shown = tok;
            if (profane && tok.find ('*') == std::string::npos)
                for (size_t c = 1; c < shown.size(); ++c)
                    if (std::isalpha ((unsigned char) shown[c]))
                        shown[c] = '*';
            line += (line.empty() ? "" : " ") + shown;
        }
        displayLines.push_back (line);
    }

    std::lock_guard<std::mutex> lock (mutex);
    if (songKey != currentKey)
        return;

    lyrics = std::move (words);
    lyricLines = std::move (displayLines);
    ++songGeneration;
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

        // Local copy first (pasted, or found earlier), so each song is only
        // looked up online once
        auto cache = cacheFileFor (key);
        if (cache.existsAsFile())
        {
            setLyricsText (key, cache.loadFileAsString().toStdString(), "local copy");
            continue;
        }

        { std::lock_guard<std::mutex> lock (mutex); searching = true; }

        std::string text, src;
        bool hadError = false;
        auto candidates = candidatesFor (artist, title);

        for (bool fuzzy : { false, true })
        {
            for (auto& c : candidates)
            {
                auto result = fetchFromLrclib (c, fuzzy);
                hadError = hadError || result.status == Lookup::Error;
                if (result.status == Lookup::Found) { text = result.text; src = "LRCLIB"; break; }
            }
            if (! text.empty())
                break;
        }
        // Genius, with the user's own API key
        auto geniusToken = geniusTokenFile().loadFileAsString().trim().toStdString();
        if (text.empty() && ! geniusToken.empty())
        {
            for (auto& c : candidates)
            {
                text = fetchFromGenius (c, geniusToken);
                if (! text.empty()) { src = "Genius"; break; }
            }
        }
        if (text.empty())
        {
            for (auto& c : candidates)
            {
                if (c.artist.empty())
                    continue;
                text = LyricsAlignment::fetchLyricsFromOvh (c.artist, c.title).lyrics;
                if (! text.empty()) { src = "lyrics.ovh"; break; }
            }
        }

        { std::lock_guard<std::mutex> lock (mutex); searching = false; }

        if (text.empty())
        {
            std::cout << "[LyricsGuide] No lyrics found for " << artist << " - " << title
                      << (hadError ? " (a lookup failed; will retry)" : "") << std::endl;
            std::lock_guard<std::mutex> lock (mutex);
            // A busy or unreachable site is retried after a minute; a clean "no
            // match" after an hour, in case lyrics get added
            notFound[key] = juce::Time::getMillisecondCounterHiRes()
                            + (hadError ? 60.0 * 1000.0 : 60.0 * 60.0 * 1000.0);
            continue;
        }

        cache.getParentDirectory().createDirectory();
        cache.replaceWithText (juce::String ("# source: ") + src + "\n" + juce::String (text));
        setLyricsText (key, text, src);
    }
}

void LyricsGuide::setManualLyrics (const std::string& text)
{
    std::string key;
    {
        std::lock_guard<std::mutex> lock (mutex);
        key = currentKey;
    }
    if (key.empty() || juce::String (text).trim().isEmpty())
        return;

    auto cache = cacheFileFor (key);
    cache.getParentDirectory().createDirectory();
    cache.replaceWithText (juce::String ("# source: pasted\n") + juce::String (text));
    setLyricsText (key, text, "pasted");
}

//==============================================================================
juce::File LyricsGuide::geniusTokenFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("ExplicitlyDesktop").getChildFile ("genius_token.txt");
}

void LyricsGuide::setGeniusToken (const std::string& token)
{
    auto file = geniusTokenFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::String (token).trim());

    // Songs that found nothing may be found now
    std::lock_guard<std::mutex> lock (mutex);
    notFound.clear();
    if (! currentKey.empty() && loadedKey != currentKey && ! searching && ! pendingKey.empty())
    {
        pending = true;
        wake.notify_all();
    }
}

bool LyricsGuide::hasGeniusToken()
{
    return geniusTokenFile().loadFileAsString().trim().isNotEmpty();
}

namespace
{
    // Index just past the </div> closing the <div ...> that starts at `start`
    size_t endOfDiv (const std::string& html, size_t start)
    {
        int depth = 0;
        size_t pos = start;
        while (pos < html.size())
        {
            auto open = html.find ("<div", pos);
            auto close = html.find ("</div>", pos);
            if (close == std::string::npos)
                return html.size();
            if (open != std::string::npos && open < close)
            {
                ++depth;
                pos = open + 4;
            }
            else
            {
                --depth;
                pos = close + 6;
                if (depth == 0)
                    return pos;
            }
        }
        return html.size();
    }

    std::string decodeEntities (std::string s)
    {
        static const std::pair<const char*, const char*> named[] = {
            { "&amp;", "&" }, { "&quot;", "\"" }, { "&#x27;", "'" }, { "&#39;", "'" },
            { "&apos;", "'" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&nbsp;", " " } };
        for (auto& [from, to] : named)
            for (size_t p; (p = s.find (from)) != std::string::npos;)
                s.replace (p, std::strlen (from), to);
        return s;
    }
}

std::string LyricsGuide::fetchFromGenius (const Candidate& c, const std::string& token) const
{
    // Official API: find the song and its page
    juce::URL search ("https://api.genius.com/search");
    search = search.withParameter ("q", juce::String (c.artist + " " + c.title));
    auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs (8000)
                       .withNumRedirectsToFollow (3)
                       .withExtraHeaders ("Authorization: Bearer " + juce::String (token)
                                          + "\r\nUser-Agent: ExplicitlyDesktop/1.0 (personal profanity filter)");
    auto stream = search.createInputStream (options);
    if (stream == nullptr)
        return {};

    auto json = juce::JSON::parse (stream->readEntireStreamAsString());
    auto* hits = json["response"]["hits"].getArray();
    if (hits == nullptr)
    {
        std::cout << "[LyricsGuide] Genius search failed (check the Genius key)" << std::endl;
        return {};
    }

    auto wantArtist = normalizeWord (c.artist);
    auto wantTitle = normalizeWord (c.title);
    juce::String pageUrl;
    for (auto& hit : *hits)
    {
        auto result = hit["result"];
        auto gotArtist = normalizeWord (result["primary_artist"]["name"].toString().toStdString());
        auto gotTitle = normalizeWord (result["title"].toString().toStdString());
        bool artistOk = wantArtist.empty() || gotArtist.find (wantArtist) != std::string::npos
                        || wantArtist.find (gotArtist) != std::string::npos
                        || LyricsAlignment::calculateSimilarity (gotArtist, wantArtist) >= 0.7f;
        bool titleOk = gotTitle.find (wantTitle) != std::string::npos || wantTitle.find (gotTitle) != std::string::npos
                       || LyricsAlignment::calculateSimilarity (gotTitle, wantTitle) >= 0.6f;
        if (artistOk && titleOk && result["url"].toString().startsWith ("https://genius.com/"))
        {
            pageUrl = result["url"].toString();
            break;
        }
    }
    if (pageUrl.isEmpty())
        return {};

    // The API has no lyrics text; it is in the song page's lyrics containers
    auto pageOptions = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                           .withConnectionTimeoutMs (8000)
                           .withNumRedirectsToFollow (3)
                           .withExtraHeaders ("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) ExplicitlyDesktop/1.0");
    auto page = juce::URL (pageUrl).createInputStream (pageOptions);
    if (page == nullptr)
        return {};
    auto html = page->readEntireStreamAsString().toStdString();

    std::string lyricsHtml;
    for (size_t pos = 0; (pos = html.find ("<div data-lyrics-container=\"true\"", pos)) != std::string::npos;)
    {
        auto end = endOfDiv (html, pos);
        auto block = html.substr (pos, end - pos);

        // Drop the "N Contributors ... Lyrics" header Genius puts inside
        for (size_t ex; (ex = block.find ("data-exclude-from-selection=\"true\"")) != std::string::npos;)
        {
            auto divStart = block.rfind ("<div", ex);
            if (divStart == std::string::npos)
                break;
            block.erase (divStart, endOfDiv (block, divStart) - divStart);
        }
        lyricsHtml += block + "<br/>";
        pos = end;
    }
    if (lyricsHtml.empty())
        return {};

    lyricsHtml = std::regex_replace (lyricsHtml, std::regex ("<br\\s*/?>"), "\n");
    lyricsHtml = std::regex_replace (lyricsHtml, std::regex ("<[^>]*>"), "");
    return trim (decodeEntities (lyricsHtml));
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

LyricsGuide::Decision LyricsGuide::analyze (const std::vector<WordSegment>& whisperWords, double chunkSeconds,
                                            double startBias)
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

    // Karaoke cues: the first Whisper-timed word of each matched line says when
    // that line starts being sung
    d.songGeneration = songGeneration;
    for (auto& op : ops)
    {
        if (op.i < 0 || op.j < 0)
            continue;
        int line = lyrics[op.j].line;
        if (! d.lineCues.empty() && d.lineCues.back().line == line)
            continue;
        if (line < 0 || line >= (int) lyricLines.size())
            continue;
        d.lineCues.push_back ({ line, std::max (0.0, whisperWords[op.i].start + startBias), lyricLines[line],
                                line + 1 < (int) lyricLines.size() ? lyricLines[line + 1] : std::string() });
    }

    // Lines covered by this chunk, for the "Actual Lyrics" display
    int firstJ = -1;
    for (auto& op : ops)
        if (op.j >= 0) { firstJ = op.j; break; }
    if (firstJ >= 0)
    {
        for (int line = lyrics[firstJ].line; line <= lyrics[bj - 1].line && line < (int) lyricLines.size(); ++line)
            d.lyricsLines += (d.lyricsLines.empty() ? "" : "  /  ") + lyricLines[line];
    }

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
                d.extraMuteIsFinal.push_back (false);
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
            // Lyrics word Whisper missed: it was sung just before the next word Whisper
            // did hear. Whisper's start times run late by startBias, so the next word
            // really starts at (its start + startBias); mute the ~0.5s before that,
            // without reaching back past the previous heard word's start
            double prevStart = 0.0, nextStart = chunkSeconds;
            for (int b = (int) k - 1; b >= 0; --b)
                if (ops[b].i >= 0) { prevStart = whisperWords[ops[b].i].start + startBias; break; }
            for (size_t f = k + 1; f < ops.size(); ++f)
                if (ops[f].i >= 0) { nextStart = whisperWords[ops[f].i].start + startBias; break; }

            double muteEnd = nextStart + 0.05;
            double muteStart = std::max ({ 0.0, prevStart + 0.1, nextStart - 0.55 });
            if (muteEnd - muteStart < 0.25)
                muteStart = std::max (0.0, muteEnd - 0.25);
            d.extraMutes.push_back ({ muteStart, muteEnd });
            d.extraMuteIsFinal.push_back (true);
            d.extraMuteWords.push_back (lyrics[op.j].text + " (not heard)");
        }
    }

    std::cout << "[LyricsGuide] Aligned " << matched << "/" << n << " words at lyrics word "
              << (bj - (int) ops.size() + 1) << "-" << bj << std::endl;
    return d;
}
