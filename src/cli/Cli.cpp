#include "cli/Cli.hpp"

#include "archive/MapArchive.hpp"
#include "formats/Png.hpp"
#include "formats/W3E.hpp"
#include "shadow/Pattern.hpp"
#include "shadow/ShadowMap.hpp"
#include "util/FileIO.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace w3shadow {
namespace {

enum class ExitCode : int {
    Success = 0,
    InvalidArguments = 1,
    MapOpenError = 2,
    MalformedData = 3,
    GenerationFailure = 5,
    SaveFailure = 6
};

struct Options {
    std::optional<std::filesystem::path> output;
    std::optional<std::filesystem::path> png;
    std::optional<std::filesystem::path> exportShadow;
    std::optional<std::uint32_t> mapWidth;
    std::optional<std::uint32_t> mapHeight;
    std::optional<Pattern> pattern;
    bool inPlace = false;
    bool force = false;
};

void printHelp()
{
    std::cout <<
        "w3shadow 0.2.0 - Warcraft III shadow-map validation tool\n\n"
        "Commands:\n"
        "  pattern --map-width W --map-height H --pattern NAME --output FILE [--png FILE] [--force]\n"
        "  inspect MAP [--export-shadow FILE] [--png FILE] [--force]\n"
        "  inspect-shadow SHD --map-width W --map-height H [--png FILE] [--force]\n"
        "  replace-shd MAP SHD [--output MAP | --in-place] [--force]\n\n"
        "replace-shd defaults to <name>.shadowed.<extension>; --in-place keeps a backup.\n"
        "Patterns: black, white, checker, x-gradient, y-gradient, quadrants\n";
}

std::uint32_t parsePositiveU32(const std::string_view text, const std::string_view option)
{
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0) {
        throw std::invalid_argument(std::string(option) + " requires a positive integer");
    }
    return value;
}

Options parseOptions(const int argc, char* argv[], const int first)
{
    Options options;
    for (int index = first; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto requireValue = [&](const std::string_view option) -> std::string_view {
            if (index + 1 >= argc) throw std::invalid_argument(std::string(option) + " requires a value");
            return argv[++index];
        };

        if (argument == "--output") {
            options.output = std::filesystem::path(requireValue(argument));
        } else if (argument == "--png") {
            options.png = std::filesystem::path(requireValue(argument));
        } else if (argument == "--export-shadow") {
            options.exportShadow = std::filesystem::path(requireValue(argument));
        } else if (argument == "--map-width") {
            options.mapWidth = parsePositiveU32(requireValue(argument), argument);
        } else if (argument == "--map-height") {
            options.mapHeight = parsePositiveU32(requireValue(argument), argument);
        } else if (argument == "--pattern") {
            options.pattern = parsePattern(requireValue(argument));
            if (!options.pattern) throw std::invalid_argument("unknown pattern name");
        } else if (argument == "--in-place") {
            options.inPlace = true;
        } else if (argument == "--force") {
            options.force = true;
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    return options;
}

std::uint64_t expectedShadowBytes(const W3EInfo& info)
{
    return static_cast<std::uint64_t>(info.tileWidth) * info.tileHeight * 16U;
}

W3EInfo readMapDimensions(const MapArchive& archive)
{
    const auto terrain = archive.read("war3map.w3e");
    const auto parsed = parseW3E(terrain);
    if (!parsed) throw std::runtime_error(parsed.error);
    return parsed.info;
}

bool samePath(const std::filesystem::path& left, const std::filesystem::path& right)
{
    std::error_code leftError;
    std::error_code rightError;
    const auto normalizedLeft = std::filesystem::weakly_canonical(left, leftError);
    const auto normalizedRight = std::filesystem::weakly_canonical(right, rightError);
    return !leftError && !rightError && normalizedLeft == normalizedRight;
}

void writePreview(
    const std::filesystem::path& path,
    const std::uint32_t widthTiles,
    const std::uint32_t heightTiles,
    const std::span<const std::byte> bytes,
    const bool force)
{
    const auto png = makeShadowPreviewPng(widthTiles * 4U, heightTiles * 4U, bytes);
    writeBinaryFileAtomic(path, png, force);
}

int commandPattern(const int argc, char* argv[])
{
    const auto options = parseOptions(argc, argv, 2);
    if (!options.mapWidth || !options.mapHeight || !options.pattern || !options.output) {
        throw std::invalid_argument("pattern requires --map-width, --map-height, --pattern, and --output");
    }
    if (options.inPlace || options.exportShadow) {
        throw std::invalid_argument("pattern received an option that belongs to another command");
    }
    if (options.png && samePath(*options.output, *options.png)) {
        throw std::invalid_argument("--output and --png must name different files");
    }

    ShadowMap shadow(*options.mapWidth, *options.mapHeight);
    generatePattern(shadow, *options.pattern);
    writeBinaryFileAtomic(*options.output, shadow.bytes(), options.force);
    if (options.png) {
        writePreview(*options.png, shadow.widthTiles(), shadow.heightTiles(), shadow.bytes(), options.force);
    }
    std::cout << "Wrote " << shadow.bytes().size() << " SHD bytes ("
              << shadow.widthPixels() << 'x' << shadow.heightPixels() << ") to "
              << options.output->string() << '\n';
    return static_cast<int>(ExitCode::Success);
}

int commandInspect(const int argc, char* argv[])
{
    if (argc < 3) throw std::invalid_argument("inspect requires a map path");
    const std::filesystem::path mapPath(argv[2]);
    const auto options = parseOptions(argc, argv, 3);
    if (options.output || options.mapWidth || options.mapHeight || options.pattern || options.inPlace) {
        throw std::invalid_argument("inspect received an option that belongs to another command");
    }
    MapArchive archive(mapPath);
    const auto info = readMapDimensions(archive);

    std::cout << "Map: " << mapPath.string() << '\n'
              << "W3E version: " << info.version << '\n'
              << "Tileset: " << info.tileset << '\n'
              << "Terrain: " << info.tileWidth << 'x' << info.tileHeight << " tiles ("
              << info.vertexWidth << 'x' << info.vertexHeight << " vertices)\n"
              << "Expected SHD: " << info.tileWidth * 4U << 'x' << info.tileHeight * 4U
              << " pixels, " << expectedShadowBytes(info) << " bytes\n";

    std::optional<std::vector<std::byte>> shadow;
    if (archive.contains("war3map.shd")) {
        shadow = archive.read("war3map.shd");
        std::cout << "Existing SHD: " << shadow->size() << " bytes"
                  << (shadow->size() == expectedShadowBytes(info) ? " (size matches)\n" : " (SIZE MISMATCH)\n");
    } else {
        std::cout << "Existing SHD: not present\n";
    }

    const auto files = archive.listFiles();
    std::cout << "Known archive files (" << files.size() << "):\n";
    for (const auto& file : files) std::cout << "  " << file << '\n';

    if ((options.exportShadow || options.png) && !shadow) {
        throw std::runtime_error("the map has no war3map.shd to export");
    }
    if (options.exportShadow && options.png && samePath(*options.exportShadow, *options.png)) {
        throw std::invalid_argument("--export-shadow and --png must name different files");
    }
    if (options.exportShadow) writeBinaryFileAtomic(*options.exportShadow, *shadow, options.force);
    if (options.png) writePreview(*options.png, info.tileWidth, info.tileHeight, *shadow, options.force);
    return static_cast<int>(ExitCode::Success);
}

int commandInspectShadow(const int argc, char* argv[])
{
    if (argc < 3) throw std::invalid_argument("inspect-shadow requires an SHD path");
    const std::filesystem::path shadowPath(argv[2]);
    const auto options = parseOptions(argc, argv, 3);
    if (!options.mapWidth || !options.mapHeight) {
        throw std::invalid_argument("inspect-shadow requires --map-width and --map-height");
    }
    if (options.output || options.exportShadow || options.pattern || options.inPlace) {
        throw std::invalid_argument("inspect-shadow received an option that belongs to another command");
    }
    const auto shadow = readBinaryFile(shadowPath);
    ShadowMap expected(*options.mapWidth, *options.mapHeight);
    if (shadow.size() != expected.bytes().size()) {
        throw std::runtime_error("SHD size does not match the supplied map dimensions");
    }
    std::cout << "SHD: " << shadowPath.string() << '\n'
              << "Dimensions: " << expected.widthPixels() << 'x' << expected.heightPixels() << '\n'
              << "Bytes: " << shadow.size() << " (size matches)\n";
    if (options.png) writePreview(*options.png, *options.mapWidth, *options.mapHeight, shadow, options.force);
    return static_cast<int>(ExitCode::Success);
}

int commandReplace(const int argc, char* argv[])
{
    if (argc < 4) throw std::invalid_argument("replace-shd requires a map path and an SHD path");
    const std::filesystem::path mapPath(argv[2]);
    const std::filesystem::path shadowPath(argv[3]);
    auto options = parseOptions(argc, argv, 4);
    if (options.inPlace && options.output) {
        throw std::invalid_argument("choose only one of --output or --in-place");
    }
    if (options.inPlace && options.force) {
        throw std::invalid_argument("--force is not used with --in-place");
    }
    if (options.png || options.exportShadow || options.mapWidth || options.mapHeight || options.pattern) {
        throw std::invalid_argument("replace-shd received an option that belongs to another command");
    }
    if (!options.inPlace && !options.output) {
        const auto extension = mapPath.extension();
        options.output = mapPath.parent_path() /
                         (mapPath.stem().string() + ".shadowed" + extension.string());
    }

    const auto shadow = readBinaryFile(shadowPath);
    W3EInfo info{};
    {
        const MapArchive source(mapPath);
        info = readMapDimensions(source);
    }
    if (shadow.size() != expectedShadowBytes(info)) {
        throw std::runtime_error("SHD byte count does not match the map's W3E dimensions");
    }

    if (options.inPlace) {
        const auto backup = MapArchive::replaceShadowInPlace(mapPath, shadow);
        std::cout << "Replaced war3map.shd in " << mapPath.string()
                  << "\nBackup: " << backup.string() << '\n';
    } else {
        MapArchive::replaceShadowInCopy(mapPath, *options.output, shadow, options.force);
        std::cout << "Wrote validated map copy: " << options.output->string() << '\n';
    }
    return static_cast<int>(ExitCode::Success);
}

} // namespace

int runCli(const int argc, char* argv[])
{
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
        printHelp();
        return argc < 2 ? static_cast<int>(ExitCode::InvalidArguments) : 0;
    }

    try {
        const std::string_view command(argv[1]);
        if (command == "pattern") return commandPattern(argc, argv);
        if (command == "inspect") return commandInspect(argc, argv);
        if (command == "inspect-shadow") return commandInspectShadow(argc, argv);
        if (command == "replace-shd") return commandReplace(argc, argv);
        throw std::invalid_argument("unknown command: " + std::string(command));
    } catch (const std::invalid_argument& error) {
        std::cerr << "ERROR: " << error.what() << "\n\n";
        printHelp();
        return static_cast<int>(ExitCode::InvalidArguments);
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return static_cast<int>(ExitCode::SaveFailure);
    }
}

} // namespace w3shadow

