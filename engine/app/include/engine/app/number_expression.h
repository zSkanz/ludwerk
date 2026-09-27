// Arithmetic typed into a number field (the owner: `0+2` is 2, `1/2` is 0.5).
#pragma once

#include <optional>
#include <string_view>

namespace engine::app {

// `+ - * /`, parentheses and a sign, over decimal numbers -- a comma counts as
// a point -- and a unit after a number (`m`, `deg`, the degree sign) is
// ignored, so a field's own text can be typed back. Nothing on a division by
// zero, an unfinished expression or anything else it cannot read.
[[nodiscard]] std::optional<double> evaluateNumberExpression(std::string_view text);

} // namespace engine::app
