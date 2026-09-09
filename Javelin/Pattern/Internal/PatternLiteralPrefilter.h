#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Javelin::PatternInternal {
struct IComponent;
class PatternProcessor;

struct LiteralPrefixRun {
    std::array<uint64_t, 4> bytes{};
    uint32_t minimum = 0, maximum = 0;
    bool Contains(unsigned char c) const { return (bytes[c / 64] >> (c % 64)) & 1; }
};

struct LiteralPrefilter {
    std::vector<std::string> literals;
    // Conservative byte-class prefixes, verified by the matcher unless completeMatch.
    std::vector<std::vector<LiteralPrefixRun>> prefixBytes;
    std::vector<LiteralPrefixRun> reversePrefix;
    std::vector<LiteralPrefixRun> forwardSuffix;
    size_t minimumPrefix = 0, maximumPrefix = 0;
    size_t suffixLength = 0;
    bool endAnchored = false;
    bool completeMatch = false;
    bool startWordBoundary = false, endWordBoundary = false, requireEnd = false;
    bool unboundedVerification = false;
    bool greedyLiteralSuffix = false;
    bool wholeRun = false;
    bool byteRun = false;
    LiteralPrefixRun runBytes;
    bool delimited = false, delimiterTail = false;
    bool delimiterDisjointOpener = false;
    LiteralPrefixRun delimiterBody, delimiterLastByte;
    std::vector<unsigned char> closingBytes;
    int excludedRunByte = -1;
    bool HasData() const { return !literals.empty(); }
};

LiteralPrefilter BuildLiteralPrefilter(const IComponent* component);
PatternProcessor* CreateLiteralPrefilterProcessor(PatternProcessor* processor,
                                                  LiteralPrefilter&& filter,
                                                  size_t numberOfCaptures);
}
