#include "engine/app/manager_link.h"

namespace engine::app {

std::optional<ManagerMessage> ManagerMessage::read(std::string_view line)
{
    if (line.empty() || line.front() != ManagerMark)
        return std::nullopt;
    line.remove_prefix(1);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);
    ManagerMessage message;
    message.m_document = std::make_shared<core::JsonDocument>();
    if (!message.m_document->parse(line, "manager") || message.m_document->root().type() != core::JsonType::Object ||
        message.m_document->root()["type"].asString().empty())
        return std::nullopt;
    return message;
}

std::string managerLine(const core::JsonWriter& message)
{
    std::string line;
    line.reserve(message.text().size() + 2);
    line.push_back(ManagerMark);
    line += message.text();
    line.push_back('\n');
    return line;
}

} // namespace engine::app
