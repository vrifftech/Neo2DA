#include "BrowserOpenSelection.hpp"

#include "core/Common.hpp"

#include <stdexcept>
#include <string>

namespace neo2da::browseropen {
namespace {

bool isGdaColumnDictionary(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name == "gda_column_names.tsv" || name == "gda_column_names.csv";
}

} // namespace

std::filesystem::path selectTablePath(
    const std::vector<std::filesystem::path>& importedPaths) {
    std::filesystem::path tablePath;
    bool hasDictionary = false;

    for (const auto& path : importedPaths) {
        if (isGdaColumnDictionary(path)) {
            hasDictionary = true;
            continue;
        }

        const std::string extension = lowerAscii(path.extension().string());
        if (extension != ".2da" && extension != ".gda") {
            throw std::runtime_error(
                "Select one .2da or .gda table. The only additional files accepted are "
                "gda_column_names.tsv and gda_column_names.csv (using those exact names).");
        }
        if (!tablePath.empty()) {
            throw std::runtime_error("Select exactly one 2DA/GDA table per Open operation.");
        }
        tablePath = path;
    }

    if (tablePath.empty()) {
        throw std::runtime_error("No .2da or .gda table was selected.");
    }
    if (hasDictionary && lowerAscii(tablePath.extension().string()) != ".gda") {
        throw std::runtime_error(
            "GDA column-name dictionaries may only accompany a .gda table.");
    }
    return tablePath;
}

} // namespace neo2da::browseropen
