//============================================================================

#include "Javelin/Pattern/Internal/PatternScanOptimizer.h"
#include "Javelin/Pattern/Internal/PatternComponent.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

//============================================================================

using namespace Javelin;
using namespace Javelin::PatternInternal;

//============================================================================

namespace
{
enum class WordMode
{
	None,
	Ascii,
	UnicodeByte,
	UnicodeUtf8
};

bool AsciiWord(uint32_t c)
{
	return c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z');
}

// Match the compiler's canonical UTF-8 encoding, including its supported
// five/six-byte characters. Malformed/truncated sequences consume one byte
// for searching but never belong to a character class.
uint32_t ReadCharacter(const unsigned char*& p, const unsigned char* end)
{
	uint32_t c = *p++;
	if(c < 128) return c;
	unsigned extra;
	uint32_t minimum;
	if(c >= 0xc2 && c < 0xe0)
	{
		c &= 0x1f;
		extra = 1;
		minimum = 0x80;
	}
	else if(c >= 0xe0 && c < 0xf0)
	{
		c &= 0xf;
		extra = 2;
		minimum = 0x800;
	}
	else if(c >= 0xf0 && c < 0xf8)
	{
		c &= 7;
		extra = 3;
		minimum = 0x10000;
	}
	else if(c >= 0xf8 && c < 0xfc)
	{
		c &= 3;
		extra = 4;
		minimum = 0x200000;
	}
	else if(c >= 0xfc && c < 0xfe)
	{
		c &= 1;
		extra = 5;
		minimum = 0x4000000;
	}
	else
		return UINT32_MAX;
	if(size_t(end - p) < extra) return UINT32_MAX;
	for(unsigned i = 0; i < extra; ++i)
	{
		if((p[i] & 0xc0) != 0x80) return UINT32_MAX;
		c = (c << 6) | (p[i] & 0x3f);
	}
	if(c < minimum) return UINT32_MAX;
	p += extra;
	return c;
}

const IComponent* Unwrap(const IComponent* component)
{
	while(const auto* capture = dynamic_cast<const CaptureComponent*>(component))
	{
		if(capture->isBackReferenceTarget || capture->isRecurseTarget || capture->hasResetCapture) return nullptr;
		component = capture->content;
	}
	return component;
}

void Flatten(const IComponent* component, std::vector<const IComponent*>& parts)
{
	component = Unwrap(component);
	if(const auto* group = dynamic_cast<const ConcatenateComponent*>(component))
	{
		for(const auto* part : group->componentList) Flatten(part, parts);
	}
	else
		parts.push_back(component);
}

WordMode Boundary(const IComponent* component)
{
	if(const auto* assertion = dynamic_cast<const AssertComponent*>(component))
		if(assertion->assertType == AssertType::WordBoundary) return WordMode::Ascii;
	if(const auto* assertion = dynamic_cast<const UnicodeWordBoundaryComponent*>(component))
		if(assertion->boundary) return assertion->useUtf8 ? WordMode::UnicodeUtf8 : WordMode::UnicodeByte;
	return WordMode::None;
}

class CharacterRunScanner final : public PatternScanOptimizer
{
public:
	CharacterRunScanner(const CharacterRangeListComponent& range, const CounterComponent& repeat, WordMode wordMode,
	                    bool startBoundary, bool endBoundary)
	    : minimum(repeat.minimum), maximum(repeat.maximum), wordMode(wordMode), startBoundary(startBoundary),
	      endBoundary(endBoundary)
	{
		useUtf8 = range.useUtf8 && range.characterRangeList.Back().max >= 128;
		for(const auto& interval : range.characterRangeList)
		{
			const uint32_t last = std::min(uint32_t(interval.max), uint32_t(low.size() - 1));
			for(uint32_t c = interval.min; c <= last; ++c) low[c] = 1;
			if(interval.max >= low.size()) high.push_back({uint32_t(interval.min), uint32_t(interval.max)});
		}
	}

	int Scan(const void* data, size_t length, void* user, MatchCallback onMatch, size_t offset) const override
	{
		if(offset >= length) return 0;
		if(useUtf8) return Dispatch<true>(data, length, user, onMatch, offset);
		return Dispatch<false>(data, length, user, onMatch, offset);
	}

private:
	std::array<uint8_t, 2048> low{};
	std::vector<std::pair<uint32_t, uint32_t>> high;
	uint32_t minimum, maximum;
	WordMode wordMode;
	bool useUtf8, startBoundary, endBoundary;

	bool Contains(uint32_t c) const
	{
		if(c < low.size()) return low[c];
		size_t left = 0, right = high.size();
		while(left < right)
		{
			size_t middle = (left + right) / 2;
			if(high[middle].second < c)
				left = middle + 1;
			else
				right = middle;
		}
		return left < high.size() && high[left].first <= c;
	}

	bool WordAt(const unsigned char* p, const unsigned char* end) const
	{
		if(p == end) return false;
		uint32_t c = *p;
		if(c < 128 || wordMode == WordMode::Ascii) return AsciiWord(c);
		if(wordMode == WordMode::UnicodeUtf8) c = ReadCharacter(p, end);
		return c <= 0x10ffff && CharacterRangeList::UNICODE_WORD_CHARACTERS.Contains(Character(c));
	}

	bool WordBefore(const unsigned char* p, const unsigned char* begin) const
	{
		if(p == begin) return false;
		const auto* end = p;
		--p;
		if(wordMode == WordMode::UnicodeUtf8)
		{
			for(unsigned i = 0; i < 5 && p > begin && (*p & 0xc0) == 0x80; ++i) --p;
			const auto* next = p;
			uint32_t c = ReadCharacter(next, end);
			if(next != end || c == UINT32_MAX) return false;
		}
		return WordAt(p, end);
	}

	template <bool utf8> uint32_t Read(const unsigned char*& p, const unsigned char* end) const
	{
		if constexpr(utf8)
			return ReadCharacter(p, end);
		else
			return *p++;
	}

	template <bool utf8> bool Has(uint32_t c) const
	{
		if constexpr(utf8)
			return Contains(c);
		else
			return low[c];
	}

	template <bool utf8>
	int Dispatch(const void* data, size_t length, void* user, MatchCallback onMatch, size_t offset) const
	{
		if(startBoundary || endBoundary) return ScanRuns<utf8, true, true>(data, length, user, onMatch, offset);
		if(maximum == UINT32_MAX) return ScanRuns<utf8, false, false>(data, length, user, onMatch, offset);
		return ScanRuns<utf8, true, false>(data, length, user, onMatch, offset);
	}

	template <bool utf8, bool bounded, bool boundaries>
	int ScanRuns(const void* data, size_t length, void* user, MatchCallback onMatch, size_t offset) const
	{
		const auto* begin = static_cast<const unsigned char*>(data);
		const auto* end = begin + length;
		const auto* p = begin + offset;
		while(p < end)
		{
			const auto* start = p;
			if(!Has<utf8>(Read<utf8>(p, end))) continue;
			size_t characters = 0;
			const auto* firstMaximum = end;
			for(;;)
			{
				++characters;
				if constexpr(bounded)
				{
					if(characters == maximum)
					{
						if constexpr(boundaries)
							firstMaximum = p;
						else
						{
							if(int result = onMatch(start - begin, p - begin, user)) return result;
							start = p;
							characters = 0;
						}
					}
				}
				if(p == end) break;
				const auto* next = p;
				if(!Has<utf8>(Read<utf8>(next, end))) break;
				p = next;
			}
			if constexpr(boundaries)
			{
				if(characters < minimum || (startBoundary && WordBefore(start, begin)) ||
				   (endBoundary && WordAt(p, end)))
					continue;
				// The Unicode assertion explicitly rejects positions inside a
				// UTF-8 sequence, even when that sequence is malformed.
				if(endBoundary && wordMode == WordMode::UnicodeUtf8 && p < end && (*p & 0xc0) == 0x80) continue;
				const auto* finish = p;
				if(maximum != UINT32_MAX && characters > maximum)
				{
					if(startBoundary && endBoundary) continue;
					if(endBoundary)
					{
						for(size_t skip = characters - maximum; skip; --skip) Read<utf8>(start, end);
					}
					else
						finish = firstMaximum;
				}
				if(int result = onMatch(start - begin, finish - begin, user)) return result;
			}
			else
			{
				if(characters >= minimum)
					if(int result = onMatch(start - begin, p - begin, user)) return result;
			}
		}
		return 0;
	}
};

std::unique_ptr<PatternScanOptimizer> BuildCharacterRun(const IComponent* root)
{
	std::vector<const IComponent*> parts;
	Flatten(root, parts);
	if(parts.empty()) return {};
	WordMode startMode = Boundary(parts.front()), endMode = Boundary(parts.back());
	size_t first = startMode != WordMode::None, last = parts.size() - (endMode != WordMode::None);
	if(last != first + 1 || !parts[first]) return {};
	if(startMode != WordMode::None && endMode != WordMode::None && startMode != endMode) return {};
	WordMode wordMode = startMode != WordMode::None ? startMode : endMode;
	const auto* repeat = dynamic_cast<const CounterComponent*>(parts[first]);
	if(!repeat || !repeat->minimum || repeat->mode != CounterComponent::Maximal) return {};
	const auto* range = dynamic_cast<const CharacterRangeListComponent*>(Unwrap(repeat->content));
	if(!range || range->characterRangeList.IsEmpty()) return {};
	// Keep native searches for long ASCII prefixes and runs without boundaries.
	if(repeat->minimum >= 8 && range->characterRangeList.Back().max < 128) return {};
	if(wordMode == WordMode::None && range->characterRangeList.Back().max < 128) return {};
	uint64_t alphabetSize = 0;
	uint32_t asciiAlphabet = 0;
	for(const auto& interval : range->characterRangeList)
	{
		if(!range->useUtf8 && interval.max > 255) return {};
		alphabetSize += uint64_t(interval.max) - interval.min + 1;
		if(interval.min < 128) asciiAlphabet += std::min(uint32_t(interval.max), 127u) - interval.min + 1;
		if(wordMode == WordMode::None) continue;
		if(wordMode == WordMode::Ascii)
		{
			if(interval.max >= 128) return {};
			for(uint32_t c = interval.min; c <= interval.max; ++c)
				if(!AsciiWord(c)) return {};
		}
		else
		{
			if((wordMode == WordMode::UnicodeByte && range->useUtf8 && interval.max >= 128) ||
			   (wordMode == WordMode::UnicodeUtf8 && !range->useUtf8 && interval.max >= 128))
				return {};
			// Every member must be a word character. Then boundaries can only
			// occur at the ends of a run, never between its characters.
			bool contained = false;
			for(const auto& word : CharacterRangeList::UNICODE_WORD_CHARACTERS)
				if(word.min <= interval.min && interval.max <= word.max)
				{
					contained = true;
					break;
				}
			if(!contained) return {};
		}
	}
	// Narrow alphabets and long homogeneous runs already have native SIMD
	// search/skip instructions. This plan is for broad positive classes.
	if(alphabetSize <= 4 || asciiAlphabet > 96 || (!range->useUtf8 && alphabetSize > 128)) return {};
	return std::make_unique<CharacterRunScanner>(*range, *repeat, wordMode, startMode != WordMode::None,
	                                             endMode != WordMode::None);
}

bool AddByteClass(const IComponent* component, std::array<uint8_t, 256>& table, uint8_t flag)
{
	component = Unwrap(component);
	if(const auto* byte = dynamic_cast<const ByteComponent*>(component))
	{
		table[byte->c] |= flag;
		return true;
	}
	const auto* range = dynamic_cast<const CharacterRangeListComponent*>(component);
	if(!range || range->characterRangeList.IsEmpty()) return false;
	for(const auto& interval : range->characterRangeList)
	{
		if(interval.max > (range->useUtf8 ? 127u : 255u)) return false;
		for(uint32_t c = interval.min; c <= interval.max; ++c) table[c] |= flag;
	}
	return true;
}

// For greedy R* C | D, scan each R run once to find its last C. After that
// match, only D can match until the run ends. This preserves match priority
// without rescanning the suffix for each match.
class GreedySuffixScanner final : public PatternScanOptimizer
{
public:
	std::array<uint8_t, 256> table{};
	int Scan(const void* data, size_t length, void* user, MatchCallback onMatch, size_t offset) const override
	{
		if(offset >= length) return 0;
		const auto* begin = static_cast<const unsigned char*>(data);
		const auto* p = begin + offset;
		const auto* end = begin + length;
		while(p < end)
		{
			const auto* start = p;
			const unsigned char* last = nullptr;
			while(p < end && (table[*p] & 1))
			{
				if(table[*p] & 2) last = p;
				++p;
			}
			// C may consume the first byte excluded from R (for example,
			// a newline excluded by dot but accepted by a negated class).
			if(p < end && (table[*p] & 2)) last = p++;
			if(last)
			{
				if(int result = onMatch(start - begin, last + 1 - begin, user)) return result;
				start = last + 1;
			}
			while(start < p)
			{
				if(table[*start] & 4)
					if(int result = onMatch(start - begin, start + 1 - begin, user)) return result;
				++start;
			}
			if(p < end && !(table[*p] & 1) && !(table[*p] & 2))
			{
				if(table[*p] & 4)
					if(int result = onMatch(p - begin, p + 1 - begin, user)) return result;
				++p;
			}
		}
		return 0;
	}
};

std::unique_ptr<PatternScanOptimizer> BuildGreedySuffix(const IComponent* root)
{
	const auto* alternatives = dynamic_cast<const AlternationComponent*>(Unwrap(root));
	if(!alternatives || alternatives->componentList.GetCount() != 2 ||
	   dynamic_cast<const CharacterRangeListComponent*>(alternatives))
		return {};
	std::vector<const IComponent*> parts;
	Flatten(alternatives->componentList[0], parts);
	if(parts.size() != 2) return {};
	const auto* repeat = dynamic_cast<const CounterComponent*>(parts[0]);
	if(!repeat || repeat->minimum != 0 || repeat->maximum != UINT32_MAX || repeat->mode != CounterComponent::Maximal)
		return {};
	auto result = std::make_unique<GreedySuffixScanner>();
	if(!AddByteClass(repeat->content, result->table, 1) || !AddByteClass(parts[1], result->table, 2) ||
	   !AddByteClass(alternatives->componentList[1], result->table, 4))
		return {};
	return result;
}
}

std::unique_ptr<PatternScanOptimizer> PatternScanOptimizer::Build(const IComponent* root)
{
	if(auto result = BuildCharacterRun(root)) return result;
	return BuildGreedySuffix(root);
}

//============================================================================
