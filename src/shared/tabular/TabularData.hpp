#pragma once

#include <filesystem>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <vector>

namespace neotabular {

struct Table {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
};

class TabularError : public std::runtime_error {
public:
    explicit TabularError(const std::string& message) : std::runtime_error(message) {}
};

enum class Format {
    Csv,
    Tsv
};

Format parseFormat(const std::string& text);
Format formatFromPath(const std::filesystem::path& path);
std::string formatName(Format format);
std::string wildcardText();

Table readTable(const std::filesystem::path& path, Format format);
void writeTable(const Table& table, const std::filesystem::path& path, Format format);

std::string serializeDelimited(const Table& table, char delimiter);
Table parseDelimited(const std::string& text, char delimiter);

bool rowMatches(const Table& table, const std::vector<std::string>& row, const std::string& term, bool caseSensitive = false);
Table filterRows(const Table& table, const std::string& term, bool caseSensitive = false);

} // namespace neotabular
