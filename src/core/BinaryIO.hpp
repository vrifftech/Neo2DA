#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>

namespace neo2da::detail {

inline std::string readBytes(std::istream& is, std::size_t size, const char* what = "bytes") {
    std::string data(size, '\0');
    if (size > 0) {
        is.read(&data[0], static_cast<std::streamsize>(size));
    }
    if (!is && size > 0) {
        throw std::runtime_error(std::string("Unexpected end of file while reading ") + what);
    }
    return data;
}

inline void writeBytes(std::ostream& os, const void* data, std::size_t size) {
    if (size > 0) {
        os.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    if (!os) {
        throw std::runtime_error("Unable to write bytes");
    }
}

inline void writeString(std::ostream& os, const std::string& text) {
    writeBytes(os, text.data(), text.size());
}

inline std::uint16_t readUInt16LE(std::istream& is, const char* what = "uint16") {
    unsigned char bytes[2]{};
    is.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!is) {
        throw std::runtime_error(std::string("Unexpected end of file while reading ") + what);
    }
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8u);
}

inline std::uint32_t readUInt32LE(std::istream& is, const char* what = "uint32") {
    unsigned char bytes[4]{};
    is.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!is) {
        throw std::runtime_error(std::string("Unexpected end of file while reading ") + what);
    }
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

inline void writeUInt16LE(std::ostream& os, std::uint16_t value) {
    const unsigned char bytes[2] = {
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8u) & 0xffu),
    };
    writeBytes(os, bytes, sizeof(bytes));
}

inline void writeUInt32LE(std::ostream& os, std::uint32_t value) {
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8u) & 0xffu),
        static_cast<unsigned char>((value >> 16u) & 0xffu),
        static_cast<unsigned char>((value >> 24u) & 0xffu),
    };
    writeBytes(os, bytes, sizeof(bytes));
}

inline std::uint64_t fileSize(std::istream& is) {
    const auto here = is.tellg();
    if (here < std::streampos(0)) {
        throw std::runtime_error("Unable to determine stream position");
    }
    is.seekg(0, std::ios::end);
    const auto end = is.tellg();
    if (end < std::streampos(0)) {
        throw std::runtime_error("Unable to determine file size");
    }
    is.seekg(here, std::ios::beg);
    return static_cast<std::uint64_t>(end);
}

inline std::uint64_t tellg(std::istream& is) {
    const auto pos = is.tellg();
    if (pos < std::streampos(0)) {
        throw std::runtime_error("Unable to determine stream read position");
    }
    return static_cast<std::uint64_t>(pos);
}

inline std::uint64_t tellp(std::ostream& os) {
    const auto pos = os.tellp();
    if (pos < std::streampos(0)) {
        throw std::runtime_error("Unable to determine stream write position");
    }
    return static_cast<std::uint64_t>(pos);
}

} // namespace neo2da::detail
