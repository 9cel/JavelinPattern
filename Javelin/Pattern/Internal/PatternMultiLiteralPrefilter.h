//============================================================================

#pragma once
#include "Javelin/Pattern/Internal/PatternLiteralPrefilter.h"
#include "Javelin/Type/DataBlock.h"

//============================================================================

namespace Javelin::PatternInternal
{
	class Compiler;
	// Every matching branch contains at least one of its triggers, with its start
	// in the stated prefix interval. Literals use conservative byte folding;
	// they identify candidates, never complete matches.
	struct MultiLiteralTrigger
	{
		std::string literal;
		size_t minimumPrefix = 0, maximumPrefix = 0;
		LiteralPrefixRun prefixAlphabet;
		size_t branch = 0;
	};
	struct MultiLiteralPrefilter
	{
		std::vector<MultiLiteralTrigger> triggers;
		std::vector<DataBlock> branches; // AST priority order; original capture indices.
		bool HasData() const { return !branches.empty(); }
	};
	MultiLiteralPrefilter BuildMultiLiteralPrefilter(const IComponent*, Compiler&, int options, size_t captures);
	PatternProcessor* CreateMultiLiteralPrefilterProcessor(PatternProcessor*, MultiLiteralPrefilter&&);
} // namespace Javelin::PatternInternal

//============================================================================
