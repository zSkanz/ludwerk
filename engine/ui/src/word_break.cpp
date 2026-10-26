#include "word_break.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>

#include "engine/platform/file.h"
#include "engine/platform/platform.h"

namespace engine::ui::detail {
namespace {

using core::u32;
using core::usize;

// One script with a word list: the block its letters are in, and the file
// the list is read from beside the binary.
struct Script
{
    char32_t first;
    char32_t last;
    const char* file;
};

constexpr std::array<Script, 2> Scripts{{
    {0x0E00, 0x0E7F, "thaidict.txt"},
    {0x0E80, 0x0EFF, "laodict.txt"},
}};

// A script's list: its words sorted, as codepoints, and the longest of them.
struct WordList
{
    bool loaded = false;
    std::vector<std::u32string> words;
    usize longest = 0;
};

struct Lists
{
    std::mutex lock;
    std::array<WordList, Scripts.size()> lists;
};

[[nodiscard]] Lists& lists()
{
    static Lists instance;
    return instance;
}

[[nodiscard]] usize scriptOf(char32_t c) noexcept
{
    for (usize at = 0; at < Scripts.size(); ++at) {
        if (c >= Scripts[at].first && c <= Scripts[at].last)
            return at;
    }
    return Scripts.size();
}

struct Decoded
{
    char32_t point = 0;
    usize size = 1;
};

[[nodiscard]] Decoded decode(std::string_view text, usize at) noexcept
{
    const auto lead = static_cast<unsigned char>(text[at]);
    const usize size = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    if (at + size > text.size())
        return Decoded{0xFFFD, 1};
    char32_t point = size == 1 ? lead : size == 2 ? (lead & 0x1Fu) : size == 3 ? (lead & 0x0Fu) : (lead & 0x07u);
    for (usize index = 1; index < size; ++index)
        point = (point << 6) | (static_cast<unsigned char>(text[at + index]) & 0x3Fu);
    return Decoded{point, size};
}

// A mark: it belongs to the letter before it, and no word starts with one.
[[nodiscard]] bool markOf(char32_t c) noexcept
{
    return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E3A) || (c >= 0x0E47 && c <= 0x0E4E) || c == 0x0EB1 ||
           (c >= 0x0EB4 && c <= 0x0EBC) || (c >= 0x0EC8 && c <= 0x0ECE);
}

// The list as ICU writes it: a word a line, after the copyright's comment
// lines, some with a tab and a frequency after the word.
void load(WordList& list, const Script& script)
{
    list.loaded = true;
    std::vector<std::byte> bytes;
    if (!platform::readFile(platform::paths().contentDir / "text" / script.file, bytes))
        return;
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    usize at = 0;
    if (text.starts_with("\xEF\xBB\xBF"))
        at = 3;
    while (at < text.size()) {
        usize end = text.find('\n', at);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        const usize stop = line.find_first_of("\t\r #");
        if (stop != std::string_view::npos)
            line = line.substr(0, stop);
        if (line.empty())
            continue;
        std::u32string word;
        for (usize index = 0; index < line.size();) {
            const Decoded decoded = decode(line, index);
            word.push_back(decoded.point);
            index += decoded.size;
        }
        list.longest = std::max(list.longest, word.size());
        list.words.push_back(std::move(word));
    }
    std::sort(list.words.begin(), list.words.end());
    list.words.erase(std::unique(list.words.begin(), list.words.end()), list.words.end());
}

// The best way to cut `run` into words: fewest letters in no word, then
// fewest pieces -- the longest words the list has, chosen over the whole run
// rather than greedily from its start, which cuts a word that begins with a
// shorter one in two. Letters in no word stay together as one piece. Fills
// `starts` with the index of each piece after the first.
void cut(const std::u32string& run, const WordList& list, std::vector<usize>& starts)
{
    const usize count = run.size();
    struct Cost
    {
        usize unknown = std::numeric_limits<usize>::max();
        usize pieces = 0;
        usize from = 0;
        bool word = false;
    };
    std::vector<Cost> best(count + 1);
    best[0] = Cost{0, 0, 0, true};
    std::u32string probe;
    for (usize at = 0; at < count; ++at) {
        if (best[at].unknown == std::numeric_limits<usize>::max() || markOf(run[at]))
            continue;
        const auto offer = [&](usize end, usize unknown, usize pieces, bool word) {
            Cost& held = best[end];
            if (unknown < held.unknown || (unknown == held.unknown && pieces < held.pieces))
                held = Cost{unknown, pieces, at, word};
        };
        const usize reach = std::min(count, at + list.longest);
        probe.clear();
        for (usize end = at + 1; end <= reach; ++end) {
            probe.push_back(run[end - 1]);
            // A word never ends before a mark that belongs to its last letter.
            if (end < count && markOf(run[end]))
                continue;
            if (std::binary_search(list.words.begin(), list.words.end(), probe))
                offer(end, best[at].unknown, best[at].pieces + 1, true);
        }
        // A letter in no word, with its marks.
        usize next = at + 1;
        while (next < count && markOf(run[next]))
            ++next;
        offer(next, best[at].unknown + (next - at), best[at].pieces + 1, false);
    }
    // Back from the end: the pieces, and a place to break before each but
    // the first -- except between two pieces in no word, which stay one.
    std::vector<std::pair<usize, bool>> pieces;
    for (usize at = count; at > 0; at = best[at].from)
        pieces.emplace_back(best[at].from, best[at].word);
    std::reverse(pieces.begin(), pieces.end());
    for (usize index = 1; index < pieces.size(); ++index) {
        if (!pieces[index].second && !pieces[index - 1].second)
            continue;
        starts.push_back(pieces[index].first);
    }
}

} // namespace

bool dictionaryScript(u32 c) noexcept
{
    return scriptOf(static_cast<char32_t>(c)) < Scripts.size();
}

void dictionaryBreaks(std::string_view text, std::vector<usize>& out)
{
    Lists& held = lists();
    const std::lock_guard guard(held.lock);
    std::u32string run;
    std::vector<usize> offsets;
    usize script = Scripts.size();
    const auto finish = [&] {
        if (run.size() > 1 && script < Scripts.size()) {
            WordList& list = held.lists[script];
            if (!list.loaded)
                load(list, Scripts[script]);
            if (!list.words.empty()) {
                std::vector<usize> starts;
                cut(run, list, starts);
                for (const usize start : starts)
                    out.push_back(offsets[start]);
            }
        }
        run.clear();
        offsets.clear();
        script = Scripts.size();
    };
    for (usize at = 0; at < text.size();) {
        const Decoded decoded = decode(text, at);
        const usize of = scriptOf(decoded.point);
        if (of != script)
            finish();
        if (of < Scripts.size()) {
            script = of;
            run.push_back(decoded.point);
            offsets.push_back(at);
        }
        at += decoded.size;
    }
    finish();
}

void setWordListForTests(char32_t letter, std::vector<std::u32string> words)
{
    Lists& held = lists();
    const std::lock_guard guard(held.lock);
    const usize script = scriptOf(letter);
    if (script >= Scripts.size())
        return;
    WordList& list = held.lists[script];
    list = WordList{};
    if (words.empty())
        return;
    list.loaded = true;
    for (const std::u32string& word : words)
        list.longest = std::max(list.longest, word.size());
    list.words = std::move(words);
    std::sort(list.words.begin(), list.words.end());
}

} // namespace engine::ui::detail
