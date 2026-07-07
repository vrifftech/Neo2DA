#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "TabularData.hpp"

namespace neo2da {

class TwoDAError : public std::runtime_error {
public:
    explicit TwoDAError(const std::string& message, int helpContext = 0);
    int helpContext() const noexcept;

private:
    int helpContext_ = 0;
};

std::uint32_t gdaColumnHashForName(const std::string& name);

class TwoDAFile {
public:
    TwoDAFile() = default;
    explicit TwoDAFile(const std::filesystem::path& filename);

    static TwoDAFile create(std::vector<std::string> columns, std::size_t rows = 1);
    static TwoDAFile fromTable(const neotabular::Table& table);

    void load(const std::filesystem::path& filename);
    void save(const std::filesystem::path& filename) const;

    bool loaded() const noexcept { return loaded_; }
    bool dirty() const noexcept { return dirty_; }
    void setDirty(bool dirty) noexcept { dirty_ = dirty; }
    std::string nativeFormatName() const;
    const std::filesystem::path& filename() const noexcept { return filename_; }
    void setFilename(std::filesystem::path filename) { filename_ = std::move(filename); }

    std::size_t rowCount() const;
    std::size_t columnCount() const;

    const std::string& rowLabel(std::size_t row) const;
    const std::string& columnLabel(std::size_t column) const;
    const std::string& cell(std::size_t row, std::size_t column) const;

    std::size_t rowIndexByLabel(const std::string& label) const;
    std::size_t columnIndexByLabel(const std::string& label) const;

    std::size_t addRow(std::string label = {}, std::string defaultValue = "****");
    std::size_t addColumn(std::string label = {}, std::string defaultValue = "****");
    std::size_t cloneRow(std::size_t sourceRow, std::string label = {});
    void removeRow(std::size_t row);
    void removeColumn(std::size_t column);

    void setRowLabel(std::size_t row, std::string label);
    void setColumnLabel(std::size_t column, std::string label);
    void setCell(std::size_t row, std::size_t column, std::string value);

    neotabular::Table toTable() const;

private:
    enum class NativeFormat { Binary2DA, Text2DA, GDA };

    std::vector<std::string> columnLabels_;
    std::vector<std::string> rowLabels_;
    std::vector<std::vector<std::string>> entries_;
    bool loaded_ = false;
    bool dirty_ = false;
    std::filesystem::path filename_;
    char unknown1_ = '\0';
    char unknown2_ = '\0';
    NativeFormat nativeFormat_ = NativeFormat::Binary2DA;
    std::string gdaVersion_ = "V0.2";
    std::array<char, 4> gdaTopLabel_{{'g', 't', 'o', 'p'}};
    std::array<char, 4> gdaColumnLabel_{{'c', 'o', 'l', 'm'}};
    std::array<char, 4> gdaRowLabel_{{'r', 'o', 'w', 's'}};
    std::vector<std::uint32_t> gdaColumnHashes_;
    std::vector<int> gdaColumnTypes_;

    void requireLoaded(const std::string& message, int helpContext) const;
    void validateShape() const;
    void loadGDA(std::istream& in, const std::filesystem::path& filename);
    void saveText2DA(const std::filesystem::path& filename) const;
    void saveBinary2DA(const std::filesystem::path& filename) const;
    void saveGDA(const std::filesystem::path& filename) const;
};

} // namespace neo2da
