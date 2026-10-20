#include "engine/app/backend_data_store.h"

#include <array>
#include <chrono>
#include <cmath>
#include <span>
#include <thread>
#include <utility>

#include "engine/core/base64.h"
#include "engine/core/crypto.h"
#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/core/log.h"
#include "engine/net/http.h"

namespace engine::app {
namespace {

// An IPv4 address written out, as four numbers; none for anything else.
[[nodiscard]] std::optional<std::array<unsigned, 4>> ipv4(std::string_view host)
{
    std::array<unsigned, 4> parts{};
    core::usize at = 0;
    for (core::usize part = 0; part < 4; ++part) {
        if (at >= host.size() || host[at] < '0' || host[at] > '9')
            return std::nullopt;
        unsigned value = 0;
        core::usize digits = 0;
        while (at < host.size() && host[at] >= '0' && host[at] <= '9' && digits < 3) {
            value = value * 10u + static_cast<unsigned>(host[at] - '0');
            ++at;
            ++digits;
        }
        if (value > 255)
            return std::nullopt;
        parts[part] = value;
        if (part < 3) {
            if (at >= host.size() || host[at] != '.')
                return std::nullopt;
            ++at;
        }
    }
    if (at != host.size())
        return std::nullopt;
    return parts;
}

[[nodiscard]] bool nearHost(std::string_view host)
{
    if (host == "localhost")
        return true;
    // An IPv6 address, as a URL writes one: in brackets.
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
        std::string inner(host.substr(1, host.size() - 2));
        for (char& c : inner)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (inner == "::1")
            return true;
        // fe80::/10: fe80 to febf.
        return inner.size() > 4 && inner[0] == 'f' && inner[1] == 'e' &&
               (inner[2] == '8' || inner[2] == '9' || inner[2] == 'a' || inner[2] == 'b') && inner[4] == ':';
    }
    const std::optional<std::array<unsigned, 4>> address = ipv4(host);
    if (!address.has_value())
        return false;
    const auto [a, b, c, d] = *address;
    (void)c;
    (void)d;
    return a == 127 || a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 169 && b == 254);
}

// A request's id: 128 random bits, so two managers never share one.
[[nodiscard]] std::string requestId()
{
    std::array<core::u8, 16> bytes{};
    core::secureRandom(bytes);
    return core::toHex(bytes);
}

[[nodiscard]] std::string base64Of(std::string_view bytes)
{
    return core::base64Encode(std::span<const core::u8>(reinterpret_cast<const core::u8*>(bytes.data()), bytes.size()));
}

[[nodiscard]] std::optional<core::i64> whole(const core::JsonValue& value)
{
    if (value.type() != core::JsonType::Number)
        return std::nullopt;
    const core::f64 number = value.asNumber();
    if (!std::isfinite(number) || std::floor(number) != number)
        return std::nullopt;
    return static_cast<core::i64>(number);
}

class BackendDataStore final : public IDataStore
{
public:
    explicit BackendDataStore(BackendOptions options) : m_options(std::move(options))
    {
        if (!m_options.send) {
            m_options.send = [](const std::string& url, const std::string& body, const std::string& token,
                                core::u32 timeoutMs) -> std::optional<std::string> {
                net::HttpRequest request;
                request.url = url;
                request.method = "POST";
                request.body = body;
                request.timeoutMs = timeoutMs;
                request.headers.push_back({"Content-Type", "application/json"});
                if (!token.empty())
                    request.headers.push_back({"Authorization", "Bearer " + token});
                net::HttpResponse response;
                if (net::performHttp(request, response).has_value() || !response.ok || response.statusCode != 200)
                    return std::nullopt;
                return std::move(response.body);
            };
        }
        if (!m_options.endpoint.empty()) {
            m_endpoint = m_options.endpoint;
        }
        else {
            m_endpoint = m_options.url;
            while (!m_endpoint.empty() && m_endpoint.back() == '/')
                m_endpoint.pop_back();
            m_endpoint += "/v1/data";
        }
    }

    [[nodiscard]] bool keepsItsLog() const override { return false; }
    [[nodiscard]] bool unavailable() const override { return m_failed; }

    [[nodiscard]] RecordRead readRecord(std::string_view account) override
    {
        core::JsonWriter request = begin("load");
        request.field("account", account);
        const std::optional<core::JsonDocument> answer = call(request);
        if (!answer.has_value())
            return RecordRead{false, std::nullopt};
        const core::JsonValue root = answer->root();
        if (!root["found"].asBool(false))
            return RecordRead{true, std::nullopt};
        const std::optional<std::vector<core::u8>> body = core::base64Decode(root["body"].asString());
        if (!body.has_value())
            return RecordRead{false, std::nullopt};
        return RecordRead{true, StoredRecord{std::string(body->begin(), body->end()), root["version"].asNumber(1.0)}};
    }

    [[nodiscard]] std::optional<StoredRecord> loadRecord(std::string_view account) override
    {
        return readRecord(account).record;
    }

    [[nodiscard]] bool writeRecord(std::string_view account, const StoredRecord& record) override
    {
        core::JsonWriter request = begin("write");
        request.field("account", account);
        request.field("body", std::string_view{base64Of(record.body)});
        request.field("version", record.version);
        const std::optional<core::JsonDocument> answer = call(request);
        return answer.has_value() && answer->root()["ok"].asBool(false);
    }

    [[nodiscard]] DataTransaction transact(std::string_view world, const std::vector<DataMove>& moves) override
    {
        core::JsonWriter request = begin("transact");
        request.field("world", world);
        request.key("moves");
        request.beginArray();
        for (const DataMove& move : moves) {
            request.beginObject();
            if (move.item != 0) {
                request.field("item", move.item);
            }
            else {
                request.field("currency", std::string_view{move.currency});
                request.field("amount", move.amount);
            }
            request.field("from", std::string_view{move.from});
            request.field("to", std::string_view{move.to});
            request.endObject();
        }
        request.endArray();
        const std::optional<core::JsonDocument> answer = call(request);
        if (!answer.has_value())
            return DataTransaction{false, "Unavailable", 0};
        const core::JsonValue root = answer->root();
        if (!root["ok"].asBool(false))
            return DataTransaction{false, std::string(root["reason"].asString()), 0};
        return DataTransaction{true, {}, whole(root["transaction"]).value_or(0)};
    }

    [[nodiscard]] std::optional<core::i64> createItem(std::string_view world, std::string_view owner,
                                                      std::string_view kind, std::string_view fields) override
    {
        core::JsonWriter request = begin("create");
        request.field("world", world);
        request.field("owner", owner);
        request.field("kind", kind);
        request.field("fields", fields);
        const std::optional<core::JsonDocument> answer = call(request);
        if (!answer.has_value() || !answer->root()["ok"].asBool(false))
            return std::nullopt;
        return whole(answer->root()["item"]);
    }

    [[nodiscard]] bool destroyItem(std::string_view world, core::i64 id) override
    {
        core::JsonWriter request = begin("destroy");
        request.field("world", world);
        request.field("item", id);
        const std::optional<core::JsonDocument> answer = call(request);
        return answer.has_value() && answer->root()["ok"].asBool(false);
    }

    [[nodiscard]] std::vector<DataItem> items(std::string_view owner) override
    {
        core::JsonWriter request = begin("items");
        request.field("owner", owner);
        std::vector<DataItem> out;
        const std::optional<core::JsonDocument> answer = call(request);
        if (!answer.has_value())
            return out;
        const core::JsonValue listed = answer->root()["items"];
        for (core::usize at = 0; at < listed.size(); ++at) {
            const core::JsonValue one = listed.at(at);
            out.push_back(DataItem{whole(one["id"]).value_or(0), std::string(one["kind"].asString()),
                                   std::string(owner), std::string(one["fields"].asString())});
        }
        return out;
    }

    [[nodiscard]] core::i64 balance(std::string_view owner, std::string_view currency) override
    {
        core::JsonWriter request = begin("balance");
        request.field("owner", owner);
        request.field("currency", currency);
        const std::optional<core::JsonDocument> answer = call(request);
        return answer.has_value() ? whole(answer->root()["balance"]).value_or(0) : 0;
    }

    [[nodiscard]] SharedDatum getShared(std::string_view scope, std::string_view key) override
    {
        core::JsonWriter request = begin("get");
        request.field("scope", scope);
        request.field("key", key);
        const std::optional<core::JsonDocument> answer = call(request);
        if (!answer.has_value())
            return {};
        return SharedDatum{std::string(answer->root()["value"].asString()),
                           whole(answer->root()["version"]).value_or(0)};
    }

    [[nodiscard]] SharedWrite setShared(std::string_view scope, std::string_view key, std::string_view value,
                                        core::i64 version) override
    {
        core::JsonWriter request = begin("set");
        request.field("scope", scope);
        request.field("key", key);
        request.field("value", value);
        request.field("version", version);
        const std::optional<core::JsonDocument> answer = call(request);
        if (!answer.has_value())
            return {};
        const core::JsonValue root = answer->root();
        return SharedWrite{root["ok"].asBool(false), SharedDatum{std::string(root["value"].asString()),
                                                                 whole(root["version"]).value_or(0)}};
    }

    // **The backend's own**: its log, its counts, its backups.
    [[nodiscard]] std::vector<DataMoveLine> moves(const DataMoveQuery&) override { return {}; }
    [[nodiscard]] DataTransaction undo(std::string_view, core::i64) override
    {
        return DataTransaction{false, "Invalid", 0};
    }
    core::i64 forgetMovesBefore(core::i64, core::i64) override { return 0; }
    [[nodiscard]] DataCounts counts() override { return {}; }
    [[nodiscard]] bool startBackup(const std::filesystem::path&, std::string& error) override
    {
        error = core::engineCatalog().format(ENG_TR("data.backend_keeps"));
        return false;
    }
    DataBackupProgress stepBackup(int) override { return {}; }

    // **Accounts are the manager's**, in its own database, whatever keeps
    // the game's data: this store is never asked for one.
    [[nodiscard]] std::optional<DataAccount> accountNamed(std::string_view) override { return std::nullopt; }
    [[nodiscard]] std::optional<DataAccount> accountWithId(std::string_view) override { return std::nullopt; }
    [[nodiscard]] bool createAccount(const DataAccount&) override { return false; }
    [[nodiscard]] bool renameAccount(std::string_view, std::string_view, std::string_view) override { return false; }
    [[nodiscard]] bool setAccountHash(std::string_view, std::string_view) override { return false; }
    void noteSignIn(std::string_view, core::i64) override {}
    [[nodiscard]] bool deleteAccount(std::string_view) override { return false; }

private:
    [[nodiscard]] core::JsonWriter begin(std::string_view op)
    {
        core::JsonWriter request(core::JsonLayout::Compact);
        request.beginObject();
        request.field("request", std::string_view{requestId()});
        request.field("op", op);
        return request;
    }

    // **Sent, and sent again with the same id** while tries and time remain;
    // the answer, or none -- and the store `unavailable` until the next call.
    [[nodiscard]] std::optional<core::JsonDocument> call(core::JsonWriter& request)
    {
        request.endObject();
        const std::string body = request.text();
        m_failed = false;
        const auto start = std::chrono::steady_clock::now();
        for (core::u32 attempt = 0; attempt < m_options.tries; ++attempt) {
            const auto spent = static_cast<core::u32>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                    .count());
            if (spent >= m_options.totalMs)
                break;
            const core::u32 timeout = std::min(m_options.tryMs, m_options.totalMs - spent);
            if (const std::optional<std::string> text = m_options.send(m_endpoint, body, m_options.token, timeout);
                text.has_value()) {
                core::JsonDocument answer;
                if (answer.parse(*text, "backend")) {
                    if (m_warned) {
                        m_warned = false;
                        const std::array<core::I18nArg, 1> args{core::I18nArg{"url", std::string_view{m_endpoint}}};
                        core::log(core::LogLevel::Info, ENG_TR("data.info.backend_answers"), args);
                    }
                    return answer;
                }
            }
            // A moment between tries, longer each time: a backend restarting
            // is given time to come back.
            if (attempt + 1 < m_options.tries)
                std::this_thread::sleep_for(std::chrono::milliseconds(250u << attempt));
        }
        m_failed = true;
        if (!m_warned) {
            m_warned = true;
            const std::array<core::I18nArg, 1> args{core::I18nArg{"url", std::string_view{m_endpoint}}};
            core::log(core::LogLevel::Error, ENG_TR("data.err.backend_unanswered"), args);
        }
        return std::nullopt;
    }

    BackendOptions m_options;
    std::string m_endpoint;
    bool m_failed = false;
    // Said once until it answers again, not once a request.
    bool m_warned = false;
};

} // namespace

bool backendUrlAllowed(std::string_view url, std::string& why)
{
    net::ParsedUrl parsed;
    if (net::parseUrl(url, parsed).has_value() || (parsed.scheme != "http" && parsed.scheme != "https")) {
        why = core::engineCatalog().format(ENG_TR("data.err.backend_url"));
        return false;
    }
    if (parsed.scheme == "http" && !nearHost(parsed.host)) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"host", std::string_view{parsed.host}}};
        why = core::engineCatalog().format(ENG_TR("data.err.backend_cleartext"), args);
        return false;
    }
    return true;
}

std::unique_ptr<IDataStore> makeBackendDataStore(BackendOptions options)
{
    return std::make_unique<BackendDataStore>(std::move(options));
}

namespace {

[[nodiscard]] std::string trimmed(std::string_view url)
{
    std::string out(url);
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    return out;
}

// A query string's value, escaped as RFC 3986 asks of one.
[[nodiscard]] std::string escaped(std::string_view text)
{
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
            byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            out.push_back(c);
            continue;
        }
        out.push_back('%');
        out.push_back(Hex[byte >> 4]);
        out.push_back(Hex[byte & 15]);
    }
    return out;
}

} // namespace

std::string nakamaDataEndpoint(std::string_view url, std::string_view httpKey)
{
    return trimmed(url) + "/v2/rpc/engine_data?unwrap&http_key=" + escaped(httpKey);
}

std::function<std::optional<std::string>(const std::string& session)> nakamaWhoami(std::string url)
{
    return [endpoint = trimmed(url) + "/v2/rpc/engine_whoami?unwrap"](
               const std::string& session) -> std::optional<std::string> {
        net::HttpRequest request;
        request.url = endpoint;
        request.method = "POST";
        request.body = "{}";
        request.timeoutMs = 5000;
        request.headers.push_back({"Content-Type", "application/json"});
        request.headers.push_back({"Authorization", "Bearer " + session});
        net::HttpResponse response;
        if (net::performHttp(request, response).has_value() || !response.ok || response.statusCode != 200)
            return std::nullopt;
        core::JsonDocument answer;
        if (!answer.parse(response.body, "nakama"))
            return std::nullopt;
        const std::string_view user = answer.root()["user_id"].asString();
        if (user.empty())
            return std::nullopt;
        return std::string(user);
    };
}

} // namespace engine::app
