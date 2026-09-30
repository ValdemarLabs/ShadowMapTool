#include "formats/DOO.hpp"

#include "util/BinaryReader.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace w3shadow {
namespace {

DOOParseResult parseImpl(const std::span<const std::byte> bytes)
{
    BinaryReader reader(bytes, "DOO");
    if (reader.readTag("signature") != "W3do") {
        throw std::runtime_error("DOO parse error: invalid signature");
    }
    DOOParseResult result;
    result.version = reader.readU32("format version");
    result.subversion = reader.readU32("format subversion");
    if (result.version != 7U && result.version != 8U && result.version != 13U) {
        throw std::runtime_error("DOO parse error: unsupported format version " +
                                 std::to_string(result.version));
    }
    const auto count = reader.readU32("doodad count");
    if (count > 10000000U) throw std::runtime_error("DOO parse error: doodad count exceeds safety limit");
    result.placements.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        DoodadPlacement placement;
        placement.rawcode = reader.readTag("doodad rawcode");
        placement.variation = reader.readU32("variation");
        placement.position = {reader.readF32("position X"), reader.readF32("position Y"),
                              reader.readF32("position Z")};
        placement.rotation = reader.readF32("rotation");
        placement.scale = {reader.readF32("scale X"), reader.readF32("scale Y"),
                           reader.readF32("scale Z")};
        placement.skinRawcode = result.version >= 8U
            ? reader.readTag("skin rawcode")
            : placement.rawcode;
        if (result.version >= 13U) placement.groupId = reader.readI32("group ID");
        placement.flags = reader.readU8("flags");
        placement.life = reader.readU8("life percentage");
        static_cast<void>(reader.readI32("item table pointer"));
        const auto setCount = reader.readU32("item set count");
        if (setCount > 100000U) throw std::runtime_error("DOO parse error: item set count exceeds safety limit");
        for (std::uint32_t set = 0; set < setCount; ++set) {
            const auto itemCount = reader.readU32("item count");
            if (itemCount > 1000000U ||
                static_cast<std::uint64_t>(itemCount) * 8U > reader.remaining()) {
                throw std::runtime_error("DOO parse error: invalid dropped-item count");
            }
            reader.skip(static_cast<std::size_t>(itemCount) * 8U, "dropped items");
        }
        if (result.version >= 13U) placement.color = reader.readU32("vertex color");
        placement.editorId = reader.readU32("editor ID");
        if (result.version >= 13U) {
            placement.roll = reader.readF32("roll");
            placement.pitch = reader.readF32("pitch");
            placement.lightCount = reader.readU32("light count");
            constexpr std::size_t lightRecordSize = 36U;
            if (placement.lightCount > 100000U ||
                static_cast<std::uint64_t>(placement.lightCount) * lightRecordSize > reader.remaining()) {
                throw std::runtime_error("DOO parse error: invalid light count");
            }
            reader.skip(static_cast<std::size_t>(placement.lightCount) * lightRecordSize,
                        "doodad lights");
        }
        const auto finite = [](const float value) { return std::isfinite(value); };
        if (!finite(placement.position.x) || !finite(placement.position.y) ||
            !finite(placement.position.z) || !finite(placement.rotation) ||
            !finite(placement.scale.x) || !finite(placement.scale.y) ||
            !finite(placement.scale.z) || !finite(placement.roll) || !finite(placement.pitch)) {
            throw std::runtime_error("DOO parse error: placement contains non-finite transform values");
        }
        result.placements.push_back(std::move(placement));
    }
    if (!reader.empty()) {
        result.specialVersion = reader.readU32("special doodad version");
        const auto specialCount = reader.readU32("special doodad count");
        constexpr std::size_t specialRecordSize = 16U;
        if (specialCount > 10000000U ||
            static_cast<std::uint64_t>(specialCount) * specialRecordSize > reader.remaining()) {
            throw std::runtime_error("DOO parse error: invalid special doodad count");
        }
        result.specialPlacements.reserve(specialCount);
        for (std::uint32_t index = 0; index < specialCount; ++index) {
            SpecialDoodadPlacement placement;
            placement.rawcode = reader.readTag("special doodad rawcode");
            placement.variation = reader.readU32("special doodad variation");
            placement.tileX = reader.readI32("special doodad tile X");
            placement.tileY = reader.readI32("special doodad tile Y");
            result.specialPlacements.push_back(std::move(placement));
        }
        result.trailingBytes = reader.remaining();
    }
    return result;
}

} // namespace

DOOParseResult parseDOO(const std::span<const std::byte> bytes)
{
    try {
        return parseImpl(bytes);
    } catch (const std::exception& error) {
        DOOParseResult result;
        result.error = error.what();
        return result;
    }
}

} // namespace w3shadow
