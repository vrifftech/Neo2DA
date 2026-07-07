#pragma once

#include "TabularData.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace neotsl {

struct KeyValue {
    std::string key;
    std::string value;
};

struct IniSection {
    std::string name;
    std::vector<KeyValue> entries;
};

struct StagedAsset {
    std::filesystem::path source;
    std::string targetName;
};

struct PatchProject {
    std::vector<IniSection> sections;
    std::vector<StagedAsset> assets;
    std::vector<std::string> warnings;
    std::vector<std::string> unsupported;

    IniSection& section(const std::string& name);
    const IniSection* findSection(const std::string& name) const;
    void add(const std::string& sectionName, std::string key, std::string value);
    bool emptyInstructions() const;
};

struct DiffOptions {
    std::string patchFilename;
    std::string destination = "override";
    bool includeSettings = true;
    bool copyBaselineAsset = true;
    bool failOnUnsupported = true;
};

std::string sanitizeSectionName(std::string value);
std::string basenameForPatch(const std::filesystem::path& path);
std::string writeIniText(const PatchProject& project, bool includeSettings = true);
void writeIniFile(const PatchProject& project, const std::filesystem::path& path, bool includeSettings = true);
void writePackage(const PatchProject& project, const std::filesystem::path& outputDir, bool includeSettings = true);
void writeFragment(const PatchProject& project, const std::filesystem::path& outputIni);
void throwIfUnsupported(const PatchProject& project);
void printReport(const PatchProject& project);

PatchProject diffTwoDA(const neotabular::Table& original,
                       const neotabular::Table& modified,
                       const std::string& patchFilename,
                       bool copyBaselineAsset = true,
                       const std::filesystem::path& baselineAsset = {});

} // namespace neotsl
