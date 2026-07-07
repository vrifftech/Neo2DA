#include "core/Common.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neo2da {

bool isUnsignedDecimal(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
}

std::optional<std::uint32_t> parseUInt32Decimal(std::string_view s) {
    if (!isUnsignedDecimal(s)) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, 10);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return s;
}

std::string trimAscii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

namespace {

std::filesystem::path uniqueSiblingPath(const std::filesystem::path& target, const std::string& infix) {
    const auto parent = target.parent_path();
    const std::string base = target.filename().empty() ? std::string("neo2da-output") : target.filename().string();
    const auto tick = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        auto candidate = parent / (base + ".neo2da-" + infix + "-" +
                                   std::to_string(static_cast<long long>(tick)) + "-" + std::to_string(attempt));
        std::error_code ec;
        const auto st = std::filesystem::symlink_status(candidate, ec);
        if (st.type() == std::filesystem::file_type::not_found) {
            return candidate;
        }
    }
    throw std::runtime_error("Unable to allocate temporary filename near " + target.string());
}

void removeNoThrow(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

bool existsNoThrow(const std::filesystem::path& path) noexcept {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

std::filesystem::path resolveSymlinkWriteTarget(const std::filesystem::path& filename) {
    std::vector<std::filesystem::path> seen;
    std::filesystem::path current = filename.lexically_normal();

    for (int depth = 0; depth < 32; ++depth) {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(current, ec);
        if (ec || !std::filesystem::is_symlink(status)) {
            return depth == 0 ? std::filesystem::path{} : current;
        }

        const auto normalizedCurrent = current.lexically_normal();
        if (std::find(seen.begin(), seen.end(), normalizedCurrent) != seen.end()) {
            throw std::runtime_error("Refusing to write through a cyclic symbolic link: " + filename.string());
        }
        seen.push_back(normalizedCurrent);

        auto target = std::filesystem::read_symlink(current, ec);
        if (ec) {
            throw std::runtime_error("Unable to read symbolic link target: " + current.string());
        }
        if (!target.is_absolute()) {
            target = current.parent_path() / target;
        }
        current = target.lexically_normal();
    }

    throw std::runtime_error("Refusing to write through an excessively deep symbolic-link chain: " + filename.string());
}

void restorePermissionsNoThrow(const std::filesystem::path& path, std::filesystem::perms perms) noexcept {
    std::error_code ec;
    std::filesystem::permissions(path, perms, std::filesystem::perm_options::replace, ec);
}

void makeFileWritable(const std::filesystem::path& filename) {
    std::error_code ec;
    if (!std::filesystem::exists(filename, ec)) {
        return;
    }
    auto perms = std::filesystem::status(filename, ec).permissions();
    if (ec) {
        return;
    }
    if ((perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none) {
        std::filesystem::permissions(filename,
                                     std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add,
                                     ec);
    }
}

void replacePathAtomically(const std::filesystem::path& temp, const std::filesystem::path& target) {
#if defined(_WIN32)
    if (!MoveFileExW(temp.native().c_str(),
                     target.native().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Unable to replace file atomically: " + target.string());
    }
#else
    std::filesystem::rename(temp, target);
#endif
}

} // namespace

void writeFileAtomically(const std::filesystem::path& filename,
                         std::ios::openmode mode,
                         const std::function<void(std::ostream&)>& writer) {
    if (filename.empty()) {
        throw std::runtime_error("Unable to write file: empty path");
    }

    const auto linkedTarget = resolveSymlinkWriteTarget(filename);
    if (!linkedTarget.empty()) {
        writeFileAtomically(linkedTarget, mode, writer);
        return;
    }

    std::filesystem::perms originalPerms = std::filesystem::perms::unknown;
    {
        std::error_code typeEc;
        const auto st = std::filesystem::status(filename, typeEc);
        if (!typeEc && std::filesystem::exists(st)) {
            originalPerms = st.permissions();
            if (!std::filesystem::is_regular_file(st)) {
                throw std::runtime_error("Refusing to overwrite non-regular file: " + filename.string());
            }
        }
    }

    const auto temp = uniqueSiblingPath(filename, "tmp");
    try {
        {
            std::ofstream out(temp, mode | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("Unable to create temporary output file: " + temp.string());
            }
            writer(out);
            out.flush();
            if (!out) {
                throw std::runtime_error("Unable to flush output file: " + temp.string());
            }
        }

        makeFileWritable(filename);
        replacePathAtomically(temp, filename);
        if (originalPerms != std::filesystem::perms::unknown && existsNoThrow(filename)) {
            restorePermissionsNoThrow(filename, originalPerms);
        }
    } catch (...) {
        removeNoThrow(temp);
        throw;
    }
}

} // namespace neo2da
