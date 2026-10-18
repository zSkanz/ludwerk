// **The data service's operations on items and money** (ADR 0238 §3): each a
// request and an answer, as JSON -- the same text whether it crossed a
// manager's pipe or was run by a server that keeps its own database.
//
// A request names its `op`:
//   `transact`  `moves`: [{`item`} or {`amount`, `currency`}, with `from`, `to`]
//   `create`    `owner`, `kind`, `fields`
//   `destroy`   `item`
//   `items`     `owner`
//   `balance`   `owner`, `currency`
// and the answer has `ok`, and `reason` where it is false; then `transaction`,
// `item`, `items` ([{`id`, `kind`, `fields`}]) or `balance`.
#pragma once

#include <string>
#include <string_view>

#include "engine/app/data_store.h"

namespace engine::app {

[[nodiscard]] std::string runDataOp(IDataStore& store, std::string_view world, std::string_view request);

} // namespace engine::app
