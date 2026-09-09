//============================================================================
//
// The dfa states need to be created/update/destroyed in a thread safe fashion
//
// Options:
//   a. Lock access to the pattern
//   b. Lock access to each dfa state
//   c. Ensure that all threads are in a known/safe state before performing any actions
//
// (a) & (b) are too impactful on performance and not considered.
//
// (c) is achieved with the following:
//   1. When initiating a pattern, increment a counter representing the number of threads processing the pattern
//	 2. When leaving a pattern, decrement this counter
//   3. When populating a state check if memory threshold if reached during state creation
//   4. Resetting of states requiers all threads to reach a safe point
//
//============================================================================

#include "Javelin/Pattern/Internal/DfaProcessorBase.h"

#include "Javelin/Cryptography/Crc64.h"
#include "Javelin/Pattern/Internal/PatternDfaMemoryManager.h"
#include "Javelin/Pattern/Internal/PatternDfaState.h"
#include "Javelin/Pattern/Internal/PatternNfaState.h"
#include <algorithm>
#include <deque>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

//============================================================================

#include "Javelin/Stream/StandardWriter.h"
#define DEBUG_PATTERN			0
#define VERBOSE_DEBUG_PATTERN	0

//============================================================================

using namespace Javelin;
using namespace Javelin::PatternInternal;

//============================================================================

constexpr uint8_t MINIMUM_SELF_ORIGINATED_RESETS	= 2;
constexpr size_t MINIMUM_STATES_TO_FAIL				= 32;
constexpr size_t MAXIMUM_BYTES_PER_STATE_TO_FAIL	= 4096;

//============================================================================

class DfaProcessorBase::NfaStateKey
{
public:
	NfaStateKey(const NfaState* aP) : p(aP) { }

	const NfaState*	p;

	bool operator==(const NfaStateKey& a) const
	{
		if(p->numberOfStates != a.p->numberOfStates) return false;
		return memcmp(p, a.p, NfaState::GetSizeRequiredForNumberOfStates(p->numberOfStates)) == 0;
	}
};

namespace Javelin::PatternInternal
{
	size_t GetHash(const DfaProcessorBase::NfaStateKey& key);
	size_t GetHash(const DfaProcessorBase::NfaStateKey& key)
	{
		return Crc64(key.p, NfaState::GetSizeRequiredForNumberOfStates(key.p->numberOfStates));
	}
}

//============================================================================

DfaProcessorBase::DfaProcessorBase(const void* aPatternData, uint32_t aNumberOfInstructions)
{
	numberOfInstructions    = aNumberOfInstructions;
	patternData.p           = (const unsigned char*) aPatternData;
	CalculateMinimumRemainingLengths();
	DfaMemoryManager::AddProcessor(*this);
}

void DfaProcessorBase::CalculateMinimumRemainingLengths()
{
	// Assertions only reject paths, so shortest paths are safe lower bounds.
	struct Edge { uint32_t source, next; uint8_t cost; };
	const uint32_t infinity = UINT32_MAX;
	std::vector<uint32_t> heads(numberOfInstructions, infinity);
	std::vector<Edge> edges;
	std::deque<uint32_t> pending;
	minimumRemainingLengths.assign(numberOfInstructions, infinity);
	for(uint32_t pc = 0; pc < numberOfInstructions; ++pc)
	{
		const ByteCodeInstruction instruction = patternData[pc];
		auto edge = [&](uint32_t target, uint8_t cost) {
			if(target < numberOfInstructions) {
				edges.push_back({pc, heads[target], cost});
				heads[target] = uint32_t(edges.size() - 1);
			}
		};
		switch(instruction.type)
		{
		case InstructionType::AnyByte:
		case InstructionType::Byte:
		case InstructionType::ByteEitherOf2:
		case InstructionType::ByteEitherOf3:
		case InstructionType::ByteRange:
		case InstructionType::ByteBitMask:
		case InstructionType::ByteNot:
		case InstructionType::ByteNotEitherOf2:
		case InstructionType::ByteNotEitherOf3:
		case InstructionType::ByteNotRange:
			edge(pc + 1, 1); break;
		case InstructionType::Save:
		case InstructionType::SaveNoRecurse:
		case InstructionType::ProgressCheck:
		case InstructionType::AssertStartOfInput:
		case InstructionType::AssertEndOfInput:
		case InstructionType::AssertStartOfLine:
		case InstructionType::AssertEndOfLine:
		case InstructionType::AssertWordBoundary:
		case InstructionType::AssertNotWordBoundary:
		case InstructionType::AssertStartOfSearch:
			edge(pc + 1, 0); break;
		case InstructionType::Jump:
			edge(instruction.data, 0); break;
		case InstructionType::Split:
		{
			const auto* split = patternData.GetData<ByteCodeSplitData>(instruction.data);
			for(uint32_t i = 0; i < split->numberOfTargets; ++i) edge(split->targetList[i], 0);
			break;
		}
		case InstructionType::SplitMatch:
		{
			const auto* targets = patternData.GetData<uint32_t>(instruction.data);
			edge(targets[0], 0); edge(targets[1], 0); break;
		}
		case InstructionType::SplitNextN:
		case InstructionType::SplitNNext:
		case InstructionType::SplitNextMatchN:
		case InstructionType::SplitNMatchNext:
			edge(pc + 1, 0); edge(instruction.data, 0); break;
		case InstructionType::ByteJumpTable:
		case InstructionType::DispatchTable:
		{
			const auto* table = patternData.GetData<ByteCodeJumpTableData>(instruction.data);
			for(uint32_t i = 0; i < table->numberOfTargets; ++i)
				edge(table->pcData[i], instruction.type == InstructionType::ByteJumpTable);
			break;
		}
		case InstructionType::ByteJumpMask:
		case InstructionType::DispatchMask:
		{
			const auto* table = patternData.GetData<ByteCodeJumpMaskData>(instruction.data);
			for(unsigned i = 0; i < 2; ++i) edge(table->pcData[i], instruction.type == InstructionType::ByteJumpMask);
			break;
		}
		case InstructionType::ByteJumpRange:
		case InstructionType::DispatchRange:
		{
			const auto* table = patternData.GetData<ByteCodeJumpRangeData>(instruction.data);
			for(unsigned i = 0; i < 2; ++i) edge(table->pcData[i], instruction.type == InstructionType::ByteJumpRange);
			break;
		}
		case InstructionType::Fail: break;
		default:
			minimumRemainingLengths[pc] = 0;
			pending.push_back(pc);
			break;
		}
	}
	while(!pending.empty())
	{
		uint32_t target = pending.front();
		pending.pop_front();
		for(uint32_t i = heads[target]; i != infinity; i = edges[i].next)
		{
			const Edge& edge = edges[i];
			uint32_t distance = minimumRemainingLengths[target] + edge.cost;
			if(distance < minimumRemainingLengths[edge.source]) {
				minimumRemainingLengths[edge.source] = distance;
				if(edge.cost) pending.push_back(edge.source);
				else pending.push_front(edge.source);
			}
		}
	}
}

DfaProcessorBase::~DfaProcessorBase()
{
	DfaMemoryManager::RemoveProcessor(*this);
//	StandardOutput.PrintF("%z states\n", nfaToDfaMap.GetCount());
	FreeAllStates();
	DfaMemoryManager::OnReleaseAll(stateMemoryAllocated);
}

void DfaProcessorBase::FreeAllStates()
{
	for(const auto& it : nfaToDfaMap)
	{
		delete it.value;
	}
}

//============================================================================

const unsigned char* DfaProcessorBase::NoSearchHandler(const unsigned char* p, const void* data, const unsigned char* pStop)
{
	return p;
}

const unsigned char* DfaProcessorBase::SearchByte0Handler(const unsigned char* p, const void* data, const unsigned char* pStop)
{
	return nullptr;
}

namespace {
template<bool reverse>
const unsigned char* FindOutsideRange(const unsigned char* p, const unsigned char* bound, const unsigned char* range)
{
	const unsigned low = range[0], width = unsigned(range[1]) - low;
	// Most runs are short. Avoid the SIMD setup for them.
	for(unsigned i = 0; i < 2; ++i)
	{
		if(p == bound) return nullptr;
		if constexpr(reverse) {
			if(unsigned(p[-1]) - low > width) return p;
			--p;
		} else {
			if(unsigned(*p) - low > width) return p;
			++p;
		}
	}
#if defined(__aarch64__)
	const uint8x16_t lo = vdupq_n_u8(low), span = vdupq_n_u8(width);
	auto outside = [&](const unsigned char* at) {
		return vcgtq_u8(vsubq_u8(vld1q_u8(at), lo), span);
	};
	auto positions = [](uint8x16_t mask) {
		return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(mask), 4)), 0);
	};
	while((reverse ? p - bound : bound - p) >= 32)
	{
		const unsigned char* block = reverse ? p - 32 : p;
		uint8x16_t a = outside(block), b = outside(block + 16);
		if(vmaxvq_u8(vorrq_u8(a, b)))
		{
			uint64_t bits = positions(reverse ? b : a);
			if(bits) return reverse ? block + 32 - __builtin_clzll(bits) / 4 : block + __builtin_ctzll(bits) / 4;
			bits = positions(reverse ? a : b);
			return reverse ? block + 16 - __builtin_clzll(bits) / 4 : block + 16 + __builtin_ctzll(bits) / 4;
		}
		p += reverse ? -32 : 32;
	}
#elif defined(__SSE2__)
	const __m128i lo = _mm_set1_epi8(low), span = _mm_set1_epi8(width), zero = _mm_setzero_si128();
	while((reverse ? p - bound : bound - p) >= 16)
	{
		const unsigned char* block = reverse ? p - 16 : p;
		__m128i delta = _mm_sub_epi8(_mm_loadu_si128((const __m128i*)block), lo);
		unsigned bits = unsigned(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_subs_epu8(delta, span), zero))) ^ 65535;
		if(bits) return reverse ? block + 32 - __builtin_clz(bits) : block + __builtin_ctz(bits);
		p += reverse ? -16 : 16;
	}
#endif
	while(p != bound)
	{
		if constexpr(reverse) {
			if(unsigned(p[-1]) - low > width) return p;
			--p;
		} else {
			if(unsigned(*p) - low > width) return p;
			++p;
		}
	}
	return nullptr;
}
}

const unsigned char* DfaProcessorBase::FindByteNotRangeForward(const unsigned char* p, const void* data, const unsigned char* end)
{
	return FindOutsideRange<false>(p, end, ((const ByteCodeSearchByteData*)data)->bytes);
}

const unsigned char* DfaProcessorBase::FindByteNotRangeReverse(const unsigned char* p, const void* data, const unsigned char* stop)
{
	return FindOutsideRange<true>(p, stop, ((const ByteCodeSearchByteData*)data)->bytes);
}

//============================================================================

DfaProcessorBase::State* DfaProcessorBase::GetStateForNfaState(NfaState& nfaState) const
{
//	StandardOutput.PrintF("state flags: %x\n", nfaState.stateFlags);
	if(hasFailed)
	{
#if DEBUG_PATTERN
		StandardOutput.PrintF("Returned FailState\n");
#endif
		return (State*) &State::FAIL_STATE;
	}
	if(nfaState.numberOfStates == 0)
	{
		nfaState.stateFlags = (nfaState.stateFlags &
							   ~(NfaState::Flag::ASSERT_MASK
								 | NfaState::Flag::WAS_MASK
								 | NfaState::Flag::PARTIAL_MATCH_IS_ALLOWED))
							  | NfaState::Flag::HAS_EMPTY_STATES;

		if(nfaState.stateFlags == State::EMPTY_STATE)
		{
#if DEBUG_PATTERN
			StandardOutput.PrintF("Returned EmptyState\n");
#endif
			return (State*) &State::EMPTY_STATE;
		}
		if(nfaState.stateFlags == State::EMPTY_MATCH_STATE)
		{
#if DEBUG_PATTERN
			StandardOutput.PrintF("Returned EmptyMatchState\n");
#endif
			return (State*) &State::EMPTY_MATCH_STATE;
		}
	}
	if(!nfaState.IsSearch()) nfaState.stateFlags &= ~NfaState::Flag::IS_SEARCH;

	NfaStateKey key{&nfaState};
	NfaToDfaMap::Iterator it = nfaToDfaMap.Find(key);
	if(it != nfaToDfaMap.End())
	{
		JASSERT(it->value != nullptr);
#if DEBUG_PATTERN
		it->value->Dump("Reusing state");
#endif
		return it->value;
	}
	else
	{
		State* state = new(nfaState.numberOfStates, *(DfaProcessorBase*) this) State;
		memcpy(&state->nfaState, &nfaState, NfaState::GetSizeRequiredForNumberOfStates(nfaState.numberOfStates));

		state->stateFlags = nfaState.stateFlags | NfaState::Flag::DFA_NEEDS_POPULATING;
		state->minimumRemainingLength = UINT32_MAX;
		for(uint32_t i = 0; i < nfaState.numberOfStates; ++i)
			state->minimumRemainingLength = std::min(state->minimumRemainingLength, minimumRemainingLengths[nfaState.stateList[i]]);
		nfaToDfaMap.Insert(NfaStateKey{&state->nfaState}, state);

		state->searchHandler = &NoSearchHandler;
		// Setup processor if it's a search state
		if(state->stateFlags & NfaState::Flag::IS_SEARCH)
		{
			JASSERT(nfaState.numberOfStates > 0);
			ByteCodeInstruction instruction = patternData[nfaState.stateList[nfaState.numberOfStates-1]];
			SearchHandlerEnum handlerValue = SearchHandlerEnum::Normal;
			state->searchData = patternData.GetData<void>(instruction.data);
			switch(instruction.type)
			{
			case InstructionType::FindByte:
				handlerValue = SearchHandlerEnum::SearchByte;
				state->localSearchData.offset = 0;
				state->localSearchData.bytes[0] = instruction.data & 0xff;
				state->searchData = &state->localSearchData;
				break;

			case InstructionType::SearchByte:
				handlerValue = SearchHandlerEnum::SearchByte;
				state->localSearchData.offset = instruction.data >> 8;
				state->localSearchData.bytes[0] = instruction.data & 0xff;
				state->searchData = &state->localSearchData;
				break;

			case InstructionType::SearchByteEitherOf2:		handlerValue = SearchHandlerEnum::SearchByteEitherOf2;		break;
			case InstructionType::SearchByteEitherOf3:		handlerValue = SearchHandlerEnum::SearchByteEitherOf3;		break;
			case InstructionType::SearchByteEitherOf4:		handlerValue = SearchHandlerEnum::SearchByteEitherOf4;		break;
			case InstructionType::SearchByteEitherOf5:		handlerValue = SearchHandlerEnum::SearchByteEitherOf5;		break;
			case InstructionType::SearchByteEitherOf6:		handlerValue = SearchHandlerEnum::SearchByteEitherOf6;		break;
			case InstructionType::SearchByteEitherOf7:		handlerValue = SearchHandlerEnum::SearchByteEitherOf7;		break;
			case InstructionType::SearchByteEitherOf8:		handlerValue = SearchHandlerEnum::SearchByteEitherOf8;		break;
			case InstructionType::SearchBytePair:			handlerValue = SearchHandlerEnum::SearchBytePair;			break;
			case InstructionType::SearchBytePair2:			handlerValue = SearchHandlerEnum::SearchBytePair2;			break;
			case InstructionType::SearchBytePair3:			handlerValue = SearchHandlerEnum::SearchBytePair3;			break;
			case InstructionType::SearchBytePair4:			handlerValue = SearchHandlerEnum::SearchBytePair4;			break;
			case InstructionType::SearchByteTriplet:		handlerValue = SearchHandlerEnum::SearchByteTriplet;		break;
			case InstructionType::SearchByteTriplet2:		handlerValue = SearchHandlerEnum::SearchByteTriplet2;		break;
			case InstructionType::SearchByteRange:			handlerValue = SearchHandlerEnum::SearchByteRange;			break;
			case InstructionType::SearchByteRangePair:		handlerValue = SearchHandlerEnum::SearchByteRangePair;		break;
			case InstructionType::SearchBoyerMoore:			handlerValue = SearchHandlerEnum::SearchBoyerMoore;			break;
			case InstructionType::SearchShiftOr:			handlerValue = SearchHandlerEnum::SearchShiftOr;			break;
				break;

			default:
				JERROR("Unexpected search state");
			}

			if(state->stateFlags & NfaState::Flag::ASSERT_MASK)
			{
				// Extra care required!
				handlerValue = SearchHandlerEnum(uint32_t(handlerValue) + (uint32_t(SearchHandlerEnum::SearchByte0WithAssert) - uint32_t(SearchHandlerEnum::SearchByte0)));
			}

			state->searchHandler = GetSearchHandler(handlerValue, state);
			if(state->searchHandler == &NoSearchHandler)
			{
				state->stateFlags &= ~NfaState::Flag::IS_SEARCH;
			}
		}

#if DEBUG_PATTERN
		state->Dump("Created state");
#endif
		return state;
	}
}

//============================================================================

void DfaProcessorBase::PopulateState(State* state) const
{
	const int MAXIMUM_CHARACTER = 256;
	const uint8_t *const FLAGS = GetCharacterFlags();

	state->activeCounter++;
	BeginPopulate();

	// Check again after lock has been acquired
	if(state->stateFlags & NfaState::Flag::DFA_NEEDS_POPULATING)
	{
		state->TagAsPopulating();

		unsigned char updateCacheBacking[NfaState::UpdateCache::GetSizeRequiredForNumberOfStates(numberOfInstructions)];
		NfaState::UpdateCache* updateCache = (NfaState::UpdateCache*) updateCacheBacking;

#if VERBOSE_DEBUG_PATTERN
		state->Dump("Populating state");
#endif

		int c = 0;
		int numberOfRepeatStates = 0;
		while(c <= MAXIMUM_CHARACTER)
		{
			CharacterRange relevancyInterval(c, MAXIMUM_CHARACTER);

			unsigned char nfaStateBacking[NfaState::GetSizeRequiredForNumberOfStates(numberOfInstructions)];
			NfaState* nfaState = (NfaState*) nfaStateBacking;

			int flags = FLAGS[c] | (state->stateFlags & NfaState::Flag::PARTIAL_MATCH_IS_ALLOWED);
			ProcessNfaState(*nfaState, state->nfaState, patternData, relevancyInterval, flags, updateCache);
			JASSERT(relevancyInterval.Contains(c));
			State* nextState = GetStateForNfaState(*nfaState);

#if VERBOSE_DEBUG_PATTERN
			StandardOutput.PrintF("Populating range: {%C, %C}\n", relevancyInterval.min, relevancyInterval.max);
#endif

			state->nextStates[c++] = nextState;
			while(c <= relevancyInterval.max)
			{
				state->nextStates[c++] = nextState;
			}

			// Count self-loop transitions.
			if(nextState == state)
			{
				if(relevancyInterval.max > 255) relevancyInterval.max = 255;
				if(relevancyInterval.IsValid())
				{
					numberOfRepeatStates += relevancyInterval.GetSize() + 1;
				}
			}
		}

		// Create a shell state for start of search mask if requried.
		if((state->stateFlags & (NfaState::Flag::IS_START_OF_SEARCH_MASK | NfaState::Flag::IS_START_OF_SEARCH)) == NfaState::Flag::IS_START_OF_SEARCH_MASK)
		{
			unsigned char nfaStateBacking[NfaState::GetSizeRequiredForNumberOfStates(state->nfaState.numberOfStates)];
			NfaState* nfaState = (NfaState*) nfaStateBacking;
			memcpy(nfaState, &state->nfaState, NfaState::GetSizeRequiredForNumberOfStates(state->nfaState.numberOfStates));
			nfaState->stateFlags |= NfaState::Flag::IS_START_OF_SEARCH;
			state->nextStates[State::START_OF_SEARCH_INDEX] = GetStateForNfaState(*nfaState);
		}

		if((state->stateFlags & NfaState::Flag::IS_SEARCH) == 0 && numberOfRepeatStates >= 256-32)
		{
			StaticBitTable<256> exitBits;
			int numberOfExitStates = 256-numberOfRepeatStates;

#if DEBUG_PATTERN
			StandardOutput.PrintF("Repeat states: %d, exit states: %d\n", numberOfRepeatStates, numberOfExitStates);
#endif

			for(int i = 0; i < 256; ++i)
			{
				if(state->nextStates[i] != state)
				{
					exitBits.SetBit(i);
				}
			}

			if(exitBits.IsContiguous() && numberOfExitStates < 32 && numberOfExitStates > 1)
			{
				Interval<size_t> range = exitBits.GetContiguousRange();
				state->searchData = &state->localSearchData;
				state->localSearchData.bytes[0] = (uint8_t) range.min;
				state->localSearchData.bytes[1] = (uint8_t) (range.max-1);

				SearchHandlerEnum handlerValue = (state->stateFlags & NfaState::Flag::ASSERT_MASK) ?
													SearchHandlerEnum::SearchByteRangeWithAssert :
													SearchHandlerEnum::SearchByteRange;
				state->searchHandler = GetSearchHandler(handlerValue, state);
				if(state->searchHandler != &NoSearchHandler)
				{
					state->stateFlags |= NfaState::Flag::IS_SEARCH;

#if DEBUG_PATTERN
					StandardOutput.PrintF("Converted to search range {'%C', '%C'}\n", state->localSearchData.bytes[0], state->localSearchData.bytes[1]);
#endif
				}
			}
			else if(numberOfExitStates <= 8)
			{
				int exitBitIndex = 0;
				while(exitBits.HasAnyBitSet())
				{
					size_t lowestBit = exitBits.CountTrailingZeros();
					state->localSearchData.bytes[exitBitIndex++] = lowestBit;
					exitBits.ClearBit(lowestBit);
				}
				state->searchData = &state->localSearchData;
				SearchHandlerEnum handlerValue = (state->stateFlags & NfaState::Flag::ASSERT_MASK) ?
													SearchHandlerEnum(uint8_t(SearchHandlerEnum::SearchByte0WithAssert) + numberOfExitStates) :
													SearchHandlerEnum(uint8_t(SearchHandlerEnum::SearchByte0) + numberOfExitStates);

				state->searchHandler = GetSearchHandler(handlerValue, state);
				if(state->searchHandler != &NoSearchHandler)
				{
					state->stateFlags |= NfaState::Flag::IS_SEARCH;
#if DEBUG_PATTERN
					StandardOutput.PrintF("Converted to search %d: %U\n", numberOfExitStates, state->localSearchData.GetAllBytes());
#endif
				}
			}
		}

		// Small alphabets produce short runs that cost more to skip than to step.
		if((state->stateFlags & NfaState::Flag::IS_SEARCH) == 0
		   && numberOfRepeatStates >= 16 && numberOfRepeatStates < 256)
		{
			StaticBitTable<256> repeatBits;
			for(unsigned i = 0; i < 256; ++i) if(state->nextStates[i] == state) repeatBits.SetBit(i);
			if(repeatBits.IsContiguous())
			{
				Interval<size_t> range = repeatBits.GetContiguousRange();
				state->localSearchData.bytes[0] = range.min;
				state->localSearchData.bytes[1] = range.max - 1;
				state->searchData = &state->localSearchData;
				state->searchHandler = GetSearchHandler(SearchHandlerEnum::SearchByteNotRange, state);
				state->stateFlags |= NfaState::Flag::IS_SEARCH;
			}
		}

#if defined(JBUILDCONFIG_DEBUG)
		for(int i = 0; i <= MAXIMUM_CHARACTER; ++i)
		{
			JASSERT(state->nextStates[i] != nullptr);
		}
#endif

		if(!(state->stateFlags & NfaState::Flag::IS_SEARCH)) state->searchHandler = nullptr;
		state->stateFlags &= ~(NfaState::Flag::DFA_NEEDS_POPULATING
							   | NfaState::Flag::DFA_STATE_IS_POPULATING);
	}
	EndPopulate();
	state->activeCounter--;
}

void DfaProcessorBase::ClearNfaToDfaMap()
{
	for(NfaToDfaMap::Iterator it = nfaToDfaMap.Begin(); it != nfaToDfaMap.End(); ++it)
	{
		State* state = it->value;

		if(state->IsPopulating())
		{
			state->stateFlags &= ~NfaState::Flag::DFA_STATE_IS_RESETTING;
			if(hasFailed) state->stateFlags |= NfaState::Flag::DFA_FAILED;
		}
		else if(state->ShouldRelease())
		{
			JASSERT(state->activeCounter == 0);
			DfaMemoryManager::Release(state, state->GetNumberOfAllocatedBytes(), *this);
			it.Remove();
		}
		else
		{
			state->Reset();
			if(hasFailed) state->stateFlags |= NfaState::Flag::DFA_FAILED;
		}
	}
}

void DfaProcessorBase::ResetStates(bool selfOriginated)
{
	// Wait for all threads
	int32_t localActiveCount;

	resetLock.Synchronize([&] {
		if(resetCounter < 0) { localActiveCount = 0; ++resetCounter; }
		else localActiveCount = (resetCounter += (int32_t) 0x80000000);
	});

	// If we're already resetting, just wait for it
	if(localActiveCount == 0)
	{
		waitForResetEndSemaphore.Wait();
		--resetCounter;
		return;
	}
	JASSERT(localActiveCount < 0);

	// Tag all states so that they will call HandleStateReset();
	populateLock.Synchronize([&] {
		for(const auto& it : nfaToDfaMap)
		{
			it.value->MarkAsResetting();
		}
	});

	// Wait for all threads to reach safe points.
	waitForResetBeginSemaphore.Wait(localActiveCount & 0x7fffffff);

	// Step through all of the states again, and mark any of them that are not active for release
	populateLock.Synchronize([&] {
		State* populatingState = nullptr;
		for(const auto& it : nfaToDfaMap)
		{
			State* state = it.value;
			if(state->IsPopulating())
			{
				JASSERT(populatingState == nullptr);
				populatingState = state;
			}
			else if(state->activeCounter == 0)
			{
				state->TagForRelease();
			}
		}

		// If the states are used by a currently-populating state, then do not let them be released
		if(populatingState)
		{
			for(State* nextState : populatingState->nextStates)
			{
				// The ShouldRelease check is required, in case the next state points to
				// State::FAIL_STATE, EMPTY_STATE or EMPTY_MATCH_STATE
				if(nextState && nextState->ShouldRelease()) nextState->RemoveTagForRelease();
			}
		}

#if DEBUG_PATTERN
		StandardError.PrintF("Reset called with %z states, %U bytes\n", nfaToDfaMap.GetCount(), bytesProcessedSinceReset);
#endif
		if(selfOriginated)
		{
			++numberOfSelfOriginatedResets;
			if(numberOfSelfOriginatedResets >= MINIMUM_SELF_ORIGINATED_RESETS
				&& nfaToDfaMap.GetCount() >= MINIMUM_STATES_TO_FAIL
				&& bytesProcessedSinceReset < nfaToDfaMap.GetCount() * MAXIMUM_BYTES_PER_STATE_TO_FAIL)
			{
				hasFailed = true;
			}
		}
		else
		{
			numberOfSelfOriginatedResets = 0;
		}
		bytesProcessedSinceReset = 0;

		// Do reset!
		ClearStartingStates();
		ClearNfaToDfaMap();
	});

	// Release other threads
	resetLock.Synchronize([&] {
		localActiveCount = (resetCounter -= (int32_t) 0x80000000);
	});
	JASSERT(localActiveCount >= 0);

	// Wait for all other threads to progress before exiting.
	waitForResetEndSemaphore.Signal(localActiveCount);
}

void DfaProcessorBase::HandleStateReset(State* state) const
{
	JASSERT(resetCounter < 0 && (state->stateFlags & NfaState::Flag::DFA_STATE_IS_RESETTING) != 0);
	state->activeCounter++;
	waitForResetBeginSemaphore.Signal();
	waitForResetEndSemaphore.Wait();
	state->activeCounter--;
}

void DfaProcessorBase::BeginMatch() const
{
	int32_t localResetCounter = ++resetCounter;
	if(JUNLIKELY(localResetCounter < 0))
	{
		waitForResetEndSemaphore.Wait();
	}
}

void DfaProcessorBase::EndMatch() const
{
	int32_t localResetCounter = --resetCounter;
	if(JUNLIKELY(localResetCounter) < 0)
	{
		waitForResetBeginSemaphore.Signal();
	}
}

// During populate, this thread should not be part of the ResetStates consideration
void DfaProcessorBase::BeginPopulate() const
{
	EndMatch();
	populateLock.BeginLock();
}

void DfaProcessorBase::EndPopulate() const
{
	int32_t localResetCounter = ++resetCounter;
	populateLock.EndLock();
	if(JUNLIKELY(localResetCounter < 0))
	{
		waitForResetEndSemaphore.Wait();
	}
}

//============================================================================
