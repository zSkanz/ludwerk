// A file the engine is reading can still be saved by the program that makes
// it. Windows refuses a writer whatever the reader did not agree to
// share when it opened the file, and the platform library agreed to share
// reading only: for the length of a read, a sound or a model tool saving the
// file the engine had just been told about got "file in use" from its own
// save.
#include <doctest/doctest.h>

#ifdef _WIN32

#include <SDL3/SDL_asyncio.h>
#include <SDL3/SDL_iostream.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <windows.h>

namespace {

[[nodiscard]] std::string utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

void write(const std::filesystem::path& path, const char* text)
{
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    file << text;
    file.close();
    REQUIRE(file.good());
}

[[nodiscard]] std::string read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

struct Folder
{
    std::filesystem::path root;
    explicit Folder(const char* name)
    {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec) / name;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }
    ~Folder()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
};

} // namespace

TEST_CASE("a file open for a whole read is written over, replaced and deleted by another program")
{
    // As `platform::readFile` has it while it reads: `SDL_LoadFile` opens the
    // file this way.
    const Folder folder("engine-file-share-read");
    const std::filesystem::path file = folder.root / "line.wav";
    write(file, "the first recording");
    SDL_IOStream* const reading = SDL_IOFromFile(utf8(file).c_str(), "rb");
    REQUIRE(reading != nullptr);

    // A tool that saves in place.
    {
        std::ofstream saving(file, std::ios::binary);
        CHECK(saving.is_open());
        saving << "the second";
    }
    // A tool that saves beside the file and puts the new one in its place, by
    // the call Windows has for that. (A plain rename over a file somebody has
    // open is refused by Windows whatever the reader shares -- error 5 -- and
    // no reader can change that; a tool that saves that way deletes first.)
    write(folder.root / "line.wav.tmp", "the third recording");
    CHECK(ReplaceFileW(file.c_str(), (folder.root / "line.wav.tmp").c_str(), nullptr, 0, nullptr, nullptr) != 0);
    CHECK(read(file) == "the third recording");
    // And a tool that deletes it.
    CHECK(DeleteFileW(file.c_str()) != 0);

    CHECK(SDL_CloseIO(reading));
}

TEST_CASE("a file open for an asynchronous read is written over by another program")
{
    // As `platform::readFileAsync` has it: `SDL_LoadFileAsync` opens the file
    // this way, and it is the read a frame's prefetch starts.
    const Folder folder("engine-file-share-async");
    const std::filesystem::path file = folder.root / "line.wav";
    write(file, "the first recording");
    SDL_AsyncIOQueue* const queue = SDL_CreateAsyncIOQueue();
    REQUIRE(queue != nullptr);
    SDL_AsyncIO* const reading = SDL_AsyncIOFromFile(utf8(file).c_str(), "r");
    REQUIRE(reading != nullptr);

    {
        std::ofstream saving(file, std::ios::binary);
        CHECK(saving.is_open());
        saving << "the second";
    }

    REQUIRE(SDL_CloseAsyncIO(reading, false, queue, nullptr));
    SDL_AsyncIOOutcome outcome{};
    CHECK(SDL_WaitAsyncIOResult(queue, &outcome, 5000));
    SDL_DestroyAsyncIOQueue(queue);
    CHECK(read(file) == "the second");
}

#endif
