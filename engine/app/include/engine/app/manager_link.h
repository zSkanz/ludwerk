// **What a manager and its worlds say to each other** (ADR 0220 §2): lines on
// the world's standard input and output, each a mark and a JSON object with a
// `type` -- a byte no console command begins with, so a line typed by a person
// is never read as one, and the world's log lines pass beside them.
//
// No port is opened for this: the pipes are the manager's and its children's
// alone.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/json.h"
#include "engine/core/json_writer.h"

namespace engine::app {

// The ASCII record separator.
inline constexpr char ManagerMark = '\x1e';

class ManagerMessage
{
public:
    // A line, read: nothing when it is not a manager's message.
    [[nodiscard]] static std::optional<ManagerMessage> read(std::string_view line);

    [[nodiscard]] std::string_view type() const { return m_document->root()["type"].asString(); }
    [[nodiscard]] core::JsonValue root() const { return m_document->root(); }

private:
    std::shared_ptr<core::JsonDocument> m_document;
};

// **One line to send**: the mark, a whole JSON object the caller wrote --
// `type` among its fields -- and a newline.
[[nodiscard]] std::string managerLine(const core::JsonWriter& message);

} // namespace engine::app
