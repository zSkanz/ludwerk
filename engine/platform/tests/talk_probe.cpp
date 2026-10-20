// A child that says a line, waits, says another, then stays until it is
// killed or the time it was given runs out: what `child_process_tests.cpp`
// starts to see its output reach the parent the moment it is written.
//
//   engine_talk_probe <milliseconds between the lines> <milliseconds to stay after>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv)
{
    const int between = argc >= 2 ? std::atoi(argv[1]) : 300;
    const int after = argc >= 3 ? std::atoi(argv[2]) : 0;
    std::fputs("one\n", stdout);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(between));
    std::fputs("two\n", stdout);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(after));
    return 0;
}
