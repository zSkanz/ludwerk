#pragma once

#include <memory>

#include "engine/core/types.h"

namespace engine::audio::detail {

// UWP device output only. Decoding, voices and effects stay in AudioSystem.
class UwpOutput
{
public:
    using Render = void (*)(void*, float*, core::u32);
    UwpOutput();
    ~UwpOutput();
    UwpOutput(const UwpOutput&) = delete;
    UwpOutput& operator=(const UwpOutput&) = delete;
    bool start(Render render, void* context);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace engine::audio::detail
