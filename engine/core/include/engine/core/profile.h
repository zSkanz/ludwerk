// **Where a frame's time goes, system by system** (H0): a tree of scoped
// timers, each named by a string literal at the place it measures, nested as
// the calls nest. A frame's report says the median and p95 of every scope, how
// many times it ran, and what of it no scope under it accounts for -- which is
// how a cost nobody instrumented shows up as a number instead of a guess.
//
// **One thread's.** The thread that turned it on records; a scope anywhere
// else costs the same one branch as a scope with it off. The tree is what the
// main thread waits on, and a job's time is inside the scope that waited for
// it.
//
// Off, a scope is a load and a branch: compiled in everywhere, so a player's
// build can be measured as it ships. Reading a clock is fine here and nowhere
// in the simulation (R10): what is measured never decides anything.
#pragma once

#include <string>
#include <vector>

#include "engine/core/types.h"

namespace engine::core::profile {

// A scope's name, registered once per call site. Literals only: the pointer is
// kept, not the text.
[[nodiscard]] u32 registerSite(const char* name);

// Records on the calling thread from here on, or stops. Turning it on starts
// an empty tree.
void setEnabled(bool on);
[[nodiscard]] bool enabled() noexcept;

// Closes the frame: what every scope spent in it becomes one sample of that
// scope's history, and the next frame starts at zero. Scopes still open carry
// on into the next frame and are counted where they close.
void endFrame();

// The clock, in nanoseconds; replaced only by tests.
using Clock = u64 (*)();
void setClockForTests(Clock clock) noexcept;

struct ScopeReport
{
    // The scope's name, and how deep in the tree it is: a root is 0.
    std::string name;
    u32 depth = 0;
    f64 medianMs = 0.0;
    f64 p95Ms = 0.0;
    // Its worst frame: where a spike shows that a median and a p95 hide.
    f64 worstMs = 0.0;
    // What of the scope the scopes under it do not account for.
    f64 selfMedianMs = 0.0;
    // How many times it ran in a frame, at the median.
    f64 calls = 0.0;
};

// The tree, depth first in the order scopes were first entered, over every
// frame closed since it was turned on less the first `skipFrames`. A scope
// that did not run in a frame counts that frame as zero.
[[nodiscard]] std::vector<ScopeReport> report(usize skipFrames);

class Scope
{
public:
    explicit Scope(u32 site) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    u64 m_start = 0;
    i32 m_node = -1;
};

// **One function's stretches, one after another**: each `next` closes the
// stretch before it and opens the next, and the last closes with the object.
// What a long function with a heading a pass -- the renderer's -- is measured
// with, without a block around each pass.
class Sections
{
public:
    Sections() noexcept = default;
    ~Sections() { close(); }
    Sections(const Sections&) = delete;
    Sections& operator=(const Sections&) = delete;

    void next(u32 site) noexcept;
    void close() noexcept;

private:
    u64 m_start = 0;
    i32 m_node = -1;
};

} // namespace engine::core::profile

#define ENG_PROFILE_CONCAT_INNER(a, b) a##b
#define ENG_PROFILE_CONCAT(a, b) ENG_PROFILE_CONCAT_INNER(a, b)
// The next stretch of `sections`, named `literal`.
#define ENG_PROFILE_NEXT(sections, literal)                                                                            \
    do {                                                                                                               \
        static const ::engine::core::u32 engProfileNextSite = ::engine::core::profile::registerSite(literal);          \
        (sections).next(engProfileNextSite);                                                                           \
    } while (false)
// A scope from here to the end of the enclosing block, named `literal`.
#define ENG_PROFILE_SCOPE(literal)                                                                                     \
    static const ::engine::core::u32 ENG_PROFILE_CONCAT(engProfileSite, __LINE__) =                                    \
        ::engine::core::profile::registerSite(literal);                                                                \
    const ::engine::core::profile::Scope ENG_PROFILE_CONCAT(engProfileScope, __LINE__)                                 \
    {                                                                                                                  \
        ENG_PROFILE_CONCAT(engProfileSite, __LINE__)                                                                   \
    }
