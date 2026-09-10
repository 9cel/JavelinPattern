//============================================================================

#include "Javelin/Pattern/Internal/PatternMultiLiteralPrefilter.h"
#include "Javelin/Pattern/Internal/PatternCompiler.h"
#include "Javelin/Pattern/Internal/PatternComponent.h"
#include "Javelin/Pattern/Internal/PatternProcessor.h"
#include "Javelin/Pattern/Pattern.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <array>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define JP_MULTI_X86 1
#endif

//============================================================================

using namespace Javelin;
using namespace Javelin::PatternInternal;

//============================================================================

namespace
{
constexpr size_t UNBOUNDED = UINT32_MAX;
using Triggers = std::vector<MultiLiteralTrigger>;
size_t Add(size_t a, size_t b)
{
	return std::min(UNBOUNDED, a + b);
}

// Allow for NFA epsilon traversal, with headroom to avoid size_t overflow.
size_t WorkLimit(size_t scanned)
{
	return 65536 + 4 * std::min(scanned, (SIZE_MAX - 65536) / 8);
}

void Union(LiteralPrefixRun& a, const LiteralPrefixRun& b)
{
	for(size_t i = 0; i < 4; ++i) a.bytes[i] |= b.bytes[i];
}

LiteralPrefixRun Alphabet(const IComponent* component)
{
	LiteralPrefixRun result;
	if(const auto* b = dynamic_cast<const ByteComponent*>(component))
	{
		result.bytes[b->c / 64] |= uint64_t(1) << (b->c % 64);
	}
	else if(const auto* range = dynamic_cast<const CharacterRangeListComponent*>(component))
	{
		for(const auto& interval : range->characterRangeList)
		{
			if(interval.max > 127 && range->useUtf8)
			{
				result.bytes.fill(UINT64_MAX);
				break;
			}
			for(unsigned c = interval.min; c <= interval.max && c < 256; ++c)
				result.bytes[c / 64] |= uint64_t(1) << (c % 64);
		}
	}
	else if(const auto* body = dynamic_cast<const ContentComponent*>(component))
	{
		result = Alphabet(body->content);
	}
	else if(const auto* group = dynamic_cast<const GroupComponent*>(component))
	{
		for(const auto* child : group->componentList) Union(result, Alphabet(child));
	}
	else if(!component->IsEmpty() && !dynamic_cast<const AssertComponent*>(component))
	{
		result.bytes.fill(UINT64_MAX);
	}
	return result;
}

bool Supported(const IComponent* c)
{
	if(const auto* a = dynamic_cast<const AssertComponent*>(c)) return a->assertType != AssertType::StartOfSearch;
	if(const auto* r = dynamic_cast<const CounterComponent*>(c))
		return r->mode != CounterComponent::Possessive && Supported(r->content);
	if(const auto* capture = dynamic_cast<const CaptureComponent*>(c)) return Supported(capture->content);
	if(dynamic_cast<const ByteComponent*>(c) || dynamic_cast<const CharacterRangeListComponent*>(c) || c->IsEmpty())
		return true;
	if(const auto* g = dynamic_cast<const GroupComponent*>(c))
	{
		for(const auto* child : g->componentList)
			if(!Supported(child)) return false;
		return true;
	}
	return false;
}

// OR 0x20 may add false candidates, including punctuation. The verifier
// rejects them; every possible match must still pass the filter.
bool FoldLiteral(const IComponent* c, std::string& out)
{
	if(out.size() > 128) return false;
	if(const auto* b = dynamic_cast<const ByteComponent*>(c))
	{
		out += char(b->c | 32);
		return true;
	}
	if(const auto* capture = dynamic_cast<const CaptureComponent*>(c)) return FoldLiteral(capture->content, out);
	if(const auto* r = dynamic_cast<const CharacterRangeListComponent*>(c))
	{
		int value = -1;
		for(const auto& interval : r->characterRangeList)
		{
			if(interval.max > 127) return false;
			for(unsigned b = interval.min; b <= interval.max; ++b)
			{
				if(value >= 0 && value != int(b | 32)) return false;
				value = b | 32;
			}
		}
		if(value < 0) return false;
		out += char(value);
		return true;
	}
	if(const auto* g = dynamic_cast<const ConcatenateComponent*>(c))
	{
		for(const auto* child : g->componentList)
			if(!FoldLiteral(child, out)) return false;
		return true;
	}
	if(const auto* r = dynamic_cast<const CounterComponent*>(c))
	{
		if(r->minimum != r->maximum || r->minimum > 128) return false;
		for(unsigned i = 0; i < r->minimum; ++i)
			if(!FoldLiteral(r->content, out)) return false;
		return true;
	}
	return c->IsEmpty() || dynamic_cast<const AssertComponent*>(c);
}

void Flatten(const IComponent* c, std::vector<const IComponent*>& out)
{
	if(const auto* capture = dynamic_cast<const CaptureComponent*>(c))
		Flatten(capture->content, out);
	else if(const auto* group = dynamic_cast<const ConcatenateComponent*>(c))
		for(const auto* child : group->componentList) Flatten(child, out);
	else
		out.push_back(c);
}

struct PrefixPath
{
	std::string literal;
	bool stopped = false;
};

using PrefixPaths = std::vector<PrefixPath>;
void ExtendPrefixes(const IComponent* c, PrefixPaths& paths, unsigned depth = 0)
{
	if(depth > 64)
	{
		for(auto& p : paths) p.stopped = true;
		return;
	}
	if(std::all_of(paths.begin(), paths.end(), [](const auto& p) { return p.stopped || p.literal.size() >= 4; }))
		return;
	if(const auto* capture = dynamic_cast<const CaptureComponent*>(c))
	{
		ExtendPrefixes(capture->content, paths, depth + 1);
		return;
	}
	if(const auto* concat = dynamic_cast<const ConcatenateComponent*>(c))
	{
		for(const auto* child : concat->componentList) ExtendPrefixes(child, paths, depth + 1);
		return;
	}
	if(const auto* repeat = dynamic_cast<const CounterComponent*>(c))
	{
		for(unsigned i = 0; i < std::min(repeat->minimum, 4u); ++i) ExtendPrefixes(repeat->content, paths, depth + 1);
		if(repeat->minimum != repeat->maximum)
			for(auto& p : paths) p.stopped = true;
		return;
	}
	if(dynamic_cast<const ByteComponent*>(c) || dynamic_cast<const CharacterRangeListComponent*>(c))
	{
		auto bytes = Alphabet(c);
		std::vector<unsigned char> values;
		for(unsigned b = 0; b < 256; ++b)
			if(bytes.Contains(b))
			{
				unsigned char folded = b | 32;
				if(std::find(values.begin(), values.end(), folded) == values.end()) values.push_back(folded);
			}
		size_t count = 0;
		for(const auto& p : paths) count += p.stopped || p.literal.size() >= 4 ? 1 : values.size();
		if(values.size() > 8 || count > 256)
		{
			for(auto& p : paths) p.stopped = true;
			return;
		}
		PrefixPaths result;
		for(const auto& p : paths)
		{
			if(p.stopped || p.literal.size() >= 4)
				result.push_back(p);
			else
				for(auto value : values) result.push_back({p.literal + char(value), false});
		}
		paths = std::move(result);
		return;
	}
	if(const auto* alt = dynamic_cast<const AlternationComponent*>(c))
	{
		PrefixPaths result;
		for(const auto* child : alt->componentList)
		{
			auto branch = paths;
			ExtendPrefixes(child, branch, depth + 1);
			if(result.size() + branch.size() > 256)
			{
				for(auto& p : paths) p.stopped = true;
				return;
			}
			result.insert(result.end(), branch.begin(), branch.end());
		}
		paths = std::move(result);
		return;
	}
	if(!c->IsEmpty() && !dynamic_cast<const AssertComponent*>(c))
		for(auto& p : paths) p.stopped = true;
}

Triggers PrefixTriggers(const IComponent* c)
{
	PrefixPaths paths(1);
	ExtendPrefixes(c, paths);
	Triggers result;
	for(auto& p : paths)
	{
		if(p.literal.size() < 2) return {};
		if(std::none_of(result.begin(), result.end(), [&](const auto& t) { return t.literal == p.literal; }))
			result.push_back({std::move(p.literal)});
	}
	return result;
}

double Score(const Triggers& triggers)
{
	if(triggers.empty()) return -1e9;
	double score = 1000;
	for(const auto& t : triggers)
	{
		if(t.literal.empty()) return -1e9;
		if(t.maximumPrefix == UNBOUNDED && std::all_of(t.prefixAlphabet.bytes.begin(), t.prefixAlphabet.bytes.end(),
		                                               [](uint64_t b) { return b == UINT64_MAX; }))
			return -1e9;
		double s = 0;
		for(unsigned char c : t.literal)
		{
			// Prefer long discriminating literals to short common prefixes.
			s += c >= 'a' && c <= 'z' ? 3 : 4;
		}
		s -= std::log2(1.0 + (t.maximumPrefix == UNBOUNDED ? 256 : t.maximumPrefix));
		score = std::min(score, s);
	}
	return score - std::log2(double(triggers.size()));
}

Triggers Select(const IComponent* c, unsigned depth = 0)
{
	if(depth > 64) return {};
	if(const auto* capture = dynamic_cast<const CaptureComponent*>(c)) return Select(capture->content, depth + 1);
	std::string literal;
	if(FoldLiteral(c, literal) && !literal.empty()) return {{std::move(literal)}};
	if(const auto* repeat = dynamic_cast<const CounterComponent*>(c))
		return repeat->minimum ? Select(repeat->content, depth + 1) : Triggers{};
	if(const auto* concat = dynamic_cast<const ConcatenateComponent*>(c))
	{
		std::vector<const IComponent*> parts;
		Flatten(concat, parts);
		Triggers best;
		size_t minimum = 0, maximum = 0;
		LiteralPrefixRun alphabet;
		std::string precedingLiteral;
		for(size_t i = 0; i < parts.size(); ++i)
		{
			Triggers candidate = Select(parts[i], depth + 1);
			std::string run;
			for(size_t j = i; j < parts.size(); ++j)
			{
				std::string part;
				if(!FoldLiteral(parts[j], part) || run.size() + part.size() > 128) break;
				run += part;
			}
			if(run.size() >= 2 && Score({{run}}) > Score(candidate)) candidate = {{std::move(run)}};
			for(auto& t : candidate)
			{
				size_t joined = 0;
				if(!t.maximumPrefix && !precedingLiteral.empty() && precedingLiteral.size() + t.literal.size() <= 128)
				{
					joined = precedingLiteral.size();
					t.literal = precedingLiteral + t.literal;
				}
				t.minimumPrefix = Add(minimum - joined, t.minimumPrefix);
				t.maximumPrefix = maximum == UNBOUNDED ? UNBOUNDED : Add(maximum - joined, t.maximumPrefix);
				Union(t.prefixAlphabet, alphabet);
			}
			if(Score(candidate) > Score(best)) best = std::move(candidate);
			minimum = Add(minimum, parts[i]->GetMinimumLength());
			maximum = Add(maximum, parts[i]->GetMaximumLength());
			Union(alphabet, Alphabet(parts[i]));
			std::string fixed;
			if(FoldLiteral(parts[i], fixed))
			{
				precedingLiteral += fixed;
				if(precedingLiteral.size() > 128) precedingLiteral.erase(0, precedingLiteral.size() - 128);
			}
			else
				precedingLiteral.clear();
		}
		return best;
	}
	if(const auto* alt = dynamic_cast<const AlternationComponent*>(c))
	{
		Triggers result;
		for(const auto* child : alt->componentList)
		{
			auto part = Select(child, depth + 1);
			if(part.empty() || result.size() + part.size() > 256) return {};
			result.insert(result.end(), part.begin(), part.end());
		}
		return result;
	}
	return {};
}
} // namespace

MultiLiteralPrefilter Javelin::PatternInternal::BuildMultiLiteralPrefilter(const IComponent* component,
                                                                           Compiler& compiler, int options,
                                                                           size_t captures)
{
	while(const auto* capture = dynamic_cast<const CaptureComponent*>(component)) component = capture->content;
	const auto* alt = dynamic_cast<const AlternationComponent*>(component);
	if(!alt || dynamic_cast<const CharacterRangeListComponent*>(component) || alt->componentList.GetCount() <= 8 ||
	   alt->componentList.GetCount() > 128 || !Supported(component))
		return {};
	MultiLiteralPrefilter plan;
	for(size_t i = 0; i < alt->componentList.GetCount(); ++i)
	{
		auto triggers = Select(alt->componentList[i]);
		auto prefixes = PrefixTriggers(alt->componentList[i]);
		if(!prefixes.empty() &&
		   (Score(prefixes) > Score(triggers) ||
		    std::any_of(triggers.begin(), triggers.end(), [](const auto& t) { return t.literal.size() < 2; })))
			triggers = std::move(prefixes);
		if(Score(triggers) < -100 || plan.triggers.size() + triggers.size() > 256 ||
		   std::any_of(triggers.begin(), triggers.end(), [](const auto& t) { return t.literal.size() < 2; }))
			return {};
		for(auto& t : triggers)
		{
			t.branch = i;
			plan.triggers.push_back(std::move(t));
		}
	}
	size_t totalInstructions = 0, totalBytes = 0;
	try
	{
		for(auto* branch : alt->componentList)
		{
			InstructionList program;
			const int branchOptions = (options & ~Pattern::PREFER_MASK) | Pattern::PREFER_BACK_TRACKING;
			program.Build(branchOptions, InstructionList::Forwards, branch, compiler, false,
			              PatternProcessorType::BackTracking, false);
			DataBlockWriter writer;
			program.WriteByteCode(writer, String(), captures);
			const auto* header = (const ByteCodeHeader*)writer.GetBuffer().GetData();
			totalInstructions += header->numberOfInstructions;
			totalBytes += writer.GetBuffer().GetCount();
			// Keep all verifier programs together within the instruction/data
			// capacity of one ordinary bytecode program (16- and 24-bit fields).
			if(totalInstructions > UINT16_MAX || totalBytes > (size_t(1) << 24)) return {};
			plan.branches.emplace_back((DataBlock&&)writer.GetBuffer());
		}
	}
	catch(const PatternException&)
	{
		// Side programs are optional. Their encoding/optimization limits must
		// not reject a pattern accepted by the ordinary compiler.
		return {};
	}
	return plan;
}

namespace
{
class MultiLiteralProcessor final : public PatternProcessor
{
	struct Key
	{
		uint32_t value;
		uint16_t trigger;
		uint8_t offset, length;
		uint16_t next;
	};
	std::unique_ptr<PatternProcessor> processor;
	MultiLiteralPrefilter plan;
	std::vector<std::unique_ptr<CandidatePatternProcessor>> branches;
	std::vector<Key> keys;
	std::array<uint16_t, 65536> lookup;
	alignas(16) unsigned char masks[3][8][16]{};
	alignas(64) unsigned char byteMasks[3][4][64]{};
	bool hasVbmi = false;
	LiteralPrefixRun unboundedAlphabet;
	size_t finiteLookbehind = 0;
	bool hasUnbounded = false;
	unsigned vectorBytes = 1;

	static uint32_t ReadKey(const unsigned char* p, unsigned length)
	{
		uint32_t value = (p[0] | 32) | ((p[1] | 32) << 8);
		if(length >= 3) value |= (p[2] | 32) << 16;
		if(length == 4) value |= uint32_t(p[3] | 32) << 24;
		return value;
	}

public:
	MultiLiteralProcessor(PatternProcessor* p, MultiLiteralPrefilter&& input) : processor(p), plan(std::move(input))
	{
		lookup.fill(UINT16_MAX);
		for(auto& program : plan.branches)
			branches.emplace_back(
			    PatternProcessor::CreateAnchoredThompsonNfaProcessor(program.GetData(), program.GetCount()));
		for(size_t i = 0; i < plan.triggers.size(); ++i)
		{
			const auto& t = plan.triggers[i];
			unsigned length = std::min(size_t(4), t.literal.size()), offset = 0;
			// A rare character in the selected gram reduces exact lookups.
			static const unsigned char frequency[26] = {81, 15, 28, 43, 128, 22, 20, 61, 70, 1,  8, 40, 24,
			                                            67, 75, 19, 1,  60,  63, 90, 28, 10, 24, 2, 20, 1};
			unsigned best = UINT32_MAX;
			for(size_t at = 0; at + length <= t.literal.size(); ++at)
			{
				unsigned score = 1;
				for(size_t j = 0; j < length; ++j)
				{
					unsigned char c = t.literal[at + j];
					score *= c >= 'a' && c <= 'z' ? frequency[c - 'a'] : 10;
				}
				if(score < best)
				{
					best = score;
					offset = at;
				}
			}
			uint32_t value = ReadKey((const unsigned char*)t.literal.data() + offset, length);
			keys.push_back({value, uint16_t(i), uint8_t(offset), uint8_t(length), lookup[value & 65535]});
			lookup[value & 65535] = keys.size() - 1;
			if(t.maximumPrefix == UNBOUNDED)
			{
				hasUnbounded = true;
				Union(unboundedAlphabet, t.prefixAlphabet);
				for(unsigned char c : t.literal)
				{
					unboundedAlphabet.bytes[c / 64] |= uint64_t(1) << (c % 64);
					c &= ~32;
					unboundedAlphabet.bytes[c / 64] |= uint64_t(1) << (c % 64);
				}
			}
			else
				finiteLookbehind = std::max(finiteLookbehind, t.maximumPrefix + offset);
		}
		// Group similar grams to reduce false candidates in the shared masks.
		// Short grams have wildcard columns.
		struct Bucket
		{
			std::array<uint64_t, 4> bytes{};
			std::vector<size_t> keys;
		};
		std::vector<Bucket> buckets;
		for(size_t i = 0; i < keys.size(); ++i)
		{
			Bucket bucket;
			bucket.keys.push_back(i);
			for(unsigned j = 0; j < 4; ++j)
			{
				unsigned c = (keys[i].value >> (8 * j)) & 255;
				bucket.bytes[j] = j < keys[i].length ? uint64_t(1) << ((c ^ (c >> 1)) & 63) : UINT64_MAX;
			}
			buckets.push_back(std::move(bucket));
		}
		auto volume = [](const auto& bytes)
		{
			double v = 1;
			for(auto b : bytes) v *= __builtin_popcountll(b);
			return v;
		};
		while(buckets.size() > 24)
		{
			size_t left = 0, right = 1;
			double best = 1e100;
			for(size_t a = 0; a < buckets.size(); ++a)
				for(size_t b = a + 1; b < buckets.size(); ++b)
				{
					std::array<uint64_t, 4> combined;
					for(unsigned j = 0; j < 4; ++j) combined[j] = buckets[a].bytes[j] | buckets[b].bytes[j];
					double increase = volume(combined) - volume(buckets[a].bytes) - volume(buckets[b].bytes);
					if(increase < best)
					{
						best = increase;
						left = a;
						right = b;
					}
				}
			for(unsigned j = 0; j < 4; ++j) buckets[left].bytes[j] |= buckets[right].bytes[j];
			buckets[left].keys.insert(buckets[left].keys.end(), buckets[right].keys.begin(), buckets[right].keys.end());
			buckets.erase(buckets.begin() + right);
		}
		for(size_t b = 0; b < buckets.size(); ++b)
			for(size_t i : buckets[b].keys)
			{
				const auto& key = keys[i];
				for(unsigned j = 0; j < 4; ++j)
					for(unsigned c = 0; c < 256; ++c)
					{
						if(j < key.length && c != ((key.value >> (8 * j)) & 255)) continue;
						masks[b / 8][2 * j][c & 15] |= 1u << (b % 8);
						masks[b / 8][2 * j + 1][c >> 4] |= 1u << (b % 8);
						byteMasks[b / 8][j][(c ^ (c >> 1)) & 63] |= 1u << (b % 8);
					}
			}
#if defined(JP_MULTI_X86)
		hasVbmi = __builtin_cpu_supports("avx512vbmi");
		if(__builtin_cpu_supports("avx512bw"))
			vectorBytes = 64;
		else if(__builtin_cpu_supports("avx2"))
			vectorBytes = 32;
#endif
	}
	const void* FullMatch(const void* d, size_t n) const override { return processor->FullMatch(d, n); }
	const void* FullMatch(const void* d, size_t n, const char** c) const override
	{
		return processor->FullMatch(d, n, c);
	}
	const void* PopulateCaptures(const void* d, size_t n, size_t o, const char** c) const override
	{
		return processor->PopulateCaptures(d, n, o, c);
	}
	const void* PartialMatch(const void* d, size_t n, size_t o) const override { return Search(d, n, o).max; }
	const void* PartialMatch(const void* d, size_t n, size_t o, const char** c) const override
	{
		auto result = Search(d, n, o);
		if(!result.max) return nullptr;
		return processor->PopulateCaptures(d, n, (const char*)result.min - (const char*)d, c);
	}
	Interval<const void*> LocatePartialMatch(const void* d, size_t n, size_t o) const override
	{
		return Search(d, n, o);
	}

private:
#if defined(JP_MULTI_X86)
	__attribute__((target("avx2"))) uint64_t Filter32(const unsigned char* p) const
	{
		__m256i result = _mm256_setzero_si256();
		for(unsigned bank = 0; bank < 3; ++bank)
		{
			__m256i candidates = _mm256_set1_epi8(-1);
			const auto nibble = _mm256_set1_epi8(15), fold = _mm256_set1_epi8(32);
			for(unsigned j = 0; j < 4; ++j)
			{
				auto c = _mm256_or_si256(_mm256_loadu_si256((const __m256i*)(p + j)), fold);
				auto lo = _mm256_broadcastsi128_si256(_mm_load_si128((const __m128i*)masks[bank][2 * j]));
				auto hi = _mm256_broadcastsi128_si256(_mm_load_si128((const __m128i*)masks[bank][2 * j + 1]));
				auto bits =
				    _mm256_and_si256(_mm256_shuffle_epi8(lo, _mm256_and_si256(c, nibble)),
				                     _mm256_shuffle_epi8(hi, _mm256_and_si256(_mm256_srli_epi16(c, 4), nibble)));
				candidates = _mm256_and_si256(candidates, bits);
			}
			result = _mm256_or_si256(result, candidates);
		}
		return uint32_t(~_mm256_movemask_epi8(_mm256_cmpeq_epi8(result, _mm256_setzero_si256())));
	}
	__attribute__((target("avx512f,avx512bw,avx512vbmi"))) uint64_t FilterVbmi(const unsigned char* p) const
	{
		__m512i index[4];
		for(unsigned j = 0; j < 4; ++j)
		{
			auto c = _mm512_or_si512(_mm512_loadu_si512(p + j), _mm512_set1_epi8(32));
			index[j] = _mm512_xor_si512(c, _mm512_srli_epi16(c, 1));
		}
		__m512i result = _mm512_setzero_si512();
		for(unsigned bank = 0; bank < 3; ++bank)
		{
			auto bits = _mm512_permutexvar_epi8(index[0], _mm512_load_si512(byteMasks[bank][0]));
			bits = _mm512_and_si512(bits, _mm512_permutexvar_epi8(index[1], _mm512_load_si512(byteMasks[bank][1])));
			bits = _mm512_and_si512(bits, _mm512_permutexvar_epi8(index[2], _mm512_load_si512(byteMasks[bank][2])));
			bits = _mm512_and_si512(bits, _mm512_permutexvar_epi8(index[3], _mm512_load_si512(byteMasks[bank][3])));
			result = _mm512_or_si512(result, bits);
		}
		return _mm512_cmpneq_epi8_mask(result, _mm512_setzero_si512());
	}
	__attribute__((target("avx512f,avx512bw"))) uint64_t Filter64(const unsigned char* p) const
	{
		__m512i result = _mm512_setzero_si512();
		for(unsigned bank = 0; bank < 3; ++bank)
		{
			__m512i candidates = _mm512_set1_epi8(-1);
			const auto nibble = _mm512_set1_epi8(15), fold = _mm512_set1_epi8(32);
			for(unsigned j = 0; j < 4; ++j)
			{
				auto c = _mm512_or_si512(_mm512_loadu_si512(p + j), fold);
				auto lo = _mm512_broadcast_i32x4(_mm_load_si128((const __m128i*)masks[bank][2 * j]));
				auto hi = _mm512_broadcast_i32x4(_mm_load_si128((const __m128i*)masks[bank][2 * j + 1]));
				auto bits =
				    _mm512_and_si512(_mm512_shuffle_epi8(lo, _mm512_and_si512(c, nibble)),
				                     _mm512_shuffle_epi8(hi, _mm512_and_si512(_mm512_srli_epi16(c, 4), nibble)));
				candidates = _mm512_and_si512(candidates, bits);
			}
			result = _mm512_or_si512(result, candidates);
		}
		return _mm512_cmpneq_epi8_mask(result, _mm512_setzero_si512());
	}
#endif
	uint64_t Filter(const unsigned char* p, size_t remaining) const
	{
#if defined(JP_MULTI_X86)
		if(vectorBytes == 64 && remaining >= 67) return hasVbmi ? FilterVbmi(p) : Filter64(p);
		if(vectorBytes == 32 && remaining >= 35) return Filter32(p);
#endif
		unsigned result = 0;
		for(unsigned bank = 0; bank < 3; ++bank)
		{
			unsigned bits = 255;
			for(unsigned j = 0; j < 4; ++j)
			{
				unsigned c = j < remaining ? p[j] | 32 : 32;
				bits &= masks[bank][2 * j][c & 15] & masks[bank][2 * j + 1][c >> 4];
			}
			result |= bits;
		}
		return result != 0;
	}
	Interval<const void*> Search(const void* d, size_t n, size_t o) const
	{
		if(o > n || n - o < 2) return {nullptr, nullptr};
		const auto* data = (const unsigned char*)d;
		size_t best = n, bestBranch = branches.size(), stop = n - 1;
		const void* end = nullptr;
		size_t work = 0;
		for(size_t at = o; at < stop;)
		{
			size_t width = vectorBytes > 1 && n - at >= vectorBytes + 3 ? vectorBytes : 1;
			uint64_t candidates = Filter(data + at, n - at);
			while(candidates)
			{
				size_t position = at + __builtin_ctzll(candidates);
				candidates &= candidates - 1;
				if(position >= stop) break;
				// Include work rejected inside the SIMD filter in the budget.
				if(++work > WorkLimit(position - o)) return processor->LocatePartialMatch(d, n, o);
				{
					uint32_t pair = ReadKey(data + position, 2);
					for(size_t index = lookup[pair]; index != UINT16_MAX; index = keys[index].next)
					{
						if(++work > WorkLimit(position - o)) return processor->LocatePartialMatch(d, n, o);
						const auto& key = keys[index];
						if(key.length > n - position || key.value != ReadKey(data + position, key.length) ||
						   position < key.offset)
							continue;
						const auto& trigger = plan.triggers[key.trigger];
						size_t literalStart = position - key.offset;
						if(literalStart < o || trigger.literal.size() > n - literalStart) continue;
						bool equal = true;
						for(size_t j = 0; j < trigger.literal.size(); ++j)
							if((data[literalStart + j] | 32) != (unsigned char)trigger.literal[j])
							{
								equal = false;
								break;
							}
						work += trigger.literal.size();
						if(!equal || trigger.minimumPrefix > literalStart - o) continue;
						size_t first = o;
						if(trigger.maximumPrefix != UNBOUNDED)
						{
							if(literalStart - o > trigger.maximumPrefix) first = literalStart - trigger.maximumPrefix;
						}
						else
						{
							first = literalStart;
							while(first > o && trigger.prefixAlphabet.Contains(data[first - 1]))
							{
								--first;
								if(++work > WorkLimit(position - o)) return processor->LocatePartialMatch(d, n, o);
							}
						}
						size_t last = std::min(best, literalStart - trigger.minimumPrefix);
						for(size_t start = first; start <= last; ++start)
						{
							if(start == best && trigger.branch >= bestBranch) break;
							work += 16;
							if(work > WorkLimit(position - o)) return processor->LocatePartialMatch(d, n, o);
							size_t allowance = WorkLimit(position - o) - work;
							size_t remaining = allowance;
							const void* match = branches[trigger.branch]->MatchCandidate(d, n, start, remaining);
							work += allowance - remaining;
							if(match == (const void*)CandidatePatternProcessor::CANDIDATE_BUDGET_EXHAUSTED)
								return processor->LocatePartialMatch(d, n, o);
							if(!match) continue;
							best = start;
							bestBranch = trigger.branch;
							end = match;
							// A later gram may belong to an earlier match. For
							// bounded prefixes, finish the maximum lookbehind
							// window. For unbounded prefixes, also pass a byte
							// that no prefix/trigger path can cross.
							stop = std::min(n - 1, best + std::min(n - best - 1, finiteLookbehind) + 1);
							if(hasUnbounded)
							{
								size_t barrier = best;
								while(barrier < n && unboundedAlphabet.Contains(data[barrier]))
								{
									++barrier;
									if(++work > WorkLimit(position - o)) return processor->LocatePartialMatch(d, n, o);
								}
								stop = std::max(stop, std::min(n - 1, barrier + 1));
							}
							break;
						}
					}
				}
			}
			at += width;
		}
		return end ? Interval<const void*>{data + best, end} : Interval<const void*>{nullptr, nullptr};
	}
};
} // namespace

PatternProcessor* Javelin::PatternInternal::CreateMultiLiteralPrefilterProcessor(PatternProcessor* processor,
                                                                                 MultiLiteralPrefilter&& plan)
{
	if(!plan.HasData()) return processor;
	return new MultiLiteralProcessor(processor, std::move(plan));
}

//============================================================================
