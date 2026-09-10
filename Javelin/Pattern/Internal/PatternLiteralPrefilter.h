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

// Plans that can compute group zero directly. Other captures still require
// the regex processor; None requires it to verify every candidate.
struct LiteralMatchPlan {
    enum class Kind { None, Runs, WholeRun, Delimited };
    Kind kind = Kind::None;

    // Runs: a literal/byte product with disjoint greedy prefix/suffix runs.
    // reversePrefix is shared with candidate verification in LiteralPrefilter.
    std::vector<LiteralPrefixRun> forwardSuffix;
    bool startWordBoundary = false, endWordBoundary = false, requireEnd = false;
    bool greedyLiteralSuffix = false;

    // WholeRun: identical unbounded greedy runs surrounding a literal whose
    // bytes all belong to the run. At most one byte is excluded from the run.
    int excludedRunByte = -1;

    // Delimited: a one-byte opener, a greedy body that excludes every closer,
    // and a one-byte closer. The optional tail is a subset of the body class.
    bool delimiterTail = false;
    bool delimiterDisjointOpener = false;
    LiteralPrefixRun delimiterBody, delimiterLastByte;
    std::vector<unsigned char> closingBytes;

    bool IsComplete() const { return kind != Kind::None; }
};

struct LiteralPrefilter {
    std::vector<std::string> literals;
    // Conservative byte-class prefixes: false positives are allowed, false
    // negatives are not. Only match.kind authorizes returning a final span.
    std::vector<std::vector<LiteralPrefixRun>> prefixBytes;
    std::vector<LiteralPrefixRun> reversePrefix;
    size_t minimumPrefix = 0, maximumPrefix = 0;
    size_t suffixLength = 0;
    bool endAnchored = false;
    bool unboundedVerification = false;
    bool byteRun = false;
    LiteralPrefixRun runBytes;
    LiteralMatchPlan match;
    bool HasData() const { return !literals.empty(); }
};

LiteralPrefilter BuildLiteralPrefilter(const IComponent* component);
PatternProcessor* CreateLiteralPrefilterProcessor(PatternProcessor* processor,
                                                  LiteralPrefilter&& filter,
                                                  size_t numberOfCaptures);
}
