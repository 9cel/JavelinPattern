//============================================================================

#include "Javelin/Pattern/Internal/PatternProcessor.h"
#include "Javelin/Pattern/Internal/PatternReverseProcessor.h"

//============================================================================

using namespace Javelin;
using namespace Javelin::PatternInternal;

//============================================================================

class ScanAndCapturePatternProcessor final : public PatternProcessor
{
public:
	ScanAndCapturePatternProcessor(const void* data, size_t length);
	ScanAndCapturePatternProcessor(DataBlock&& dataBlock);
	~ScanAndCapturePatternProcessor();

	virtual const void* FullMatch(const void* data, size_t length) const;
	virtual const void* FullMatch(const void* data, size_t length, const char **captures) const;
	virtual const void* PartialMatch(const void* data, size_t length, size_t offset) const;
	virtual const void* PartialMatch(const void* data, size_t length, size_t offset, const char **captures) const;
	virtual Interval<const void*> LocatePartialMatch(const void* data, size_t length, size_t offset) const;
	virtual const void* PopulateCaptures(const void* data, size_t length, size_t offset, const char **captures) const;

private:
	bool					preferReverseProcessorForFullMatchCapture;
	bool					smallAnchoredCaptureProgram;
	bool					reverseMatchRequiresStartOfSearch;
	uint8_t					numberOfCaptures;
	DataBlock				dataStore;
	PatternProcessor*		scanProcessor;
	ReverseProcessor*		reverseProcessor;
	PatternProcessor*		populateCaptureProcessor;

	void Set(const void* data, size_t length);
	JNOINLINE Interval<const void*> LocateWithCaptures(const void* data, size_t length, size_t offset, const void* result) const;
};

//============================================================================

ScanAndCapturePatternProcessor::ScanAndCapturePatternProcessor(const void* data, size_t length)
{
	Set(data, length);
}

ScanAndCapturePatternProcessor::ScanAndCapturePatternProcessor(DataBlock&& dataBlock)
: dataStore((DataBlock&&) dataBlock)
{
	Set(dataStore.GetData(), dataStore.GetCount());
}

void ScanAndCapturePatternProcessor::Set(const void* data, size_t length)
{
	const ByteCodeHeader* header = (ByteCodeHeader*) data;
	numberOfCaptures = header->numberOfCaptures;
	// Short anchored inputs can skip the forward/reverse scans and run the
	// bounded capture processor directly.
	smallAnchoredCaptureProgram = header->flags.hasStartAnchor
		&& header->numberOfCaptures > 1 && header->numberOfInstructions <= 256
		&& header->numberOfProgressChecks == 0;

	// To prevent the optimizations
	if(numberOfCaptures == 1 && header->flags.hasResetCapture) numberOfCaptures = 2;

	reverseMatchRequiresStartOfSearch = header->partialMatchStartingInstruction == header->fullMatchStartingInstruction;
	scanProcessor = PatternProcessor::CreateNfaOrDfaProcessor(data, length);

	preferReverseProcessorForFullMatchCapture = (header->flags.reverseProcessorType == PatternProcessorType::OnePass);
	reverseProcessor = ReverseProcessor::Create(data, length, true);

	if(header->flags.fullMatchProcessorType == PatternProcessorType::OnePass)
	{
		populateCaptureProcessor = PatternProcessor::CreateOnePassProcessor(data, length, false);
	}
	else
	{
		populateCaptureProcessor = PatternProcessor::CreateNfaOrBitStateProcessor(data, length, false);
	}
}

ScanAndCapturePatternProcessor::~ScanAndCapturePatternProcessor()
{
	delete scanProcessor;
	delete reverseProcessor;
	delete populateCaptureProcessor;
}

//============================================================================

const void* ScanAndCapturePatternProcessor::FullMatch(const void* data, size_t length) const
{
	return scanProcessor->FullMatch(data, length);
}

const void* ScanAndCapturePatternProcessor::FullMatch(const void* data, size_t length, const char **captures) const
{
	if(smallAnchoredCaptureProgram && length <= 1024)
		return populateCaptureProcessor->FullMatch(data, length, captures);
	const void* result = scanProcessor->FullMatch(data, length);
	if(!result) return nullptr;

	if(numberOfCaptures == 1)
	{
		captures[0] = (const char*) data;
		captures[1] = (const char*) result;
		return result;
	}
	else if(preferReverseProcessorForFullMatchCapture)
	{
		reverseProcessor->Match(data, length, 0, result, captures, true);
	}
	else
	{
		populateCaptureProcessor->FullMatch(data, length, captures);
	}

	return result;
}

const void* ScanAndCapturePatternProcessor::PartialMatch(const void* data, size_t length, size_t offset) const
{
	return scanProcessor->PartialMatch(data, length, offset);
}

const void* ScanAndCapturePatternProcessor::PartialMatch(const void* data, size_t length, size_t offset, const char **captures) const
{
	if(smallAnchoredCaptureProgram && length <= 1024)
		return populateCaptureProcessor->PartialMatch(data, length, offset, captures);
	const void* result = scanProcessor->PartialMatch(data, length, offset);
	if(!result) return nullptr;

	const void* start = reverseProcessor->Match(data, length, offset, result, captures, reverseMatchRequiresStartOfSearch);
	if(start)
	{
		if(numberOfCaptures == 1)
		{
			captures[0] = (const char*) start;
			captures[1] = (const char*) result;
		}
		else
		{
			// Truncating the input can turn a failed assertion into a match.
			const void* populated = populateCaptureProcessor->PopulateCaptures(data, length, intptr_t(start)-intptr_t(data), captures);
			JASSERT(populated != nullptr);
			if(!populated)
			{
				captures[0] = (const char*) start;
				captures[1] = (const char*) result;
			}
		}
	}

	return result;
}

Interval<const void*> ScanAndCapturePatternProcessor::LocatePartialMatch(const void* data, size_t length, size_t offset) const
{
	const void* result = scanProcessor->PartialMatch(data, length, offset);
	if(!result) return {nullptr, nullptr};
	// An empty match at the search boundary needs no reverse scan.
	if(numberOfCaptures == 1 && result == static_cast<const char*>(data) + offset)
		return {result, result};
	// Only the one-pass reverse processor writes captures.
	if(!preferReverseProcessorForFullMatchCapture)
		return {reverseProcessor->Match(data, length, offset, result, nullptr, reverseMatchRequiresStartOfSearch), result};
	return LocateWithCaptures(data, length, offset, result);
}

Interval<const void*> ScanAndCapturePatternProcessor::LocateWithCaptures(const void* data, size_t length, size_t offset, const void* result) const
{
	const char* captures[numberOfCaptures*2];
	const void* start = reverseProcessor->Match(data, length, offset, result, captures, reverseMatchRequiresStartOfSearch);
	if(start) return {start, result};
	return {captures[0], captures[1]};
}

const void* ScanAndCapturePatternProcessor::PopulateCaptures(const void* data, size_t length, size_t offset, const char **captures) const
{
	return populateCaptureProcessor->PopulateCaptures(data, length, offset, captures);
}

//============================================================================

PatternProcessor* PatternProcessor::CreateScanAndCaptureProcessor(DataBlock&& dataBlock)
{
	return new ScanAndCapturePatternProcessor((DataBlock&&) dataBlock);
}

PatternProcessor* PatternProcessor::CreateScanAndCaptureProcessor(const void* data, size_t length, bool makeCopy)
{
	if(makeCopy)
	{
		DataBlock dataBlock(data, length);
		return new ScanAndCapturePatternProcessor((DataBlock&&) dataBlock);
	}
	else
	{
		return new ScanAndCapturePatternProcessor(data, length);
	}
}

//============================================================================
