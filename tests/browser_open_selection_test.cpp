#include "wx/BrowserOpenSelection.hpp"

#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Paths = std::vector<std::filesystem::path>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void requireThrows(const Paths& paths, const std::string& label) {
    try {
        (void)neo2da::browseropen::selectTablePath(paths);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(label + " did not reject an invalid selection");
}

} // namespace

int main() {
    try {
        require(neo2da::browseropen::selectTablePath({"appearance.2da"}) ==
                    std::filesystem::path("appearance.2da"),
                "single 2DA selection failed");
        require(neo2da::browseropen::selectTablePath({
                    "/tmp/import/table.gda", "/tmp/import/gda_column_names.tsv"}) ==
                    std::filesystem::path("/tmp/import/table.gda"),
                "GDA plus TSV dictionary selection failed");
        require(neo2da::browseropen::selectTablePath({
                    "/tmp/import/gda_column_names.csv", "/tmp/import/TABLE.GDA"}) ==
                    std::filesystem::path("/tmp/import/TABLE.GDA"),
                "GDA plus CSV dictionary selection failed");
        requireThrows({}, "empty selection");
        requireThrows({"gda_column_names.tsv"}, "dictionary-only selection");
        requireThrows({"table.2da", "gda_column_names.tsv"}, "2DA plus dictionary");
        requireThrows({"one.gda", "two.gda"}, "multiple table selection");
        requireThrows({"one.gda", "notes.tsv"}, "unrecognized sidecar");
        requireThrows({"one.gda", "GDA_COLUMN_NAMES.TSV"}, "case-mismatched sidecar");
        std::cout << "Neo2DA browser-open selection tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
