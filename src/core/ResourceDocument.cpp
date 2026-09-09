#include "ResourceDocument.hpp"
#include <algorithm>
#include <cctype>
#include <system_error>
namespace neo2da {
namespace {
std::filesystem::path normalized(const std::filesystem::path& p) {
    std::error_code ec;
    auto result=std::filesystem::weakly_canonical(std::filesystem::absolute(p),ec);
    if(ec) throw TwoDAError("Unable to check output path: " + p.string());
    return result;
}
}
void checkDetachedDestination(const std::filesystem::path& destination,
                              const std::vector<std::filesystem::path>& inputs) {
    if(destination.empty()) throw TwoDAError("Choose a separate output file for the archive resource.");
    auto ext=destination.extension().string();
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(ext!=".2da" && ext!=".gda") throw TwoDAError("Save the table as .2da or .gda, not as an archive.");
    checkProtectedOutput(destination,inputs);
}
void checkProtectedOutput(const std::filesystem::path& destination,
                          const std::vector<std::filesystem::path>& inputs) {
    if(destination.empty())throw TwoDAError("Choose an output filename.");
    const auto out=normalized(destination);
    for(const auto& source:inputs) {
        if(source.empty())continue;
        const auto input=normalized(source);
        std::error_code ec;
        const bool alias=std::filesystem::equivalent(destination,source,ec);
        if(out==input || (!ec && alias)) throw TwoDAError("The destination is a source archive or KEY (or an alias). Choose a separate table file.");
    }
}
ResourceDocument ResourceDocument::load(neoshared::ResourceDocument input) {
    if(input.identity.empty())throw TwoDAError("Archive resource has no source identity.");
    if(input.type!=2017)throw TwoDAError("This editor accepts 2DA archive resources only.");
    ResourceDocument result;
    result.table.loadBytes(input.bytes);
    result.source=std::move(input);
    return result;
}
void ResourceDocument::checkSaveDestination(const std::filesystem::path& path) const {
    checkDetachedDestination(path,source.protectedInputs);
}
}
