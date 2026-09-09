#pragma once
#include "TwoDAFile.hpp"
#include <neoshared/ResourceDocument.hpp>
namespace neo2da {
// Archive provenance is separate from TwoDAFile::filename(), which is empty
// until Save As commits a working copy. This prevents accidental archive writes.
struct ResourceDocument {
    neoshared::ResourceDocument source;
    TwoDAFile table;
    static ResourceDocument load(neoshared::ResourceDocument input);
    void checkSaveDestination(const std::filesystem::path& path) const;
};
void checkProtectedOutput(const std::filesystem::path& destination,
                          const std::vector<std::filesystem::path>& protectedInputs);
void checkDetachedDestination(const std::filesystem::path& destination,
                              const std::vector<std::filesystem::path>& protectedInputs);
}
