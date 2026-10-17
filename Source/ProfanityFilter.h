/*
  ==============================================================================

    ProfanityFilter.h
    Created: 9 Dec 2024
    Author: Explicitly Audio Systems

    Lexicon-based profanity detection with multi-token support.
    
    Features:
    - Simple string matching (<1ms processing)
    - Case-insensitive
    - Multi-token phrases (e.g., "what the hell")
    - Loads profanity list from text file

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <string>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cctype>

/**
    Lexicon-based profanity detection.
    
    Usage:
        ProfanityFilter filter;
        filter.loadLexicon("profanity_lexicon.txt");
        
        if (filter.isProfane("damn")) {
            // Censor this word
        }
*/
class ProfanityFilter
{
public:
    ProfanityFilter()
    {
    }
    
    /**
        Load profanity lexicon from text file.
        
        File format: one word/phrase per line
        Example:
            damn
            hell
            what the hell
            son of a bitch
        
        @param file_path    Path to lexicon file
        @return             true if loaded successfully
    */
    bool loadLexicon(const juce::File& lexicon_file)
    {
        if (!lexicon_file.existsAsFile())
        {
            juce::Logger::writeToLog("[ProfanityFilter] ERROR: Lexicon file not found: " 
                + lexicon_file.getFullPathName());
            return false;
        }
        
        lexicon_.clear();
        
        juce::StringArray lines;
        lexicon_file.readLines(lines);
        
        for (const auto& line : lines)
        {
            juce::String word = line.trim().toLowerCase();
            if (word.isNotEmpty() && !word.startsWith("#"))  // Skip empty and comments
            {
                lexicon_.insert(word.toStdString());
            }
        }
        
        juce::Logger::writeToLog("[ProfanityFilter] Loaded " 
            + juce::String(lexicon_.size()) + " profanity entries");
        
        return lexicon_.size() > 0;
    }
    
    /**
        Check if a word is profane.
        
        @param word     Word to check (case-insensitive)
        @return         true if word is in lexicon
        
        Complexity: O(1) average case (hash lookup)
    */
    /** All lexicon entries (lowercase). */
    const auto& words() const { return lexicon_; }
    
    bool isProfane(const std::string& word) const
    {
        std::string lower = word;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return lexicon_.find(lower) != lexicon_.end();
    }
    
    /**
        Check if a word is profane (JUCE String version).
    */
    bool isProfane(const juce::String& word) const
    {
        return isProfane(word.toLowerCase().toStdString());
    }
    
    /**
        Detect profanity in a list of transcribed words with timestamps.
        
        This supports multi-token phrases by checking sliding windows.
        
        Example:
            Input: ["what", "the", "hell", "is", "this"]
            Detects: "what the hell" (3-token phrase)
        
        @param words        List of words with timestamps
        @return             List of profanity spans with start/end indices
    */
    struct Word
    {
        std::string text;
        double start_time;      // Seconds from audio start
        double end_time;
    };
    
    struct ProfanitySpan
    {
        size_t start_word_idx;  // Index of first word
        size_t end_word_idx;    // Index of last word (inclusive)
        double start_time;      // Seconds
        double end_time;
        std::string text;       // Full profane phrase
    };
    
    std::vector<ProfanitySpan> detectProfanity(const std::vector<Word>& words) const
    {
        std::vector<ProfanitySpan> profanity_spans;
        
        // Maximum phrase length to check (e.g., "son of a bitch" = 4 tokens)
        const size_t max_phrase_length = 5;
        
        for (size_t i = 0; i < words.size(); ++i)
        {
            // Try phrases of decreasing length starting at position i
            for (size_t phrase_len = std::min(max_phrase_length, words.size() - i); 
                 phrase_len >= 1; 
                 --phrase_len)
            {
                // Build phrase from words[i] to words[i + phrase_len - 1]
                std::string phrase;
                for (size_t j = 0; j < phrase_len; ++j)
                {
                    if (j > 0) phrase += " ";
                    phrase += words[i + j].text;
                }
                
                // Check if phrase is profane
                if (isProfane(phrase))
                {
                    ProfanitySpan span;
                    span.start_word_idx = i;
                    span.end_word_idx = i + phrase_len - 1;
                    span.start_time = words[i].start_time;
                    span.end_time = words[i + phrase_len - 1].end_time;
                    span.text = phrase;
                    
                    profanity_spans.push_back(span);
                    
                    // Skip ahead to avoid overlapping detections
                    i += phrase_len - 1;
                    break;
                }
            }
        }
        
        return profanity_spans;
    }
    
    /**
        Get number of profanity entries loaded.
    */
    size_t size() const
    {
        return lexicon_.size();
    }
    
    /**
        Check if lexicon is loaded.
    */
    bool isLoaded() const
    {
        return lexicon_.size() > 0;
    }
    
private:
    std::unordered_set<std::string> lexicon_;
};
