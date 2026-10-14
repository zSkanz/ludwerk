// The archives an export ends in (ADR 0104 §7): a `.zip` beside a Windows
// folder, a `.tar.gz` beside a Linux one.
//
// **Written here, from any desktop, with no tool on the machine.** A `.tar.gz`
// made on Windows has to carry the execute bit a Linux player needs -- which no
// Windows archiver writes, because the file system it reads has none -- so the
// mode is the caller's to say, per entry. Deflate is `stb_image_write`'s, the
// one already compiled for PNGs, and the containers are the page of code each
// format is.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::asset {

struct ArchiveEntry
{
    // In the archive, '/' separators, no leading slash.
    std::string path;
    std::vector<std::byte> bytes;
    // `0755` rather than `0644`: a Linux player, a launcher script.
    bool executable = false;
};

// Every file under `root`, as entries under `prefix/`, in path order (the same
// archive for the same tree, R10). `executables` names, relative to `root`, the
// files that get the execute bit.
[[nodiscard]] std::optional<core::EngineError> collectArchiveEntries(const std::filesystem::path& root,
                                                                     const std::string& prefix,
                                                                     const std::vector<std::string>& executables,
                                                                     std::vector<ArchiveEntry>& out);

[[nodiscard]] std::optional<core::EngineError> writeZip(const std::vector<ArchiveEntry>& entries,
                                                        const std::filesystem::path& path);

[[nodiscard]] std::optional<core::EngineError> writeTarGz(const std::vector<ArchiveEntry>& entries,
                                                          const std::filesystem::path& path);

// CRC-32 (IEEE), which both formats carry.
[[nodiscard]] core::u32 crc32(const std::byte* data, std::size_t size) noexcept;

} // namespace engine::asset
