#include "smaa_tables.h"

#include "../../../third_party/smaa/Textures/AreaTex.h"
#include "../../../third_party/smaa/Textures/SearchTex.h"

namespace engine::render {

static_assert(AREATEX_WIDTH == kSmaaAreaWidth && AREATEX_HEIGHT == kSmaaAreaHeight &&
              AREATEX_PITCH == 2 * kSmaaAreaWidth);
static_assert(SEARCHTEX_WIDTH == kSmaaSearchWidth && SEARCHTEX_HEIGHT == kSmaaSearchHeight);

std::span<const std::byte> smaaAreaTable() noexcept
{
    return std::as_bytes(std::span<const unsigned char>(areaTexBytes, AREATEX_SIZE));
}

std::span<const std::byte> smaaSearchTable() noexcept
{
    return std::as_bytes(std::span<const unsigned char>(searchTexBytes, SEARCHTEX_SIZE));
}

} // namespace engine::render
