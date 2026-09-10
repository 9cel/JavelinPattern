//============================================================================

#include "Javelin/Pattern/Internal/PatternProcessor.h"
#include "Javelin/Container/BitTable.h"
#include "Javelin/Container/EnumSet.h"
#include "Javelin/Container/StackBuffer.h"
#include "Javelin/Pattern/Internal/PatternByteCode.h"
#include "Javelin/Stream/ICharacterWriter.h"
#include "Javelin/Template/Memory.h"

#include "Javelin/Stream/StandardWriter.h"

//============================================================================

using namespace Javelin;
using namespace Javelin::PatternInternal;

//============================================================================

#define VERBOSE_DEBUG_PATTERN	0

//============================================================================

class ThompsonNfaPatternProcessor final : public CandidatePatternProcessor
{
public:
	ThompsonNfaPatternProcessor(const void* data, size_t length, bool anchored = false);

	virtual const void* FullMatch(const void* data, size_t length) const;
	virtual const void* FullMatch(const void* data, size_t length, const char **captures) const;
	virtual const void* PartialMatch(const void* data, size_t length, size_t offset) const;
	virtual const void* MatchCandidate(const void* data, size_t length, size_t offset, size_t& budget) const;
	virtual const void* PartialMatch(const void* data, size_t length, size_t offset, const char **captures) const;
	virtual Interval<const void*> LocatePartialMatch(const void* data, size_t length, size_t offset) const;
	virtual const void* PopulateCaptures(const void* data, size_t length, size_t offset, const char **captures) const;

	virtual bool ProvidesCaptures() const { return false; }

	size_t GetNumberOfBytesForState() const;

private:
	bool anchoredCandidate = false;
	bool				matchRequiresEndOfInput;
	PatternData			patternData;
	uint32_t			numberOfInstructions;
	uint32_t			partialMatchStartingInstruction;
	uint32_t			fullMatchStartingInstruction;
	ExpandedJumpTables	expandedJumpTables;
	Table<StaticBitTable<256>> firstByteMasks;
	void BuildFirstByteMasks();

	void Set(const void* data, size_t length);

	struct State;
	struct ProcessData;

	void ProcessState(const State* currentState, State* nextState, const unsigned char* &p, ProcessData& processData) const;
	const void* Process(const unsigned char* pIn, ProcessData& processData) const;
	const void* MatchPartial(const void* data, size_t length, size_t offset, size_t* budget) const;
};

//============================================================================

struct ThompsonNfaPatternProcessor::ProcessData
{
	const bool					isFullMatch;
	const void*					match = nullptr;
	const unsigned char *const 	pStart;
	const unsigned char *const 	pSearchStart;
	const unsigned char *const 	pEnd;
	uint32_t const				startingInstruction;
	const PatternData			patternData;
	State*						currentState;
	const ExpandedJumpTables&	expandedJumpTables;
	const StaticBitTable<256>*	firstByteMasks;
	size_t* candidateBudget = nullptr;

	ProcessData(bool aIsFullMatch, const void* data, size_t length, size_t offset, uint32_t aStartingInstruction, const ThompsonNfaPatternProcessor& processor)
	: isFullMatch(aIsFullMatch),
	  pStart((const unsigned char*) data),
	  pSearchStart((const unsigned char*) data + offset),
	  pEnd((const unsigned char*) data + length),
	  startingInstruction(aStartingInstruction),
	  patternData(processor.patternData),
	  expandedJumpTables(processor.expandedJumpTables),
	  firstByteMasks(processor.firstByteMasks.GetData())
	{
	}
};

//============================================================================

struct ThompsonNfaPatternProcessor::State
{
	uint32_t		numberOfThreads;
	uint32_t*		threadList;
	uint32_t*		updateCache;
	// A generation per input step replaces the sparse visited-state list.
	uint32_t		generation;			// Zero stops lower priority threads after a match.
	uint32_t		threadStorage[1];

	static size_t GetSize(uint32_t maximumNumberOfThreads) 				{ return (sizeof(State) + maximumNumberOfThreads*sizeof(uint32_t) + 7) & -8; }
	static size_t GetUpdateCacheSize(uint32_t maximumNumberOfThreads) 	{ return (maximumNumberOfThreads*(sizeof(uint32_t)) + 7) & -8; }

	void Dump(ICharacterWriter& output) const;
	bool HasNoThreads() const 							{ return numberOfThreads == 0;			}

	void ResetThreads(uint32_t aGeneration)
	{
		generation = aGeneration;
		numberOfThreads = 0;
	}

	JINLINE bool Check(uint32_t pc)
	{
		if(generation == 0 || updateCache[pc] == generation) return false;
		updateCache[pc] = generation;
		return true;
	}

	void Prepare(uint32_t* aUpdateCache)
	{
		updateCache = aUpdateCache;
		threadList = threadStorage;
		ResetThreads(0);
	}

	void AddThread(uint32_t pc)
	{
#if VERBOSE_DEBUG_PATTERN
		StandardOutput.PrintF("Adding pc: %u\n", pc);
#endif

		threadList[numberOfThreads++] = pc;
	}

	void AddThread(uint32_t pc, const unsigned char* p, ProcessData& processData);
};

void ThompsonNfaPatternProcessor::State::AddThread(uint32_t pc, const unsigned char* p, ProcessData& processData)
{
Loop:
	// Reject a branch before walking its assertions and epsilon transitions.
	// At EOF, leave nullable paths and assertions to the interpreter.
	if(p != processData.pEnd && !processData.firstByteMasks[pc][*p]) return;
#if VERBOSE_DEBUG_PATTERN
	StandardOutput.PrintF("Adding to next thread pc: %u\n", pc);
#endif
	if(!Check(pc)) return;

	const ByteCodeInstruction instruction = processData.patternData[pc];
	switch(instruction.type)
	{
	case InstructionType::AssertStartOfInput:
		if(p == processData.pStart)	goto ProcessNextInstruction;
		break;

	case InstructionType::AssertEndOfInput:
		if(p == processData.pEnd) goto ProcessNextInstruction;
		break;

	case InstructionType::AssertStartOfLine:
		if(p == processData.pStart || p[-1] == '\n') goto ProcessNextInstruction;
		break;

	case InstructionType::AssertEndOfLine:
		if(p == processData.pEnd || *p == '\n') goto ProcessNextInstruction;
		break;

	case InstructionType::AssertWordBoundary:
		if(p != processData.pEnd && WORD_MASK[*p])
		{
			if(p == processData.pStart) goto ProcessNextInstruction;
			if(!WORD_MASK[p[-1]]) goto ProcessNextInstruction;
		}
		else
		{
			if(p != processData.pStart
			   && WORD_MASK[p[-1]]) goto ProcessNextInstruction;
		}
		break;

	case InstructionType::AssertNotWordBoundary:
		if(p != processData.pEnd && WORD_MASK[*p])
		{
			if(p != processData.pStart
			   && WORD_MASK[p[-1]]) goto ProcessNextInstruction;
		}
		else
		{
			if(p == processData.pStart) goto ProcessNextInstruction;
			if(!WORD_MASK[p[-1]]) goto ProcessNextInstruction;
		}
		break;

	case InstructionType::AssertStartOfSearch:
		if(p != processData.pSearchStart) break;
		goto ProcessNextInstruction;

	case InstructionType::DispatchTable:
		if(p == processData.pEnd)
		{
			const ByteCodeJumpTableData* data = processData.patternData.GetData<ByteCodeJumpTableData>(instruction.data);
			pc = data->pcData[0];
		}
		else
		{
			const uint32_t* data = processData.expandedJumpTables.GetJumpTable(pc);
			pc = data[*p];
		}
		if(pc != TypeData<uint32_t>::Maximum()) goto Loop;
		break;

	case InstructionType::DispatchMask:
		if(p == processData.pEnd)
		{
			const ByteCodeJumpMaskData* data = processData.patternData.GetData<ByteCodeJumpMaskData>(instruction.data);
			pc = data->pcData[0];
		}
		else
		{
			const uint32_t* data = processData.expandedJumpTables.GetJumpTable(pc);
			pc = data[*p];
		}
		if(pc != TypeData<uint32_t>::Maximum()) goto Loop;
		break;

	case InstructionType::DispatchRange:
		{
			const ByteCodeJumpRangeData* data = processData.patternData.GetData<ByteCodeJumpRangeData>(instruction.data);
			if(p == processData.pEnd)
			{
				pc = data->pcData[0];
			}
			else
			{
				pc = data->pcData[data->range.Contains(*p)];
			}
			if(pc != TypeData<uint32_t>::Maximum()) goto Loop;
			break;
		}

	case InstructionType::Fail:
		break;

	case InstructionType::Jump:
		pc = instruction.data;
		goto Loop;

	case InstructionType::Match:
		if(processData.isFullMatch && p != processData.pEnd) break;
		processData.match = p;
		// Prevent processing of lower priority threads
		generation = 0;
		processData.currentState->numberOfThreads = 0;
		return;

	case InstructionType::SearchByte:
		{
			unsigned char c = instruction.data & 0xff;
			unsigned offset = instruction.data >> 8;
			if(p+offset >= processData.pEnd) return;

			if(p[offset] == c) AddThread(pc+1, p, processData);
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteEitherOf2:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset >= processData.pEnd) return;

			if(p[data->offset] == data->bytes[0]
			   || p[data->offset] == data->bytes[1])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteEitherOf3:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset >= processData.pEnd) return;

			if(p[data->offset] == data->bytes[0]
			   || p[data->offset] == data->bytes[1]
			   || p[data->offset] == data->bytes[2])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteEitherOf4:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset >= processData.pEnd) return;

			if(p[data->offset] == data->bytes[0]
			   || p[data->offset] == data->bytes[1]
			   || p[data->offset] == data->bytes[2]
			   || p[data->offset] == data->bytes[3])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteEitherOf5:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset >= processData.pEnd) return;

			if(p[data->offset] == data->bytes[0]
			   || p[data->offset] == data->bytes[1]
			   || p[data->offset] == data->bytes[2]
			   || p[data->offset] == data->bytes[3]
			   || p[data->offset] == data->bytes[4])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteEitherOf6:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset >= processData.pEnd) return;

			if(p[data->offset] == data->bytes[0]
			   || p[data->offset] == data->bytes[1]
			   || p[data->offset] == data->bytes[2]
			   || p[data->offset] == data->bytes[3]
			   || p[data->offset] == data->bytes[4]
			   || p[data->offset] == data->bytes[5])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteEitherOf7:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset >= processData.pEnd) return;

			if(p[data->offset] == data->bytes[0]
			   || p[data->offset] == data->bytes[1]
			   || p[data->offset] == data->bytes[2]
			   || p[data->offset] == data->bytes[3]
			   || p[data->offset] == data->bytes[4]
			   || p[data->offset] == data->bytes[5]
			   || p[data->offset] == data->bytes[6])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteEitherOf8:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset >= processData.pEnd) return;

			if(p[data->offset] == data->bytes[0]
			   || p[data->offset] == data->bytes[1]
			   || p[data->offset] == data->bytes[2]
			   || p[data->offset] == data->bytes[3]
			   || p[data->offset] == data->bytes[4]
			   || p[data->offset] == data->bytes[5]
			   || p[data->offset] == data->bytes[6]
			   || p[data->offset] == data->bytes[7])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchBytePair:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset+1 >= processData.pEnd) return;

			if(p[data->offset] == data->bytes[0]
			   && p[data->offset+1] == data->bytes[1])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchBytePair2:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset+1 >= processData.pEnd) return;

			if((p[data->offset] == data->bytes[0] || p[data->offset] == data->bytes[1])
			   && (p[data->offset+1] == data->bytes[2] || p[data->offset+1] == data->bytes[3]))
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchBytePair3:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset+1 >= processData.pEnd) return;

			if((p[data->offset] == data->bytes[0] || p[data->offset] == data->bytes[1] || p[data->offset] == data->bytes[2])
			   && (p[data->offset+1] == data->bytes[3] || p[data->offset+1] == data->bytes[4] || p[data->offset+1] == data->bytes[5]))
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchBytePair4:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset+1 >= processData.pEnd) return;

			if((p[data->offset] == data->bytes[0] || p[data->offset] == data->bytes[1] || p[data->offset] == data->bytes[2] || p[data->offset] == data->bytes[3])
			   && (p[data->offset+1] == data->bytes[4] || p[data->offset+1] == data->bytes[5] || p[data->offset+1] == data->bytes[6] || p[data->offset+1] == data->bytes[7]))
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteRange:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);

			if(p[data->offset] >= data->bytes[0] && p[data->offset] <= data->bytes[1])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteRangePair:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset+1 >= processData.pEnd) return;

			if(p[data->offset] >= data->bytes[0]
				&& p[data->offset] <= data->bytes[1]
			    && p[data->offset+1] >= data->bytes[2]
			    && p[data->offset+1] <= data->bytes[3])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteTriplet:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset+2 >= processData.pEnd) return;

			if(p[data->offset] == data->bytes[0]
			   && p[data->offset+1] == data->bytes[1]
				&& p[data->offset+2] == data->bytes[2])
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchByteTriplet2:
		{
			const ByteCodeSearchByteData* data = processData.patternData.GetData<ByteCodeSearchByteData>(instruction.data);
			if(p+data->offset+2 >= processData.pEnd) return;

			if((p[data->offset] == data->bytes[0] || p[data->offset] == data->bytes[1])
				&& (p[data->offset+1] == data->bytes[2] || p[data->offset+1] == data->bytes[3])
				&& (p[data->offset+2] == data->bytes[4] || p[data->offset+2] == data->bytes[5]))
			{
				AddThread(pc+1, p, processData);
			}
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchBoyerMoore:
		{
			const ByteCodeSearchData* searchData = processData.patternData.GetData<ByteCodeSearchData>(instruction.data);
			if(p+searchData->length >= processData.pEnd) return;

			if(searchData->data[p[searchData->length]] == 0) AddThread(pc+1, p, processData);
			else AddThread(pc);
		}
		break;

	case InstructionType::SearchShiftOr:
		{
			const ByteCodeSearchData* searchData = processData.patternData.GetData<ByteCodeSearchData>(instruction.data);
			if(p >= processData.pEnd) return;

			if((searchData->data[*p] & 1) == 0) AddThread(pc+1, p, processData);
			else AddThread(pc);
		}
		break;

	case InstructionType::Split:
		{
			const ByteCodeSplitData* splitData = processData.patternData.GetData<ByteCodeSplitData>(instruction.data);

			uint32_t numberOfTargets = splitData->numberOfTargets;
			for(uint32_t i = 0; i < numberOfTargets-1; ++i)
			{
				AddThread(splitData->targetList[i], p, processData);
			}
			pc = splitData->targetList[numberOfTargets-1];
			goto Loop;
		}

	case InstructionType::SplitMatch:
		{
			const uint32_t* splitData = processData.patternData.GetData<uint32_t>(instruction.data);
			pc = (!processData.isFullMatch || p == processData.pEnd) ? splitData[0] : splitData[1];
			goto Loop;
		}

	case InstructionType::SplitNextN:
		AddThread(pc+1, p, processData);
		pc = instruction.data;
		goto Loop;

	case InstructionType::SplitNNext:
		AddThread(instruction.data, p, processData);
		goto ProcessNextInstruction;

	case InstructionType::SplitNextMatchN:
		pc = (!processData.isFullMatch || p == processData.pEnd) ? pc+1 : instruction.data;
		goto Loop;

	case InstructionType::SplitNMatchNext:
		pc = (!processData.isFullMatch || p == processData.pEnd) ? instruction.data : pc+1;
		goto Loop;

	default:
		AddThread(pc);
		break;

	case InstructionType::ProgressCheck:
	case InstructionType::PropagateBackwards:
	case InstructionType::Save:
	case InstructionType::SaveNoRecurse:
	ProcessNextInstruction:
		++pc;
		goto Loop;
	}
}

void ThompsonNfaPatternProcessor::State::Dump(ICharacterWriter& output) const
{
	output.PrintF("ThreadList:");
	for(size_t i = 0; i < numberOfThreads; ++i)
	{
		output.PrintF(" %u", threadList[i]);
	}
	output.PrintF("\n");
}

//============================================================================

ThompsonNfaPatternProcessor::ThompsonNfaPatternProcessor(const void* data, size_t length, bool anchored)
{
	Set(data, length);
	anchoredCandidate = anchored;
	if(anchored) partialMatchStartingInstruction = fullMatchStartingInstruction;
}

void ThompsonNfaPatternProcessor::Set(const void* data, size_t length)
{
	ByteCodeHeader* header = (ByteCodeHeader*) data;
	matchRequiresEndOfInput = header->flags.matchRequiresEndOfInput;
	numberOfInstructions = header->numberOfInstructions;
	partialMatchStartingInstruction = header->partialMatchStartingInstruction;
	fullMatchStartingInstruction = header->fullMatchStartingInstruction;
	patternData.p = (const unsigned char*) header->GetForwardProgram();
	expandedJumpTables.Set(patternData, numberOfInstructions);
	BuildFirstByteMasks();
}

//============================================================================

// Follow short non-consuming paths to reject impossible first bytes. Leave
// assertions in the program and stop at splits, matches and search instructions.
void ThompsonNfaPatternProcessor::BuildFirstByteMasks()
{
	firstByteMasks.SetCount(numberOfInstructions);
	for(uint32_t start = 0; start < numberOfInstructions; ++start)
	{
		auto& mask = firstByteMasks[start];
		mask.SetAllBits();
		uint32_t pc = start;
		for(unsigned steps = 0; steps < 8; ++steps)
		{
			const auto instruction = patternData[pc];
			switch(instruction.type)
			{
			case InstructionType::Jump: pc = instruction.data; continue;
			case InstructionType::Save:
			case InstructionType::SaveNoRecurse:
			case InstructionType::ProgressCheck:
			case InstructionType::PropagateBackwards:
			case InstructionType::AssertStartOfInput:
			case InstructionType::AssertEndOfInput:
			case InstructionType::AssertStartOfLine:
			case InstructionType::AssertEndOfLine:
			case InstructionType::AssertWordBoundary:
			case InstructionType::AssertNotWordBoundary:
			case InstructionType::AssertStartOfSearch:
				++pc; continue;
			default: break;
			}
			for(unsigned c = 0; c < 256; ++c)
			{
				bool accepts = true;
				const uint32_t value = instruction.data;
				switch(instruction.type)
				{
				case InstructionType::Byte: accepts = c == value; break;
				case InstructionType::ByteEitherOf2: accepts = c == (value & 255) || c == (value >> 8 & 255); break;
				case InstructionType::ByteEitherOf3: accepts = c == (value & 255) || c == (value >> 8 & 255) || c == (value >> 16 & 255); break;
				case InstructionType::ByteRange: accepts = c >= (value & 255) && c <= (value >> 8 & 255); break;
				case InstructionType::ByteNot: accepts = c != value; break;
				case InstructionType::ByteNotEitherOf2: accepts = c != (value & 255) && c != (value >> 8 & 255); break;
				case InstructionType::ByteNotEitherOf3: accepts = c != (value & 255) && c != (value >> 8 & 255) && c != (value >> 16 & 255); break;
				case InstructionType::ByteNotRange: accepts = c < (value & 255) || c > (value >> 8 & 255); break;
				case InstructionType::ByteBitMask: accepts = (*patternData.GetData<StaticBitTable<256>>(value))[c]; break;
				case InstructionType::ByteJumpTable:
				case InstructionType::ByteJumpMask:
				case InstructionType::DispatchTable:
				case InstructionType::DispatchMask:
					accepts = expandedJumpTables.GetJumpTable(pc)[c] != TypeData<uint32_t>::Maximum(); break;
				case InstructionType::ByteJumpRange:
				case InstructionType::DispatchRange:
					{ auto data = patternData.GetData<ByteCodeJumpRangeData>(value); accepts = data->pcData[data->range.Contains(c)] != TypeData<uint32_t>::Maximum(); break; }
				case InstructionType::Fail: accepts = false; break;
				default: break;
				}
				if(!accepts) mask.ClearBit(c);
			}
			break;
		}
	}
}

//============================================================================

void ThompsonNfaPatternProcessor::ProcessState(const State* currentState, State* nextState, const unsigned char* &p, ProcessData& processData) const
{
#if VERBOSE_DEBUG_PATTERN
	StandardOutput.PrintF("\nProcessing byte: '%c'\n", *p);
	currentState->Dump(StandardOutput);
#endif

	for(uint32_t pcIndex = 0; pcIndex < currentState->numberOfThreads; ++pcIndex)
	{
		uint32_t pc = currentState->threadList[pcIndex];

#if VERBOSE_DEBUG_PATTERN
		StandardOutput.PrintF("Processing pc: %u\n", pc);
#endif
	Loop:
		const ByteCodeInstruction instruction = patternData[pc];

		switch(instruction.type)
		{
		case InstructionType::AdvanceByte:
		case InstructionType::AnyByte:
			nextState->AddThread(pc+1, p+1, processData);
			break;

		case InstructionType::Byte:
			if(instruction.data == *p)
			{
				nextState->AddThread(pc+1, p+1, processData);
			}
			break;

		case InstructionType::ByteEitherOf2:
			if((instruction.data & 0xff) == *p ||
			   ((instruction.data >> 8) & 0xff) == *p)
			{
				nextState->AddThread(pc+1, p+1, processData);
			}
			break;

		case InstructionType::ByteEitherOf3:
			if((instruction.data & 0xff) == *p ||
			   ((instruction.data >> 8) & 0xff) == *p ||
			   ((instruction.data >> 16) & 0xff) == *p)
			{
				nextState->AddThread(pc+1, p+1, processData);
			}
			break;

		case InstructionType::ByteRange:
			{
				unsigned char low = instruction.data & 0xff;
				unsigned char high = (instruction.data >> 8) & 0xff;
				uint32_t delta = high - low;
				if(uint32_t(*p - low) <= delta)
				{
					nextState->AddThread(pc+1, p+1, processData);
				}
			}
			break;

		case InstructionType::ByteBitMask:
			{
				const StaticBitTable<256>& data = *patternData.GetData<StaticBitTable<256>>(instruction.data);
				if(data[*p])
				{
					nextState->AddThread(pc+1, p+1, processData);
				}
			}
			break;

		case InstructionType::ByteJumpTable:
		case InstructionType::ByteJumpMask:
			{
				const uint32_t* data = expandedJumpTables.GetJumpTable(pc);
				uint32_t nextPc = data[*p];
				if(nextPc != TypeData<uint32_t>::Maximum())
				{
					nextState->AddThread(nextPc, p+1, processData);
				}
			}
			break;

		case InstructionType::ByteJumpRange:
			{
				const ByteCodeJumpRangeData* data = patternData.GetData<ByteCodeJumpRangeData>(instruction.data);
				uint32_t nextPc = data->pcData[data->range.Contains(*p)];
				if(nextPc != TypeData<uint32_t>::Maximum())
				{
					nextState->AddThread(nextPc, p+1, processData);
				}
			}
			break;

		case InstructionType::ByteNot:
			if(instruction.data != *p)
			{
				nextState->AddThread(pc+1, p+1, processData);
			}
			break;

		case InstructionType::ByteNotEitherOf2:
			if((instruction.data & 0xff) != *p &&
			   ((instruction.data >> 8) & 0xff) != *p)
			{
				nextState->AddThread(pc+1, p+1, processData);
			}
			break;

		case InstructionType::ByteNotEitherOf3:
			if((instruction.data & 0xff) != *p &&
			   ((instruction.data >> 8) & 0xff) != *p &&
			   ((instruction.data >> 16) & 0xff) != *p)
			{
				nextState->AddThread(pc+1, p+1, processData);
			}
			break;

		case InstructionType::ByteNotRange:
			{
				unsigned char low = instruction.data & 0xff;
				unsigned char high = (instruction.data >> 8) & 0xff;
				if(*p < low || *p > high)
				{
					nextState->AddThread(pc+1, p+1, processData);
				}
			}
			break;

		case InstructionType::FindByte:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				unsigned char c = instruction.data & 0xff;
				p = (const unsigned char*) FindByte(p, c, processData.pEnd);
				if(!p) return;
				nextState->AddThread(instruction.data>>8, p+1, processData);
			}
			else
			{
				unsigned char c = instruction.data & 0xff;
				nextState->AddThread((*p == c) ? instruction.data>>8 : pc, p+1, processData);
			}
			break;

		case InstructionType::SearchByte:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				unsigned char c = instruction.data & 0xff;
				unsigned offset = instruction.data >> 8;

				const unsigned char* pSearch = p+offset;
				if(pSearch >= processData.pEnd) return;

				size_t remaining = processData.pEnd - pSearch;
				pSearch = (const unsigned char*) memchr(pSearch, c, remaining);
				if(!pSearch) return;
				p = pSearch-offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteEitherOf2:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteEitherOf2(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteEitherOf3:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteEitherOf3(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteEitherOf4:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteEitherOf4(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteEitherOf5:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteEitherOf5(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteEitherOf6:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteEitherOf6(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteEitherOf7:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteEitherOf7(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteEitherOf8:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteEitherOf8(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchBytePair:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindBytePair(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchBytePair2:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindBytePair2(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchBytePair3:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindBytePair3(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchBytePair4:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindBytePair4(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteRange:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteRange(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteRangePair:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteRangePair(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteTriplet:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteTriplet(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchByteTriplet2:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchByteData* data = patternData.GetData<ByteCodeSearchByteData>(instruction.data);

				const unsigned char* pSearch = p+data->offset;
				if(pSearch >= processData.pEnd) return;

				pSearch = (const unsigned char*) FindByteTriplet2(pSearch, data->GetAllBytes(), processData.pEnd);
				if(!pSearch) return;
				p = pSearch-data->offset-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchBoyerMoore:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchData* searchData = patternData.GetData<ByteCodeSearchData>(instruction.data);
				const unsigned char* pSearch = (const unsigned char*) FindBoyerMoore(p, searchData, processData.pEnd);
				if(!pSearch) return;
				if(pSearch > p) p = pSearch-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::SearchShiftOr:
			if(nextState->HasNoThreads()
			   && currentState->numberOfThreads == 1)
			{
				const ByteCodeSearchData* searchData = patternData.GetData<ByteCodeSearchData>(instruction.data);
				const unsigned char* pSearch = (const unsigned char*) FindShiftOr(p, searchData, processData.pEnd);
				if(!pSearch) return;
				if(pSearch > p) p = pSearch-1;
			}
			nextState->AddThread(pc, p+1, processData);
			break;

		case InstructionType::AssertEndOfInput:
		case InstructionType::AssertEndOfLine:
		case InstructionType::AssertWordBoundary:
		case InstructionType::AssertNotWordBoundary:
		case InstructionType::AssertRecurseValue:
		case InstructionType::AssertStartOfInput:
		case InstructionType::AssertStartOfLine:
		case InstructionType::AssertStartOfSearch:
		case InstructionType::BackReference:
		case InstructionType::Call:
		case InstructionType::DispatchMask:
		case InstructionType::DispatchRange:
		case InstructionType::DispatchTable:
		case InstructionType::Fail:
		case InstructionType::Jump:
		case InstructionType::Match:
		case InstructionType::Save:
		case InstructionType::SaveNoRecurse:
		case InstructionType::Split:
		case InstructionType::SplitMatch:
		case InstructionType::SplitNNext:
		case InstructionType::SplitNMatchNext:
		case InstructionType::SplitNextN:
		case InstructionType::SplitNextMatchN:
		case InstructionType::StepBack:
		case InstructionType::Possess:
		case InstructionType::ProgressCheck:
		case InstructionType::PropagateBackwards:
		case InstructionType::Recurse:
		case InstructionType::ReturnIfRecurseValue:
		case InstructionType::Success:
			JERROR("Unexpected instruction");
		}
	}
}

const void* ThompsonNfaPatternProcessor::Process(const unsigned char* pIn, ProcessData& processData) const
{
	if (processData.candidateBudget)
	{
		if (*processData.candidateBudget < numberOfInstructions) return (const void*) CANDIDATE_BUDGET_EXHAUSTED;
		*processData.candidateBudget -= numberOfInstructions;
	}
	uint32_t processSize = State::GetSize(numberOfInstructions);
	uint32_t updateCacheSize = State::GetUpdateCacheSize(numberOfInstructions);
	return StackBuffer(2*processSize+updateCacheSize, [=, &processData](unsigned char* pBuffer) -> const void*
	{
		State* currentState = (State*) pBuffer;
		State* nextState = (State*) (pBuffer + processSize);
		uint32_t* updateCache = (uint32_t*) (pBuffer + 2*processSize);

		currentState->Prepare(updateCache);
		nextState->Prepare(updateCache);

		const unsigned char* p = pIn;
		const unsigned char* pEnd = processData.pEnd;

		memset(updateCache, 0, updateCacheSize);
		uint32_t generation = 1;
		nextState->ResetThreads(generation);
		processData.currentState = currentState;
		nextState->AddThread(processData.startingInstruction, pIn, processData);

		for(; p < pEnd; ++p)
		{
			if(nextState->HasNoThreads()) return processData.match;
			if (processData.candidateBudget)
			{
				// Generation deduplication visits at most the program once per
				// input position. Charge that upper bound, including epsilon work.
				if (*processData.candidateBudget < numberOfInstructions) return (const void*) CANDIDATE_BUDGET_EXHAUSTED;
				*processData.candidateBudget -= numberOfInstructions;
			}
			Swap(currentState, nextState);
			// Reuse generation values safely even on inputs exceeding 4 GiB.
			if(++generation == 0)
			{
				memset(updateCache, 0, updateCacheSize);
				generation = 1;
			}
			nextState->ResetThreads(generation);

			processData.currentState = currentState;
			const unsigned char* before = p;
			ProcessState(currentState, nextState, p, processData);
			if (processData.candidateBudget)
			{
				// Search instructions may skip input within one NFA step.
				size_t skipped = p == nullptr ? size_t(pEnd - before) : p > before ? size_t(p - before) : 0;
				if (*processData.candidateBudget < skipped) return (const void*) CANDIDATE_BUDGET_EXHAUSTED;
				*processData.candidateBudget -= skipped;
			}
			if (!p) return processData.match;
		}

		return processData.match;
	});
}

const void* ThompsonNfaPatternProcessor::FullMatch(const void* data, size_t length) const
{
	ProcessData processData(true, data, length, 0, fullMatchStartingInstruction, *this);
	return Process(processData.pStart, processData);
}

const void* ThompsonNfaPatternProcessor::FullMatch(const void* data, size_t length, const char **captures) const
{
	JERROR("Should never be called");
	return nullptr;
}

const void* ThompsonNfaPatternProcessor::PartialMatch(const void* data, size_t length, size_t offset) const
{
	return MatchPartial(data, length, offset, nullptr);
}

const void* ThompsonNfaPatternProcessor::MatchCandidate(const void* data, size_t length, size_t offset, size_t& budget) const
{
	return MatchPartial(data, length, offset, &budget);
}

const void* ThompsonNfaPatternProcessor::MatchPartial(const void* data, size_t length, size_t offset, size_t* budget) const
{
	ProcessData processData(matchRequiresEndOfInput, data, length, offset, partialMatchStartingInstruction, *this);
	processData.candidateBudget = budget;
	if (anchoredCandidate)
	{
		// Most literal candidates fail along a deterministic prefix. Avoid
		// allocating and clearing NFA state until the first actual fork.
		const auto* p = processData.pSearchStart;
		uint32_t pc = fullMatchStartingInstruction;
		for (unsigned steps = 0; steps < 64; ++steps)
		{
			if (budget)
			{
				if (!*budget) return (const void*) CANDIDATE_BUDGET_EXHAUSTED;
				--*budget;
			}
			if (pc == UINT32_MAX) return nullptr;
			if (p != processData.pEnd && !firstByteMasks[pc][*p]) return nullptr;
			const auto instruction = patternData[pc];
			switch (instruction.type)
			{
			case InstructionType::Fail: return nullptr;
			case InstructionType::Match:
				return !matchRequiresEndOfInput || p == processData.pEnd ? p : nullptr;
			case InstructionType::Jump: pc = instruction.data; continue;
			case InstructionType::Save:
			case InstructionType::SaveNoRecurse:
			case InstructionType::ProgressCheck: break;
			case InstructionType::AssertStartOfInput: if (p != processData.pStart) return nullptr; break;
			case InstructionType::AssertEndOfInput: if (p != processData.pEnd) return nullptr; break;
			case InstructionType::AssertStartOfLine: if (p != processData.pStart && p[-1] != '\n') return nullptr; break;
			case InstructionType::AssertEndOfLine: if (p != processData.pEnd && *p != '\n') return nullptr; break;
			case InstructionType::AssertWordBoundary:
			case InstructionType::AssertNotWordBoundary:
				if (((p != processData.pStart && WORD_MASK[p[-1]]) != (p != processData.pEnd && WORD_MASK[*p]))
					!= (instruction.type == InstructionType::AssertWordBoundary)) return nullptr;
				break;
			case InstructionType::AnyByte:
			case InstructionType::AdvanceByte:
			case InstructionType::Byte:
			case InstructionType::ByteEitherOf2:
			case InstructionType::ByteEitherOf3:
			case InstructionType::ByteRange:
			case InstructionType::ByteNot:
			case InstructionType::ByteNotEitherOf2:
			case InstructionType::ByteNotEitherOf3:
			case InstructionType::ByteNotRange:
			case InstructionType::ByteBitMask:
				if (p == processData.pEnd) return nullptr;
				++p; break;
			case InstructionType::ByteJumpTable:
			case InstructionType::ByteJumpMask:
				if (p == processData.pEnd) return nullptr;
				pc = expandedJumpTables.GetJumpTable(pc)[*p++]; continue;
			case InstructionType::DispatchTable:
			case InstructionType::DispatchMask:
				if (p == processData.pEnd) goto UseNfa;
				pc = expandedJumpTables.GetJumpTable(pc)[*p]; continue;
			case InstructionType::ByteJumpRange:
			case InstructionType::DispatchRange:
				if (p == processData.pEnd) goto UseNfa;
				pc = patternData.GetData<ByteCodeJumpRangeData>(instruction.data)->pcData[
					patternData.GetData<ByteCodeJumpRangeData>(instruction.data)->range.Contains(*p)];
				if (instruction.type == InstructionType::ByteJumpRange) ++p;
				continue;
			default: goto UseNfa;
			}
			++pc;
		}
	}
UseNfa:
	return Process(processData.pSearchStart, processData);
}

const void* ThompsonNfaPatternProcessor::PartialMatch(const void* data, size_t length, size_t offset, const char **captures) const
{
	JERROR("Should never be called");
	return nullptr;
}

Interval<const void*> ThompsonNfaPatternProcessor::LocatePartialMatch(const void* data, size_t length, size_t offset) const
{
	JERROR("Should never be called");
	return {nullptr, nullptr};
}

const void* ThompsonNfaPatternProcessor::PopulateCaptures(const void* data, size_t length, size_t offset, const char **captures) const
{
	JERROR("Should never be called");
	return nullptr;
}

//============================================================================

PatternProcessor* PatternProcessor::CreateThompsonNfaProcessor(const void* data, size_t length)
{
	return new ThompsonNfaPatternProcessor(data, length);
}

CandidatePatternProcessor* PatternProcessor::CreateAnchoredThompsonNfaProcessor(const void* data, size_t length)
{
	return new ThompsonNfaPatternProcessor(data, length, true);
}

//============================================================================
