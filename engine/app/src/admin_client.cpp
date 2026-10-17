#include "engine/app/admin_client.h"

#include <array>
#include <charconv>
#include <cstdio>

#include "engine/app/admin_channel.h"
#include "engine/app/match_keys.h"
#include "engine/app/server_console.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/platform/file.h"

namespace engine::app {

int runAdminKeygen(const std::filesystem::path& file)
{
    // The private half readable by its owner only, the public beside it.
    const core::X25519Pair pair = core::x25519Generate();
    std::filesystem::path pub = file;
    pub.replace_extension(".pub");
    if (!platform::createDirectories(file.parent_path().empty() ? std::filesystem::path(".") : file.parent_path()) ||
        !platform::writeTextFile(file, core::toHex(pair.secret)) || !platform::restrictToOwner(file) ||
        !platform::writeTextFile(pub, core::toHex(pair.pub))) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"path", file.string()}};
        core::log(core::LogLevel::Error, ENG_TR("admin.err.key_write"), args);
        return 2;
    }
    std::fprintf(stdout, "%s\n", core::toHex(pair.pub).c_str());
    return 0;
}

int runAdminConnect(const AdminConnect& options)
{
    // The administrator's own key, from a file only they can read.
    std::string text;
    if (!platform::readTextFile(options.keyFile, text) || platform::readableByOthers(options.keyFile)) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"path", options.keyFile.string()}};
        core::log(core::LogLevel::Error, ENG_TR("admin.err.key_read"), args);
        return 2;
    }
    const std::optional<std::array<core::u8, 32>> secret = matchKeyFromHex(text);
    if (!secret.has_value()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"path", options.keyFile.string()}};
        core::log(core::LogLevel::Error, ENG_TR("admin.err.key_read"), args);
        return 2;
    }
    const core::X25519Pair mine{*secret, core::x25519Public(*secret)};
    std::array<core::u8, 32> serverKey{};
    std::optional<core::EngineError> error;
    std::optional<AdminChannel> channel =
        AdminChannel::connect(options.host, options.port, mine, adminPrologue(), serverKey, error);
    if (!channel.has_value()) {
        core::logText(core::LogLevel::Error, error.has_value() ? error->message : std::string{});
        return 1;
    }
    // **The server it reached is the one it expected**: the key it was told,
    // or the one it met here before -- a change is refused, never trusted.
    const std::string where = options.host + ":" + std::to_string(options.port);
    if (!options.serverKey.empty()) {
        if (core::toHex(serverKey) != options.serverKey) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"server", std::string_view{where}}};
            core::log(core::LogLevel::Error, ENG_TR("admin.err.server_key"), args);
            return 1;
        }
    }
    else {
        std::filesystem::path pins = options.keyFile;
        pins += ".servers.json";
        KeyPins known(pins);
        if (!known.trust(where, serverKey)) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"server", std::string_view{where}}};
            core::log(core::LogLevel::Error, ENG_TR("admin.err.server_key"), args);
            return 1;
        }
    }
    if (options.follow)
        (void)channel->send(AdminKind::Follow, {});

    // **One command, and its outcome as the exit code**, for a script.
    if (!options.command.empty()) {
        if (channel->send(AdminKind::Line, options.command).has_value())
            return 1;
        while (true) {
            AdminKind kind = AdminKind::Answer;
            std::string said;
            bool got = false;
            if (channel->receive(kind, said, got, 1000).has_value())
                return 1;
            if (!got)
                continue;
            if (kind == AdminKind::LogLine) {
                std::fprintf(stdout, "%s", said.c_str());
                continue;
            }
            std::fprintf(stdout, "%s\n", said.c_str());
            std::fflush(stdout);
            return kind == AdminKind::Failed ? 1 : 0;
        }
    }

    // **A prompt**: each line sent, each answer printed as it comes.
    const std::array<core::I18nArg, 2> args{core::I18nArg{"server", std::string_view{where}},
                                            core::I18nArg{"key", core::toHex(serverKey).substr(0, 16)}};
    core::log(core::LogLevel::Info, ENG_TR("admin.info.connected"), args);
    ConsoleInput input;
    input.start();
    while (!input.ended()) {
        std::vector<std::string> lines;
        input.take(lines);
        for (const std::string& line : lines) {
            if (line == "exit" || line == "quit")
                return 0;
            if (channel->send(AdminKind::Line, line).has_value())
                return 1;
        }
        AdminKind kind = AdminKind::Answer;
        std::string said;
        bool got = false;
        if (channel->receive(kind, said, got, 50).has_value()) {
            core::log(core::LogLevel::Info, ENG_TR("admin.info.disconnected"));
            return 1;
        }
        if (got) {
            std::fprintf(stdout, kind == AdminKind::LogLine ? "%s" : "%s\n", said.c_str());
            std::fflush(stdout);
        }
    }
    return 0;
}

} // namespace engine::app
