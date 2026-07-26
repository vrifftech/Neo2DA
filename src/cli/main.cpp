#include "core/Common.hpp"
#include "core/TwoDAFile.hpp"
#include "core/Version.hpp"
#include "TabularData.hpp"
#include "TslPatcher.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace {

using neo2da::TwoDAFile;

void printUsage(std::ostream& out) {
    out << "Neo2DA " << neo2da::kNeo2DAVersion << " C++ command-line utility\n"
        << "\n"
        << "Usage:\n"
        << "  neo2da-cli info <file.2da|file.gda>\n"
        << "  neo2da-cli dump <file.2da|file.gda> [filter-term]\n"
        << "  neo2da-cli search <file.2da|file.gda> <term>\n"
        << "  neo2da-cli export <file.2da|file.gda> <csv|tsv> <output> [filter-term]\n"
        << "  neo2da-cli import <input-table> <csv|tsv> <output.2da|output.gda>\n"
        << "  neo2da-cli diff-tslpatcher <original.2da> <modified-input> <output-dir|fragment.ini> [--modified-format csv|tsv|2da|native|auto] [--package|--fragment] [--filename name] [--allow-unsupported]\n"
        << "  neo2da-cli diff-tslpatcher-import <original.2da> <modified-input> <csv|tsv|2da|native|auto> <output-dir|fragment.ini> [--package|--fragment] [--filename name] [--allow-unsupported]\n"
        << "  neo2da-cli roundtrip <input.2da|input.gda> <output.2da|output.gda>\n"
        << "  neo2da-cli new <output.2da|output.gda> <column> [column...]\n"
        << "  neo2da-cli gda-hash <column-name> [column-name...]\n"
        << "  neo2da-cli add-row <input.2da|input.gda> <output.2da|output.gda> [label] [default-value]\n"
        << "  neo2da-cli add-column <input.2da|input.gda> <output.2da|output.gda> <label> [default-value]\n"
        << "  neo2da-cli clone-row <input.2da|input.gda> <output.2da|output.gda> <source-row> [new-label]\n"
        << "  neo2da-cli delete-row <input.2da|input.gda> <output.2da|output.gda> <row>\n"
        << "  neo2da-cli delete-column <input.2da|input.gda> <output.2da|output.gda> <column>\n"
        << "  neo2da-cli set-cell <input.2da|input.gda> <output.2da|output.gda> <row> <column> <value>\n"
        << "  neo2da-cli set-row-label <input.2da|input.gda> <output.2da|output.gda> <row> <label>\n"
        << "  neo2da-cli set-column-label <input.2da|input.gda> <output.2da|output.gda> <column> <label>\n"
        << "\n"
        << "Rows and columns are zero-based numeric indexes by default. Non-numeric\n"
        << "row/column arguments are resolved as case-insensitive labels. Use row:LABEL\n"
        << "or col:LABEL to force label lookup when the label is numeric.\n"
        << "The import command accepts CSV/TSV only. For patch diffs, 2da/native mean a native KotOR 2DA modified table;\n"
        << "auto selects csv/tsv/2da by extension and otherwise falls back to 2DA. XML/JSON are not supported by Neo2DA.\n"
        << "TSLPatcher/HoloPatcher [2DAList] output supports KotOR-style 2DA files only; Dragon Age GDA files remain editable but are not valid patcher inputs.\n";
}

std::size_t parseIndex(const std::string& text, const std::string& what) {
    const auto parsed = neo2da::parseUInt32Decimal(text);
    if (!parsed) {
        throw neo2da::TwoDAError("Invalid " + what + " index: " + text);
    }
    return static_cast<std::size_t>(*parsed);
}

std::size_t resolveRow(const TwoDAFile& table, const std::string& ref) {
    if (ref.rfind("row:", 0) == 0) {
        return table.rowIndexByLabel(ref.substr(4));
    }
    if (ref.rfind("label:", 0) == 0) {
        return table.rowIndexByLabel(ref.substr(6));
    }
    if (neo2da::isUnsignedDecimal(ref)) {
        return parseIndex(ref, "row");
    }
    return table.rowIndexByLabel(ref);
}

std::size_t resolveColumn(const TwoDAFile& table, const std::string& ref) {
    if (ref.rfind("col:", 0) == 0) {
        return table.columnIndexByLabel(ref.substr(4));
    }
    if (ref.rfind("column:", 0) == 0) {
        return table.columnIndexByLabel(ref.substr(7));
    }
    if (ref.rfind("label:", 0) == 0) {
        return table.columnIndexByLabel(ref.substr(6));
    }
    if (neo2da::isUnsignedDecimal(ref)) {
        return parseIndex(ref, "column");
    }
    return table.columnIndexByLabel(ref);
}

std::string escapeTsv(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\n': out += "\\n"; break;
        case '\\': out += "\\\\"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

void dumpTable(const TwoDAFile& table, const std::string& filter = {}) {
    const auto exported = filter.empty() ? table.toTable() : neotabular::filterRows(table.toTable(), filter);
    for (std::size_t c = 0; c < exported.columns.size(); ++c) {
        if (c) std::cout << '\t';
        std::cout << escapeTsv(exported.columns[c]);
    }
    std::cout << '\n';
    for (const auto& row : exported.rows) {
        for (std::size_t c = 0; c < row.size(); ++c) {
            if (c) std::cout << '\t';
            std::cout << escapeTsv(row[c]);
        }
        std::cout << '\n';
    }
}


std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string extensionFormatName(const std::filesystem::path& path) {
    std::string ext = lowerAscii(path.extension().string());
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    if (ext == "csv" || ext == "tsv" || ext == "xml" || ext == "json" || ext == "2da" || ext == "gda") return ext;
    return "2da";
}

bool isNativeTwoDAFormat(std::string formatName) {
    formatName = lowerAscii(std::move(formatName));
    return formatName == "2da" || formatName == "gda" || formatName == "dragonage" || formatName == "native";
}

neotabular::Format parseFlatTwoDAFormat(std::string formatName) {
    formatName = lowerAscii(std::move(formatName));
    if (formatName == "csv") return neotabular::Format::Csv;
    if (formatName == "tsv") return neotabular::Format::Tsv;
    if (formatName == "xml" || formatName == "json") {
        throw neo2da::TwoDAError("Neo2DA supports CSV or TSV table import/export; XML/JSON are not supported.");
    }
    if (formatName == "2da" || formatName == "gda" || formatName == "kotor" ||
        formatName == "dragonage" || formatName == "native" || formatName == "auto") {
        throw neo2da::TwoDAError("The standalone Neo2DA import command accepts CSV or TSV only. Use Open/Roundtrip for native 2DA/GDA files, or use native formats as modified inputs for diff-tslpatcher.");
    }
    throw neo2da::TwoDAError("Unsupported Neo2DA table format: " + formatName);
}

TwoDAFile loadTwoDAFromImport(const std::filesystem::path& path, std::string formatName) {
    formatName = lowerAscii(std::move(formatName));
    if (formatName.empty() || formatName == "auto") {
        formatName = extensionFormatName(path);
    }
    if (isNativeTwoDAFormat(formatName)) {
        return TwoDAFile(path);
    }
    const auto format = parseFlatTwoDAFormat(formatName);
    return TwoDAFile::fromTable(neotabular::readTable(path, format));
}

struct PatchOutputOptions {
    bool package = true;
    bool allowUnsupported = false;
    std::string patchFilename;
    std::string modifiedFormat = "auto";
};

PatchOutputOptions parsePatchOutputOptions(int argc, char** argv, int begin, const std::filesystem::path& original) {
    PatchOutputOptions options;
    options.patchFilename = neotsl::basenameForPatch(original);
    for (int i = begin; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--package") {
            options.package = true;
        } else if (arg == "--fragment") {
            options.package = false;
        } else if (arg == "--filename") {
            if (i + 1 >= argc) throw neo2da::TwoDAError("--filename requires a value.");
            options.patchFilename = argv[++i];
        } else if (arg == "--modified-format" || arg == "--input-format") {
            if (i + 1 >= argc) throw neo2da::TwoDAError(arg + " requires a value.");
            options.modifiedFormat = argv[++i];
        } else if (arg == "--allow-unsupported") {
            options.allowUnsupported = true;
        } else {
            throw neo2da::TwoDAError("Unknown diff-tslpatcher option: " + arg);
        }
    }
    return options;
}

void requireKotORPatcherTwoDA(const TwoDAFile& table, const std::string& role) {
    if (table.isGda()) {
        throw neo2da::TwoDAError(
            role + " is a Dragon Age GDA file. TSLPatcher/HoloPatcher [2DAList] supports KotOR-style 2DA files only; "
                   "use Neo2DA's native GDA save or CSV/TSV interchange instead.");
    }
}

void writePatchOutput(const neotsl::PatchProject& project, const std::filesystem::path& output, const PatchOutputOptions& options) {
    if (!options.allowUnsupported) neotsl::throwIfUnsupported(project);
    else neotsl::printReport(project);
    if (options.package) neotsl::writePackage(project, output, true);
    else neotsl::writeFragment(project, output);
}

std::vector<std::string> collectArgs(int argc, char** argv, int begin) {
    std::vector<std::string> out;
    for (int i = begin; i < argc; ++i) {
        out.emplace_back(argv[i]);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            printUsage(std::cerr);
            return 1;
        }

        const std::string command = argv[1];

        if (command == "help" || command == "--help" || command == "-h") {
            printUsage(std::cout);
            return 0;
        }
        if (command == "version" || command == "--version" || command == "-v") {
            std::cout << "Neo2DA " << neo2da::kNeo2DAVersion << '\n';
            return 0;
        }

        if (command == "info") {
            if (argc != 3) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            std::cout << "Type: " << table.nativeFormatName() << '\n'
                      << "Rows: " << table.rowCount() << '\n'
                      << "Columns: " << table.columnCount() << '\n';
            return 0;
        }

        if (command == "dump") {
            if (argc < 3 || argc > 4) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            dumpTable(table, argc == 4 ? std::string(argv[3]) : std::string{});
            return 0;
        }

        if (command == "search") {
            if (argc != 4) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            dumpTable(table, argv[3]);
            return 0;
        }

        if (command == "export") {
            if (argc < 5 || argc > 6) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            auto exported = table.toTable();
            if (argc == 6) {
                exported = neotabular::filterRows(exported, argv[5]);
            }
            const auto format = parseFlatTwoDAFormat(argv[3]);
            neotabular::writeTable(exported, argv[4], format);
            return 0;
        }

        if (command == "import") {
            if (argc != 5) {
                printUsage(std::cerr);
                return 1;
            }
            const auto format = parseFlatTwoDAFormat(argv[3]);
            TwoDAFile table = TwoDAFile::fromTable(neotabular::readTable(argv[2], format));
            table.save(argv[4]);
            return 0;
        }


        if (command == "diff-tslpatcher" || command == "diff-tslpatcher-import") {
            if ((command == "diff-tslpatcher" && argc < 5) || (command == "diff-tslpatcher-import" && argc < 6)) {
                printUsage(std::cerr);
                return 1;
            }
            const std::filesystem::path originalPath = argv[2];
            const std::filesystem::path modifiedPath = argv[3];
            int optionsBegin = 5;
            PatchOutputOptions options;
            if (command == "diff-tslpatcher-import") {
                const std::filesystem::path output = argv[5];
                options = parsePatchOutputOptions(argc, argv, 6, originalPath);
                options.modifiedFormat = argv[4];
                TwoDAFile original(originalPath);
                TwoDAFile modified = loadTwoDAFromImport(modifiedPath, options.modifiedFormat);
                requireKotORPatcherTwoDA(original, "The original patch baseline");
                requireKotORPatcherTwoDA(modified, "The modified patch input");
                auto project = neotsl::diffTwoDA(original.toTable(), modified.toTable(), options.patchFilename, options.package, originalPath);
                writePatchOutput(project, output, options);
                return 0;
            }
            const std::filesystem::path output = argv[4];
            options = parsePatchOutputOptions(argc, argv, optionsBegin, originalPath);
            TwoDAFile original(originalPath);
            TwoDAFile modified = loadTwoDAFromImport(modifiedPath, options.modifiedFormat);
            requireKotORPatcherTwoDA(original, "The original patch baseline");
            requireKotORPatcherTwoDA(modified, "The modified patch input");
            auto project = neotsl::diffTwoDA(original.toTable(), modified.toTable(), options.patchFilename, options.package, originalPath);
            writePatchOutput(project, output, options);
            return 0;
        }


        if (command == "gda-hash" || command == "hash-gda-column") {
            if (argc < 3) {
                printUsage(std::cerr);
                return 1;
            }
            for (int i = 2; i < argc; ++i) {
                const std::string name = argv[i];
                const std::uint32_t hash = neo2da::gdaColumnHashForName(name);
                std::cout << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << hash
                          << std::nouppercase << std::dec << std::setfill(' ') << '\t' << name << '\n';
            }
            return 0;
        }

        if (command == "roundtrip") {
            if (argc != 4) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            table.save(argv[3]);
            return 0;
        }

        if (command == "new") {
            if (argc < 4) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table = TwoDAFile::create(collectArgs(argc, argv, 3), 1);
            table.save(argv[2]);
            return 0;
        }

        if (command == "add-row") {
            if (argc < 4 || argc > 6) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            const std::string label = argc >= 5 ? std::string(argv[4]) : std::string{};
            const std::string defaultValue = argc >= 6 ? std::string(argv[5]) : std::string("****");
            table.addRow(label, defaultValue);
            table.save(argv[3]);
            return 0;
        }

        if (command == "add-column") {
            if (argc < 5 || argc > 6) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            const std::string defaultValue = argc >= 6 ? std::string(argv[5]) : std::string("****");
            table.addColumn(argv[4], defaultValue);
            table.save(argv[3]);
            return 0;
        }

        if (command == "clone-row") {
            if (argc < 5 || argc > 6) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            const std::size_t source = resolveRow(table, argv[4]);
            const std::string label = argc >= 6 ? std::string(argv[5]) : std::string{};
            table.cloneRow(source, label);
            table.save(argv[3]);
            return 0;
        }

        if (command == "delete-row" || command == "remove-row") {
            if (argc != 5) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            table.removeRow(resolveRow(table, argv[4]));
            table.save(argv[3]);
            return 0;
        }

        if (command == "delete-column" || command == "remove-column") {
            if (argc != 5) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            table.removeColumn(resolveColumn(table, argv[4]));
            table.save(argv[3]);
            return 0;
        }

        if (command == "set-cell") {
            if (argc != 7) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            table.setCell(resolveRow(table, argv[4]), resolveColumn(table, argv[5]), argv[6]);
            table.save(argv[3]);
            return 0;
        }

        if (command == "set-row-label") {
            if (argc != 6) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            table.setRowLabel(resolveRow(table, argv[4]), argv[5]);
            table.save(argv[3]);
            return 0;
        }

        if (command == "set-column-label") {
            if (argc != 6) {
                printUsage(std::cerr);
                return 1;
            }
            TwoDAFile table(argv[2]);
            table.setColumnLabel(resolveColumn(table, argv[4]), argv[5]);
            table.save(argv[3]);
            return 0;
        }

        printUsage(std::cerr);
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << '\n';
        return 2;
    }
}
