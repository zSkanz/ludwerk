#include "engine/app/account_door.h"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

#include "engine/core/crypto.h"
#include "engine/core/i18n.h"
#include "engine/core/ticket.h"

namespace engine::app {
namespace {

constexpr core::u64 NsPerSecond = 1'000'000'000ull;
// How long an address's failures count, and a name's.
constexpr core::u64 AddressWindowNs = 60ull * NsPerSecond;
constexpr core::u64 NameWindowNs = 15ull * 60ull * NsPerSecond;
// A password the console makes: letters a player can read out and type,
// none that look alike.
constexpr std::string_view PasswordLetters = "abcdefghjkmnpqrstuvwxyz23456789";
constexpr core::usize MadePasswordLength = 16;

[[nodiscard]] std::string phrase(core::TextKey key, std::span<const core::I18nArg> args = {})
{
    return core::engineCatalog().format(key, args);
}

[[nodiscard]] std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::pair<std::string_view, std::string_view> split(std::string_view text)
{
    text = trim(text);
    const std::size_t space = text.find(' ');
    if (space == std::string_view::npos)
        return {text, {}};
    return {text.substr(0, space), trim(text.substr(space + 1))};
}

[[nodiscard]] std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

[[nodiscard]] std::string randomId()
{
    std::array<core::u8, 16> bytes{};
    core::secureRandom(bytes);
    return core::toHex(bytes);
}

[[nodiscard]] std::string madePassword()
{
    std::array<core::u8, MadePasswordLength> bytes{};
    core::secureRandom(bytes);
    std::string out;
    for (const core::u8 byte : bytes)
        out.push_back(PasswordLetters[byte % PasswordLetters.size()]);
    return out;
}

} // namespace

AccountDoor::AccountDoor(IDataStore& store, Options options) : m_store(store), m_options(std::move(options))
{
    if (!m_options.namePattern.empty()) {
        try {
            m_pattern.emplace(m_options.namePattern, std::regex::ECMAScript);
        } catch (const std::regex_error&) {
            // `readServersConfig` refused it already; a pattern that does not
            // compile here narrows nothing.
            m_pattern.reset();
        }
    }
    std::array<core::u8, 32> nothing{};
    core::secureRandom(nothing);
    m_decoy = core::hashPassword(core::toHex(nothing));
    unsigned threads = m_options.threads;
    if (threads == 0)
        threads = std::clamp(std::thread::hardware_concurrency() / 2u, 1u, 4u);
    for (unsigned at = 0; at < threads; ++at)
        m_threads.emplace_back([this]() { work(); });
}

AccountDoor::~AccountDoor()
{
    {
        const std::lock_guard guard(m_lock);
        m_stopping = true;
    }
    m_wake.notify_all();
    for (std::thread& thread : m_threads)
        thread.join();
    for (Job& job : m_jobs)
        core::wipe(job.password);
    for (Asked& asked : m_delayed)
        core::wipe(asked.job.password);
    for (auto& [id, asked] : m_inPool)
        core::wipe(asked.job.password);
}

core::u64 AccountDoor::nowNs() const
{
    return m_options.clock ? m_options.clock() : 0;
}

std::optional<std::string> AccountDoor::skeletonOf(std::string_view name)
{
    if (name.size() < 3 || name.size() > 20)
        return std::nullopt;
    std::string skeleton;
    skeleton.reserve(name.size());
    for (const char c : name) {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && c != '_' && c != '.' && c != '-')
            return std::nullopt;
        char folded = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (folded == 'i' || folded == '1')
            folded = 'l';
        else if (folded == '0')
            folded = 'o';
        skeleton.push_back(folded);
    }
    return skeleton;
}

bool AccountDoor::nameAllowed(std::string_view name) const
{
    if (!skeletonOf(name).has_value())
        return false;
    return !m_pattern.has_value() || std::regex_match(name.begin(), name.end(), *m_pattern);
}

std::string AccountDoor::ticketFor(const DataAccount& account) const
{
    const std::int64_t now = m_options.unixClock ? m_options.unixClock() : 0;
    return core::signTicket(m_options.secret, account.id, m_options.audience, now, TicketSeconds, core::uniqueId());
}

void AccountDoor::ask(core::u32 peer, std::string_view address, std::string_view name, std::string password,
                      bool registering)
{
    ask(peer, address, name, std::move(password), registering ? Kind::Register : Kind::SignIn);
}

void AccountDoor::ask(core::u32 peer, std::string_view address, std::string_view name, std::string password, Kind kind)
{
    const bool registering = kind == Kind::Register;
    const core::u64 now = nowNs();
    const auto refuse = [&](Failure failure, core::u32 retryAfter) {
        core::wipe(password);
        answer(Result{peer, failure, {}, {}, retryAfter}, 0, {});
    };
    // **Five failures from an address in a minute**: the sixth is refused
    // without a hash (§2).
    std::deque<core::u64>& recent = m_addressFailures[std::string(address)];
    while (!recent.empty() && recent.front() + AddressWindowNs <= now)
        recent.pop_front();
    if (recent.size() >= AddressFailures) {
        const core::u64 freeAt = recent.front() + AddressWindowNs;
        refuse(Failure::Refused, static_cast<core::u32>((freeAt - now + NsPerSecond - 1) / NsPerSecond));
        return;
    }
    if (registering && !m_options.registration) {
        refuse(Failure::Refused, 0);
        return;
    }
    // **A session of the game's Nakama** (ADR 0242): no name, no hash --
    // Nakama is asked whose it is, on the pool, under the address's limit;
    // and for a match, whether Nakama put that player in it.
    if (kind == Kind::Session || kind == Kind::Match) {
        if (!m_options.whoami || (kind == Kind::Match && (!m_options.matchUsers || name.empty()))) {
            refuse(Failure::Refused, 0);
            return;
        }
        // No session is no account: the same answer as a wrong one.
        if (password.empty()) {
            refuse(Failure::WrongNameOrPassword, 0);
            return;
        }
        if (m_delayed.size() + m_inPool.size() >= m_options.most) {
            refuse(Failure::Busy, 1);
            return;
        }
        Asked asked;
        asked.peer = peer;
        asked.address = std::string(address);
        asked.session = true;
        asked.job.id = m_nextJob++;
        asked.job.session = true;
        asked.job.password = std::move(password);
        if (kind == Kind::Match) {
            asked.match = std::string(name);
            asked.job.match = std::string(name);
        }
        m_delayed.push_back(std::move(asked));
        return;
    }
    const std::optional<std::string> skeleton = skeletonOf(name);
    if (!skeleton.has_value() || !nameAllowed(name)) {
        refuse(Failure::InvalidName, 0);
        return;
    }
    if (registering && (password.size() < ShortestPassword || password.size() > LongestPassword)) {
        refuse(Failure::InvalidPassword, 0);
        return;
    }
    if (m_delayed.size() + m_inPool.size() >= m_options.most) {
        refuse(Failure::Busy, 1);
        return;
    }

    Asked asked;
    asked.peer = peer;
    asked.address = std::string(address);
    asked.name = std::string(name);
    asked.skeleton = *skeleton;
    asked.registering = registering;
    asked.job.id = m_nextJob++;
    asked.job.hashing = registering;
    asked.job.password = std::move(password);
    if (!registering) {
        // **Found whatever its case**, and only so: a name that merely looks
        // like one is not the account's.
        asked.account = m_store.accountNamed(*skeleton);
        if (asked.account.has_value() && lower(asked.account->name) != lower(name))
            asked.account.reset();
        asked.job.hash = asked.account.has_value() ? asked.account->hash : m_decoy;
    }
    // **A name tried too often waits** (amendment G): by its skeleton,
    // whether an account has it or not.
    if (const auto found = m_nameFailures.find(*skeleton); found != m_nameFailures.end()) {
        if (found->second.lastNs + NameWindowNs <= now) {
            m_nameFailures.erase(found);
        }
        else if (found->second.count >= NameFailures) {
            const core::u32 past = std::min<core::u32>(found->second.count - NameFailures, 5);
            const core::u64 delay = std::min<core::u64>(1ull << past, MostDelaySeconds);
            asked.notBeforeNs = now + delay * NsPerSecond;
        }
    }
    m_delayed.push_back(std::move(asked));
}

void AccountDoor::frame()
{
    const core::u64 now = nowNs();
    // **The counts forget**: an address or a name past its window holds no
    // memory, so many addresses tried once cost nothing a minute later.
    if (m_addressFailures.size() + m_nameFailures.size() > 1024) {
        std::erase_if(m_addressFailures, [now](const auto& entry) {
            return entry.second.empty() || entry.second.back() + AddressWindowNs <= now;
        });
        std::erase_if(m_nameFailures, [now](const auto& entry) { return entry.second.lastNs + NameWindowNs <= now; });
    }
    {
        const std::lock_guard guard(m_lock);
        for (auto at = m_delayed.begin(); at != m_delayed.end();) {
            if (at->notBeforeNs > now) {
                ++at;
                continue;
            }
            m_jobs.push_back(std::move(at->job));
            const core::u64 id = m_jobs.back().id;
            m_inPool.emplace(id, std::move(*at));
            at = m_delayed.erase(at);
        }
    }
    m_wake.notify_all();
    std::vector<Done> done;
    {
        const std::lock_guard guard(m_lock);
        done.swap(m_done);
    }
    for (const Done& finished : done) {
        const auto asked = m_inPool.find(finished.id);
        if (asked == m_inPool.end())
            continue;
        finish(asked->second, finished);
        m_inPool.erase(asked);
    }
}

void AccountDoor::failed(const Asked& asked)
{
    const core::u64 now = nowNs();
    m_addressFailures[asked.address].push_back(now);
    Failures& name = m_nameFailures[asked.skeleton];
    if (name.lastNs + NameWindowNs <= now)
        name.count = 0;
    name.count += 1;
    name.lastNs = now;
}

void AccountDoor::finish(Asked& asked, const Done& done)
{
    const std::int64_t unixNow = m_options.unixClock ? m_options.unixClock() : 0;
    if (asked.session) {
        if (!done.ok) {
            // A session Nakama does not know counts against its address.
            m_addressFailures[asked.address].push_back(nowNs());
            answer(Result{asked.peer, Failure::WrongNameOrPassword, {}, {}, 0}, done.hashes, done.cost);
            return;
        }
        DataAccount account;
        account.id = "nakama:" + done.user;
        if (!asked.match.empty()) {
            // **In the match Nakama made**: no ticket -- the player is in a
            // world already -- and the match's players, by their accounts.
            MatchFound found;
            found.match = asked.match;
            found.queue = done.queue;
            for (const std::string& user : done.users)
                found.accounts.push_back("nakama:" + user);
            m_matched.push_back(std::move(found));
            answer(Result{asked.peer, Failure::None, {}, account.id, 0}, done.hashes, done.cost);
            return;
        }
        answer(Result{asked.peer, Failure::None, ticketFor(account), account.id, 0}, done.hashes, done.cost);
        return;
    }
    if (asked.registering) {
        if (!done.ok) {
            answer(Result{asked.peer, Failure::Busy, {}, {}, 1}, done.hashes, done.cost);
            return;
        }
        const DataAccount account{randomId(), asked.name, asked.skeleton, done.hash, unixNow, unixNow};
        if (!m_store.createAccount(account)) {
            answer(Result{asked.peer, Failure::Taken, {}, {}, 0}, done.hashes, done.cost);
            return;
        }
        answer(Result{asked.peer, Failure::None, ticketFor(account), account.id, 0}, done.hashes, done.cost);
        return;
    }
    if (!done.ok || !asked.account.has_value()) {
        failed(asked);
        answer(Result{asked.peer, Failure::WrongNameOrPassword, {}, {}, 0}, done.hashes, done.cost);
        return;
    }
    m_nameFailures.erase(asked.skeleton);
    m_store.noteSignIn(asked.account->id, unixNow);
    answer(Result{asked.peer, Failure::None, ticketFor(*asked.account), asked.account->id, 0}, done.hashes, done.cost);
}

void AccountDoor::answer(Result result, core::u32 hashes, std::string cost)
{
    if (m_options.keepWork)
        m_work.push_back(Work{result.peer, result.failure, hashes, std::move(cost)});
    m_results.push_back(std::move(result));
}

std::vector<AccountDoor::Work> AccountDoor::takeWork()
{
    std::vector<Work> out;
    out.swap(m_work);
    return out;
}

std::vector<AccountDoor::MatchFound> AccountDoor::takeMatches()
{
    std::vector<MatchFound> out;
    out.swap(m_matched);
    return out;
}

std::vector<AccountDoor::Result> AccountDoor::take()
{
    std::vector<Result> out;
    out.swap(m_results);
    return out;
}

bool AccountDoor::idle() const
{
    return m_delayed.empty() && m_inPool.empty();
}

void AccountDoor::work()
{
    for (;;) {
        Job job;
        {
            std::unique_lock guard(m_lock);
            m_wake.wait(guard, [this]() { return m_stopping || !m_jobs.empty(); });
            if (m_stopping)
                return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        Done done;
        done.id = job.id;
        // **Counted where it is made**: a hash checked that no Argon2 ran
        // for -- an empty decoy, a stored hash not one of ours, a session of Nakama's -- costs
        // nothing and says so.
        if (job.session) {
            const std::optional<std::string> user = m_options.whoami(job.password);
            done.ok = user.has_value() && !user->empty();
            if (done.ok)
                done.user = *user;
            // **Believed of Nakama, never of the machine**: the match as
            // Nakama kept it, and this player in it.
            if (done.ok && !job.match.empty()) {
                const std::optional<Options::NakamaMatch> match = m_options.matchUsers(job.match);
                done.ok = match.has_value() &&
                          std::find(match->users.begin(), match->users.end(), done.user) != match->users.end();
                if (done.ok) {
                    done.users = match->users;
                    done.queue = match->queue;
                }
            }
        }
        else {
            {
                const std::lock_guard guard(m_lock);
                m_hashing += 1;
                m_mostHashing = std::max(m_mostHashing, m_hashing);
            }
            if (job.hashing) {
                done.hash = core::hashPassword(job.password);
                done.ok = !done.hash.empty();
                done.cost = core::passwordCost(done.hash);
            }
            else {
                done.ok = core::verifyPassword(job.hash, job.password);
                done.cost = core::passwordCost(job.hash);
            }
            const std::lock_guard guard(m_lock);
            m_hashing -= 1;
        }
        done.hashes = done.cost.empty() ? 0 : 1;
        core::wipe(job.password);
        const std::lock_guard guard(m_lock);
        m_done.push_back(std::move(done));
    }
}

core::u32 AccountDoor::mostAtOnce() const
{
    const std::lock_guard guard(m_lock);
    return m_mostHashing;
}

std::string AccountDoor::run(std::string_view line)
{
    const auto [verb, rest] = split(line);
    (void)verb;
    const auto [action, arguments] = split(rest);
    const auto [name, other] = split(arguments);
    const auto named = [this](std::string_view who) -> std::optional<DataAccount> {
        const std::optional<std::string> skeleton = skeletonOf(who);
        if (!skeleton.has_value())
            return std::nullopt;
        std::optional<DataAccount> account = m_store.accountNamed(*skeleton);
        if (account.has_value() && lower(account->name) != lower(who))
            return std::nullopt;
        return account;
    };
    const std::array<core::I18nArg, 1> who{core::I18nArg{"name", name}};
    if (name.empty())
        return phrase(ENG_TR("account.usage"));
    // **The console makes passwords, and never takes one**: a line an
    // operator types is logged with who typed it, and a password must be in
    // no log. The one it makes is shown once, in the answer.
    if (action == "create" && other.empty()) {
        const std::optional<std::string> skeleton = skeletonOf(name);
        if (!skeleton.has_value() || !nameAllowed(name))
            return phrase(ENG_TR("account.err.invalid_name"), who);
        std::string password = madePassword();
        const std::string hash = core::hashPassword(password);
        const std::int64_t now = m_options.unixClock ? m_options.unixClock() : 0;
        const DataAccount account{randomId(), std::string(name), *skeleton, hash, now, 0};
        if (hash.empty() || !m_store.createAccount(account)) {
            core::wipe(password);
            return phrase(ENG_TR("account.err.taken"), who);
        }
        const std::array<core::I18nArg, 3> args{core::I18nArg{"name", name},
                                                core::I18nArg{"id", std::string_view{account.id}},
                                                core::I18nArg{"password", std::string_view{password}}};
        std::string text = phrase(ENG_TR("account.done.created"), args);
        core::wipe(password);
        return text;
    }
    const std::optional<DataAccount> account = named(name);
    if (!account.has_value() && (action == "rename" || action == "password" || action == "delete"))
        return phrase(ENG_TR("account.err.no_such"), who);
    if (action == "rename" && !other.empty()) {
        const std::optional<std::string> skeleton = skeletonOf(other);
        const std::array<core::I18nArg, 2> args{core::I18nArg{"name", name}, core::I18nArg{"new", other}};
        if (!skeleton.has_value() || !nameAllowed(other))
            return phrase(ENG_TR("account.err.invalid_name"), std::span<const core::I18nArg>(&args[1], 1));
        if (!m_store.renameAccount(account->id, other, *skeleton))
            return phrase(ENG_TR("account.err.taken"), std::span<const core::I18nArg>(&args[1], 1));
        return phrase(ENG_TR("account.done.renamed"), args);
    }
    if (action == "password" && other.empty()) {
        std::string password = madePassword();
        const std::string hash = core::hashPassword(password);
        if (hash.empty() || !m_store.setAccountHash(account->id, hash)) {
            core::wipe(password);
            return phrase(ENG_TR("account.err.no_such"), who);
        }
        const std::array<core::I18nArg, 2> args{core::I18nArg{"name", name},
                                                core::I18nArg{"password", std::string_view{password}}};
        std::string text = phrase(ENG_TR("account.done.password"), args);
        core::wipe(password);
        return text;
    }
    if (action == "delete" && other.empty()) {
        if (!m_store.deleteAccount(account->id))
            return phrase(ENG_TR("account.err.no_such"), who);
        const std::array<core::I18nArg, 2> args{core::I18nArg{"name", name},
                                                core::I18nArg{"id", std::string_view{account->id}}};
        return phrase(ENG_TR("account.done.deleted"), args);
    }
    return phrase(ENG_TR("account.usage"));
}

} // namespace engine::app
