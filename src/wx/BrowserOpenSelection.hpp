#pragma once

#include <filesystem>
#include <vector>

namespace neo2da::browseropen {

// Selects exactly one .2da/.gda table from a browser multi-file import. A GDA
// may be accompanied by the conventional gda_column_names.tsv/.csv sidecars;
// every selected file is imported into the same process-local directory.
std::filesystem::path selectTablePath(
    const std::vector<std::filesystem::path>& importedPaths);

} // namespace neo2da::browseropen
