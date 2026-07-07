#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <ios>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace neo2da {

bool isUnsignedDecimal(std::string_view s);
std::optional<std::uint32_t> parseUInt32Decimal(std::string_view s);
std::string lowerAscii(std::string s);
std::string trimAscii(std::string_view text);
void writeFileAtomically(const std::filesystem::path& filename,
                         std::ios::openmode mode,
                         const std::function<void(std::ostream&)>& writer);

} // namespace neo2da
