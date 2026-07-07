#include "core/TwoDAFile.hpp"

#include "core/BinaryIO.hpp"
#include "core/Common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace neo2da {
namespace {
constexpr std::size_t kBufferSize = 4096;
constexpr std::uint32_t kMaxSaneRows = 9999;

std::string defaultColumnLabel(std::size_t zeroBasedColumn) {
    return "Column" + std::to_string(zeroBasedColumn + 1);
}

std::string defaultRowLabel(std::size_t zeroBasedRow) {
    return std::to_string(zeroBasedRow);
}

std::vector<std::string> tokenizeText2DALine(const std::string& line) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        if (line[i] == '#') {
            break;
        }
        std::string token;
        if (line[i] == '"') {
            ++i;
            bool closed = false;
            while (i < line.size()) {
                char ch = line[i++];
                if (ch == '"') {
                    if (i < line.size() && line[i] == '"') {
                        token.push_back('"');
                        ++i;
                        continue;
                    }
                    closed = true;
                    break;
                }
                token.push_back(ch);
            }
            if (!closed) {
                throw TwoDAError("Malformed quoted token encountered while reading text 2DA.", 3);
            }
        } else {
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
                if (line[i] == '#') {
                    break;
                }
                token.push_back(line[i++]);
            }
        }
        tokens.push_back(std::move(token));
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i < line.size() && line[i] == '#') {
            break;
        }
    }
    return tokens;
}


std::string trimText2DACell(const std::string& cell) {
    std::size_t first = 0;
    while (first < cell.size() && (cell[first] == ' ' || cell[first] == '\f' || cell[first] == '\v')) {
        ++first;
    }
    std::size_t last = cell.size();
    while (last > first && (cell[last - 1] == ' ' || cell[last - 1] == '\f' || cell[last - 1] == '\v')) {
        --last;
    }
    return cell.substr(first, last - first);
}

std::vector<std::string> tokenizeText2DALineForImport(const std::string& line) {
    const auto firstNonSpace = line.find_first_not_of(" \t\f\v");
    if (firstNonSpace == std::string::npos || line[firstNonSpace] == '#') {
        return {};
    }
    if (line.find('\t') == std::string::npos) {
        return tokenizeText2DALine(line);
    }

    std::vector<std::string> cells;
    std::string current;
    bool inQuote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (inQuote && i + 1 < line.size() && line[i + 1] == '"') {
                current.push_back('"');
                ++i;
                continue;
            }
            inQuote = !inQuote;
            continue;
        }
        if (ch == '\t' && !inQuote) {
            cells.push_back(trimText2DACell(current));
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (inQuote) {
        throw TwoDAError("Malformed quoted token encountered while reading text 2DA.", 3);
    }
    cells.push_back(trimText2DACell(current));

    const bool anyValue = std::any_of(cells.begin(), cells.end(), [](const std::string& cell) {
        return !cell.empty();
    });
    if (!anyValue) {
        return {};
    }
    return cells;
}

bool isText2DAHeaderLine(const std::string& line) {
    const auto tokens = tokenizeText2DALine(line);
    return tokens.size() >= 2 &&
           (tokens[0] == "2DA" || tokens[0] == "c2DA") &&
           tokens[1] == "V2.0";
}

void mergeLeadingOverflowCells(std::vector<std::string>& cells, std::size_t targetCount) {
    if (targetCount == 0 || cells.size() <= targetCount) {
        return;
    }
    const std::size_t mergeCount = cells.size() - targetCount + 1;
    std::vector<std::string> fixed;
    fixed.reserve(targetCount);
    std::string merged;
    for (std::size_t i = 0; i < mergeCount; ++i) {
        if (i != 0) {
            merged.push_back(' ');
        }
        merged += cells[i];
    }
    fixed.push_back(std::move(merged));
    for (std::size_t i = mergeCount; i < cells.size(); ++i) {
        fixed.push_back(std::move(cells[i]));
    }
    cells = std::move(fixed);
}

std::vector<std::string> splitText2DALines(std::string text) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            lines.push_back(std::move(current));
            current.clear();
        } else if (ch == '\n') {
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    lines.push_back(std::move(current));
    return lines;
}

void validateTokenSize(const std::string& text, const std::string& label) {
    if (text.size() > kBufferSize - 1) {
        throw TwoDAError(label + " exceeds the 4095-byte 2DA token limit.");
    }
}

void writeBounded2DAString(std::ostream& out, const std::string& text, const std::string& label) {
    validateTokenSize(text, label);
    detail::writeBytes(out, text.data(), text.size());
}

void writeText2DACell(std::ostream& out, const std::string& text) {
    const bool quote = text.find_first_of("\t\r\n\"") != std::string::npos;
    if (!quote) {
        detail::writeBytes(out, text.data(), text.size());
        return;
    }
    char ch = '"';
    detail::writeBytes(out, &ch, 1);
    for (char value : text) {
        if (value == '"') {
            detail::writeBytes(out, &value, 1);
            detail::writeBytes(out, &value, 1);
        } else if (value == '\r' || value == '\n') {
            ch = ' ';
            detail::writeBytes(out, &ch, 1);
        } else {
            detail::writeBytes(out, &value, 1);
        }
    }
    ch = '"';
    detail::writeBytes(out, &ch, 1);
}

std::uint64_t checkedDataOffset(std::uint64_t dataOffset, std::uint16_t relative, std::uint64_t size) {
    const std::uint64_t absolute = dataOffset + relative;
    if (absolute < dataOffset || absolute >= size) {
        throw TwoDAError("Attempted to read past end of file while reading a 2DA cell entry.", 5);
    }
    return absolute;
}


constexpr std::uint32_t kGdaColumnListField = 10002u;
constexpr std::uint32_t kGdaRowListField = 10003u;
constexpr std::uint32_t kGdaColumnNameField = 10000u;
constexpr std::uint32_t kGdaColumnHashField = 10001u;
constexpr std::uint32_t kGdaColumnTypeField = 10999u;
constexpr std::uint32_t kGdaFirstRowColumnField = 10005u;
constexpr std::uint32_t kGdaNullOffset = 0xFFFFFFFFu;

constexpr std::uint16_t kGff4Uint8 = 0;
constexpr std::uint16_t kGff4Sint32 = 5;
constexpr std::uint16_t kGff4Float32 = 8;
constexpr std::uint16_t kGff4String = 14;

constexpr int kGdaTypeEmpty = -1;
constexpr int kGdaTypeString = 0;
constexpr int kGdaTypeInt = 1;
constexpr int kGdaTypeFloat = 2;
constexpr int kGdaTypeBool = 3;
constexpr int kGdaTypeResource = 4;

struct GdaTemplateField {
    std::uint32_t label = 0;
    std::uint16_t type = 0;
    std::uint16_t flags = 0;
    std::uint32_t offset = 0;
};

struct GdaStructTemplate {
    std::array<char, 4> label{{0, 0, 0, 0}};
    std::vector<GdaTemplateField> fields;
    std::uint32_t size = 0;
};

std::uint32_t readU32At(const std::vector<unsigned char>& data, std::size_t offset, const char* what) {
    if (offset > data.size() || data.size() - offset < 4) {
        throw TwoDAError(std::string("Malformed GDA: unable to read ") + what + ".", 3);
    }
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24u);
}

std::uint16_t readU16At(const std::vector<unsigned char>& data, std::size_t offset, const char* what) {
    if (offset > data.size() || data.size() - offset < 2) {
        throw TwoDAError(std::string("Malformed GDA: unable to read ") + what + ".", 3);
    }
    return static_cast<std::uint16_t>(data[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::array<char, 4> readTagAt(const std::vector<unsigned char>& data, std::size_t offset, const char* what) {
    if (offset > data.size() || data.size() - offset < 4) {
        throw TwoDAError(std::string("Malformed GDA: unable to read ") + what + ".", 3);
    }
    return std::array<char, 4>{{static_cast<char>(data[offset]), static_cast<char>(data[offset + 1]), static_cast<char>(data[offset + 2]), static_cast<char>(data[offset + 3])}};
}

std::string formatHashLabel(std::uint32_t hash) {
    std::ostringstream out;
    out << "hash_" << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << hash;
    return out.str();
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6u) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12u) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18u) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string utf16LeToUtf8(const std::vector<unsigned char>& data, std::size_t offset, std::size_t codeUnits) {
    std::string out;
    for (std::size_t i = 0; i < codeUnits; ++i) {
        const std::uint16_t unit = readU16At(data, offset + i * 2u, "GDA UTF-16 text");
        if (unit == 0) {
            break;
        }
        if (unit >= 0xD800u && unit <= 0xDBFFu && i + 1 < codeUnits) {
            const std::uint16_t next = readU16At(data, offset + (i + 1u) * 2u, "GDA UTF-16 surrogate");
            if (next >= 0xDC00u && next <= 0xDFFFu) {
                const std::uint32_t cp = 0x10000u + (((static_cast<std::uint32_t>(unit) - 0xD800u) << 10u) |
                                                     (static_cast<std::uint32_t>(next) - 0xDC00u));
                appendUtf8(out, cp);
                ++i;
                continue;
            }
        }
        appendUtf8(out, unit);
    }
    return out;
}

std::vector<std::uint16_t> utf8ToUtf16LeUnits(const std::string& text) {
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = 0;
        std::size_t advance = 1;
        if (ch < 0x80u) {
            cp = ch;
        } else if ((ch & 0xE0u) == 0xC0u && i + 1 < text.size()) {
            cp = ((ch & 0x1Fu) << 6u) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            advance = 2;
        } else if ((ch & 0xF0u) == 0xE0u && i + 2 < text.size()) {
            cp = ((ch & 0x0Fu) << 12u) |
                 ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6u) |
                 (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            advance = 3;
        } else if ((ch & 0xF8u) == 0xF0u && i + 3 < text.size()) {
            cp = ((ch & 0x07u) << 18u) |
                 ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12u) |
                 ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6u) |
                 (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
            advance = 4;
        } else {
            cp = 0xFFFDu;
        }
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<std::uint16_t>(cp));
        } else {
            cp -= 0x10000u;
            out.push_back(static_cast<std::uint16_t>(0xD800u + ((cp >> 10u) & 0x3FFu)));
            out.push_back(static_cast<std::uint16_t>(0xDC00u + (cp & 0x3FFu)));
        }
        i += advance;
    }
    out.push_back(0);
    return out;
}

std::string readGdaStringAt(const std::vector<unsigned char>& data, std::uint32_t dataOffset, std::uint32_t relativeOffset) {
    if (relativeOffset == kGdaNullOffset) {
        return {};
    }
    const std::uint64_t absolute = static_cast<std::uint64_t>(dataOffset) + relativeOffset;
    if (absolute > data.size() || data.size() - static_cast<std::size_t>(absolute) < 4) {
        throw TwoDAError("Malformed GDA: string offset points outside the file.", 5);
    }
    const std::uint32_t codeUnits = readU32At(data, static_cast<std::size_t>(absolute), "GDA string length");
    const std::uint64_t byteCount = static_cast<std::uint64_t>(codeUnits) * 2u;
    const std::uint64_t textOffset = absolute + 4u;
    if (byteCount > data.size() || textOffset > data.size() || data.size() - static_cast<std::size_t>(textOffset) < byteCount) {
        throw TwoDAError("Malformed GDA: string length points outside the file.", 5);
    }
    return utf16LeToUtf8(data, static_cast<std::size_t>(textOffset), codeUnits);
}

std::string readGdaStringPointer(const std::vector<unsigned char>& data, std::uint32_t dataOffset, std::size_t fieldOffset) {
    const std::uint32_t relativeOffset = readU32At(data, fieldOffset, "GDA string pointer");
    return readGdaStringAt(data, dataOffset, relativeOffset);
}

std::int64_t readSignedField(const std::vector<unsigned char>& data, std::size_t offset, std::uint16_t type) {
    switch (type) {
    case 1: return static_cast<std::int8_t>(data.at(offset));
    case 3: return static_cast<std::int16_t>(readU16At(data, offset, "GDA int16"));
    case 5: return static_cast<std::int32_t>(readU32At(data, offset, "GDA int32"));
    case 7: {
        const std::uint64_t lo = readU32At(data, offset, "GDA int64 lo");
        const std::uint64_t hi = readU32At(data, offset + 4u, "GDA int64 hi");
        return static_cast<std::int64_t>((hi << 32u) | lo);
    }
    default: return static_cast<std::int32_t>(readU32At(data, offset, "GDA integer"));
    }
}

std::uint64_t readUnsignedField(const std::vector<unsigned char>& data, std::size_t offset, std::uint16_t type) {
    switch (type) {
    case 0: return data.at(offset);
    case 2: return readU16At(data, offset, "GDA uint16");
    case 4: return readU32At(data, offset, "GDA uint32");
    case 6: {
        const std::uint64_t lo = readU32At(data, offset, "GDA uint64 lo");
        const std::uint64_t hi = readU32At(data, offset + 4u, "GDA uint64 hi");
        return (hi << 32u) | lo;
    }
    default: return readU32At(data, offset, "GDA unsigned integer");
    }
}

std::string formatGdaFloat(double value) {
    std::ostringstream out;
    out << std::setprecision(9) << value;
    return out.str();
}

int gdaTypeFromRowFieldType(std::uint16_t fieldType) {
    switch (fieldType) {
    case kGff4String: return kGdaTypeString;
    case kGff4Float32:
    case 9: return kGdaTypeFloat;
    case kGff4Uint8: return kGdaTypeBool;
    case 1:
    case 2:
    case 3:
    case 4:
    case kGff4Sint32:
    case 6:
    case 7: return kGdaTypeInt;
    default: return kGdaTypeString;
    }
}

std::uint16_t gdaRowFieldTypeFromColumnType(int columnType) {
    switch (columnType) {
    case kGdaTypeInt: return kGff4Sint32;
    case kGdaTypeFloat: return kGff4Float32;
    case kGdaTypeBool: return kGff4Uint8;
    case kGdaTypeString:
    case kGdaTypeResource:
    case kGdaTypeEmpty:
    default: return kGff4String;
    }
}

bool parseGdaHashToken(const std::string& token, std::uint32_t& hash) {
    std::string s = token;
    if (s.rfind("hash_", 0) == 0) s = s.substr(5);
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s = s.substr(2);
    if (s.empty() || s.size() > 8) return false;
    std::uint32_t value = 0;
    for (char ch : s) {
        value <<= 4u;
        if (ch >= '0' && ch <= '9') value |= static_cast<std::uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') value |= static_cast<std::uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') value |= static_cast<std::uint32_t>(ch - 'A' + 10);
        else return false;
    }
    hash = value;
    return true;
}

void addBuiltinGdaColumnNames(std::unordered_map<std::uint32_t, std::string>& out) {
    // These cover the common labels visible in DAO/DA2 GDAs and bootstrap a
    // useful editor even when no project-specific schema cache exists.
    out.emplace(0x66FBC936u, "ID");
    out.emplace(0x1BD4C5A2u, "COUNT");
    out.emplace(0xB49CA166u, "label");
    out.emplace(0xABD5A48Du, "worksheet");
    out.emplace(0xF333FE09u, "script");
    out.emplace(0xB22D0123u, "name");
    out.emplace(0x88578372u, "tag");
    out.emplace(0x3FC13DE1u, "value");
    out.emplace(0x03C7F222u, "condition");
    out.emplace(0x9E52DA7Fu, "plot");
    out.emplace(0x5C78C4A3u, "resource");
}

void loadGdaDictionaryFile(const std::filesystem::path& path, std::unordered_map<std::uint32_t, std::string>& out) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        line.erase(std::find_if(line.rbegin(), line.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), line.end());
        if (line.empty()) continue;

        std::string first;
        std::string second;
        const auto comma = line.find(',');
        const auto tab = line.find('\t');
        const auto sep = (tab != std::string::npos) ? tab : comma;
        if (sep != std::string::npos) {
            first = line.substr(0, sep);
            second = line.substr(sep + 1);
        } else {
            std::istringstream parts(line);
            parts >> first;
            std::getline(parts, second);
        }
        auto trimLocal = [](std::string v) {
            v.erase(v.begin(), std::find_if(v.begin(), v.end(), [](unsigned char ch) { return !std::isspace(ch); }));
            v.erase(std::find_if(v.rbegin(), v.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), v.end());
            return v;
        };
        first = trimLocal(first);
        second = trimLocal(second);
        if (first.empty() || second.empty()) continue;
        std::uint32_t hash = 0;
        if (!parseGdaHashToken(first, hash)) {
            // Also accept "Name hash" as a convenience for hand-written caches.
            if (!parseGdaHashToken(second, hash)) continue;
            std::swap(first, second);
        }
        out[hash] = second;
    }
}

std::string environmentVariable(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
#endif
}

std::vector<std::filesystem::path> splitDictionaryPathList(const std::string& envValue) {
    std::vector<std::filesystem::path> out;
    if (envValue.empty()) return out;
    std::string current;
    for (char ch : envValue) {
        if (ch == ';') {
            if (!current.empty()) out.emplace_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) out.emplace_back(current);
    return out;
}

std::unordered_map<std::uint32_t, std::string> loadGdaColumnDictionary(const std::filesystem::path& gdaPath) {
    std::unordered_map<std::uint32_t, std::string> out;
    addBuiltinGdaColumnNames(out);

    const std::vector<std::filesystem::path> candidates = {
        gdaPath.parent_path() / "gda_column_names.tsv",
        gdaPath.parent_path() / "gda_column_names.csv",
        std::filesystem::current_path() / "gda_column_names.tsv",
        std::filesystem::current_path() / "gda_column_names.csv",
        std::filesystem::current_path() / "resources" / "gda_column_names.tsv"
    };
    for (const auto& candidate : candidates) {
        loadGdaDictionaryFile(candidate, out);
    }
    for (const auto& candidate : splitDictionaryPathList(environmentVariable("NEO2DA_GDA_COLUMN_NAMES"))) {
        loadGdaDictionaryFile(candidate, out);
    }
    return out;
}

std::string knownGdaColumnName(std::uint32_t hash, const std::unordered_map<std::uint32_t, std::string>& dictionary) {
    const auto it = dictionary.find(hash);
    if (it != dictionary.end()) return it->second;
    return {};
}

std::uint32_t crc32Utf16Lower(const std::string& text) {
    std::uint32_t crc = 0xFFFFFFFFu;
    auto feed = [&](unsigned char byte) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 1u) ? ((crc >> 1u) ^ 0xEDB88320u) : (crc >> 1u);
        }
    };
    const std::string lower = lowerAscii(text);
    for (unsigned char ch : lower) {
        feed(ch);
        feed(0);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool parseHashLabel(const std::string& label, std::uint32_t& hash) {
    if (label.size() != 13 || label.rfind("hash_", 0) != 0) {
        return false;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 5; i < label.size(); ++i) {
        const char ch = label[i];
        value <<= 4u;
        if (ch >= '0' && ch <= '9') value |= static_cast<std::uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') value |= static_cast<std::uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') value |= static_cast<std::uint32_t>(ch - 'A' + 10);
        else return false;
    }
    hash = value;
    return true;
}

std::uint32_t gdaHashForColumnLabel(const std::string& label) {
    std::uint32_t parsed = 0;
    if (parseHashLabel(label, parsed)) {
        return parsed;
    }
    return crc32Utf16Lower(label);
}

bool looksInteger(const std::string& text) {
    if (text.empty() || text == "****") return true;
    char* end = nullptr;
    errno = 0;
    std::strtoll(text.c_str(), &end, 10);
    return errno == 0 && end && *end == '\0';
}

bool looksFloat(const std::string& text) {
    if (text.empty() || text == "****") return true;
    char* end = nullptr;
    errno = 0;
    std::strtod(text.c_str(), &end);
    return errno == 0 && end && *end == '\0';
}

int inferGdaColumnType(const std::vector<std::vector<std::string>>& rows, std::size_t column) {
    bool haveValue = false;
    bool allInteger = true;
    bool allFloat = true;
    for (const auto& row : rows) {
        if (column >= row.size()) continue;
        const auto& value = row[column];
        if (value.empty() || value == "****") continue;
        haveValue = true;
        allInteger = allInteger && looksInteger(value);
        allFloat = allFloat && looksFloat(value);
    }
    if (!haveValue) return kGdaTypeString;
    if (allInteger) return kGdaTypeInt;
    if (allFloat) return kGdaTypeFloat;
    return kGdaTypeString;
}

std::int32_t parseGdaIntValue(const std::string& text) {
    if (text.empty() || text == "****") return 0;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0') return 0;
    if (value < static_cast<long>(std::numeric_limits<std::int32_t>::min())) return std::numeric_limits<std::int32_t>::min();
    if (value > static_cast<long>(std::numeric_limits<std::int32_t>::max())) return std::numeric_limits<std::int32_t>::max();
    return static_cast<std::int32_t>(value);
}

float parseGdaFloatValue(const std::string& text) {
    if (text.empty() || text == "****") return 0.0f;
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(text.c_str(), &end);
    if (errno != 0 || !end || *end != '\0') return 0.0f;
    return value;
}

bool parseGdaBoolValue(const std::string& text) {
    const std::string lower = lowerAscii(text);
    return lower == "1" || lower == "true" || lower == "yes";
}

void writeU8(std::ostream& out, std::uint8_t value) {
    detail::writeBytes(out, &value, 1);
}

void writeTag(std::ostream& out, const std::array<char, 4>& tag) {
    detail::writeBytes(out, tag.data(), tag.size());
}

void writeF32(std::ostream& out, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float32 size mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    detail::writeUInt32LE(out, bits);
}

std::uint32_t checkedU32Offset(std::uint64_t value, const char* what) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw TwoDAError(std::string("GDA ") + what + " exceeds the 32-bit offset range.");
    }
    return static_cast<std::uint32_t>(value);
}

void writeGdaUtf16String(std::ostream& out, const std::string& text) {
    const auto units = utf8ToUtf16LeUnits(text);
    if (units.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw TwoDAError("GDA string is too large to save.");
    }
    detail::writeUInt32LE(out, static_cast<std::uint32_t>(units.size()));
    for (const auto unit : units) {
        detail::writeUInt16LE(out, unit);
    }
}

void patchGdaStringPointer(std::ostream& out, std::uint32_t dataOffset, std::uint64_t pointerPosition, const std::string& value) {
    if (value.empty() || value == "****") {
        const auto returnPos = detail::tellp(out);
        out.seekp(static_cast<std::streamoff>(pointerPosition), std::ios::beg);
        detail::writeUInt32LE(out, kGdaNullOffset);
        out.seekp(static_cast<std::streamoff>(returnPos), std::ios::beg);
        return;
    }
    const std::uint64_t stringPosition = detail::tellp(out);
    if (stringPosition < dataOffset) {
        throw TwoDAError("Internal GDA writer error: string position precedes data section.");
    }
    const std::uint32_t relative = checkedU32Offset(stringPosition - dataOffset, "string offset");
    writeGdaUtf16String(out, value);
    const auto returnPos = detail::tellp(out);
    out.seekp(static_cast<std::streamoff>(pointerPosition), std::ios::beg);
    detail::writeUInt32LE(out, relative);
    out.seekp(static_cast<std::streamoff>(returnPos), std::ios::beg);
}

} // namespace

std::uint32_t gdaColumnHashForName(const std::string& name) {
    return crc32Utf16Lower(name);
}

TwoDAError::TwoDAError(const std::string& message, int helpContext)
    : std::runtime_error(message), helpContext_(helpContext) {}

int TwoDAError::helpContext() const noexcept {
    return helpContext_;
}

TwoDAFile::TwoDAFile(const std::filesystem::path& filename) {
    load(filename);
}

TwoDAFile TwoDAFile::create(std::vector<std::string> columns, std::size_t rows) {
    if (columns.empty()) {
        columns.push_back(defaultColumnLabel(0));
    }
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].empty()) {
            columns[i] = defaultColumnLabel(i);
        }
        validateTokenSize(columns[i], "Column label");
    }

    TwoDAFile table;
    table.columnLabels_ = std::move(columns);
    table.rowLabels_.reserve(rows);
    table.entries_.reserve(rows);
    for (std::size_t r = 0; r < rows; ++r) {
        table.rowLabels_.push_back(defaultRowLabel(r));
        table.entries_.push_back(std::vector<std::string>(table.columnLabels_.size(), "****"));
    }
    table.loaded_ = true;
    table.dirty_ = true;
    table.validateShape();
    return table;
}


TwoDAFile TwoDAFile::fromTable(const neotabular::Table& imported) {
    if (imported.columns.size() < 2) {
        throw TwoDAError("Imported 2DA table must contain RowLabel plus at least one data column.");
    }
    std::vector<std::string> dataColumns(imported.columns.begin() + 1, imported.columns.end());
    TwoDAFile table = TwoDAFile::create(std::move(dataColumns), imported.rows.size());
    for (std::size_t r = 0; r < imported.rows.size(); ++r) {
        const auto& row = imported.rows[r];
        table.rowLabels_[r] = row.empty() ? defaultRowLabel(r) : row[0];
        for (std::size_t c = 0; c < table.columnLabels_.size(); ++c) {
            table.entries_[r][c] = (c + 1 < row.size()) ? row[c + 1] : std::string("****");
        }
    }
    table.loaded_ = true;
    table.dirty_ = true;
    table.validateShape();
    return table;
}

void TwoDAFile::requireLoaded(const std::string& message, int helpContext) const {
    if (!loaded_) {
        throw TwoDAError(message, helpContext);
    }
}

void TwoDAFile::validateShape() const {
    if (columnLabels_.empty()) {
        throw TwoDAError("A 2DA table must contain at least one column.", 2);
    }
    if (rowLabels_.size() != entries_.size()) {
        throw TwoDAError("Internal 2DA row-label and row-data counts do not match.");
    }
    for (const auto& label : columnLabels_) {
        validateTokenSize(label, "Column label");
    }
    for (std::size_t r = 0; r < entries_.size(); ++r) {
        validateTokenSize(rowLabels_[r], "Row label");
        if (entries_[r].size() != columnLabels_.size()) {
            throw TwoDAError("Internal 2DA row width does not match the column count.");
        }
        for (const auto& value : entries_[r]) {
            validateTokenSize(value, "Cell value");
        }
    }
}


void TwoDAFile::loadGDA(std::istream& in, const std::filesystem::path& filename) {
    in.clear();
    in.seekg(0, std::ios::beg);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<unsigned char> data(bytes.begin(), bytes.end());
    if (data.size() < 28u) {
        throw TwoDAError("Specified file is not a valid GDA file.", 2);
    }

    const std::string magic(reinterpret_cast<const char*>(data.data()), 8);
    if (magic != "GFF V4.0" && magic != "GFF V4.1") {
        throw TwoDAError("Specified file is not a valid Dragon Age GDA file.", 2);
    }
    const std::string platform(reinterpret_cast<const char*>(data.data() + 8), 4);
    if (platform != "PC  ") {
        throw TwoDAError("Only little-endian PC GDA files are supported.", 2);
    }
    const std::string fileType(reinterpret_cast<const char*>(data.data() + 12), 4);
    if (fileType != "G2DA") {
        throw TwoDAError("Specified GFF V4 file is not a GDA/G2DA table.", 2);
    }
    const std::string gdaVersion(reinterpret_cast<const char*>(data.data() + 16), 4);
    if (gdaVersion != "V0.1" && gdaVersion != "V0.2") {
        throw TwoDAError("Unsupported GDA version: " + gdaVersion, 2);
    }

    const bool gff41 = magic == "GFF V4.1";
    const std::uint32_t structCount = readU32At(data, 20, "GDA structure-template count");
    if (structCount < 3u || structCount > 100000u) {
        throw TwoDAError("Malformed GDA: unreasonable structure-template count.", 4);
    }

    std::uint32_t dataOffset = 0;
    std::size_t templateOffset = 0;
    if (gff41) {
        const std::uint32_t stringCount = readU32At(data, 24, "GDA V4.1 string count");
        const std::uint32_t stringOffset = readU32At(data, 28, "GDA V4.1 string offset");
        dataOffset = readU32At(data, 32, "GDA V4.1 data offset");
        templateOffset = 36u;
        if (stringCount != 0 || stringOffset != kGdaNullOffset) {
            throw TwoDAError("GFF V4.1 GDA global-string tables are not supported in Neo2DA yet.", 2);
        }
    } else {
        dataOffset = readU32At(data, 24, "GDA data offset");
        templateOffset = 28u;
    }
    if (dataOffset >= data.size()) {
        throw TwoDAError("Malformed GDA: data section offset points outside the file.", 5);
    }
    const std::uint64_t templateBytes = static_cast<std::uint64_t>(structCount) * 16u;
    if (templateBytes > data.size() || templateOffset > data.size() || data.size() - templateOffset < templateBytes) {
        throw TwoDAError("Malformed GDA: structure-template table points outside the file.", 5);
    }

    std::vector<GdaStructTemplate> templates;
    templates.reserve(structCount);
    for (std::uint32_t i = 0; i < structCount; ++i) {
        const std::size_t off = templateOffset + static_cast<std::size_t>(i) * 16u;
        GdaStructTemplate tmpl;
        tmpl.label = readTagAt(data, off, "GDA structure-template label");
        const std::uint32_t fieldCount = readU32At(data, off + 4u, "GDA structure-template field count");
        const std::uint32_t fieldOffset = readU32At(data, off + 8u, "GDA structure-template field offset");
        tmpl.size = readU32At(data, off + 12u, "GDA structure-template struct size");
        if (fieldCount > 100000u) {
            throw TwoDAError("Malformed GDA: unreasonable field count in structure template.", 4);
        }
        const std::uint64_t fieldBytes = static_cast<std::uint64_t>(fieldCount) * 12u;
        if (fieldBytes > data.size() || fieldOffset > data.size() || data.size() - fieldOffset < fieldBytes) {
            throw TwoDAError("Malformed GDA: field table points outside the file.", 5);
        }
        tmpl.fields.reserve(fieldCount);
        for (std::uint32_t f = 0; f < fieldCount; ++f) {
            const std::size_t fieldPos = static_cast<std::size_t>(fieldOffset) + static_cast<std::size_t>(f) * 12u;
            GdaTemplateField field;
            field.label = readU32At(data, fieldPos, "GDA field label");
            const std::uint32_t typeAndFlags = readU32At(data, fieldPos + 4u, "GDA field type/flags");
            field.type = static_cast<std::uint16_t>(typeAndFlags & 0xFFFFu);
            field.flags = static_cast<std::uint16_t>((typeAndFlags >> 16u) & 0xFFFFu);
            field.offset = readU32At(data, fieldPos + 8u, "GDA field offset");
            tmpl.fields.push_back(field);
        }
        templates.push_back(std::move(tmpl));
    }

    auto findField = [](const GdaStructTemplate& tmpl, std::uint32_t label) -> const GdaTemplateField* {
        for (const auto& field : tmpl.fields) {
            if (field.label == label) return &field;
        }
        return nullptr;
    };

    const auto& topTemplate = templates.at(0);
    const GdaTemplateField* columnListField = findField(topTemplate, kGdaColumnListField);
    const GdaTemplateField* rowListField = findField(topTemplate, kGdaRowListField);
    if (!columnListField || !rowListField) {
        throw TwoDAError("Malformed GDA: missing column or row list field.", 3);
    }

    auto listStructOffsets = [&](const GdaTemplateField& listField, const char* label) {
        if (listField.type >= templates.size()) {
            throw TwoDAError(std::string("Malformed GDA: ") + label + " list references an invalid structure template.", 3);
        }
        const auto& itemTemplate = templates.at(listField.type);
        const std::uint64_t pointerPos = static_cast<std::uint64_t>(dataOffset) + listField.offset;
        if (pointerPos > data.size() || data.size() - static_cast<std::size_t>(pointerPos) < 4u) {
            throw TwoDAError(std::string("Malformed GDA: ") + label + " list pointer is outside the file.", 5);
        }
        const std::uint32_t relativeListOffset = readU32At(data, static_cast<std::size_t>(pointerPos), "GDA structure-list pointer");
        std::vector<std::size_t> offsets;
        if (relativeListOffset == kGdaNullOffset) {
            return offsets;
        }
        const std::uint64_t listPos64 = static_cast<std::uint64_t>(dataOffset) + relativeListOffset;
        if (listPos64 > data.size() || data.size() - static_cast<std::size_t>(listPos64) < 4u) {
            throw TwoDAError(std::string("Malformed GDA: ") + label + " list offset is outside the file.", 5);
        }
        const std::uint32_t count = readU32At(data, static_cast<std::size_t>(listPos64), "GDA structure-list count");
        if (count > 1000000u) {
            throw TwoDAError(std::string("Malformed GDA: unreasonable ") + label + " count.", 4);
        }
        const std::uint64_t firstItem = listPos64 + 4u;
        const std::uint64_t itemBytes = static_cast<std::uint64_t>(count) * itemTemplate.size;
        if (itemBytes > data.size() || firstItem > data.size() || data.size() - static_cast<std::size_t>(firstItem) < itemBytes) {
            throw TwoDAError(std::string("Malformed GDA: ") + label + " inline structure list exceeds the file size.", 5);
        }
        offsets.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            offsets.push_back(static_cast<std::size_t>(firstItem + static_cast<std::uint64_t>(i) * itemTemplate.size));
        }
        return offsets;
    };

    const std::vector<std::size_t> columnOffsets = listStructOffsets(*columnListField, "column");
    const std::vector<std::size_t> rowOffsets = listStructOffsets(*rowListField, "row");
    if (columnOffsets.empty()) {
        throw TwoDAError("Specified GDA file does not contain any columns.", 2);
    }
    if (columnOffsets.size() > 100000u || rowOffsets.size() > 1000000u) {
        throw TwoDAError("Malformed GDA: table dimensions are too large.", 4);
    }

    const auto& columnTemplate = templates.at(columnListField->type);
    const auto& rowTemplate = templates.at(rowListField->type);
    const auto columnNameDictionary = loadGdaColumnDictionary(filename);
    const GdaTemplateField* columnNameField = findField(columnTemplate, kGdaColumnNameField);
    const GdaTemplateField* columnHashField = findField(columnTemplate, kGdaColumnHashField);
    const GdaTemplateField* columnTypeField = findField(columnTemplate, kGdaColumnTypeField);
    if (!columnHashField) {
        throw TwoDAError("Malformed GDA: missing column hash field.", 3);
    }

    std::vector<const GdaTemplateField*> rowFields;
    rowFields.reserve(columnOffsets.size());
    for (std::size_t c = 0; c < columnOffsets.size(); ++c) {
        const std::uint32_t expectedLabel = kGdaFirstRowColumnField + static_cast<std::uint32_t>(c);
        const GdaTemplateField* field = findField(rowTemplate, expectedLabel);
        if (!field && c < rowTemplate.fields.size()) {
            field = &rowTemplate.fields[c];
        }
        if (!field) {
            throw TwoDAError("Malformed GDA: row template has fewer fields than the column list.", 3);
        }
        rowFields.push_back(field);
    }

    columnLabels_.clear();
    rowLabels_.clear();
    entries_.clear();
    gdaColumnHashes_.clear();
    gdaColumnTypes_.clear();
    columnLabels_.reserve(columnOffsets.size());
    gdaColumnHashes_.reserve(columnOffsets.size());
    gdaColumnTypes_.reserve(columnOffsets.size());

    for (std::size_t c = 0; c < columnOffsets.size(); ++c) {
        const std::size_t colBase = columnOffsets[c];
        std::uint32_t hash = readU32At(data, colBase + columnHashField->offset, "GDA column hash");
        std::string label;
        if (columnNameField) {
            label = readGdaStringPointer(data, dataOffset, colBase + columnNameField->offset);
        }
        if (label.empty()) {
            label = knownGdaColumnName(hash, columnNameDictionary);
        }
        if (label.empty()) {
            label = formatHashLabel(hash);
        }
        int columnType = kGdaTypeEmpty;
        if (columnTypeField) {
            if (colBase + columnTypeField->offset >= data.size()) {
                throw TwoDAError("Malformed GDA: column type field points outside the file.", 5);
            }
            const unsigned char rawType = data[colBase + columnTypeField->offset];
            columnType = rawType == 0xFFu ? kGdaTypeEmpty : static_cast<int>(rawType);
        }
        if (columnType < kGdaTypeEmpty || columnType > kGdaTypeResource) {
            columnType = gdaTypeFromRowFieldType(rowFields[c]->type);
        }
        if (columnType == kGdaTypeEmpty) {
            columnType = gdaTypeFromRowFieldType(rowFields[c]->type);
        }
        columnLabels_.push_back(std::move(label));
        gdaColumnHashes_.push_back(hash);
        gdaColumnTypes_.push_back(columnType);
    }

    rowLabels_.reserve(rowOffsets.size());
    entries_.reserve(rowOffsets.size());
    for (std::size_t r = 0; r < rowOffsets.size(); ++r) {
        const std::size_t rowBase = rowOffsets[r];
        rowLabels_.push_back(defaultRowLabel(r));
        std::vector<std::string> row;
        row.reserve(columnLabels_.size());
        for (std::size_t c = 0; c < columnLabels_.size(); ++c) {
            const GdaTemplateField& field = *rowFields[c];
            const std::size_t fieldOffset = rowBase + field.offset;
            if (fieldOffset >= data.size()) {
                throw TwoDAError("Malformed GDA: row field points outside the file.", 5);
            }
            std::string value;
            switch (field.type) {
            case kGff4String:
                value = readGdaStringPointer(data, dataOffset, fieldOffset);
                if (value.empty()) value = "****";
                break;
            case kGff4Float32: {
                const std::uint32_t bits = readU32At(data, fieldOffset, "GDA float32");
                float f = 0.0f;
                std::memcpy(&f, &bits, sizeof(f));
                value = formatGdaFloat(f);
                break;
            }
            case 9: {
                const std::uint64_t lo = readU32At(data, fieldOffset, "GDA double lo");
                const std::uint64_t hi = readU32At(data, fieldOffset + 4u, "GDA double hi");
                const std::uint64_t bits = (hi << 32u) | lo;
                double d = 0.0;
                std::memcpy(&d, &bits, sizeof(d));
                value = formatGdaFloat(d);
                break;
            }
            case kGff4Uint8:
                if (c < gdaColumnTypes_.size() && gdaColumnTypes_[c] == kGdaTypeBool) {
                    value = data[fieldOffset] ? "1" : "0";
                } else {
                    value = std::to_string(static_cast<unsigned int>(data[fieldOffset]));
                }
                break;
            case 1:
            case 3:
            case kGff4Sint32:
            case 7:
                value = std::to_string(readSignedField(data, fieldOffset, field.type));
                break;
            case 2:
            case 4:
            case 6:
                value = std::to_string(readUnsignedField(data, fieldOffset, field.type));
                break;
            default:
                value = readGdaStringPointer(data, dataOffset, fieldOffset);
                if (value.empty()) value = "****";
                break;
            }
            row.push_back(std::move(value));
        }
        entries_.push_back(std::move(row));
    }

    filename_ = filename;
    gdaVersion_ = gdaVersion;
    gdaTopLabel_ = topTemplate.label;
    gdaColumnLabel_ = columnTemplate.label;
    gdaRowLabel_ = rowTemplate.label;
    nativeFormat_ = NativeFormat::GDA;
    unknown1_ = 0;
    unknown2_ = 0;
    loaded_ = true;
    dirty_ = false;
    validateShape();
}

void TwoDAFile::load(const std::filesystem::path& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in) {
        throw TwoDAError("Unable to open 2DA file: " + filename.string(), 0);
    }

    const std::string header = detail::readBytes(in, 8, "2DA/GDA header");
    TwoDAFile parsed;
    parsed.filename_ = filename;

    if (header == "GFF V4.0" || header == "GFF V4.1") {
        parsed.loadGDA(in, filename);
        *this = std::move(parsed);
        return;
    }

    in.clear();
    in.seekg(0, std::ios::beg);
    std::string fullText((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto textLines = splitText2DALines(fullText);
    std::size_t textHeaderLine = textLines.size();
    for (std::size_t i = 0; i < textLines.size(); ++i) {
        if (isText2DAHeaderLine(textLines[i])) {
            textHeaderLine = i;
            break;
        }
        if (!tokenizeText2DALine(textLines[i]).empty()) {
            break;
        }
    }
    if (textHeaderLine < textLines.size()) {
        auto parseTextLines = [&](bool tabAware) -> TwoDAFile {
            TwoDAFile candidate;
            candidate.filename_ = filename;
            bool sawColumns = false;
            for (std::size_t i = textHeaderLine + 1; i < textLines.size(); ++i) {
                const auto& line = textLines[i];
                auto tokens = tabAware ? tokenizeText2DALineForImport(line) : tokenizeText2DALine(line);
                if (tokens.empty()) {
                    continue;
                }
                if (!sawColumns) {
                    if (tabAware && line.find('\t') != std::string::npos && !tokens.empty() && tokens.front().empty()) {
                        tokens.erase(tokens.begin());
                    }
                    if (tokens.empty()) {
                        continue;
                    }
                    candidate.columnLabels_ = std::move(tokens);
                    sawColumns = true;
                    continue;
                }
                const std::string rowLabel = tokens.front();
                tokens.erase(tokens.begin());
                if (!tabAware) {
                    mergeLeadingOverflowCells(tokens, candidate.columnLabels_.size());
                }
                if (tokens.size() > candidate.columnLabels_.size()) {
                    throw TwoDAError("Text 2DA row has more cells than column labels.", 3);
                }
                while (tokens.size() < candidate.columnLabels_.size()) {
                    tokens.push_back("****");
                }
                candidate.rowLabels_.push_back(rowLabel);
                candidate.entries_.push_back(std::move(tokens));
            }
            if (!sawColumns || candidate.columnLabels_.empty()) {
                throw TwoDAError("Specified text 2DA file does not contain column labels.", 2);
            }
            candidate.unknown1_ = 0;
            candidate.unknown2_ = 0;
            candidate.nativeFormat_ = NativeFormat::Text2DA;
            candidate.loaded_ = true;
            candidate.dirty_ = false;
            candidate.validateShape();
            return candidate;
        };

        TwoDAError tabError("", 0);
        try {
            *this = parseTextLines(true);
            return;
        } catch (const TwoDAError& error) {
            tabError = error;
        }
        try {
            *this = parseTextLines(false);
            return;
        } catch (const TwoDAError&) {
            throw tabError;
        }
    }

    in.clear();
    in.seekg(8, std::ios::beg);

    if (header != "2DA V2.b") {
        throw TwoDAError("Specified file is not a valid 2DA file.", 2);
    }

    auto skipHeaderNewlines = [&]() {
        char next = '\0';
        bool sawNewline = false;
        while (in.get(next)) {
            if (next == '\n') {
                sawNewline = true;
                continue;
            }
            if (next == '\r') {
                sawNewline = true;
                if (in.peek() == '\n') {
                    in.get();
                }
                continue;
            }
            in.unget();
            break;
        }
        if (!sawNewline || !in) {
            throw TwoDAError("Specified file is not a valid binary 2DA file.", 2);
        }
    };

    skipHeaderNewlines();

    char ch = '\0';
    std::string buffer;
    int charCount = 0;
    while (true) {
        in.read(&ch, 1);
        if (!in) {
            throw TwoDAError("Malformed data encountered while reading 2DA column labels.", 3);
        }
        if (ch == '\t') {
            validateTokenSize(buffer, "Column label");
            parsed.columnLabels_.push_back(buffer);
            buffer.clear();
        } else if (ch != '\0') {
            buffer.push_back(ch);
        }
        ++charCount;
        if (ch == '\0' || charCount >= static_cast<int>(kBufferSize)) {
            break;
        }
    }
    if (charCount >= static_cast<int>(kBufferSize)) {
        throw TwoDAError("Malformed data encountered while reading 2DA column labels.", 3);
    }
    if (parsed.columnLabels_.empty()) {
        throw TwoDAError("Specified binary 2DA file does not contain any columns.", 2);
    }

    const std::uint32_t rowCount = detail::readUInt32LE(in, "2DA row count");
    if (rowCount > kMaxSaneRows) {
        throw TwoDAError("Sanity check failed: 2DA row count is unrealistically large.", 4);
    }

    parsed.rowLabels_.resize(static_cast<std::size_t>(rowCount));
    buffer.clear();
    for (std::uint32_t row = 0; row < rowCount;) {
        in.read(&ch, 1);
        if (!in) {
            throw TwoDAError("Malformed data encountered while reading 2DA row labels.", 3);
        }
        if (ch == '\t') {
            validateTokenSize(buffer, "Row label");
            parsed.rowLabels_[static_cast<std::size_t>(row)] = buffer;
            buffer.clear();
            ++row;
        } else {
            buffer.push_back(ch);
            if (buffer.size() > kBufferSize - 1) {
                throw TwoDAError("Malformed data encountered while reading a 2DA row label.", 3);
            }
        }
    }

    parsed.entries_.assign(static_cast<std::size_t>(rowCount),
                           std::vector<std::string>(parsed.columnLabels_.size(), "****"));

    std::vector<std::vector<std::uint16_t>> offsets(
        static_cast<std::size_t>(rowCount),
        std::vector<std::uint16_t>(parsed.columnLabels_.size(), 0));

    for (std::uint32_t r = 0; r < rowCount; ++r) {
        for (std::size_t c = 0; c < parsed.columnLabels_.size(); ++c) {
            offsets[static_cast<std::size_t>(r)][c] = detail::readUInt16LE(in, "2DA cell offset");
        }
    }

    const std::uint64_t size = detail::fileSize(in);
    if (detail::tellg(in) + 2u <= size) {
        in.read(&parsed.unknown1_, 1);
        in.read(&parsed.unknown2_, 1);
        if (!in) {
            throw TwoDAError("Specified file is not a valid binary 2DA file.", 2);
        }
    }

    const std::uint64_t dataOffset = detail::tellg(in);
    if (rowCount > 0) {
        for (std::uint32_t r = 0; r < rowCount; ++r) {
            for (std::size_t c = 0; c < parsed.columnLabels_.size(); ++c) {
                const std::uint64_t absolute = checkedDataOffset(dataOffset, offsets[static_cast<std::size_t>(r)][c], size);
                in.seekg(static_cast<std::streamoff>(absolute), std::ios::beg);
                std::string value;
                bool sawNullTerminator = false;
                while (true) {
                    in.read(&ch, 1);
                    if (!in) {
                        break;
                    }
                    if (value.size() > kBufferSize - 1) {
                        throw TwoDAError("Buffer overflow while reading a 2DA cell entry.", 6);
                    }
                    if (ch == '\0') {
                        sawNullTerminator = true;
                        break;
                    }
                    value.push_back(ch);
                }
                if (!sawNullTerminator) {
                    throw TwoDAError("Unterminated string encountered while reading a 2DA cell entry.", 5);
                }
                parsed.entries_[static_cast<std::size_t>(r)][c] = value.empty() ? "****" : value;
            }
        }
    }

    parsed.nativeFormat_ = NativeFormat::Binary2DA;
    parsed.loaded_ = true;
    parsed.dirty_ = false;
    parsed.validateShape();
    *this = std::move(parsed);
}

void TwoDAFile::save(const std::filesystem::path& filename) const {
    std::string ext = lowerAscii(filename.extension().string());
    if (ext == ".gda" || (ext.empty() && nativeFormat_ == NativeFormat::GDA)) {
        saveGDA(filename);
    } else if (nativeFormat_ == NativeFormat::Text2DA) {
        saveText2DA(filename);
    } else {
        saveBinary2DA(filename);
    }
}

void TwoDAFile::saveText2DA(const std::filesystem::path& filename) const {
    requireLoaded("No 2DA file has been loaded.", 27);
    validateShape();
    if (columnLabels_.empty()) {
        throw TwoDAError("The open 2DA file has no columns and cannot be saved.", 28);
    }

    try {
        writeFileAtomically(filename, std::ios::binary | std::ios::out, [&](std::ostream& out) {
            detail::writeString(out, "2DA V2.0\n\n");
            char tab = '\t';
            char newline = '\n';
            detail::writeBytes(out, &tab, 1);
            for (std::size_t c = 0; c < columnLabels_.size(); ++c) {
                if (c != 0) {
                    detail::writeBytes(out, &tab, 1);
                }
                writeText2DACell(out, columnLabels_[c]);
            }
            detail::writeBytes(out, &newline, 1);

            for (std::size_t r = 0; r < rowLabels_.size(); ++r) {
                writeText2DACell(out, rowLabels_[r]);
                for (std::size_t c = 0; c < columnLabels_.size(); ++c) {
                    detail::writeBytes(out, &tab, 1);
                    writeText2DACell(out, entries_[r][c]);
                }
                detail::writeBytes(out, &newline, 1);
            }
        });
    } catch (const std::exception& ex) {
        throw TwoDAError(std::string("Unable to save text 2DA file: ") + ex.what(), 28);
    }
}

void TwoDAFile::saveBinary2DA(const std::filesystem::path& filename) const {
    requireLoaded("No 2DA file has been loaded.", 27);
    validateShape();
    if (columnLabels_.empty()) {
        throw TwoDAError("The open 2DA file has no columns and cannot be saved.", 28);
    }
    if (rowLabels_.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw TwoDAError("The open 2DA file has too many rows to save.", 28);
    }

    try {
        writeFileAtomically(filename, std::ios::binary | std::ios::out, [&](std::ostream& out) {
            detail::writeString(out, "2DA V2.b");
            char ch = '\n';
            detail::writeBytes(out, &ch, 1);

            for (const auto& label : columnLabels_) {
                writeBounded2DAString(out, label, "Column label");
                ch = '\t';
                detail::writeBytes(out, &ch, 1);
            }
            ch = '\0';
            detail::writeBytes(out, &ch, 1);

            detail::writeUInt32LE(out, static_cast<std::uint32_t>(rowLabels_.size()));

            for (const auto& label : rowLabels_) {
                writeBounded2DAString(out, label, "Row label");
                ch = '\t';
                detail::writeBytes(out, &ch, 1);
            }

            const std::uint64_t offsetPos = detail::tellp(out);
            for (std::size_t r = 0; r < rowLabels_.size(); ++r) {
                for (std::size_t c = 0; c < columnLabels_.size(); ++c) {
                    detail::writeUInt16LE(out, 0);
                }
            }

            detail::writeBytes(out, &unknown1_, 1);
            detail::writeBytes(out, &unknown2_, 1);

            const std::uint64_t dataOffset = detail::tellp(out);
            std::vector<std::vector<std::uint64_t>> offsets(rowLabels_.size(),
                std::vector<std::uint64_t>(columnLabels_.size(), 0));
            std::unordered_map<std::string, std::uint64_t> stringOffsets;
            stringOffsets.reserve(rowLabels_.size() * columnLabels_.size());

            for (std::size_t r = 0; r < rowLabels_.size(); ++r) {
                for (std::size_t c = 0; c < columnLabels_.size(); ++c) {
                    const auto& value = entries_[r][c];
                    auto found = stringOffsets.find(value);
                    if (found != stringOffsets.end()) {
                        offsets[r][c] = found->second;
                        continue;
                    }

                    const std::uint64_t absolute = detail::tellp(out);
                    const std::uint64_t relative = absolute - dataOffset;
                    offsets[r][c] = relative;
                    stringOffsets.emplace(value, relative);
                    if (value != "****") {
                        writeBounded2DAString(out, value, "Cell value");
                    }
                    ch = '\0';
                    detail::writeBytes(out, &ch, 1);
                }
            }

            out.seekp(static_cast<std::streamoff>(offsetPos), std::ios::beg);
            for (std::size_t r = 0; r < rowLabels_.size(); ++r) {
                for (std::size_t c = 0; c < columnLabels_.size(); ++c) {
                    const auto rel = offsets[r][c];
                    if (rel > std::numeric_limits<std::uint16_t>::max()) {
                        throw std::runtime_error("Refusing to save 2DA string table with offsets larger than 65535 bytes.");
                    }
                    detail::writeUInt16LE(out, static_cast<std::uint16_t>(rel));
                }
            }
        });
    } catch (const std::exception& ex) {
        throw TwoDAError(std::string("Unable to save 2DA file: ") + ex.what(), 0);
    }
}


void TwoDAFile::saveGDA(const std::filesystem::path& filename) const {
    requireLoaded("No 2DA/GDA file has been loaded.", 27);
    validateShape();
    if (columnLabels_.empty()) {
        throw TwoDAError("The open table has no columns and cannot be saved as GDA.", 28);
    }
    if (columnLabels_.size() > 100000u || rowLabels_.size() > 1000000u) {
        throw TwoDAError("The open table is too large to save as a GDA file.", 28);
    }

    // Existing GDA files keep their on-disk G2DA version. Newly converted
    // CSV/TSV/2DA tables default to V0.1 because that variant stores readable
    // column-name strings instead of only CRC32 hashes.
    const bool writeV01 = (nativeFormat_ != NativeFormat::GDA) || gdaVersion_ == "V0.1";
    const std::string version = writeV01 ? "V0.1" : "V0.2";
    const std::size_t columnCount = columnLabels_.size();
    const std::size_t rowCount = rowLabels_.size();

    std::array<char, 4> topLabel = gdaTopLabel_;
    std::array<char, 4> columnLabel = gdaColumnLabel_;
    std::array<char, 4> rowLabel = gdaRowLabel_;
    if (nativeFormat_ != NativeFormat::GDA) {
        if (writeV01) {
            topLabel = std::array<char, 4>{{'G', '2', 'D', 'A'}};
            columnLabel = std::array<char, 4>{{'C', 'O', 'L', 'M'}};
            rowLabel = std::array<char, 4>{{'R', 'O', 'W', 'S'}};
        } else {
            topLabel = std::array<char, 4>{{'g', 't', 'o', 'p'}};
            columnLabel = std::array<char, 4>{{'c', 'o', 'l', 'm'}};
            rowLabel = std::array<char, 4>{{'r', 'o', 'w', 's'}};
        }
    }

    std::vector<std::uint32_t> hashes(columnCount, 0);
    std::vector<int> columnTypes(columnCount, kGdaTypeString);
    for (std::size_t c = 0; c < columnCount; ++c) {
        if (c < gdaColumnHashes_.size() && gdaColumnHashes_[c] != 0) {
            hashes[c] = gdaColumnHashes_[c];
        } else {
            hashes[c] = gdaHashForColumnLabel(columnLabels_[c]);
        }
        int type = (c < gdaColumnTypes_.size()) ? gdaColumnTypes_[c] : kGdaTypeEmpty;
        if (type < kGdaTypeEmpty || type > kGdaTypeResource || type == kGdaTypeEmpty) {
            type = inferGdaColumnType(entries_, c);
        }
        columnTypes[c] = type;
    }

    try {
        writeFileAtomically(filename, std::ios::binary | std::ios::out, [&](std::ostream& out) {
            const std::uint32_t structCount = 3u;
            const std::uint32_t templateOffset = 28u;
            const std::uint32_t topFieldOffset = templateOffset + structCount * 16u;
            const std::uint32_t columnFieldOffset = topFieldOffset + 2u * 12u;
            const std::uint32_t rowFieldOffset = columnFieldOffset + 2u * 12u;
            const std::uint32_t rowStructSize = checkedU32Offset(columnCount * 4u, "row structure size");
            const std::uint32_t dataOffset = checkedU32Offset(static_cast<std::uint64_t>(rowFieldOffset) + static_cast<std::uint64_t>(columnCount) * 12u, "data offset");

            detail::writeString(out, "GFF V4.0");
            detail::writeString(out, "PC  ");
            detail::writeString(out, "G2DA");
            detail::writeString(out, version);
            detail::writeUInt32LE(out, structCount);
            detail::writeUInt32LE(out, dataOffset);

            writeTag(out, topLabel);
            detail::writeUInt32LE(out, 2u);
            detail::writeUInt32LE(out, topFieldOffset);
            detail::writeUInt32LE(out, 8u);

            writeTag(out, columnLabel);
            detail::writeUInt32LE(out, 2u);
            detail::writeUInt32LE(out, columnFieldOffset);
            detail::writeUInt32LE(out, 8u);

            writeTag(out, rowLabel);
            detail::writeUInt32LE(out, checkedU32Offset(columnCount, "row field count"));
            detail::writeUInt32LE(out, rowFieldOffset);
            detail::writeUInt32LE(out, rowStructSize);

            detail::writeUInt32LE(out, kGdaColumnListField);
            detail::writeUInt32LE(out, 0xC0000001u);
            detail::writeUInt32LE(out, 0u);
            detail::writeUInt32LE(out, kGdaRowListField);
            detail::writeUInt32LE(out, 0xC0000002u);
            detail::writeUInt32LE(out, 4u);

            if (writeV01) {
                detail::writeUInt32LE(out, kGdaColumnNameField);
                detail::writeUInt32LE(out, kGff4String);
                detail::writeUInt32LE(out, 0u);
                detail::writeUInt32LE(out, kGdaColumnHashField);
                detail::writeUInt32LE(out, 4u);
                detail::writeUInt32LE(out, 4u);
            } else {
                detail::writeUInt32LE(out, kGdaColumnHashField);
                detail::writeUInt32LE(out, 4u);
                detail::writeUInt32LE(out, 0u);
                detail::writeUInt32LE(out, kGdaColumnTypeField);
                detail::writeUInt32LE(out, kGff4Uint8);
                detail::writeUInt32LE(out, 4u);
            }

            for (std::size_t c = 0; c < columnCount; ++c) {
                detail::writeUInt32LE(out, kGdaFirstRowColumnField + static_cast<std::uint32_t>(c));
                detail::writeUInt32LE(out, gdaRowFieldTypeFromColumnType(columnTypes[c]));
                detail::writeUInt32LE(out, static_cast<std::uint32_t>(c * 4u));
            }

            const std::uint64_t actualDataOffset = detail::tellp(out);
            if (actualDataOffset != dataOffset) {
                throw TwoDAError("Internal GDA writer error: data offset mismatch.");
            }

            const std::uint32_t columnListRelative = 8u;
            const std::uint32_t rowListRelative = checkedU32Offset(8u + 4u + columnCount * 8u, "row-list relative offset");
            detail::writeUInt32LE(out, columnListRelative);
            detail::writeUInt32LE(out, rowListRelative);

            detail::writeUInt32LE(out, checkedU32Offset(columnCount, "column count"));
            std::vector<std::uint64_t> columnNamePointerPositions;
            if (writeV01) columnNamePointerPositions.reserve(columnCount);
            for (std::size_t c = 0; c < columnCount; ++c) {
                if (writeV01) {
                    columnNamePointerPositions.push_back(detail::tellp(out));
                    detail::writeUInt32LE(out, kGdaNullOffset);
                    detail::writeUInt32LE(out, hashes[c]);
                } else {
                    detail::writeUInt32LE(out, hashes[c]);
                    const int type = columnTypes[c];
                    writeU8(out, type == kGdaTypeEmpty ? 0xFFu : static_cast<std::uint8_t>(type));
                    writeU8(out, 0xFFu);
                    writeU8(out, 0xFFu);
                    writeU8(out, 0xFFu);
                }
            }

            detail::writeUInt32LE(out, checkedU32Offset(rowCount, "row count"));
            struct PendingStringPointer {
                std::uint64_t position = 0;
                std::string value;
            };
            std::vector<PendingStringPointer> pendingStrings;
            pendingStrings.reserve(rowCount * columnCount + (writeV01 ? columnCount : 0));
            for (std::size_t r = 0; r < rowCount; ++r) {
                for (std::size_t c = 0; c < columnCount; ++c) {
                    const std::string value = (r < entries_.size() && c < entries_[r].size()) ? entries_[r][c] : std::string("****");
                    switch (columnTypes[c]) {
                    case kGdaTypeInt:
                        detail::writeUInt32LE(out, static_cast<std::uint32_t>(parseGdaIntValue(value)));
                        break;
                    case kGdaTypeFloat:
                        writeF32(out, parseGdaFloatValue(value));
                        break;
                    case kGdaTypeBool:
                        writeU8(out, parseGdaBoolValue(value) ? 1u : 0u);
                        writeU8(out, 0u);
                        writeU8(out, 0u);
                        writeU8(out, 0u);
                        break;
                    case kGdaTypeString:
                    case kGdaTypeResource:
                    case kGdaTypeEmpty:
                    default:
                        pendingStrings.push_back(PendingStringPointer{detail::tellp(out), value});
                        detail::writeUInt32LE(out, kGdaNullOffset);
                        break;
                    }
                }
            }

            if (writeV01) {
                for (std::size_t c = 0; c < columnCount; ++c) {
                    patchGdaStringPointer(out, dataOffset, columnNamePointerPositions[c], columnLabels_[c]);
                }
            }
            for (const auto& pending : pendingStrings) {
                patchGdaStringPointer(out, dataOffset, pending.position, pending.value);
            }
        });
    } catch (const std::exception& ex) {
        throw TwoDAError(std::string("Unable to save GDA file: ") + ex.what(), 0);
    }
}


std::string TwoDAFile::nativeFormatName() const {
    switch (nativeFormat_) {
    case NativeFormat::GDA: return std::string("GDA/G2DA ") + gdaVersion_;
    case NativeFormat::Text2DA: return "Text 2DA V2.0";
    case NativeFormat::Binary2DA:
    default: return "Binary 2DA V2.b";
    }
}

std::size_t TwoDAFile::rowCount() const {
    requireLoaded("No 2DA file has been loaded; unable to read row count.", 11);
    return rowLabels_.size();
}

std::size_t TwoDAFile::columnCount() const {
    requireLoaded("No 2DA file has been loaded; unable to read column count.", 12);
    return columnLabels_.size();
}

const std::string& TwoDAFile::rowLabel(std::size_t row) const {
    requireLoaded("No 2DA file has been loaded; unable to read row label.", 15);
    if (row >= rowLabels_.size()) {
        throw TwoDAError("Invalid row index specified.", 16);
    }
    return rowLabels_[row];
}

const std::string& TwoDAFile::columnLabel(std::size_t column) const {
    requireLoaded("No 2DA file has been loaded; unable to read column label.", 13);
    if (column >= columnLabels_.size()) {
        throw TwoDAError("Invalid column index specified.", 14);
    }
    return columnLabels_[column];
}

const std::string& TwoDAFile::cell(std::size_t row, std::size_t column) const {
    requireLoaded("No 2DA file has been loaded; unable to read cell value.", 17);
    if (row >= rowLabels_.size()) {
        throw TwoDAError("Invalid row index specified.", 18);
    }
    if (column >= columnLabels_.size()) {
        throw TwoDAError("Invalid column index specified.", 19);
    }
    return entries_[row][column];
}

std::size_t TwoDAFile::rowIndexByLabel(const std::string& label) const {
    requireLoaded("No 2DA file has been loaded; unable to look up row labels.", 9);
    const auto want = lowerAscii(label);
    for (std::size_t i = 0; i < rowLabels_.size(); ++i) {
        if (lowerAscii(rowLabels_[i]) == want) {
            return i;
        }
    }
    throw TwoDAError("Unable to find row label: " + label, 10);
}

std::size_t TwoDAFile::columnIndexByLabel(const std::string& label) const {
    requireLoaded("No 2DA file has been loaded; unable to look up column labels.", 7);
    const auto want = lowerAscii(label);
    for (std::size_t i = 0; i < columnLabels_.size(); ++i) {
        if (lowerAscii(columnLabels_[i]) == want) {
            return i;
        }
    }
    throw TwoDAError("Unable to find column label: " + label, 8);
}

std::size_t TwoDAFile::addRow(std::string label, std::string defaultValue) {
    requireLoaded("No 2DA file has been loaded; unable to add row.", 31);
    validateTokenSize(defaultValue, "Cell value");
    if (label.empty()) {
        label = defaultRowLabel(rowLabels_.size());
    }
    validateTokenSize(label, "Row label");
    const std::size_t created = rowLabels_.size();
    rowLabels_.push_back(std::move(label));
    entries_.push_back(std::vector<std::string>(columnLabels_.size(), std::move(defaultValue)));
    dirty_ = true;
    return created;
}

std::size_t TwoDAFile::addColumn(std::string label, std::string defaultValue) {
    requireLoaded("No 2DA file has been loaded; unable to add column.", 29);
    if (label.empty()) {
        label = defaultColumnLabel(columnLabels_.size());
    }
    validateTokenSize(label, "Column label");
    validateTokenSize(defaultValue, "Cell value");
    const std::size_t created = columnLabels_.size();
    columnLabels_.push_back(std::move(label));
    if (nativeFormat_ == NativeFormat::GDA) {
        gdaColumnHashes_.push_back(0);
        gdaColumnTypes_.push_back(inferGdaColumnType(entries_, created));
    }
    for (auto& row : entries_) {
        row.push_back(defaultValue);
    }
    dirty_ = true;
    return created;
}

std::size_t TwoDAFile::cloneRow(std::size_t sourceRow, std::string label) {
    requireLoaded("No 2DA file has been loaded; unable to clone row.", 30);
    if (sourceRow >= rowLabels_.size()) {
        throw TwoDAError("Invalid row index specified.", 30);
    }
    if (label.empty()) {
        label = defaultRowLabel(rowLabels_.size());
    }
    validateTokenSize(label, "Row label");
    const std::size_t created = rowLabels_.size();
    rowLabels_.push_back(std::move(label));
    entries_.push_back(entries_[sourceRow]);
    dirty_ = true;
    return created;
}

void TwoDAFile::removeRow(std::size_t row) {
    requireLoaded("No 2DA file has been loaded; unable to delete row.", 32);
    if (row >= rowLabels_.size()) {
        throw TwoDAError("Invalid row index specified.", 32);
    }
    rowLabels_.erase(rowLabels_.begin() + static_cast<std::ptrdiff_t>(row));
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(row));
    dirty_ = true;
}

void TwoDAFile::removeColumn(std::size_t column) {
    requireLoaded("No 2DA file has been loaded; unable to delete column.", 33);
    if (column >= columnLabels_.size()) {
        throw TwoDAError("Invalid column index specified.", 33);
    }
    columnLabels_.erase(columnLabels_.begin() + static_cast<std::ptrdiff_t>(column));
    if (nativeFormat_ == NativeFormat::GDA) {
        if (column < gdaColumnHashes_.size()) {
            gdaColumnHashes_.erase(gdaColumnHashes_.begin() + static_cast<std::ptrdiff_t>(column));
        }
        if (column < gdaColumnTypes_.size()) {
            gdaColumnTypes_.erase(gdaColumnTypes_.begin() + static_cast<std::ptrdiff_t>(column));
        }
    }
    for (auto& row : entries_) {
        if (column < row.size()) {
            row.erase(row.begin() + static_cast<std::ptrdiff_t>(column));
        }
    }
    dirty_ = true;
}

void TwoDAFile::setRowLabel(std::size_t row, std::string label) {
    requireLoaded("No 2DA file has been loaded; unable to set row label.", 22);
    if (row >= rowLabels_.size()) {
        throw TwoDAError("Invalid row index specified.", 23);
    }
    validateTokenSize(label, "Row label");
    rowLabels_[row] = std::move(label);
    dirty_ = true;
}

void TwoDAFile::setColumnLabel(std::size_t column, std::string label) {
    requireLoaded("No 2DA file has been loaded; unable to set column label.", 20);
    if (column >= columnLabels_.size()) {
        throw TwoDAError("Invalid column index specified.", 21);
    }
    validateTokenSize(label, "Column label");
    columnLabels_[column] = std::move(label);
    if (nativeFormat_ == NativeFormat::GDA && column < gdaColumnHashes_.size()) {
        gdaColumnHashes_[column] = 0;
    }
    dirty_ = true;
}

void TwoDAFile::setCell(std::size_t row, std::size_t column, std::string value) {
    requireLoaded("No 2DA file has been loaded; unable to set cell value.", 24);
    if (row >= rowLabels_.size()) {
        throw TwoDAError("Invalid row index specified.", 25);
    }
    if (column >= columnLabels_.size()) {
        throw TwoDAError("Invalid column index specified.", 26);
    }
    validateTokenSize(value, "Cell value");
    entries_[row][column] = std::move(value);
    dirty_ = true;
}

neotabular::Table TwoDAFile::toTable() const {
    requireLoaded("No 2DA file has been loaded; unable to export table.", 11);
    neotabular::Table table;
    table.columns.reserve(columnLabels_.size() + 1);
    table.columns.push_back("RowLabel");
    table.columns.insert(table.columns.end(), columnLabels_.begin(), columnLabels_.end());
    table.rows.reserve(rowLabels_.size());
    for (std::size_t r = 0; r < rowLabels_.size(); ++r) {
        std::vector<std::string> row;
        row.reserve(columnLabels_.size() + 1);
        row.push_back(rowLabels_[r]);
        row.insert(row.end(), entries_[r].begin(), entries_[r].end());
        table.rows.push_back(std::move(row));
    }
    return table;
}

} // namespace neo2da
