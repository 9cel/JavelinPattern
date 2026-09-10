#include "Javelin/Pattern/Internal/PatternLiteralPrefilter.h"
#include "Javelin/Pattern/Internal/PatternComponent.h"
#include "Javelin/Pattern/Internal/PatternProcessor.h"
#include "Javelin/Pattern/Internal/PatternReverseProcessor.h"
#include "Javelin/Type/Utf8Character.h"
#include <algorithm>
#include <cstring>
#include <limits>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
// Compile SIMD implementations independently of the translation unit's ISA.
// BuildLiteralPrefilter and the processor constructor dispatch at runtime.
#define JP_LITERAL_X86 1
#include <immintrin.h>
#endif

using namespace Javelin;
using namespace Javelin::PatternInternal;

namespace {
constexpr size_t UNBOUNDED = UINT32_MAX;
using Literals = std::vector<std::string>;
using MatchKind = LiteralMatchPlan::Kind;

size_t AddLength(size_t a, size_t b) { return std::min(UNBOUNDED, a + b); }
bool IsWord(unsigned c) { return c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z'); }

bool Concatenate(Literals& left, const Literals& right, size_t limit = 8) {
    if (left.size() * right.size() > limit) return false;
    Literals result;
    for (const auto& a : left) for (const auto& b : right) {
        if (a.size() + b.size() > 128) return false;
        result.push_back(a + b);
    }
    left = std::move(result);
    return true;
}

bool Extract(const IComponent* component, Literals& result, size_t limit = 8) {
    if (const auto* byte = dynamic_cast<const ByteComponent*>(component)) {
        result = {std::string(1, char(byte->c))};
        return true;
    }
    if (component->IsEmpty()) {
        result = {""};
        return true;
    }
    if (const auto* capture = dynamic_cast<const CaptureComponent*>(component))
        return Extract(capture->content, result, limit);
    if (const auto* range = dynamic_cast<const CharacterRangeListComponent*>(component)) {
        result.clear();
        for (const auto& interval : range->characterRangeList) {
            uint32_t first = interval.min, last = interval.max;
            if (last > (range->useUtf8 ? 0x7fffffffu : 255u)) return false;
            if (last - first + result.size() >= limit) return false;
            for (uint32_t c = first; c <= last; ++c) {
                if (!range->useUtf8 || c < 128) { result.emplace_back(1, char(c)); continue; }
                Utf8Character encoded{Character(c)};
                result.emplace_back(encoded.GetData(), encoded.GetNumberOfBytes());
            }
        }
        return !result.empty();
    }
    if (const auto* group = dynamic_cast<const ConcatenateComponent*>(component)) {
        result = {""};
        for (const auto* child : group->componentList) {
            Literals part;
            if (!Extract(child, part, limit) || !Concatenate(result, part, limit)) return false;
        }
        return true;
    }
    if (const auto* alternate = dynamic_cast<const AlternationComponent*>(component)) {
        result.clear();
        for (const auto* child : alternate->componentList) {
            Literals part;
            if (!Extract(child, part, limit) || result.size() + part.size() > limit) return false;
            result.insert(result.end(), part.begin(), part.end());
        }
        return !result.empty();
    }
    if (const auto* repeat = dynamic_cast<const CounterComponent*>(component)) {
        if (repeat->minimum != repeat->maximum || repeat->minimum > 32) return false;
        Literals part;
        if (!Extract(repeat->content, part, limit)) return false;
        result = {""};
        for (uint32_t i = 0; i < repeat->minimum; ++i)
            if (!Concatenate(result, part, limit)) return false;
        return true;
    }
    return false;
}

void Flatten(const IComponent* component, std::vector<const IComponent*>& parts) {
    if (const auto* capture = dynamic_cast<const CaptureComponent*>(component))
        Flatten(capture->content, parts);
    else if (const auto* group = dynamic_cast<const ConcatenateComponent*>(component))
        for (const auto* child : group->componentList) Flatten(child, parts);
    else parts.push_back(component);
}

bool HasSearchStart(const IComponent* component) {
    if (const auto* assertion = dynamic_cast<const AssertComponent*>(component))
        return assertion->assertType == AssertType::StartOfSearch;
    if (const auto* capture = dynamic_cast<const CaptureComponent*>(component)) return HasSearchStart(capture->content);
    if (const auto* repeat = dynamic_cast<const CounterComponent*>(component)) return HasSearchStart(repeat->content);
    if (const auto* group = dynamic_cast<const ConcatenateComponent*>(component))
        for (const auto* child : group->componentList) if (HasSearchStart(child)) return true;
    if (const auto* group = dynamic_cast<const AlternationComponent*>(component))
        for (const auto* child : group->componentList) if (HasSearchStart(child)) return true;
    return false;
}

bool ExtractTail(const IComponent* component, Literals& result) {
    if (Extract(component, result)) return true;
    if (const auto* capture = dynamic_cast<const CaptureComponent*>(component)) return ExtractTail(capture->content, result);
    if (const auto* repeat = dynamic_cast<const CounterComponent*>(component))
        return repeat->minimum && ExtractTail(repeat->content, result);
    if (const auto* group = dynamic_cast<const ConcatenateComponent*>(component)) {
        result = {""};
        for (size_t i = group->componentList.GetCount(); i > 0; --i) {
            Literals part;
            bool complete = Extract(group->componentList[i - 1], part);
            if (!complete && !ExtractTail(group->componentList[i - 1], part)) break;
            if (!Concatenate(part, result)) break;
            result = std::move(part);
            if (!complete) break;
        }
        return !result[0].empty();
    }
    return false;
}

bool PrefixRun(const IComponent* component, LiteralPrefixRun& run) {
    const auto* repeat = dynamic_cast<const CounterComponent*>(component);
    if (repeat && repeat->mode != CounterComponent::Maximal) return false;
    run.minimum = repeat ? repeat->minimum : 1;
    run.maximum = repeat ? repeat->maximum : 1;
    const IComponent* content = repeat ? repeat->content : component;
    if (const auto* byte = dynamic_cast<const ByteComponent*>(content)) {
        unsigned char c = byte->c;
        run.bytes[c / 64] |= uint64_t(1) << (c % 64);
        return true;
    }
    const auto* range = dynamic_cast<const CharacterRangeListComponent*>(content);
    if (!range || !range->IsByte()) return false;
    for (const auto& interval : range->characterRangeList) {
        if (uint32_t(interval.max) > 255) return false;
        for (uint32_t c = interval.min; c <= uint32_t(interval.max); ++c)
            run.bytes[c / 64] |= uint64_t(1) << (c % 64);
    }
    return true;
}

LiteralPrefilter BuildWholeRunFilter(const std::vector<const IComponent*>& parts) {
    for (size_t i = 1; i + 1 < parts.size(); ++i) {
        Literals literals;
        if (!Extract(parts[i], literals) || literals.size() != 1 || literals[0].empty()) continue;
        size_t j = i + 1;
        for (; j < parts.size(); ++j) {
            Literals next;
            if (!Extract(parts[j], next) || next.size() != 1 || !Concatenate(literals, next)) break;
        }
        if (j == parts.size()) continue;
        LiteralPrefixRun first;
        if (!PrefixRun(parts[0], first) || first.minimum || first.maximum != UNBOUNDED) continue;
        bool valid = true;
        for (size_t k = 1; k < parts.size(); ++k) {
            if (k >= i && k < j) continue;
            LiteralPrefixRun run;
            if (!PrefixRun(parts[k], run) || run.minimum || run.maximum != UNBOUNDED || run.bytes != first.bytes) valid = false;
        }
        for (unsigned char c : literals[0]) if (!first.Contains(c)) valid = false;
        int excluded = -1;
        for (unsigned c = 0; c < 256; ++c) if (!first.Contains(c)) {
            if (excluded != -1) valid = false;
            excluded = c;
        }
        if (!valid) continue;
        LiteralPrefilter result;
        result.literals = std::move(literals);
        result.match.kind = MatchKind::WholeRun;
        result.match.excludedRunByte = excluded;
        return result;
    }
    return {};
}

LiteralPrefilter BuildDelimitedFilter(const std::vector<const IComponent*>& parts) {
    if (parts.size() < 3) return {};
    LiteralPrefilter result;
    if (!Extract(parts[0], result.literals) || result.literals.size() > 2) return {};
    for (const auto& literal : result.literals) if (literal.size() != 1) return {};
    LiteralPrefixRun closing;
    if (!PrefixRun(parts.back(), closing) || closing.minimum != 1 || closing.maximum != 1) return {};
    for (unsigned c = 0; c < 256; ++c) if (closing.Contains(c)) result.match.closingBytes.push_back(c);
    if (result.match.closingBytes.empty() || result.match.closingBytes.size() > 2) return {};
    if (!PrefixRun(parts[1], result.match.delimiterBody)) return {};
    for (unsigned c = 0; c < 256; ++c)
        if (closing.Contains(c) == result.match.delimiterBody.Contains(c)) return {};
    result.match.delimiterDisjointOpener = true;
    for (const auto& literal : result.literals)
        if (result.match.delimiterBody.Contains(literal[0])) result.match.delimiterDisjointOpener = false;
    for (size_t k = 2; k + 1 < parts.size(); ++k) {
        LiteralPrefixRun run;
        if (!PrefixRun(parts[k], run)) return {};
        if (run.bytes == result.match.delimiterBody.bytes) {
            result.match.delimiterBody.minimum = AddLength(result.match.delimiterBody.minimum, run.minimum);
            result.match.delimiterBody.maximum = AddLength(result.match.delimiterBody.maximum, run.maximum);
        } else {
            if (k + 2 != parts.size() || run.minimum != 1 || run.maximum != 1) return {};
            for (size_t w = 0; w < 4; ++w) if (run.bytes[w] & ~result.match.delimiterBody.bytes[w]) return {};
            result.match.delimiterTail = true;
            result.match.delimiterLastByte = run;
        }
    }
    result.match.kind = MatchKind::Delimited;
    return result;
}

using ByteSequence = std::vector<LiteralPrefixRun>;

bool IsByteProduct(const IComponent* component, bool singleLiteral = false) {
    if (component->IsEmpty() || dynamic_cast<const ByteComponent*>(component)) return true;
    if (const auto* capture = dynamic_cast<const CaptureComponent*>(component)) return IsByteProduct(capture->content, singleLiteral);
    if (const auto* group = dynamic_cast<const ConcatenateComponent*>(component)) {
        for (const auto* child : group->componentList) if (!IsByteProduct(child, singleLiteral)) return false;
        return true;
    }
    if (const auto* repeat = dynamic_cast<const CounterComponent*>(component))
        return repeat->minimum == repeat->maximum && IsByteProduct(repeat->content, singleLiteral);
    if (const auto* range = dynamic_cast<const CharacterRangeListComponent*>(component)) {
        if (!range->IsByte()) return false;
        if (!singleLiteral) return true;
        size_t count = 0;
        for (const auto& interval : range->characterRangeList) count += interval.max - interval.min + 1;
        return count == 1;
    }
    return false;
}

// A necessary fixed-position prefix. The boolean says whether the whole component was consumed.
bool ExtractBytePrefix(const IComponent* component, ByteSequence& bytes) {
    if (const auto* capture = dynamic_cast<const CaptureComponent*>(component))
        return ExtractBytePrefix(capture->content, bytes);
    if (const auto* group = dynamic_cast<const ConcatenateComponent*>(component)) {
        for (const auto* child : group->componentList) {
            bool complete = ExtractBytePrefix(child, bytes);
            if (bytes.size() > 64) { bytes.resize(64); return false; }
            if (!complete) return false;
        }
        return true;
    }
    Literals literals;
    if (Extract(component, literals)) {
        size_t n = literals[0].size();
        for (const auto& literal : literals) n = std::min(n, literal.size());
        size_t base = bytes.size();
        bytes.resize(base + n);
        bool complete = true;
        for (const auto& literal : literals) {
            if (literal.size() != n) complete = false;
            for (size_t i = 0; i < n; ++i) {
                unsigned char c = literal[i];
                bytes[base + i].bytes[c / 64] |= uint64_t(1) << (c % 64);
            }
        }
        return complete;
    }
    if (const auto* range = dynamic_cast<const CharacterRangeListComponent*>(component)) {
        LiteralPrefixRun run;
        if (!PrefixRun(range, run)) return false;
        bytes.push_back(run);
        return true;
    }
    if (const auto* alternate = dynamic_cast<const AlternationComponent*>(component)) {
        ByteSequence merged;
        bool first = true, complete = true;
        for (const auto* child : alternate->componentList) {
            ByteSequence part;
            complete = ExtractBytePrefix(child, part) && complete;
            if (part.empty()) return false;
            if (first) { merged = std::move(part); first = false; }
            else {
                if (merged.size() != part.size()) complete = false;
                merged.resize(std::min(merged.size(), part.size()));
                for (size_t i = 0; i < merged.size(); ++i)
                    for (size_t w = 0; w < 4; ++w) merged[i].bytes[w] |= part[i].bytes[w];
            }
        }
        bytes.insert(bytes.end(), merged.begin(), merged.end());
        return complete;
    }
    if (const auto* repeat = dynamic_cast<const CounterComponent*>(component)) {
        if (!repeat->minimum || repeat->minimum > 32) return repeat->maximum == 0;
        ByteSequence part;
        bool complete = ExtractBytePrefix(repeat->content, part);
        for (uint32_t i = 0; i < repeat->minimum; ++i) {
            bytes.insert(bytes.end(), part.begin(), part.end());
            if (!complete || bytes.size() >= 64) { bytes.resize(std::min(bytes.size(), size_t(64))); return false; }
        }
        return repeat->minimum == repeat->maximum;
    }
    if (dynamic_cast<const AssertComponent*>(component)) return true;
    return false;
}

LiteralPrefilter BuildBytePrefixFilter(const IComponent* component) {
    while (const auto* capture = dynamic_cast<const CaptureComponent*>(component)) component = capture->content;
    std::vector<const IComponent*> branches;
    if (const auto* alternate = dynamic_cast<const AlternationComponent*>(component);
        alternate && !dynamic_cast<const CharacterRangeListComponent*>(component)) {
        for (const auto* branch : alternate->componentList) branches.push_back(branch);
    } else branches.push_back(component);
    if (branches.size() > 8) return {};
    LiteralPrefilter result;
    result.unboundedVerification = component->GetMaximumLength() == UNBOUNDED;
    result.match.kind = MatchKind::Runs;
    for (const auto* branch : branches) {
        ByteSequence prefix;
        bool complete = ExtractBytePrefix(branch, prefix);
        if (prefix.size() < 3) return {};
        if (!complete || !IsByteProduct(branch) || prefix.size() != branch->GetMinimumLength())
            result.match.kind = MatchKind::None;
        result.literals.emplace_back(prefix.size(), '\0');
        result.prefixBytes.push_back(std::move(prefix));
    }
    size_t length = 64;
    for (const auto& prefix : result.prefixBytes) length = std::min(length, prefix.size());
    unsigned least = 257, nextLeast = 257;
    for (size_t i = 0; i < length; ++i) {
        unsigned count = 0;
        for (unsigned c = 0; c < 256; ++c) {
            bool contains = false;
            for (const auto& prefix : result.prefixBytes) contains |= prefix[i].Contains(c);
            count += contains;
        }
        if (count < least) { nextLeast = least; least = count; }
        else nextLeast = std::min(nextLeast, count);
    }
    // Broad classes such as \w+ make a poor two-byte prefilter.
    if (least * nextLeast > 64) return {};
    return result;
}

LiteralPrefilter BuildByteRunFilter(const IComponent* component) {
    if (component->GetMinimumLength() < 8 || component->GetMaximumLength() > 128) return {};
    while (const auto* capture = dynamic_cast<const CaptureComponent*>(component)) component = capture->content;
    const auto* alternate = dynamic_cast<const AlternationComponent*>(component);
    if (!alternate || alternate->componentList.GetCount() <= 8) return {};
    LiteralPrefilter result;
    if (!Extract(component, result.literals, 4096) || result.literals.size() <= 8) return {};
    size_t minimum = 128;
    for (const auto& literal : result.literals) {
        minimum = std::min(minimum, literal.size());
        for (unsigned char c : literal) {
            if (c >= 128) return {};
            result.runBytes.bytes[c / 64] |= uint64_t(1) << (c % 64);
        }
    }
    if (minimum < 8 || minimum > 32) return {};
    unsigned alphabetSize = 0;
    for (uint64_t word : result.runBytes.bytes) alphabetSize += __builtin_popcountll(word);
    if (alphabetSize > 64) return {};
    result.byteRun = true;
    return result;
}

void CollapseLiteralProduct(LiteralPrefilter& plan) {
    if (!plan.match.IsComplete() || plan.endAnchored || plan.maximumPrefix ||
        !plan.match.forwardSuffix.empty() || plan.literals.size() < 2) return;
    const size_t length = plan.literals[0].size();
    ByteSequence bytes(length);
    for (const auto& literal : plan.literals) {
        if (literal.size() != length) return;
        for (size_t i = 0; i < length; ++i) {
            unsigned char c = literal[i];
            bytes[i].bytes[c / 64] |= uint64_t(1) << (c % 64);
        }
    }
    size_t combinations = 1;
    for (const auto& byte : bytes) {
        unsigned count = 0;
        for (uint64_t word : byte.bytes) count += __builtin_popcountll(word);
        combinations *= count;
        if (combinations > plan.literals.size()) return;
    }
    Literals distinct = plan.literals;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    if (combinations != distinct.size()) return;
    // eg: (?i)the is a sequence of independent byte classes
    plan.literals.resize(1);
    plan.prefixBytes = {std::move(bytes)};
}

class LiteralPrefilterProcessor final : public PatternProcessor {
public:
    LiteralPrefilterProcessor(PatternProcessor* processor, LiteralPrefilter&& plan, size_t captures)
        : processor(processor), plan(std::move(plan)), numberOfCaptures(captures) {
#if defined(JP_LITERAL_X86)
        if (__builtin_cpu_supports("avx512bw")) vectorBytes = 64;
        else if (__builtin_cpu_supports("avx2")) vectorBytes = 32;
#endif
        minimumLength = 128;
        for (const auto& literal : this->plan.literals) minimumLength = std::min(minimumLength, literal.size());
        literalOnly = this->plan.match.IsComplete() && !this->plan.maximumPrefix && !this->plan.endAnchored &&
            this->plan.match.forwardSuffix.empty() && !this->plan.match.startWordBoundary && !this->plan.match.endWordBoundary &&
            !this->plan.match.requireEnd && this->plan.match.kind == MatchKind::Runs;
        if (this->plan.byteRun) {
            for (unsigned c = 0; c < 128; ++c) if (this->plan.runBytes.Contains(c)) {
                masks[0][c & 15] |= 1 << (c >> 4);
                masks[1][c >> 4] = 1 << (c >> 4);
            }
            return;
        }
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        packedLiterals = this->plan.prefixBytes.empty();
#endif
        for (size_t i = 0; i < this->plan.literals.size(); ++i) {
            const auto& literal = this->plan.literals[i];
            if (literal.size() > 8) { packedLiterals = false; break; }
            std::memcpy(&literalValues[i], literal.data(), literal.size());
            literalMasks[i] = literal.size() == 8 ? UINT64_MAX : (uint64_t(1) << (8 * literal.size())) - 1;
        }
        // Many byte classes are a masked equality (including ASCII case
        // folds). Pack them so candidate verification checks eight bytes at a
        // time, while retaining the general class verifier for other sets.
        packedPrefixes = !this->plan.prefixBytes.empty();
        for (size_t i = 0; i < this->plan.prefixBytes.size(); ++i) {
            const auto& prefix = this->plan.prefixBytes[i];
            if (prefix.size() < 8 || prefix.size() > 32) { packedPrefixes = false; break; }
            uint8_t values[32]{}, bitMasks[32]{};
            for (size_t j = 0; j < prefix.size(); ++j) {
                unsigned common = 255, combined = 0, count = 0;
                for (unsigned c = 0; c < 256; ++c) if (prefix[j].Contains(c)) {
                    common &= c; combined |= c; ++count;
                }
                if (count != (1u << __builtin_popcount(common ^ combined))) packedPrefixes = false;
                values[j] = common;
                bitMasks[j] = uint8_t(~(common ^ combined));
            }
            for (size_t j = 0; j < (prefix.size() + 7) / 8; ++j) {
                size_t at = std::min(j * 8, prefix.size() - 8);
                std::memcpy(&prefixValues[i][j], values + at, 8);
                std::memcpy(&prefixMasks[i][j], bitMasks + at, 8);
            }
        }
        verificationCost = 0;
        if (!this->plan.prefixBytes.empty()) {
            for (const auto& prefix : this->plan.prefixBytes)
                verificationCost += packedPrefixes ? (prefix.size() + 7) / 8 : prefix.size();
        } else {
            for (const auto& literal : this->plan.literals)
                verificationCost += (literal.size() + 7) / 8;
        }
        if (minimumLength == 1) second = 0;
        // Prefer distinct, uncommon bytes; UTF-8 lead bytes have low selectivity.
        auto frequency = [](unsigned char c) {
            if (c >= 0xc0) return 200;
            if (c >= 0x80) return 8;
            if (c >= 'A' && c <= 'Z') return 5;
            if (c == ' ') return 180;
            static const unsigned char letters[26] = {
                81,15,28,43,128,22,20,61,70,1,8,40,24,67,75,19,1,60,63,90,28,10,24,2,20,1
            };
            if (c >= 'a' && c <= 'z') return int(letters[c - 'a']);
            return 10;
        };
        size_t best = size_t(-1);
        for (size_t a = 0; a < minimumLength; ++a) for (size_t b = a + 1; b < minimumLength; ++b) {
            size_t score = 0;
            if (this->plan.prefixBytes.empty()) {
                for (const auto& literal : this->plan.literals)
                    // Repeated bytes are correlated in runs.
                    score += frequency(literal[a]) * (literal[a] == literal[b] ? 256 : frequency(literal[b]));
            } else for (const auto& prefix : this->plan.prefixBytes) {
                size_t fa = 0, fb = 0;
                for (unsigned c = 0; c < 256; ++c) {
                    if (prefix[a].Contains(c)) fa += frequency(c);
                    if (prefix[b].Contains(c)) fb += frequency(c);
                }
                score += fa * fb;
            }
            // Near ties favor earlier positions.
            if (best == size_t(-1) || score < best - best / 10) { best = score; first = a; second = b; }
        }
        LiteralPrefixRun alphabet;
        if (this->plan.prefixBytes.empty()) for (const auto& literal : this->plan.literals)
            for (unsigned char c : literal) alphabet.bytes[c / 64] |= uint64_t(1) << (c % 64);
        else for (const auto& prefix : this->plan.prefixBytes) for (const auto& byte : prefix)
            for (size_t w = 0; w < 4; ++w) alphabet.bytes[w] |= byte.bytes[w];
        unsigned alphabetSize = 0;
        for (uint64_t word : alphabet.bytes) alphabetSize += __builtin_popcountll(word);
        // Wider x86 shuffles can afford extra columns. These avoid repeatedly
        // verifying common UTF-8 continuation-byte pairs and folded ASCII.
        fourColumns = this->plan.literals.size() > 1 && minimumLength >= 8 && alphabetSize >= 2 &&
            (alphabetSize <= 4 || vectorBytes >= 32);
        size_t columns[4] = {first, second, 0, 0};
        if (fourColumns) for (size_t k = 2; k < 4; ++k) {
            size_t bestColumnScore = size_t(-1);
            for (size_t at = 0; at < minimumLength; ++at) {
                bool used = false;
                for (size_t j = 0; j < k; ++j) used |= at == columns[j];
                if (used) continue;
                size_t score = 0;
                if (this->plan.prefixBytes.empty()) for (const auto& literal : this->plan.literals) score += frequency(literal[at]);
                else for (const auto& prefix : this->plan.prefixBytes) for (unsigned c = 0; c < 256; ++c)
                    if (prefix[at].Contains(c)) score += frequency(c);
                if (score < bestColumnScore) { bestColumnScore = score; columns[k] = at; }
            }
        }
        third = columns[2]; fourth = columns[3];
        for (size_t i = 0; i < this->plan.literals.size(); ++i) {
            const auto& literal = this->plan.literals[i];
            for (size_t j = 0; j < (fourColumns ? 4 : 2); ++j) {
                for (unsigned c = 0; c < 256; ++c) {
                    if (this->plan.prefixBytes.empty() ? c != (unsigned char) literal[columns[j]]
                        : !this->plan.prefixBytes[i][columns[j]].Contains(c)) continue;
                    masks[2*j][c & 15] |= 1 << i;
                    masks[2*j+1][c >> 4] |= 1 << i;
                }
            }
        }
        simplePair = true;
        for (size_t j = 0; j < 2; ++j) {
            unsigned count = 0;
            for (unsigned c = 0; c < 256; ++c) {
                bool contains = false;
                for (size_t i = 0; i < this->plan.literals.size(); ++i)
                    contains |= this->plan.prefixBytes.empty() ? c == (unsigned char)this->plan.literals[i][j ? second : first]
                        : this->plan.prefixBytes[i][j ? second : first].Contains(c);
                if (!contains) continue;
                if (count < 2) pairBytes[2*j + count] = c;
                ++count;
            }
            if (count == 1) pairBytes[2*j + 1] = pairBytes[2*j];
            if (count > 2) simplePair = false;
        }
    }
    ~LiteralPrefilterProcessor() { delete processor; }
    const void* FullMatch(const void* d, size_t n) const override { return processor->FullMatch(d, n); }
    const void* FullMatch(const void* d, size_t n, const char** c) const override { return processor->FullMatch(d, n, c); }
    const void* PopulateCaptures(const void* d, size_t n, size_t o, const char** c) const override {
        return processor->PopulateCaptures(d, n, o, c);
    }
    const void* PartialMatch(const void* d, size_t n, size_t o) const override {
        if (o > n || n - o < minimumLength) return nullptr;
        if (literalOnly) return LocateLiteral(d, n, o).max;
        if (!plan.match.IsComplete() && !plan.unboundedVerification && o <= n && n - o < 128)
            return processor->PartialMatch(d, n, o);
        return Search(d, n, o, nullptr);
    }
    const void* PartialMatch(const void* d, size_t n, size_t o, const char** c) const override {
        if (o > n || n - o < minimumLength) return nullptr;
        if (!plan.match.IsComplete() && !plan.maximumPrefix && o <= n && n - o < 128)
            return processor->PartialMatch(d, n, o, c);
        return Search(d, n, o, c);
    }
    Interval<const void*> LocatePartialMatch(const void* d, size_t n, size_t o) const override {
        if (o > n || n - o < minimumLength) return {nullptr, nullptr};
        if (literalOnly) return LocateLiteral(d, n, o);
        const char* captures[numberOfCaptures * 2];
        if (!Search(d, n, o, captures, true)) return {nullptr, nullptr};
        return {captures[0], captures[1]};
    }

private:
    // Per-search budget, including candidates rejected by SIMD verification.
    struct FilterBudget {
        const unsigned char* origin;
        // Reset after 64 rejections to keep work within 32 bits.
        uint32_t rejected = 0, work = 0;

        bool IsAbandoned() const { return origin == nullptr; }

        bool Reject(const unsigned char* at, size_t cost) {
            work += cost;
            if (++rejected >= 8 && work > size_t(at - origin) / 2) {
                origin = nullptr;
                return true;
            }
            // Forget distant, selective input so a later dense region can
            // independently make the filter give up.
            if (rejected == 64) { origin = at; rejected = work = 0; }
            return false;
        }
    };

    PatternProcessor* processor;
    LiteralPrefilter plan;
    size_t numberOfCaptures, minimumLength, first = 0, second = 1, third = 0, fourth = 0;
    alignas(16) uint8_t masks[8][16]{};
    uint8_t pairBytes[4]{};
    uint64_t literalValues[8]{}, literalMasks[8]{};
    uint64_t prefixValues[8][4]{}, prefixMasks[8][4]{};
    bool packedPrefixes = false;
    unsigned vectorBytes = 16;
    bool simplePair = false, packedLiterals = false, fourColumns = false, literalOnly = false;
    size_t verificationCost = 1;

    Interval<const void*> LocateLiteral(const void* d, size_t n, size_t o) const {
        if (o > n) return {nullptr, nullptr};
        const auto* begin = static_cast<const unsigned char*>(d);
        const unsigned char* finish = nullptr;
        FilterBudget budget{begin + o};
        const unsigned char* start = Find(begin + o, begin + n, finish, budget);
        if (budget.IsAbandoned()) return processor->LocatePartialMatch(d, n, o);
        return start ? Interval<const void*>{start, finish} : Interval<const void*>{nullptr, nullptr};
    }

    const unsigned char* MatchEnd(const unsigned char* p, const unsigned char* end) const {
        if (packedPrefixes) {
            for (size_t i = 0; i < plan.prefixBytes.size(); ++i) {
                size_t length = plan.prefixBytes[i].size();
                if (size_t(end - p) < length) continue;
                size_t j = 0, chunks = (length + 7) / 8;
                for (; j < chunks; ++j) {
                    uint64_t word;
                    std::memcpy(&word, p + std::min(j * 8, length - 8), 8);
                    if ((word & prefixMasks[i][j]) != prefixValues[i][j]) break;
                }
                if (j == chunks) return p + length;
            }
            return nullptr;
        }
        if (!plan.prefixBytes.empty()) {
            for (const auto& prefix : plan.prefixBytes) {
                if (size_t(end - p) < prefix.size()) continue;
                size_t i = 0;
                for (; i < prefix.size() && prefix[i].Contains(p[i]); ++i) {}
                if (i == prefix.size()) return p + i;
            }
            return nullptr;
        }
        if (packedLiterals) {
            uint64_t word = 0;
            size_t available = end - p;
            if (available >= 8) std::memcpy(&word, p, 8);
            else std::memcpy(&word, p, available);
            for (size_t i = 0; i < plan.literals.size(); ++i)
                if (available >= plan.literals[i].size() && (word & literalMasks[i]) == literalValues[i])
                    return p + plan.literals[i].size();
            return nullptr;
        }
        for (const auto& literal : plan.literals)
            if (size_t(end - p) >= literal.size() && std::memcmp(p, literal.data(), literal.size()) == 0) return p + literal.size();
        return nullptr;
    }

    template<bool single>
    const unsigned char* VerifyCandidate(const unsigned char* p, const unsigned char* end) const {
        if constexpr(single) {
            // The vector filter has already checked two distinct positions.
            if (minimumLength == 2) return p + 2;
            if (minimumLength == 3) {
                size_t remaining = 3 - first - second;
                return p[remaining] == (unsigned char)plan.literals[0][remaining] ? p + 3 : nullptr;
            }
        }
        return MatchEnd(p, end);
    }

    const unsigned char* Find(const unsigned char* p, const unsigned char* end, const unsigned char*& matchEnd, FilterBudget& budget) const {
        if (plan.byteRun) {
            const unsigned char* found = FindRun(p, end);
            if (found) matchEnd = found + minimumLength;
            return found;
        }
        if (minimumLength == 1 && plan.literals.size() == 1 && plan.prefixBytes.empty()) {
            const auto* found = static_cast<const unsigned char*>(std::memchr(p, (unsigned char)plan.literals[0][0], end - p));
            if (found) matchEnd = found + 1;
            return found;
        }
        if (minimumLength == 1 && plan.literals.size() == 2 && plan.prefixBytes.empty() &&
            plan.literals[0].size() == 1 && plan.literals[1].size() == 1) {
            const auto* found = static_cast<const unsigned char*>(PatternProcessor::FindByteEitherOf2(p,
                (unsigned char)plan.literals[0][0] | (uint64_t((unsigned char)plan.literals[1][0]) << 8), end));
            if (found) matchEnd = found + 1;
            return found;
        }
        const unsigned char* found;
        if (fourColumns) found = Find<false, false, true>(p, end, matchEnd, budget);
        else if (plan.literals.size() == 1 && plan.prefixBytes.empty()) found = Find<true, false>(p, end, matchEnd, budget);
        else found = simplePair ? Find<false, true>(p, end, matchEnd, budget) : Find<false, false>(p, end, matchEnd, budget);
        // The SIMD loops return the rejected candidate to unwind immediately
        // on exhaustion. It must never escape as a successful search result.
        return budget.IsAbandoned() ? nullptr : found;
    }

#if defined(JP_LITERAL_X86)
    __attribute__((target("ssse3")))
#endif
    const unsigned char* FindRun(const unsigned char* p, const unsigned char* end) const {
        size_t trailing = 0;
#if defined(__aarch64__)
        const uint8x16_t low = vdupq_n_u8(15);
        const uint8x16_t a = vld1q_u8(masks[0]), b = vld1q_u8(masks[1]);
        const uint8x16_t weights = {1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128};
        auto classify = [&](const unsigned char* at) {
            uint8x16_t x = vld1q_u8(at);
            return vandq_u8(vtstq_u8(vqtbl1q_u8(a, vandq_u8(x, low)),
                                     vqtbl1q_u8(b, vshrq_n_u8(x, 4))), weights);
        };
#elif defined(JP_LITERAL_X86)
        const __m128i low = _mm_set1_epi8(15), zero = _mm_setzero_si128();
        const __m128i a = _mm_loadu_si128((const __m128i*)masks[0]), b = _mm_loadu_si128((const __m128i*)masks[1]);
        auto classify = [&](const unsigned char* at) __attribute__((target("ssse3"))) {
            __m128i x = _mm_loadu_si128((const __m128i*)at);
            __m128i bits = _mm_and_si128(_mm_shuffle_epi8(a, _mm_and_si128(x, low)),
                                         _mm_shuffle_epi8(b, _mm_and_si128(_mm_srli_epi16(x, 4), low)));
            return uint64_t(unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(bits, zero))) ^ 65535);
        };
#endif
#if defined(__aarch64__) || defined(JP_LITERAL_X86)
        while (end - p >= 64) {
#if defined(__aarch64__)
            uint8x16_t pairs01 = vpaddq_u8(classify(p), classify(p + 16));
            uint8x16_t pairs23 = vpaddq_u8(classify(p + 32), classify(p + 48));
            uint8x16_t quads = vpaddq_u8(pairs01, pairs23);
            uint64_t mask = vgetq_lane_u64(vreinterpretq_u64_u8(vpaddq_u8(quads, quads)), 0);
#else
            uint64_t mask = classify(p) | (classify(p + 16) << 16) |
                            (classify(p + 32) << 32) | (classify(p + 48) << 48);
#endif
            size_t leading = mask == UINT64_MAX ? 64 : __builtin_ctzll(~mask);
            if (trailing + leading >= minimumLength) return p - trailing;
            uint64_t ends = mask;
            size_t covered = 1;
            for (; covered * 2 <= minimumLength; covered *= 2) ends &= ends << covered;
            ends &= ends << (minimumLength - covered);
            if (ends) return p + __builtin_ctzll(ends) + 1 - minimumLength;
            trailing = mask == UINT64_MAX ? 64 : __builtin_clzll(~mask);
            p += 64;
        }
#endif
        for (; p < end; ++p) {
            trailing = plan.runBytes.Contains(*p) ? trailing + 1 : 0;
            if (trailing >= minimumLength) return p + 1 - minimumLength;
        }
        return nullptr;
    }

    template<bool single, bool simple, bool four = false>
    const unsigned char* Find(const unsigned char* p, const unsigned char* end, const unsigned char*& matchEnd, FilterBudget& budget) const {
        if (size_t(end - p) < minimumLength) return nullptr;
        const unsigned char* limit = end - minimumLength + 1;
#if defined(__aarch64__)
        const uint8x16_t low = vdupq_n_u8(15);
        const uint8x16_t a = vld1q_u8(masks[0]), b = vld1q_u8(masks[1]);
        const uint8x16_t c = vld1q_u8(masks[2]), d = vld1q_u8(masks[3]);
        const uint8x16_t e = vld1q_u8(masks[4]), f = vld1q_u8(masks[5]);
        const uint8x16_t g = vld1q_u8(masks[6]), h = vld1q_u8(masks[7]);
        const uint8x16_t needleFirst = vdupq_n_u8(plan.literals[0][first]);
        const uint8x16_t needleSecond = vdupq_n_u8(plan.literals[0][second]);
        const uint8x16_t f0 = vdupq_n_u8(pairBytes[0]), f1 = vdupq_n_u8(pairBytes[1]);
        const uint8x16_t s0 = vdupq_n_u8(pairBytes[2]), s1 = vdupq_n_u8(pairBytes[3]);
        auto filter = [&](const unsigned char* at) {
            uint8x16_t x = vld1q_u8(at + first), y = vld1q_u8(at + second);
            uint8x16_t result;
            if constexpr(single) result = vandq_u8(vceqq_u8(x, needleFirst), vceqq_u8(y, needleSecond));
            else if constexpr(simple) result = vandq_u8(vorrq_u8(vceqq_u8(x, f0), vceqq_u8(x, f1)),
                                                     vorrq_u8(vceqq_u8(y, s0), vceqq_u8(y, s1)));
            else result = vandq_u8(vandq_u8(vqtbl1q_u8(a, vandq_u8(x, low)), vqtbl1q_u8(b, vshrq_n_u8(x, 4))),
                                 vandq_u8(vqtbl1q_u8(c, vandq_u8(y, low)), vqtbl1q_u8(d, vshrq_n_u8(y, 4))));
            if constexpr(four) {
                x = vld1q_u8(at + third); y = vld1q_u8(at + fourth);
                result = vandq_u8(result, vandq_u8(
                    vandq_u8(vqtbl1q_u8(e, vandq_u8(x, low)), vqtbl1q_u8(f, vshrq_n_u8(x, 4))),
                    vandq_u8(vqtbl1q_u8(g, vandq_u8(y, low)), vqtbl1q_u8(h, vshrq_n_u8(y, 4)))));
            }
            return result;
        };
        auto verify = [&](const unsigned char* at, uint8x16_t candidates) -> const unsigned char* {
            if constexpr(!single && !simple) candidates = vcgtq_u8(candidates, vdupq_n_u8(0));
            uint64_t positions = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(candidates), 4)), 0);
            while (positions) {
                unsigned shift = __builtin_ctzll(positions) & ~3u;
                const unsigned char* candidate = at + shift / 4;
                if ((matchEnd = VerifyCandidate<single>(candidate, end))) return candidate;
                if (budget.Reject(candidate, verificationCost)) return candidate;
                positions &= ~(uint64_t(15) << shift);
            }
            return nullptr;
        };
        if (limit - p >= 16) {
            if (const unsigned char* found = verify(p, filter(p))) return found;
            p += 16;
        }
        while (limit - p >= 64) {
            uint8x16_t m0 = filter(p), m1 = filter(p + 16), m2 = filter(p + 32), m3 = filter(p + 48);
            if (vmaxvq_u8(vorrq_u8(vorrq_u8(m0, m1), vorrq_u8(m2, m3)))) {
                const unsigned char* found;
                if ((found = verify(p, m0)) || (found = verify(p + 16, m1)) ||
                    (found = verify(p + 32, m2)) || (found = verify(p + 48, m3))) return found;
            }
            p += 64;
        }
        while (limit - p >= 16) {
            if (const unsigned char* found = verify(p, filter(p))) return found;
            p += 16;
        }
#elif defined(JP_LITERAL_X86)
        if (vectorBytes == 64) return FindAvx512<single, simple, four>(p, end, matchEnd, budget);
        if (vectorBytes == 32) return FindAvx2<single, simple, four>(p, end, matchEnd, budget);
        return FindSsse3<single, simple, four>(p, end, matchEnd, budget);
#endif
        return FindScalar<single>(p, end, matchEnd, budget);
    }

    template<bool single>
    const unsigned char* FindScalar(const unsigned char* p, const unsigned char* end, const unsigned char*& matchEnd, FilterBudget& budget) const {
        const unsigned char* limit = end - minimumLength + 1;
        for (; p < limit; ++p) {
            unsigned char x = p[first], y = p[second];
            if (masks[0][x & 15] & masks[1][x >> 4] & masks[2][y & 15] & masks[3][y >> 4]) {
                if ((matchEnd = VerifyCandidate<single>(p, end))) return p;
                if (budget.Reject(p, verificationCost)) return p;
            }
        }
        return nullptr;
    }

#if defined(JP_LITERAL_X86)
    template<bool single, bool simple, bool four>
    __attribute__((target("ssse3")))
    const unsigned char* FindSsse3(const unsigned char* p, const unsigned char* end, const unsigned char*& matchEnd, FilterBudget& budget) const {
        const unsigned char* limit = end - minimumLength + 1;
        const __m128i low = _mm_set1_epi8(15), zero = _mm_setzero_si128();
        const __m128i a = _mm_loadu_si128((const __m128i*)masks[0]), b = _mm_loadu_si128((const __m128i*)masks[1]);
        const __m128i c = _mm_loadu_si128((const __m128i*)masks[2]), d = _mm_loadu_si128((const __m128i*)masks[3]);
        const __m128i e = _mm_loadu_si128((const __m128i*)masks[4]), f = _mm_loadu_si128((const __m128i*)masks[5]);
        const __m128i g = _mm_loadu_si128((const __m128i*)masks[6]), h = _mm_loadu_si128((const __m128i*)masks[7]);
        const __m128i nf = _mm_set1_epi8(plan.literals[0][first]), ns = _mm_set1_epi8(plan.literals[0][second]);
        const __m128i f0 = _mm_set1_epi8(pairBytes[0]), f1 = _mm_set1_epi8(pairBytes[1]);
        const __m128i s0 = _mm_set1_epi8(pairBytes[2]), s1 = _mm_set1_epi8(pairBytes[3]);
        auto filter = [&](const unsigned char* at) __attribute__((target("ssse3"))) {
            __m128i x = _mm_loadu_si128((const __m128i*)(at + first)), y = _mm_loadu_si128((const __m128i*)(at + second));
            __m128i result;
            if constexpr(single) result = _mm_and_si128(_mm_cmpeq_epi8(x, nf), _mm_cmpeq_epi8(y, ns));
            else if constexpr(simple) result = _mm_and_si128(_mm_or_si128(_mm_cmpeq_epi8(x, f0), _mm_cmpeq_epi8(x, f1)),
                                                          _mm_or_si128(_mm_cmpeq_epi8(y, s0), _mm_cmpeq_epi8(y, s1)));
            else result = _mm_and_si128(
                _mm_and_si128(_mm_shuffle_epi8(a, _mm_and_si128(x, low)), _mm_shuffle_epi8(b, _mm_and_si128(_mm_srli_epi16(x, 4), low))),
                _mm_and_si128(_mm_shuffle_epi8(c, _mm_and_si128(y, low)), _mm_shuffle_epi8(d, _mm_and_si128(_mm_srli_epi16(y, 4), low))));
            if constexpr(four) {
                x = _mm_loadu_si128((const __m128i*)(at + third)); y = _mm_loadu_si128((const __m128i*)(at + fourth));
                result = _mm_and_si128(result, _mm_and_si128(
                    _mm_and_si128(_mm_shuffle_epi8(e, _mm_and_si128(x, low)), _mm_shuffle_epi8(f, _mm_and_si128(_mm_srli_epi16(x, 4), low))),
                    _mm_and_si128(_mm_shuffle_epi8(g, _mm_and_si128(y, low)), _mm_shuffle_epi8(h, _mm_and_si128(_mm_srli_epi16(y, 4), low)))));
            }
            return result;
        };
        auto verify = [&](const unsigned char* at, __m128i candidates) __attribute__((target("ssse3"))) -> const unsigned char* {
            unsigned positions = unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(candidates, zero))) ^ 65535;
            while (positions) {
                const unsigned char* candidate = at + __builtin_ctz(positions);
                if ((matchEnd = VerifyCandidate<single>(candidate, end))) return candidate;
                if (budget.Reject(candidate, verificationCost)) return candidate;
                positions &= positions - 1;
            }
            return nullptr;
        };
        if (limit - p >= 16) {
            if (const unsigned char* found = verify(p, filter(p))) return found;
            p += 16;
        }
        while (limit - p >= 64) {
            __m128i m0 = filter(p), m1 = filter(p + 16), m2 = filter(p + 32), m3 = filter(p + 48);
            __m128i combined = _mm_or_si128(_mm_or_si128(m0, m1), _mm_or_si128(m2, m3));
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(combined, zero)) != 65535) {
                const unsigned char* found;
                if ((found = verify(p, m0)) || (found = verify(p + 16, m1)) ||
                    (found = verify(p + 32, m2)) || (found = verify(p + 48, m3))) return found;
            }
            p += 64;
        }
        while (limit - p >= 16) {
            if (const unsigned char* found = verify(p, filter(p))) return found;
            p += 16;
        }
        return FindScalar<single>(p, end, matchEnd, budget);
    }

    template<bool single, bool simple, bool four>
    __attribute__((target("avx2")))
    const unsigned char* FindAvx2(const unsigned char* p, const unsigned char* end, const unsigned char*& matchEnd, FilterBudget& budget) const {
        const unsigned char* limit = end - minimumLength + 1;
        const __m256i low = _mm256_set1_epi8(15);
        const __m256i a = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)masks[0]));
        const __m256i b = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)masks[1]));
        const __m256i c = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)masks[2]));
        const __m256i d = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)masks[3]));
        const __m256i e = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)masks[4]));
        const __m256i f = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)masks[5]));
        const __m256i g = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)masks[6]));
        const __m256i h = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i*)masks[7]));
        const __m256i nf = _mm256_set1_epi8(plan.literals[0][first]), ns = _mm256_set1_epi8(plan.literals[0][second]);
        const __m256i f0 = _mm256_set1_epi8(pairBytes[0]), f1 = _mm256_set1_epi8(pairBytes[1]);
        const __m256i s0 = _mm256_set1_epi8(pairBytes[2]), s1 = _mm256_set1_epi8(pairBytes[3]);
        auto filter = [&](const unsigned char* at) __attribute__((target("avx2"))) -> uint64_t {
            __m256i x = _mm256_loadu_si256((const __m256i*)(at + first)), y = _mm256_loadu_si256((const __m256i*)(at + second));
            __m256i result;
            if constexpr(single) result = _mm256_and_si256(_mm256_cmpeq_epi8(x, nf), _mm256_cmpeq_epi8(y, ns));
            else if constexpr(simple) result = _mm256_and_si256(_mm256_or_si256(_mm256_cmpeq_epi8(x, f0), _mm256_cmpeq_epi8(x, f1)),
                                                               _mm256_or_si256(_mm256_cmpeq_epi8(y, s0), _mm256_cmpeq_epi8(y, s1)));
            else result = _mm256_and_si256(
                _mm256_and_si256(_mm256_shuffle_epi8(a, _mm256_and_si256(x, low)), _mm256_shuffle_epi8(b, _mm256_and_si256(_mm256_srli_epi16(x, 4), low))),
                _mm256_and_si256(_mm256_shuffle_epi8(c, _mm256_and_si256(y, low)), _mm256_shuffle_epi8(d, _mm256_and_si256(_mm256_srli_epi16(y, 4), low))));
            if constexpr(four) {
                x = _mm256_loadu_si256((const __m256i*)(at + third)); y = _mm256_loadu_si256((const __m256i*)(at + fourth));
                result = _mm256_and_si256(result, _mm256_and_si256(
                    _mm256_and_si256(_mm256_shuffle_epi8(e, _mm256_and_si256(x, low)), _mm256_shuffle_epi8(f, _mm256_and_si256(_mm256_srli_epi16(x, 4), low))),
                    _mm256_and_si256(_mm256_shuffle_epi8(g, _mm256_and_si256(y, low)), _mm256_shuffle_epi8(h, _mm256_and_si256(_mm256_srli_epi16(y, 4), low)))));
            }
            return uint32_t(~_mm256_movemask_epi8(_mm256_cmpeq_epi8(result, _mm256_setzero_si256())));
        };
        while (limit - p >= 32) {
            uint64_t positions = filter(p);
            while (positions) {
                const unsigned char* candidate = p + __builtin_ctzll(positions);
                if ((matchEnd = VerifyCandidate<single>(candidate, end))) return candidate;
                if (budget.Reject(candidate, verificationCost)) return candidate;
                positions &= positions - 1;
            }
            p += 32;
        }
        return FindSsse3<single, simple, four>(p, end, matchEnd, budget);
    }

    template<bool single, bool simple, bool four>
    __attribute__((target("avx512f,avx512bw")))
    const unsigned char* FindAvx512(const unsigned char* p, const unsigned char* end, const unsigned char*& matchEnd, FilterBudget& budget) const {
        const unsigned char* limit = end - minimumLength + 1;
        const __m512i low = _mm512_set1_epi8(15);
        const __m512i a = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i*)masks[0]));
        const __m512i b = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i*)masks[1]));
        const __m512i c = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i*)masks[2]));
        const __m512i d = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i*)masks[3]));
        const __m512i e = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i*)masks[4]));
        const __m512i f = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i*)masks[5]));
        const __m512i g = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i*)masks[6]));
        const __m512i h = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i*)masks[7]));
        const __m512i nf = _mm512_set1_epi8(plan.literals[0][first]), ns = _mm512_set1_epi8(plan.literals[0][second]);
        const __m512i f0 = _mm512_set1_epi8(pairBytes[0]), f1 = _mm512_set1_epi8(pairBytes[1]);
        const __m512i s0 = _mm512_set1_epi8(pairBytes[2]), s1 = _mm512_set1_epi8(pairBytes[3]);
        auto filter = [&](const unsigned char* at) __attribute__((target("avx512f,avx512bw"))) -> uint64_t {
            __m512i x = _mm512_loadu_si512((const __m512i*)(at + first)), y = _mm512_loadu_si512((const __m512i*)(at + second));
            if constexpr(single) return _mm512_cmpeq_epi8_mask(x, nf) & _mm512_cmpeq_epi8_mask(y, ns);
            if constexpr(simple) return (_mm512_cmpeq_epi8_mask(x, f0) | _mm512_cmpeq_epi8_mask(x, f1)) &
                                        (_mm512_cmpeq_epi8_mask(y, s0) | _mm512_cmpeq_epi8_mask(y, s1));
            __m512i result = _mm512_and_si512(
                _mm512_and_si512(_mm512_shuffle_epi8(a, _mm512_and_si512(x, low)), _mm512_shuffle_epi8(b, _mm512_and_si512(_mm512_srli_epi16(x, 4), low))),
                _mm512_and_si512(_mm512_shuffle_epi8(c, _mm512_and_si512(y, low)), _mm512_shuffle_epi8(d, _mm512_and_si512(_mm512_srli_epi16(y, 4), low))));
            if constexpr(four) {
                x = _mm512_loadu_si512((const __m512i*)(at + third)); y = _mm512_loadu_si512((const __m512i*)(at + fourth));
                result = _mm512_and_si512(result, _mm512_and_si512(
                    _mm512_and_si512(_mm512_shuffle_epi8(e, _mm512_and_si512(x, low)), _mm512_shuffle_epi8(f, _mm512_and_si512(_mm512_srli_epi16(x, 4), low))),
                    _mm512_and_si512(_mm512_shuffle_epi8(g, _mm512_and_si512(y, low)), _mm512_shuffle_epi8(h, _mm512_and_si512(_mm512_srli_epi16(y, 4), low)))));
            }
            return _mm512_test_epi8_mask(result, result);
        };
        while (limit - p >= 64) {
            uint64_t positions = filter(p);
            while (positions) {
                const unsigned char* candidate = p + __builtin_ctzll(positions);
                if ((matchEnd = VerifyCandidate<single>(candidate, end))) return candidate;
                if (budget.Reject(candidate, verificationCost)) return candidate;
                positions &= positions - 1;
            }
            p += 64;
        }
        return FindSsse3<single, simple, four>(p, end, matchEnd, budget);
    }
#endif

    const void* Fallback(const void* data, size_t length, size_t offset, const char** captures, bool spanOnly) const {
        if (spanOnly) {
            auto result = processor->LocatePartialMatch(data, length, offset);
            captures[0] = static_cast<const char*>(result.min);
            captures[1] = static_cast<const char*>(result.max);
            return result.max;
        }
        return captures ? processor->PartialMatch(data, length, offset, captures)
                        : processor->PartialMatch(data, length, offset);
    }

    const void* SearchDelimited(const void* data, size_t length, size_t offset, const char** captures, bool spanOnly) const {
        const auto* begin = static_cast<const unsigned char*>(data);
        const auto* end = begin + length;
        const unsigned char* candidate = nullptr;
        const unsigned char* matchEnd = nullptr;
        FilterBudget budget{begin + offset};
        const unsigned char* position = begin + offset;
        unsigned rejected = 0;
        while ((candidate = Find(position, end, matchEnd, budget))) {
            const unsigned char* body = candidate + 1;
            size_t available = end - body;
            if (plan.match.delimiterBody.maximum != UNBOUNDED)
                available = std::min(available, size_t(plan.match.delimiterBody.maximum) + plan.match.delimiterTail + 1);
            const unsigned char* close = nullptr;
            if (available) close = plan.match.closingBytes.size() == 1
                ? static_cast<const unsigned char*>(std::memchr(body, plan.match.closingBytes[0], available))
                : static_cast<const unsigned char*>(PatternProcessor::FindByteEitherOf2(body,
                    plan.match.closingBytes[0] | (uint64_t(plan.match.closingBytes[1]) << 8), body + available));
            if (close && size_t(close - body) >= size_t(plan.match.delimiterBody.minimum) + plan.match.delimiterTail &&
                (!plan.match.delimiterTail || plan.match.delimiterLastByte.Contains(close[-1]))) {
                if (!captures || numberOfCaptures == 1 || spanOnly) {
                    if (captures) { captures[0] = reinterpret_cast<const char*>(candidate); captures[1] = reinterpret_cast<const char*>(close + 1); }
                    return close + 1;
                }
                return processor->PopulateCaptures(data, length, candidate - begin, captures);
            }
            if (!close && plan.match.delimiterBody.maximum == UNBOUNDED) return nullptr;
            // Openers cannot occur in the body, so scans never overlap.
            if (!plan.match.delimiterDisjointOpener && ++rejected == 8)
                return captures ? processor->PartialMatch(data, length, offset, captures)
                                : processor->PartialMatch(data, length, offset);
            // An opener may occur inside a failed body.
            position = candidate + 1;
        }
        return nullptr;
    }

    const void* SearchWholeRun(const void* data, size_t length, size_t offset, const char** captures, bool spanOnly) const {
        const auto* begin = static_cast<const unsigned char*>(data);
        const auto* end = begin + length;
        const unsigned char* matchEnd = nullptr;
        FilterBudget budget{begin + offset};
        const unsigned char* candidate = Find(begin + offset, end, matchEnd, budget);
        if (budget.IsAbandoned()) return Fallback(data, length, offset, captures, spanOnly);
        if (!candidate) return nullptr;
        if (!captures || numberOfCaptures == 1 || spanOnly) {
            const unsigned char* start = begin + offset;
            const unsigned char* finish = end;
            if (plan.match.excludedRunByte >= 0) {
                if (const auto* separator = static_cast<const unsigned char*>(ReverseProcessor::FindByteReverse(candidate, plan.match.excludedRunByte, start))) start = separator + 1;
                if (const auto* separator = static_cast<const unsigned char*>(std::memchr(matchEnd, plan.match.excludedRunByte, end - matchEnd))) finish = separator;
            }
            if (captures) { captures[0] = reinterpret_cast<const char*>(start); captures[1] = reinterpret_cast<const char*>(finish); }
            return finish;
        }
        return processor->PartialMatch(data, length, offset, captures);
    }

    const unsigned char* MatchRuns(const unsigned char* begin, const unsigned char* end,
                                   const unsigned char* start, const unsigned char* candidate,
                                   const unsigned char* matchEnd, size_t& verificationWork) const {
        bool valid = true;
        const unsigned char* finish = matchEnd;
        if (plan.match.greedyLiteralSuffix) {
            const auto& run = plan.reversePrefix[0];
            const auto& literal = plan.literals[0];
            size_t reach = std::min(size_t(end - start), size_t(run.maximum) + literal.size());
            const unsigned char* limit = start + reach;
            while (finish < limit && run.Contains(*finish)) {
                ++finish;
                if (++verificationWork > 256) return nullptr;
            }
            const unsigned char* last = finish - literal.size();
            while (last > candidate && std::memcmp(last, literal.data(), literal.size())) --last;
            finish = last + literal.size();
        }
        for (const auto& run : plan.match.forwardSuffix) {
            size_t count = 0;
            while (finish < end && count < run.maximum && run.Contains(*finish)) {
                ++finish; ++count;
                if (++verificationWork > 256) return nullptr;
            }
            if (count < run.minimum) { valid = false; break; }
        }
        if (plan.match.startWordBoundary && (start > begin && IsWord(start[-1])) == (start < end && IsWord(*start))) valid = false;
        if (plan.match.endWordBoundary && (finish > begin && IsWord(finish[-1])) == (finish < end && IsWord(*finish))) valid = false;
        if (plan.match.requireEnd && finish != end) valid = false;
        return valid ? finish : nullptr;
    }

    const void* Search(const void* data, size_t length, size_t offset, const char** captures, bool spanOnly = false) const {
        if (offset > length || length - offset < plan.minimumPrefix) return nullptr;
        if (plan.match.kind == MatchKind::Delimited)
            return SearchDelimited(data, length, offset, captures, spanOnly);
        if (plan.match.kind == MatchKind::WholeRun)
            return SearchWholeRun(data, length, offset, captures, spanOnly);
        const auto* begin = static_cast<const unsigned char*>(data);
        const auto* end = begin + length;
        FilterBudget budget{begin + offset};
        const unsigned char* candidate = nullptr;
        const unsigned char* matchEnd = nullptr;
        if (plan.endAnchored) {
            // The longest alternative gives the leftmost start.
            for (const auto& literal : plan.literals) {
                size_t required = plan.suffixLength + literal.size();
                if (required > length - offset) continue;
                const unsigned char* at = end - required;
                if (candidate && at >= candidate) continue;
                if (std::memcmp(at, literal.data(), literal.size()) != 0) continue;
                candidate = at;
                matchEnd = at + literal.size();
            }
            if (!candidate) return nullptr;
            if (plan.reversePrefix.empty() && !plan.match.IsComplete())
                return captures ? processor->PartialMatch(data, length, offset, captures) : processor->PartialMatch(data, length, offset);
        }
        else candidate = Find(begin + offset + plan.minimumPrefix, end, matchEnd, budget);
        size_t nextStart = offset;
        size_t verificationWork = 0, rejectedCandidates = 0;
        auto fallback = [&](size_t from) {
            return Fallback(data, length, from, captures, spanOnly);
        };
        while (candidate) {
            if (plan.reversePrefix.empty() && !plan.match.IsComplete() && plan.maximumPrefix != 0) {
                size_t start = size_t(candidate - begin) > plan.maximumPrefix ? size_t(candidate - begin) - plan.maximumPrefix : 0;
                start = std::max(start, offset);
                if (!plan.endAnchored && plan.maximumPrefix <= 128) {
                    // Never revisit a start when successive windows overlap.
                    size_t lastStart = size_t(candidate - begin) - plan.minimumPrefix;
                    const char* temporary[numberOfCaptures * 2];
                    const char** output = captures ? captures : temporary;
                    for (nextStart = std::max(nextStart, start); nextStart <= lastStart; ++nextStart) {
                        std::fill(output, output + numberOfCaptures * 2, nullptr);
                        if (const void* result = processor->PopulateCaptures(data, length, nextStart, output)) return result;
                        if (++rejectedCandidates == 8) return fallback(nextStart + 1);
                    }
                    candidate = Find(candidate + 1, end, matchEnd, budget);
                    continue;
                }
                return captures ? processor->PartialMatch(data, length, start, captures) : processor->PartialMatch(data, length, start);
            }
            const unsigned char* start = candidate;
            bool valid = true;
            for (auto it = plan.reversePrefix.rbegin(); it != plan.reversePrefix.rend(); ++it) {
                size_t count = 0;
                while (start > begin + offset && count < it->maximum && it->Contains(start[-1])) {
                    --start; ++count;
                    if (++verificationWork > 256) return fallback(offset);
                }
                if (count < it->minimum) { valid = false; break; }
            }
            if (valid) {
                if (plan.match.IsComplete() && (!captures || numberOfCaptures == 1 || spanOnly)) {
                    const unsigned char* finish = MatchRuns(begin, end, start, candidate, matchEnd, verificationWork);
                    if (verificationWork > 256) return fallback(offset);
                    if (finish) {
                        if (captures) { captures[0] = reinterpret_cast<const char*>(start); captures[1] = reinterpret_cast<const char*>(finish); }
                        return finish;
                    }
                } else {
                    // The search processor's DFA is cheaper than the capture NFA.
                    if ((!captures || spanOnly || numberOfCaptures == 1) &&
                        (!plan.maximumPrefix || !plan.reversePrefix.empty())) {
                        if (spanOnly) {
                            auto result = processor->LocatePartialMatch(data, length, start - begin);
                            if (!result.max) return nullptr;
                            captures[0] = static_cast<const char*>(result.min);
                            captures[1] = static_cast<const char*>(result.max);
                            return result.max;
                        }
                        return captures ? processor->PartialMatch(data, length, start - begin, captures)
                                        : processor->PartialMatch(data, length, start - begin);
                    }
                    const char* temporary[numberOfCaptures * 2];
                    const char** output = captures ? captures : temporary;
                    std::fill(output, output + numberOfCaptures * 2, nullptr);
                    const void* result = processor->PopulateCaptures(data, length, start - begin, output);
                    if (result) return result;
                    if (++rejectedCandidates == 8) return fallback(start - begin);
                }
            }
            if (plan.endAnchored) return nullptr;
            if (budget.Reject(candidate, verificationCost + 1)) return fallback(offset);
            candidate = Find(candidate + 1, end, matchEnd, budget);
        }
        if (budget.IsAbandoned()) return fallback(offset);
        return nullptr;
    }
};
}

LiteralPrefilter Javelin::PatternInternal::BuildLiteralPrefilter(const IComponent* component) {
#if !defined(__aarch64__) && !defined(JP_LITERAL_X86)
    // Keep the existing accelerator on targets without a vector implementation.
    return {};
#endif
#if defined(JP_LITERAL_X86)
    if (!__builtin_cpu_supports("ssse3")) return {};
    // Anchored matches already have a single candidate start. An additional
    // scan penalizes parsers that match and recover captures on every line.
    if (component->HasStartAnchor()) return {};
#endif
    // Candidate validation moves the start of search.
    if (HasSearchStart(component)) return {};
    // Long literals already use the skip-based search.
    if (component->GetMinimumLength() > 32 && IsByteProduct(component, true)) return {};
#if defined(JP_LITERAL_X86)
    // Standalone literals already have a fused native search. The wrapper
    // adds setup and another call even when there are no false candidates.
    Literals exact;
    if (Extract(component, exact) && exact.size() == 1) {
        // Longer UTF-8 literals benefit from comparing separated continuation
        // bytes: adjacent bytes share encoding structure and reject less text.
        const auto& literal = exact[0];
        size_t wideLeads = 0;
        for(unsigned char c : literal) wideLeads += c >= 0xe0 && c < 0xfe;
        if(literal.size() < 16 || literal.size() > 32 || wideLeads < 4) return {};
    }
    // Prefer the JIT's fused search for short ASCII alternatives. Small
    // alphabets and UTF-8 benefit from the extra filter columns.
    if (component->GetMinimumLength() >= 8 && component->GetMaximumLength() <= 32) {
        LiteralPrefilter prefix = BuildBytePrefixFilter(component);
        if (prefix.prefixBytes.size() > 1) {
            LiteralPrefixRun alphabet;
            for (const auto& branch : prefix.prefixBytes) for (const auto& byte : branch)
                for (size_t w = 0; w < 4; ++w) alphabet.bytes[w] |= byte.bytes[w];
            unsigned count = __builtin_popcountll(alphabet.bytes[0]) + __builtin_popcountll(alphabet.bytes[1]);
            if (!alphabet.bytes[2] && !alphabet.bytes[3] && count >= 16) return {};
        }
    }
#endif
    LiteralPrefilter byteRun = BuildByteRunFilter(component);
    if (byteRun.HasData()) return byteRun;
    std::vector<const IComponent*> parts;
    Flatten(component, parts);
    LiteralPrefilter delimited = BuildDelimitedFilter(parts);
    if (delimited.HasData()) return delimited;
    LiteralPrefilter wholeRun = BuildWholeRunFilter(parts);
    if (wholeRun.HasData()) return wholeRun;
    LiteralPrefilter best;
    size_t minimumPrefix = 0, maximumPrefix = 0;
    size_t bestScore = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        Literals literals;
        if (Extract(parts[i], literals)) {
            size_t j = i + 1;
            for (; j < parts.size(); ++j) {
                Literals next;
                if (!Extract(parts[j], next) || !Concatenate(literals, next)) break;
            }
            size_t minimumLength = 128;
            for (const auto& literal : literals) minimumLength = std::min(minimumLength, literal.size());
            Literals initialLiterals;
            bool usefulShortSuffix = minimumLength && minimumPrefix && maximumPrefix <= 128 &&
                literals.size() == 1 && !Extract(parts[0], initialLiterals);
            if (minimumLength >= 3 || usefulShortSuffix) {
                size_t suffix = 0;
                bool fixedSuffix = true;
                for (size_t k = j; k < parts.size(); ++k) {
                    if (!parts[k]->IsFixedLength()) { fixedSuffix = false; break; }
                    suffix = AddLength(suffix, parts[k]->GetMinimumLength());
                }
                bool anchored = component->HasEndAnchor() && fixedSuffix && suffix < UNBOUNDED;
                size_t score = minimumLength * 16 / literals.size() + (anchored ? 1024 : 0);
                if (score > bestScore) {
                    bestScore = score;
                    best.literals = std::move(literals);
                    best.minimumPrefix = minimumPrefix;
                    best.maximumPrefix = maximumPrefix;
                    best.endAnchored = anchored;
                    best.suffixLength = suffix;
                    best.unboundedVerification = component->GetMaximumLength() == UNBOUNDED;
                    best.reversePrefix.clear();
                    best.match.forwardSuffix.clear();
                    best.match.startWordBoundary = best.match.endWordBoundary = best.match.requireEnd = false;
                    best.match.greedyLiteralSuffix = false;
                    bool reversible = i > 0;
                    bool prefixAssertion = false;
                    for (size_t k = 0; k < i && reversible; ++k) {
                        if (k == 0 && i > 1) {
                            const auto* assertion = dynamic_cast<const AssertComponent*>(parts[k]);
                            if (assertion && assertion->assertType == AssertType::WordBoundary) {
                                prefixAssertion = true;
                                continue;
                            }
                        }
                        LiteralPrefixRun run;
                        if (!PrefixRun(parts[k], run)) { reversible = false; break; }
                        if (prefixAssertion && best.reversePrefix.empty()) {
                            if (!run.minimum) reversible = false;
                            // A failed leading boundary stays false within the word run.
                            for (unsigned c = 0; c < 256; ++c)
                                if (run.Contains(c) && !IsWord(c)) reversible = false;
                        }
                        for (auto previous = best.reversePrefix.rbegin(); previous != best.reversePrefix.rend(); ++previous) {
                            for (size_t w = 0; w < 4; ++w)
                                if (run.bytes[w] & previous->bytes[w]) reversible = false;
                            if (previous->minimum) break;
                        }
                        best.reversePrefix.push_back(run);
                    }
                    bool overlappingPrefix = false;
                    if (reversible) for (const auto& literal : best.literals) {
                        if (!anchored && minimumPrefix != maximumPrefix)
                            for (auto run = best.reversePrefix.rbegin(); run != best.reversePrefix.rend(); ++run) {
                                if (run->Contains(literal[0])) overlappingPrefix = true;
                                if (run->minimum) break;
                            }
                        if (anchored && literal.size() != best.literals[0].size()) reversible = false;
                    }
                    if (!reversible) best.reversePrefix.clear();
                    best.match.kind = !prefixAssertion && !overlappingPrefix && (i == 0 || reversible) &&
                        (j == parts.size() || (j + 1 == parts.size() &&
                            dynamic_cast<const AssertComponent*>(parts[j]) && parts[j]->HasEndAnchor())) ? MatchKind::Runs : MatchKind::None;
                    if (reversible && !prefixAssertion && !anchored && best.reversePrefix.size() == 1 &&
                        best.literals.size() == 1 && j == parts.size()) {
                        bool allInRun = true;
                        for (unsigned char c : best.literals[0]) allInRun &= best.reversePrefix[0].Contains(c);
                        if (allInRun) { best.match.kind = MatchKind::Runs; best.match.greedyLiteralSuffix = true; }
                    }
                    if (!overlappingPrefix && (i == 0 || reversible) && best.literals.size() == 1) {
                        bool complete = true;
                        for (size_t k = j; k < parts.size(); ++k) {
                            if (k + 1 == parts.size()) {
                                const auto* assertion = dynamic_cast<const AssertComponent*>(parts[k]);
                                if (assertion && assertion->HasEndAnchor()) { best.match.requireEnd = true; break; }
                                if (assertion && assertion->assertType == AssertType::WordBoundary) {
                                    if (!best.match.forwardSuffix.empty()) {
                                        if (!best.match.forwardSuffix.back().minimum) complete = false;
                                        for (unsigned c = 0; c < 256; ++c)
                                            if (best.match.forwardSuffix.back().Contains(c) && !IsWord(c)) complete = false;
                                    }
                                    best.match.endWordBoundary = true;
                                    break;
                                }
                            }
                            LiteralPrefixRun run;
                            if (!PrefixRun(parts[k], run)) { complete = false; break; }
                            for (auto previous = best.match.forwardSuffix.rbegin(); previous != best.match.forwardSuffix.rend(); ++previous) {
                                for (size_t w = 0; w < 4; ++w)
                                    if (run.bytes[w] & previous->bytes[w]) complete = false;
                                if (previous->minimum) break;
                            }
                            best.match.forwardSuffix.push_back(run);
                        }
                        if (complete) {
                            best.match.kind = MatchKind::Runs;
                            best.match.startWordBoundary = prefixAssertion;
                        } else best.match.forwardSuffix.clear();
                    }
                }
            }
        }
        minimumPrefix = AddLength(minimumPrefix, parts[i]->GetMinimumLength());
        maximumPrefix = AddLength(maximumPrefix, parts[i]->GetMaximumLength());
    }
    if (!best.HasData()) {
        Literals tail;
        if (ExtractTail(component, tail)) {
            size_t length = tail[0].size();
            bool sameLength = true;
            for (const auto& literal : tail) sameLength &= literal.size() == length;
            if (sameLength && length >= 2 && component->GetMinimumLength() >= length) {
                best.literals = std::move(tail);
                best.minimumPrefix = component->GetMinimumLength() - length;
                best.maximumPrefix = component->GetMaximumLength();
                if (best.maximumPrefix != UNBOUNDED) best.maximumPrefix -= length;
                best.unboundedVerification = component->GetMaximumLength() == UNBOUNDED;
                return best;
            }
        }
        return BuildBytePrefixFilter(component);
    }
    // A case-folded prefix avoids enumerating exponentially many literals.
    if (best.literals.size() > 1 && !best.match.IsComplete() && !best.endAnchored) {
        LiteralPrefilter prefix = BuildBytePrefixFilter(component);
        if (prefix.HasData()) return prefix;
    }
    CollapseLiteralProduct(best);
    return best;
}

PatternProcessor* Javelin::PatternInternal::CreateLiteralPrefilterProcessor(PatternProcessor* processor,
                                                                         LiteralPrefilter&& filter,
                                                                         size_t captures) {
    if (!filter.HasData()) return processor;
    return new LiteralPrefilterProcessor(processor, std::move(filter), captures);
}
