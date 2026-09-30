#include "formats/W3R.hpp"

#include "util/BinaryReader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace w3shadow {

W3RParseResult parseW3R(const std::span<const std::byte> bytes)
{
    try {
        BinaryReader reader(bytes, "W3R");
        const auto version = reader.readU32("version");
        if (version != 5U && version != 7U) {
            throw std::runtime_error("W3R parse error: unsupported version " + std::to_string(version));
        }
        const auto count = reader.readU32("region count");
        if (count > 1000000U) throw std::runtime_error("W3R parse error: region count exceeds safety limit");
        W3RParseResult result;
        result.regions.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            MapRegion region;
            region.left = reader.readF32("left");
            region.right = reader.readF32("right");
            region.bottom = reader.readF32("bottom");
            region.top = reader.readF32("top");
            if (!std::isfinite(region.left) || !std::isfinite(region.right) ||
                !std::isfinite(region.bottom) || !std::isfinite(region.top)) {
                throw std::runtime_error("W3R parse error: non-finite region bounds");
            }
            if (region.left > region.right) std::swap(region.left, region.right);
            if (region.bottom > region.top) std::swap(region.bottom, region.top);
            region.name = reader.readCString("name");
            region.creationNumber = reader.readU32("creation number");
            reader.skip(4U, "weather rawcode");
            static_cast<void>(reader.readCString("ambient sound"));
            reader.skip(4U, "editor color");
            if (version >= 7U) reader.skip(8U, "version 7 region extension");
            result.regions.push_back(std::move(region));
        }
        return result;
    } catch (const std::exception& error) {
        return {{}, error.what()};
    }
}

bool isIgnoreShadowRegion(const std::string_view name)
{
    constexpr std::string_view prefix = "ignoreshadow";
    if (name.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(name[index]))) != prefix[index]) return false;
    }
    return true;
}

} // namespace w3shadow
