#include "TabularData.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>

namespace neotabular {
namespace {

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw TabularError("Unable to open table file for reading: " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw TabularError("Unable to open table file for writing: " + path.string());
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        throw TabularError("Unable to write table file: " + path.string());
    }
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trimAscii(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool containsNoCase(const std::string& haystack, const std::string& needle) {
    return lowerAscii(haystack).find(lowerAscii(needle)) != std::string::npos;
}

} // namespace

Format parseFormat(const std::string& text) {
    std::string key = lowerAscii(trimAscii(text));
    if (!key.empty() && key.front() == '.') key.erase(key.begin());
    if (key == "csv") return Format::Csv;
    if (key == "tsv" || key == "tab") return Format::Tsv;
    if (key == "xml" || key == "json") {
        throw TabularError("Neo2DA supports CSV and TSV table files only; XML/JSON are not supported.");
    }
    throw TabularError("Unsupported table format: " + text);
}

Format formatFromPath(const std::filesystem::path& path) {
    return parseFormat(path.extension().string());
}

std::string formatName(Format format) {
    switch (format) {
    case Format::Csv: return "CSV";
    case Format::Tsv: return "TSV";
    }
    return "unknown";
}

std::string wildcardText() {
    return "CSV files (*.csv)|*.csv|TSV files (*.tsv)|*.tsv|All files (*.*)|*.*";
}

Table readTable(const std::filesystem::path& path, Format format) {
    const std::string text = readTextFile(path);
    switch (format) {
    case Format::Csv: return parseDelimited(text, ',');
    case Format::Tsv: return parseDelimited(text, '\t');
    }
    throw TabularError("Unsupported table format.");
}

void writeTable(const Table& table, const std::filesystem::path& path, Format format) {
    switch (format) {
    case Format::Csv: writeTextFile(path, serializeDelimited(table, ',')); return;
    case Format::Tsv: writeTextFile(path, serializeDelimited(table, '\t')); return;
    }
    throw TabularError("Unsupported table format.");
}

std::string serializeDelimited(const Table& table, char delimiter) {
    auto quote = [delimiter](const std::string& value) {
        bool needsQuotes = value.find(delimiter) != std::string::npos || value.find('"') != std::string::npos ||
                          value.find('\n') != std::string::npos || value.find('\r') != std::string::npos;
        if (!needsQuotes) return value;
        std::string out = "\"";
        for (char ch : value) {
            if (ch == '"') out += "\"\"";
            else out.push_back(ch);
        }
        out.push_back('"');
        return out;
    };

    std::ostringstream out;
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        if (c) out << delimiter;
        out << quote(table.columns[c]);
    }
    out << '\n';
    for (const auto& row : table.rows) {
        const std::size_t width = std::max(row.size(), table.columns.size());
        for (std::size_t c = 0; c < width; ++c) {
            if (c) out << delimiter;
            if (c < row.size()) out << quote(row[c]);
        }
        out << '\n';
    }
    return out.str();
}

Table parseDelimited(const std::string& text, char delimiter) {
    std::vector<std::vector<std::string>> records;
    std::vector<std::string> row;
    std::string cell;
    bool inQuotes = false;
    bool atCellStart = true;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (inQuotes) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                cell.push_back(ch);
            }
            continue;
        }
        if (atCellStart && ch == '"') {
            inQuotes = true;
            atCellStart = false;
            continue;
        }
        if (ch == delimiter) {
            row.push_back(std::move(cell));
            cell.clear();
            atCellStart = true;
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            row.push_back(std::move(cell));
            cell.clear();
            records.push_back(std::move(row));
            row.clear();
            atCellStart = true;
            continue;
        }
        cell.push_back(ch);
        atCellStart = false;
    }
    if (inQuotes) {
        throw TabularError("Unterminated quoted field in delimited table.");
    }
    if (!cell.empty() || !row.empty() || (!text.empty() && text.back() == delimiter)) {
        row.push_back(std::move(cell));
        records.push_back(std::move(row));
    }
    while (!records.empty()) {
        bool empty = true;
        for (const auto& value : records.back()) {
            if (!value.empty()) { empty = false; break; }
        }
        if (!empty) break;
        records.pop_back();
    }
    if (records.empty()) {
        throw TabularError("Delimited table does not contain a header row.");
    }
    Table table;
    table.columns = std::move(records.front());
    records.erase(records.begin());
    table.rows = std::move(records);
    return table;
}

bool rowMatches(const Table& table, const std::vector<std::string>& row, const std::string& term, bool caseSensitive) {
    if (term.empty()) return true;
    auto matches = [&](const std::string& value) {
        return caseSensitive ? value.find(term) != std::string::npos : containsNoCase(value, term);
    };
    for (const auto& column : table.columns) {
        if (matches(column)) return true;
    }
    for (const auto& value : row) {
        if (matches(value)) return true;
    }
    return false;
}

Table filterRows(const Table& table, const std::string& term, bool caseSensitive) {
    if (term.empty()) return table;
    Table out;
    out.columns = table.columns;
    for (const auto& row : table.rows) {
        if (rowMatches(table, row, term, caseSensitive)) {
            out.rows.push_back(row);
        }
    }
    return out;
}

} // namespace neotabular
