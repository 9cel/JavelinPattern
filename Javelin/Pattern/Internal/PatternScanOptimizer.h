//============================================================================

#pragma once
#include <cstddef>
#include <memory>

//============================================================================

namespace Javelin::PatternInternal
{
	struct IComponent;

	// Share scanning work between nonempty matches. Capture recovery uses the
	// ordinary processor, and each scan keeps its own input state.
	class PatternScanOptimizer
	{
	public:
		using MatchCallback = int (*)(size_t from, size_t to, void* user);
		virtual ~PatternScanOptimizer() = default;
		virtual int Scan(const void* data, size_t length, void* user, MatchCallback onMatch, size_t offset) const = 0;
		static std::unique_ptr<PatternScanOptimizer> Build(const IComponent* root);
	};
}

//============================================================================
