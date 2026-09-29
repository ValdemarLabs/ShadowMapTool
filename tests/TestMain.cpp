#include "archive/MapArchive.hpp"
#include "formats/Png.hpp"
#include "formats/W3E.hpp"
#include "shadow/Pattern.hpp"
#include "shadow/ShadowMap.hpp"
#include "util/FileIO.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void appendU32(std::vector<std::byte>& bytes, const std::uint32_t value)
{
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
}

void testShadowMap()
{
    w3shadow::ShadowMap map(2, 3);
    require(map.widthPixels() == 8 && map.heightPixels() == 12, "wrong SHD dimensions");
    require(map.bytes().size() == 96, "wrong SHD byte count");
    map.set(7, 11, true);
    require(map.get(7, 11), "set/get failed");
    map.setValue(0, 0, 123);
    require(map.value(0, 0) == 123, "raw value failed");

    bool threw = false;
    try { static_cast<void>(map.get(8, 0)); } catch (const std::out_of_range&) { threw = true; }
    require(threw, "out-of-range access was not rejected");
}

void testPatterns()
{
    w3shadow::ShadowMap checker(2, 2);
    w3shadow::generatePattern(checker, w3shadow::Pattern::Checker);
    require(!checker.get(0, 0), "checker origin should be lit");
    require(checker.get(4, 0), "checker adjacent tile should be shadowed");
    require(checker.get(0, 4), "checker adjacent row should be shadowed");
    require(!checker.get(4, 4), "checker diagonal tile should be lit");

    w3shadow::ShadowMap gradient(1, 1);
    w3shadow::generatePattern(gradient, w3shadow::Pattern::XGradient);
    require(gradient.value(0, 0) == 0, "x-gradient should begin at zero");
    require(gradient.value(3, 0) == 255, "x-gradient should end at 255");
}

void testW3E()
{
    std::vector<std::byte> bytes{
        std::byte{'W'}, std::byte{'3'}, std::byte{'E'}, std::byte{'!'}};
    appendU32(bytes, 11);
    bytes.push_back(std::byte{'A'});
    appendU32(bytes, 0);
    appendU32(bytes, 1);
    bytes.insert(bytes.end(), {std::byte{'A'}, std::byte{'d'}, std::byte{'r'}, std::byte{'t'}});
    appendU32(bytes, 1);
    bytes.insert(bytes.end(), {std::byte{'C'}, std::byte{'L'}, std::byte{'i'}, std::byte{'f'}});
    appendU32(bytes, 65);
    appendU32(bytes, 97);
    bytes.resize(bytes.size() + 8U + 65U * 97U * 7U, std::byte{0});

    const auto result = w3shadow::parseW3E(bytes);
    require(static_cast<bool>(result), result.error);
    require(result.info.tileWidth == 64 && result.info.tileHeight == 96, "W3E tile dimensions are wrong");

    bytes.resize(8);
    require(!w3shadow::parseW3E(bytes), "truncated W3E was accepted");
}

void testPng()
{
    w3shadow::ShadowMap map(1, 1);
    w3shadow::generatePattern(map, w3shadow::Pattern::Checker);
    const auto png = w3shadow::makeShadowPreviewPng(4, 4, map.bytes());
    const std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    require(png.size() > signature.size(), "PNG is unexpectedly short");
    for (std::size_t index = 0; index < signature.size(); ++index) {
        require(std::to_integer<std::uint8_t>(png[index]) == signature[index], "PNG signature mismatch");
    }
}

void testArchiveReplacement()
{
    const std::filesystem::path fixture(W3SHADOW_FIXTURE_MAP);
    const auto uniqueName = "w3shadow-test-" + std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto testDirectory = std::filesystem::temp_directory_path() / uniqueName;
    std::filesystem::create_directory(testDirectory);

    try {
        const auto input = testDirectory / "input.w3m";
        const auto output = testDirectory / "output.w3m";
        std::filesystem::copy_file(fixture, input);
        const auto original = w3shadow::readBinaryFile(input);

        w3shadow::ShadowMap shadow(64, 64);
        w3shadow::generatePattern(shadow, w3shadow::Pattern::Quadrants);
        w3shadow::MapArchive::replaceShadowInCopy(input, output, shadow.bytes(), false);
        require(w3shadow::readBinaryFile(input) == original, "copy mode changed the input map");
        {
            const w3shadow::MapArchive archive(output);
            require(archive.read("war3map.shd") ==
                        std::vector<std::byte>(shadow.bytes().begin(), shadow.bytes().end()),
                    "copy-mode SHD round trip failed");
        }

        const auto backup = w3shadow::MapArchive::replaceShadowInPlace(input, shadow.bytes());
        require(std::filesystem::exists(backup), "in-place mode did not retain a backup");
        require(w3shadow::readBinaryFile(backup) == original, "in-place backup differs from the original");
        {
            const w3shadow::MapArchive archive(input);
            require(archive.read("war3map.shd") ==
                        std::vector<std::byte>(shadow.bytes().begin(), shadow.bytes().end()),
                    "in-place SHD round trip failed");
        }

        std::filesystem::remove_all(testDirectory);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(testDirectory, ignored);
        throw;
    }
}

} // namespace

int main()
{
    try {
        testShadowMap();
        testPatterns();
        testW3E();
        testPng();
        testArchiveReplacement();
        std::cout << "All tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}

