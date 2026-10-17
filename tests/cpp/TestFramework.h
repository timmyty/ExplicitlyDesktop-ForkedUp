/*
  ==============================================================================

    TestFramework.h

    A tiny self-contained test runner (no external framework):

        TEST (name) { CHECK (cond); CHECK_EQ (a, b); }

    Plus a minimal JSON reader for tests/vectors/protocol_vectors.json.

  ==============================================================================
*/

#pragma once

#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace TestFramework
{
    struct TestCase { const char* name; std::function<void()> fn; };

    inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
    inline int& failures() { static int f = 0; return f; }
    inline int& checks() { static int c = 0; return c; }

    struct Registrar
    {
        Registrar (const char* name, std::function<void()> fn) { registry().push_back ({ name, std::move (fn) }); }
    };

    inline void fail (const char* file, int line, const std::string& what)
    {
        ++failures();
        std::cout << "    FAILED " << file << ":" << line << ": " << what << std::endl;
    }

    template <typename A, typename B>
    void checkEqual (const A& a, const B& b, const char* ea, const char* eb, const char* file, int line)
    {
        ++checks();
        if (! (a == b))
        {
            std::ostringstream s;
            s << ea << " == " << eb << "\n      left:  " << a << "\n      right: " << b;
            fail (file, line, s.str());
        }
    }

    //==============================================================================
    /** Just enough JSON for the vectors file: objects, arrays, strings, numbers, bools. */
    struct Json
    {
        enum Kind { null, boolean, number, string, array, object } kind = null;
        bool b = false;
        double n = 0;
        std::string s;
        std::vector<Json> items;
        std::map<std::string, Json> fields;

        const Json& operator[] (const std::string& key) const
        {
            static const Json none;
            auto it = fields.find (key);
            return it != fields.end() ? it->second : none;
        }

        static Json parse (const std::string& text)
        {
            size_t pos = 0;
            return parseValue (text, pos);
        }

    private:
        static void skipSpace (const std::string& t, size_t& p)
        {
            while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t'))
                ++p;
        }

        static void appendUtf8 (std::string& out, unsigned cp)
        {
            if (cp < 0x80) out += (char) cp;
            else if (cp < 0x800) { out += (char) (0xC0 | (cp >> 6)); out += (char) (0x80 | (cp & 63)); }
            else { out += (char) (0xE0 | (cp >> 12)); out += (char) (0x80 | ((cp >> 6) & 63)); out += (char) (0x80 | (cp & 63)); }
        }

        static std::string parseString (const std::string& t, size_t& p)
        {
            std::string out;
            ++p;   // Opening quote
            while (p < t.size() && t[p] != '"')
            {
                char c = t[p++];
                if (c != '\\') { out += c; continue; }
                char e = t[p++];
                switch (e)
                {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': appendUtf8 (out, (unsigned) std::stoul (t.substr (p, 4), nullptr, 16)); p += 4; break;
                    default:  out += e; break;   // " \ /
                }
            }
            ++p;   // Closing quote
            return out;
        }

        static Json parseValue (const std::string& t, size_t& p)
        {
            Json j;
            skipSpace (t, p);
            if (p >= t.size())
                return j;

            char c = t[p];
            if (c == '{')
            {
                j.kind = object;
                ++p;
                for (;;)
                {
                    skipSpace (t, p);
                    if (t[p] == '}') { ++p; break; }
                    auto key = parseString (t, p);
                    skipSpace (t, p);
                    ++p;   // ':'
                    j.fields[key] = parseValue (t, p);
                    skipSpace (t, p);
                    if (t[p] == ',') ++p;
                }
            }
            else if (c == '[')
            {
                j.kind = array;
                ++p;
                for (;;)
                {
                    skipSpace (t, p);
                    if (t[p] == ']') { ++p; break; }
                    j.items.push_back (parseValue (t, p));
                    skipSpace (t, p);
                    if (t[p] == ',') ++p;
                }
            }
            else if (c == '"')
            {
                j.kind = string;
                j.s = parseString (t, p);
            }
            else if (t.compare (p, 4, "true") == 0)  { j.kind = boolean; j.b = true;  p += 4; }
            else if (t.compare (p, 5, "false") == 0) { j.kind = boolean; j.b = false; p += 5; }
            else if (t.compare (p, 4, "null") == 0)  { p += 4; }
            else
            {
                size_t end = p;
                while (end < t.size() && std::string ("+-0123456789.eE").find (t[end]) != std::string::npos)
                    ++end;
                j.kind = number;
                j.n = std::stod (t.substr (p, end - p));
                p = end;
            }
            return j;
        }
    };
}

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2 (a, b)
#define TEST(name) \
    static void TEST_CONCAT (test_, name)(); \
    static TestFramework::Registrar TEST_CONCAT (registrar_, name) (#name, TEST_CONCAT (test_, name)); \
    static void TEST_CONCAT (test_, name)()

#define CHECK(cond) \
    do { ++TestFramework::checks(); if (! (cond)) TestFramework::fail (__FILE__, __LINE__, #cond); } while (0)

#define CHECK_EQ(a, b) TestFramework::checkEqual ((a), (b), #a, #b, __FILE__, __LINE__)
