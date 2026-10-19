#include "engine/app/servers_config.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <set>

#include "engine/core/i18n.h"
#include "engine/core/toml.h"

namespace engine::app {
namespace {

[[nodiscard]] bool nameWord(std::string_view name)
{
    return !name.empty() && name.size() <= 32 && !name.starts_with("i-") &&
           std::all_of(name.begin(), name.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}

[[nodiscard]] core::EngineError problem(core::TextKey key, std::string_view source, std::string_view detail)
{
    const std::array<core::I18nArg, 2> args{core::I18nArg{"path", source}, core::I18nArg{"detail", detail}};
    return core::makeError(key, args);
}

} // namespace

const ServersConfig::World* ServersConfig::entry() const
{
    const auto found = std::find_if(worlds.begin(), worlds.end(), [](const World& w) { return w.entry; });
    return found == worlds.end() ? nullptr : &*found;
}

const ServersConfig::World* ServersConfig::world(std::string_view name) const
{
    const auto found = std::find_if(worlds.begin(), worlds.end(), [&](const World& w) { return w.name == name; });
    return found == worlds.end() ? nullptr : &*found;
}

const ServersConfig::Instance* ServersConfig::instance(std::string_view name) const
{
    const auto found =
        std::find_if(instances.begin(), instances.end(), [&](const Instance& i) { return i.name == name; });
    return found == instances.end() ? nullptr : &*found;
}

std::optional<ServersConfig> readServersConfig(std::string_view text, std::string_view source,
                                               std::optional<core::EngineError>& error)
{
    core::TomlDocument document;
    if (const core::TomlDocument::ParseResult parsed = document.parse(text, source); !parsed) {
        error = problem(ENG_TR("manager.err.servers_unreadable"), source, parsed.diagnostic);
        return std::nullopt;
    }
    ServersConfig config;
    // Whole numbers in a range; anything else is said, never guessed.
    bool wrong = false;
    const auto count = [&](const std::string& key, core::u32 lowest, core::u32 highest, core::u32& out) {
        if (!document.has(key))
            return;
        const std::optional<core::f64> value = document.number(key);
        if (!value.has_value() || *value < lowest || *value > highest || *value != std::floor(*value)) {
            error = problem(ENG_TR("manager.err.servers_value"), source, key);
            wrong = true;
            return;
        }
        out = static_cast<core::u32>(*value);
    };
    if (const std::optional<std::string_view> address = document.string("manager.public_address"))
        config.publicAddress = std::string(*address);
    if (const std::optional<std::string_view> logs = document.string("manager.logs"))
        config.logs = std::string(*logs);
    core::u32 port = config.port;
    count("manager.port", 1, 65535, port);
    config.port = static_cast<core::u16>(port);
    if (const std::optional<std::string_view> range = document.string("manager.world_ports")) {
        unsigned first = 0;
        unsigned last = 0;
        const std::size_t dash = range->find('-');
        const auto number = [](std::string_view text, unsigned& out) {
            const auto [end, failed] = std::from_chars(text.data(), text.data() + text.size(), out);
            return failed == std::errc{} && end == text.data() + text.size();
        };
        if (dash == std::string_view::npos || !number(range->substr(0, dash), first) ||
            !number(range->substr(dash + 1), last) || first == 0 || last > 65535 || first > last) {
            error = problem(ENG_TR("manager.err.servers_value"), source, "manager.world_ports");
            return std::nullopt;
        }
        config.firstWorldPort = static_cast<core::u16>(first);
        config.lastWorldPort = static_cast<core::u16>(last);
    }
    count("manager.log_megabytes", 1, 4096, config.logMegabytes);
    count("manager.log_files", 1, 100, config.logFiles);
    count("manager.restarts", 0, 1000, config.restarts);
    count("manager.restart_minutes", 1, 10080, config.restartMinutes);
    count("manager.min_free_megabytes", 0, 1u << 20, config.minFreeMegabytes);
    config.health = document.boolean("manager.health").value_or(false);
    config.admin = document.boolean("manager.admin").value_or(false);
    if (const std::optional<core::f64> value = document.number("manager.admin_port");
        value.has_value() && *value >= 1.0 && *value <= 65535.0)
        config.adminPort = static_cast<core::u16>(*value);
    for (const std::string& address : document.strings("manager.admin_allow"))
        config.adminAllow.push_back(address);

    count("manager.max_players", 0, 1000000, config.maxPlayers);
    count("manager.queue_max", 0, 1000000, config.queueMax);
    count("manager.queue_grace_seconds", 0, 3600, config.queueGraceSeconds);
    if (document.has("data.backup_every_hours")) {
        const std::optional<core::f64> hours = document.number("data.backup_every_hours");
        // A year at most; a quarter of an hour at least, where it is on.
        if (!hours.has_value() || *hours < 0.0 || *hours > 8760.0 || (*hours > 0.0 && *hours < 0.25)) {
            error = problem(ENG_TR("manager.err.servers_value"), source, "data.backup_every_hours");
            return std::nullopt;
        }
        config.backupEveryHours = *hours;
    }
    count("data.backup_keep", 1, 1000, config.backupKeep);
    count("data.moves_keep_days", 0, 36500, config.movesKeepDays);
    std::set<std::string> names;
    for (core::usize at = 0; at < document.count("world"); ++at) {
        const std::string prefix = "world." + std::to_string(at) + ".";
        ServersConfig::World world;
        world.name = std::string(document.string(prefix + "name").value_or(""));
        world.scene = std::string(document.string(prefix + "scene").value_or(""));
        world.entry = document.boolean(prefix + "entry").value_or(false);
        world.timeDilation = document.boolean(prefix + "time_dilation").value_or(false);
        world.channels = document.boolean(prefix + "channels").value_or(false);
        count(prefix + "max_channels", 1, 64, world.maxChannels);
        count(prefix + "channel_close_seconds", 0, 86400, world.channelCloseSeconds);
        if (const std::optional<core::f64> share = document.number(prefix + "channel_open_at");
            share.has_value() && *share >= 0.1 && *share <= 1.0)
            world.channelOpenAt = *share;
        if (const std::optional<core::f64> minutes = document.number(prefix + "merge_minutes");
            minutes.has_value() && *minutes >= 0.0 && *minutes <= 1440.0)
            world.mergeMinutes = *minutes;
        count(prefix + "max_players", 1, 4096, world.maxPlayers);
        if (!nameWord(world.name) || !names.insert(world.name).second) {
            error = problem(ENG_TR("manager.err.servers_name"), source, world.name);
            return std::nullopt;
        }
        config.worlds.push_back(std::move(world));
    }
    for (core::usize at = 0; at < document.count("instance"); ++at) {
        const std::string prefix = "instance." + std::to_string(at) + ".";
        ServersConfig::Instance instance;
        instance.name = std::string(document.string(prefix + "template").value_or(""));
        instance.scene = std::string(document.string(prefix + "scene").value_or(""));
        count(prefix + "max_players", 1, 4096, instance.maxPlayers);
        count(prefix + "max_instances", 1, 10000, instance.maxInstances);
        count(prefix + "warm", 0, 100, instance.warm);
        count(prefix + "idle_close_seconds", 0, 86400, instance.idleCloseSeconds);
        if (!nameWord(instance.name) || !names.insert(instance.name).second) {
            error = problem(ENG_TR("manager.err.servers_name"), source, instance.name);
            return std::nullopt;
        }
        config.instances.push_back(std::move(instance));
    }
    if (wrong)
        return std::nullopt;
    // **One entry**: where the front door sends a new player.
    const auto entries = std::count_if(config.worlds.begin(), config.worlds.end(),
                                       [](const ServersConfig::World& w) { return w.entry; });
    if (config.worlds.empty() || entries != 1) {
        error = problem(ENG_TR("manager.err.servers_entry"), source, {});
        return std::nullopt;
    }
    const core::u32 ports = static_cast<core::u32>(config.lastWorldPort - config.firstWorldPort) + 1;
    if (config.port >= config.firstWorldPort && config.port <= config.lastWorldPort) {
        error = problem(ENG_TR("manager.err.servers_value"), source, "manager.port");
        return std::nullopt;
    }
    // **Administration beside the front door**, and never on a world's
    // number: a world answers its health over TCP there.
    if (config.admin && config.adminPort == 0)
        config.adminPort = static_cast<core::u16>(config.port + 1);
    if (config.admin && config.adminPort >= config.firstWorldPort && config.adminPort <= config.lastWorldPort) {
        error = problem(ENG_TR("manager.err.servers_value"), source, "manager.admin_port");
        return std::nullopt;
    }
    if (ports < config.worlds.size()) {
        error = problem(ENG_TR("manager.err.servers_value"), source, "manager.world_ports");
        return std::nullopt;
    }
    return config;
}

} // namespace engine::app
