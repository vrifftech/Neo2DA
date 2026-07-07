#include "TslPatcher.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace neotsl {
namespace {

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool iequals(const std::string& a, const std::string& b) {
    return lowerAscii(a) == lowerAscii(b);
}

std::string cellOrEmpty(const std::vector<std::string>& row, std::size_t col) {
    return col < row.size() ? row[col] : std::string();
}

std::string baseNameNoExt(const std::string& patchFilename) {
    std::filesystem::path p(patchFilename);
    std::string stem = lowerAscii(p.stem().string());
    if (stem.empty()) stem = "file";
    return sanitizeSectionName(stem);
}

std::string uniqueSectionName(const PatchProject& project, const std::string& base) {
    std::string clean = sanitizeSectionName(base);
    if (clean.empty()) clean = "section";
    for (std::size_t i = 0;; ++i) {
        std::string candidate = clean + "_" + std::to_string(i);
        if (!project.findSection(candidate)) return candidate;
    }
}

std::string nextKey(const IniSection& section, const std::string& prefix) {
    std::size_t next = 0;
    const std::string want = lowerAscii(prefix);
    for (const auto& kv : section.entries) {
        const std::string key = lowerAscii(kv.key);
        if (key.rfind(want, 0) != 0) continue;
        const std::string suffix = key.substr(want.size());
        if (suffix.empty()) continue;
        bool ok = true;
        std::size_t value = 0;
        for (char ch : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) { ok = false; break; }
            value = value * 10 + static_cast<std::size_t>(ch - '0');
        }
        if (ok && value >= next) next = value + 1;
    }
    return prefix + std::to_string(next);
}

void addAssetIfRequested(PatchProject& project, bool copyBaselineAsset, const std::filesystem::path& baselineAsset, const std::string& patchFilename) {
    if (copyBaselineAsset && !baselineAsset.empty()) {
        project.assets.push_back({baselineAsset, patchFilename});
    }
}

void addNumberedEntry(PatchProject& project, const std::string& sectionName, const std::string& prefix, const std::string& value) {
    auto& section = project.section(sectionName);
    section.entries.push_back({nextKey(section, prefix), value});
}

std::vector<std::string> dataColumns(const neotabular::Table& table) {
    if (table.columns.empty()) return {};
    return std::vector<std::string>(table.columns.begin() + 1, table.columns.end());
}

bool tableRowsEqualAt(const neotabular::Table& a, const neotabular::Table& b, std::size_t row, std::size_t col) {
    return cellOrEmpty(a.rows[row], col) == cellOrEmpty(b.rows[row], col);
}

} // namespace

IniSection& PatchProject::section(const std::string& name) {
    for (auto& section : sections) {
        if (iequals(section.name, name)) return section;
    }
    sections.push_back({name, {}});
    return sections.back();
}

const IniSection* PatchProject::findSection(const std::string& name) const {
    for (const auto& section : sections) {
        if (iequals(section.name, name)) return &section;
    }
    return nullptr;
}

void PatchProject::add(const std::string& sectionName, std::string key, std::string value) {
    section(sectionName).entries.push_back({std::move(key), std::move(value)});
}

bool PatchProject::emptyInstructions() const {
    for (const auto& section : sections) {
        if (!section.entries.empty()) return false;
    }
    return assets.empty();
}

std::string sanitizeSectionName(std::string value) {
    for (char& ch : value) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (!std::isalnum(u) && ch != '_' && ch != '-' && ch != '.') ch = '_';
    }
    while (!value.empty() && value.front() == '_') value.erase(value.begin());
    while (!value.empty() && value.back() == '_') value.pop_back();
    if (value.empty()) return "section";
    return value;
}

std::string basenameForPatch(const std::filesystem::path& path) {
    std::string name = path.filename().string();
    if (name.empty()) throw std::runtime_error("Unable to infer a patch filename from an empty path.");
    return name;
}

std::string writeIniText(const PatchProject& project, bool includeSettings) {
    std::ostringstream out;
    out << "; Neo tool generated TSLPatcher/HoloPatcher instructions\r\n";
    out << "; This is an instruction file, not a binary delta.\r\n\r\n";
    if (includeSettings && !project.findSection("Settings")) {
        out << "[Settings]\r\nFileExists=1\r\n\r\n";
    }
    const std::vector<std::string> preferred = {"Settings", "TLKList", "InstallList", "2DAList", "GFFList", "CompileList", "SSFList"};
    std::set<std::string> emitted;
    auto emitSection = [&](const IniSection& section) {
        const std::string lname = lowerAscii(section.name);
        if (emitted.count(lname)) return;
        emitted.insert(lname);
        out << '[' << section.name << "]\r\n";
        for (const auto& kv : section.entries) {
            out << kv.key << '=' << kv.value << "\r\n";
        }
        out << "\r\n";
    };
    for (const auto& name : preferred) {
        if (const auto* section = project.findSection(name)) emitSection(*section);
        else if (includeSettings && name != "Settings") out << '[' << name << "]\r\n\r\n";
    }
    for (const auto& section : project.sections) emitSection(section);
    if (!project.unsupported.empty()) {
        out << "; Unsupported changes detected by generator:\r\n";
        for (const auto& item : project.unsupported) out << "; - " << item << "\r\n";
    }
    if (!project.warnings.empty()) {
        out << "; Warnings:\r\n";
        for (const auto& item : project.warnings) out << "; - " << item << "\r\n";
    }
    return out.str();
}

void writeIniFile(const PatchProject& project, const std::filesystem::path& path, bool includeSettings) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Unable to open changes.ini output: " + path.string());
    const std::string text = writeIniText(project, includeSettings);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) throw std::runtime_error("Unable to write changes.ini output: " + path.string());
}

void writePackage(const PatchProject& project, const std::filesystem::path& outputDir, bool includeSettings) {
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) throw std::runtime_error("Unable to create TSLPatcher package folder: " + outputDir.string() + ": " + ec.message());
    for (const auto& asset : project.assets) {
        if (asset.source.empty() || asset.targetName.empty()) continue;
        const std::filesystem::path target = outputDir / asset.targetName;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) throw std::runtime_error("Unable to create asset folder: " + target.parent_path().string() + ": " + ec.message());
        std::filesystem::copy_file(asset.source, target, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("Unable to stage asset " + asset.source.string() + " as " + target.string() + ": " + ec.message());
    }
    writeIniFile(project, outputDir / "changes.ini", includeSettings);
}

void writeFragment(const PatchProject& project, const std::filesystem::path& outputIni) {
    writeIniFile(project, outputIni, false);
}

void throwIfUnsupported(const PatchProject& project) {
    if (project.unsupported.empty()) return;
    std::ostringstream out;
    out << "TSLPatcher/HoloPatcher instruction generation found unsupported changes:";
    for (const auto& item : project.unsupported) out << "\n  - " << item;
    throw std::runtime_error(out.str());
}

void printReport(const PatchProject& project) {
    if (project.unsupported.empty() && project.warnings.empty()) return;
    if (!project.unsupported.empty()) {
        std::cerr << "Unsupported changes:\n";
        for (const auto& item : project.unsupported) std::cerr << "  - " << item << '\n';
    }
    if (!project.warnings.empty()) {
        std::cerr << "Warnings:\n";
        for (const auto& item : project.warnings) std::cerr << "  - " << item << '\n';
    }
}

PatchProject diffTwoDA(const neotabular::Table& original,
                       const neotabular::Table& modified,
                       const std::string& patchFilename,
                       bool copyBaselineAsset,
                       const std::filesystem::path& baselineAsset) {
    if (original.columns.empty() || modified.columns.empty()) throw std::runtime_error("2DA diff requires non-empty tables.");
    PatchProject project;
    project.add("2DAList", "Table0", patchFilename);
    project.section(patchFilename);
    addAssetIfRequested(project, copyBaselineAsset, baselineAsset, patchFilename);

    const auto origCols = dataColumns(original);
    const auto modCols = dataColumns(modified);
    const std::string stem = baseNameNoExt(patchFilename);

    const std::size_t commonCols = std::min(origCols.size(), modCols.size());
    for (std::size_t c = 0; c < commonCols; ++c) {
        if (!iequals(origCols[c], modCols[c])) {
            project.unsupported.push_back("2DA column rename/reorder at column " + std::to_string(c) + ": " + origCols[c] + " -> " + modCols[c]);
        }
    }
    if (origCols.size() > modCols.size()) {
        for (std::size_t c = modCols.size(); c < origCols.size(); ++c) {
            project.unsupported.push_back("2DA deleted column is not representable: " + origCols[c]);
        }
    }
    if (original.rows.size() > modified.rows.size()) {
        for (std::size_t r = modified.rows.size(); r < original.rows.size(); ++r) {
            project.unsupported.push_back("2DA deleted row is not representable at original row " + std::to_string(r));
        }
    }

    for (std::size_t c = origCols.size(); c < modCols.size(); ++c) {
        const std::string sectionName = uniqueSectionName(project, stem + "_col_" + modCols[c]);
        addNumberedEntry(project, patchFilename, "AddColumn", sectionName);
        project.add(sectionName, "ColumnLabel", modCols[c]);
        project.add(sectionName, "DefaultValue", "****");
        for (std::size_t r = 0; r < modified.rows.size(); ++r) {
            const std::string value = cellOrEmpty(modified.rows[r], c + 1);
            if (!value.empty() && value != "****") project.add(sectionName, "I" + std::to_string(r), value);
        }
    }

    const std::size_t rowCommon = std::min(original.rows.size(), modified.rows.size());
    for (std::size_t r = 0; r < rowCommon; ++r) {
        bool any = false;
        std::string sectionName;
        for (std::size_t c = 0; c < commonCols; ++c) {
            if (tableRowsEqualAt(original, modified, r, c + 1)) continue;
            if (!any) {
                any = true;
                sectionName = uniqueSectionName(project, stem + "_mod_row_" + std::to_string(r));
                addNumberedEntry(project, patchFilename, "ChangeRow", sectionName);
                project.add(sectionName, "RowIndex", std::to_string(r));
            }
            project.add(sectionName, modCols[c], cellOrEmpty(modified.rows[r], c + 1));
        }
        if (cellOrEmpty(original.rows[r], 0) != cellOrEmpty(modified.rows[r], 0)) {
            project.unsupported.push_back("2DA row label change at row " + std::to_string(r) + " is not a stock TSLPatcher row operation.");
        }
    }

    for (std::size_t r = original.rows.size(); r < modified.rows.size(); ++r) {
        const std::string sectionName = uniqueSectionName(project, stem + "_add_row_" + std::to_string(r));
        addNumberedEntry(project, patchFilename, "AddRow", sectionName);
        for (std::size_t c = 0; c < modCols.size(); ++c) {
            const std::string value = cellOrEmpty(modified.rows[r], c + 1);
            if (!value.empty() && value != "****") project.add(sectionName, modCols[c], value);
        }
    }
    return project;
}

} // namespace neotsl
