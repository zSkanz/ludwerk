#include "engine/app/data_ops.h"

#include <cmath>
#include <optional>
#include <vector>

#include "engine/core/json.h"
#include "engine/core/json_writer.h"

namespace engine::app {
namespace {

// A whole number from JSON, or none: an item's id or an amount is never a
// fraction, and one that is was made wrong.
[[nodiscard]] std::optional<core::i64> wholeNumber(const core::JsonValue& value)
{
    if (value.type() != core::JsonType::Number)
        return std::nullopt;
    const core::f64 number = value.asNumber();
    if (!std::isfinite(number) || std::floor(number) != number || std::fabs(number) > 9.0e15)
        return std::nullopt;
    return static_cast<core::i64>(number);
}

[[nodiscard]] std::string refused(std::string_view reason)
{
    core::JsonWriter answer(core::JsonLayout::Compact);
    answer.beginObject();
    answer.field("ok", false);
    answer.field("reason", reason);
    answer.endObject();
    return answer.text();
}

} // namespace

std::string runDataOp(IDataStore& store, std::string_view world, std::string_view request)
{
    core::JsonDocument document;
    if (!document.parse(request, "data-op"))
        return refused("Invalid");
    const core::JsonValue root = document.root();
    const std::string_view op = root["op"].asString();
    core::JsonWriter answer(core::JsonLayout::Compact);
    answer.beginObject();
    if (op == "transact") {
        const core::JsonValue listed = root["moves"];
        std::vector<DataMove> moves;
        for (core::usize at = 0; at < listed.size(); ++at) {
            const core::JsonValue one = listed.at(at);
            DataMove move;
            if (one.has("item")) {
                const std::optional<core::i64> item = wholeNumber(one["item"]);
                if (!item.has_value())
                    return refused("Invalid");
                move.item = *item;
            }
            else {
                const std::optional<core::i64> amount = wholeNumber(one["amount"]);
                if (!amount.has_value())
                    return refused("Invalid");
                move.amount = *amount;
                move.currency = std::string(one["currency"].asString());
            }
            move.from = std::string(one["from"].asString());
            move.to = std::string(one["to"].asString());
            moves.push_back(std::move(move));
        }
        if (moves.empty())
            return refused("Invalid");
        const DataTransaction done = store.transact(world, moves);
        answer.field("ok", done.ok);
        if (done.ok)
            answer.field("transaction", done.id);
        else
            answer.field("reason", std::string_view{done.reason});
    }
    else if (op == "create") {
        const std::optional<core::i64> item =
            store.createItem(world, root["owner"].asString(), root["kind"].asString(), root["fields"].asString());
        answer.field("ok", item.has_value());
        if (item.has_value())
            answer.field("item", *item);
        else
            answer.field("reason", "Failed");
    }
    else if (op == "destroy") {
        const std::optional<core::i64> item = wholeNumber(root["item"]);
        const bool gone = item.has_value() && store.destroyItem(world, *item);
        answer.field("ok", gone);
        if (!gone)
            answer.field("reason", "ItemNotThere");
    }
    else if (op == "items") {
        answer.field("ok", true);
        answer.key("items");
        answer.beginArray();
        for (const DataItem& item : store.items(root["owner"].asString())) {
            answer.beginObject();
            answer.field("id", item.id);
            answer.field("kind", std::string_view{item.kind});
            answer.field("fields", std::string_view{item.fields});
            answer.endObject();
        }
        answer.endArray();
    }
    else if (op == "get") {
        const SharedDatum held = store.getShared(root["scope"].asString(), root["key"].asString());
        answer.field("ok", true);
        answer.field("value", std::string_view{held.value});
        answer.field("version", held.version);
    }
    else if (op == "set") {
        const std::optional<core::i64> version = wholeNumber(root["version"]);
        if (!version.has_value())
            return refused("Invalid");
        const SharedWrite written =
            store.setShared(root["scope"].asString(), root["key"].asString(), root["value"].asString(), *version);
        answer.field("ok", written.ok);
        if (!written.ok)
            answer.field("reason", "Conflict");
        answer.field("value", std::string_view{written.now.value});
        answer.field("version", written.now.version);
    }
    else if (op == "balance") {
        answer.field("ok", true);
        answer.field("balance", store.balance(root["owner"].asString(), root["currency"].asString()));
    }
    else {
        return refused("Invalid");
    }
    answer.endObject();
    return answer.text();
}

} // namespace engine::app
