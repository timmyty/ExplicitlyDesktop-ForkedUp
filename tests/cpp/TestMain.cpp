/*
  ==============================================================================

    TestMain.cpp

    Runs every TEST. Usage: ExplicitlyTests [name-filter]
    Exit code 0 when all checks pass.

  ==============================================================================
*/

#include "TestFramework.h"

#include <cstring>
#include <exception>

int main (int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;

    for (auto& t : TestFramework::registry())
    {
        if (filter != nullptr && std::strstr (t.name, filter) == nullptr)
            continue;

        ++run;
        int before = TestFramework::failures();
        std::cout << "[ RUN  ] " << t.name << std::endl;
        try
        {
            t.fn();
        }
        catch (const std::exception& e)
        {
            TestFramework::fail (__FILE__, __LINE__, std::string ("exception: ") + e.what());
        }
        std::cout << (TestFramework::failures() == before ? "[  OK  ] " : "[ FAIL ] ") << t.name << std::endl;
    }

    std::cout << "\n" << run << " tests, " << TestFramework::checks() << " checks, "
              << TestFramework::failures() << " failures" << std::endl;
    return TestFramework::failures() == 0 ? 0 : 1;
}
