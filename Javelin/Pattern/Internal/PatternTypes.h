//==========================================================================

#pragma once
#include "Javelin/Type/Character.h"
#include "Javelin/Type/String.h"
#include "Javelin/Type/Interval.h"

//==========================================================================

namespace Javelin
{
//==========================================================================

	class ICharacterWriter;

	namespace PatternInternal
	{
//==========================================================================

		enum class TokenType
		{
			Character,
			AnyByte,
			Wildcard,

			StartOfInput,
			EndOfInput,
			StartOfLine,
			EndOfLine,
			WordBoundary,
			NotWordBoundary,
			StartOfSearch,

			Range,
			NotRange,
			WhitespaceCharacter,
			NotWhitespaceCharacter,
			WordCharacter,
			NotWordCharacter,

			Alternate,
			BackReference,

			ResetCapture,
			CaptureStart,
			ClusterStart,
			PositiveLookAhead,
			NegativeLookAhead,
			PositiveLookBehind,
			NegativeLookBehind,
			OptionChange,
			AtomicGroup,
			CloseBracket,
			Conditional,

			Recurse,
			RecurseRelative,

			Accept,
			Fail,

			CounterStart,
			CounterEnd,
			CounterEndMinimal,
			CounterEndPossessive,
			CounterValue,
			CounterSeparator,

			OneOrMoreMaximal,
			OneOrMoreMinimal,
			ZeroOrMoreMaximal,
			ZeroOrMoreMinimal,
			ZeroOrOneMaximal,
			ZeroOrOneMinimal,

			OneOrMorePossessive,
			ZeroOrMorePossessive,
			ZeroOrOnePossessive,
			End,
		};

//==========================================================================

		struct CharacterRange : public Interval<Character, true>
		{
			using Interval::Interval;
			CharacterRange() = default;
			CharacterRange(const Interval& a) : Interval(a) { }

			void TrimRelevancyInterval(CharacterRange& range) const;
		};

		struct CaseConversionData
		{
			CharacterRange	range;
			int				delta;

			static const int MERGE_EVEN_ODD 	= 0x80000000;
			static const int MERGE_ODD_EVEN 	= 0x80000001;
			static const int MERGE_ALL			= 0x80000002;
			static const int MERGE_FIXED		= 0x80000000;
			static const int MERGE_FIXED_MASK	= 0xC0000000;

			bool operator<(const CaseConversionData& other) const { return range.min < other.range.min || (range.min == other.range.min && delta < other.delta); }
			bool operator==(const CaseConversionData& other) const { return range.min == other.range.min; }

			static const CaseConversionData DATA[];
			static const uint32_t COUNT;
		};

		class CharacterRangeList : public Table<CharacterRange, TableStorage_Dynamic<DynamicTableAllocatePolicy_NewDeleteWithInlineStorage<4>::Policy>>
		{
		public:
			bool IsSingleByteUtf8() const;
			uint32_t GetNumberOfMatchingBytes() const;
			bool Contains(Character c) const;
			void Add(CharacterRange interval);
			void Add(Character c) 						{ Add(CharacterRange{c,c}); 		}
			void Add(Character min, Character max)	 	{ Add(CharacterRange{min,max});	}
			void Sort();
			void Dump(ICharacterWriter& output);

			CharacterRangeList CreateAsciiRange() const;
			CharacterRangeList CreateUnicodeScalarRange() const;
			CharacterRangeList CreateComplement() const;
			CharacterRangeList CreateCaseInsensitive() const;
			CharacterRangeList CreateUnicodeCaseInsensitive() const;

			void Append(const CharacterRange& range)	{ Table::Append(range);						}
			void Append(Character c) 					{ Table::Append(CharacterRange{c,c}); 		}
			void Append(Character min, Character max) 	{ Table::Append(CharacterRange{min,max});	}

			void TrimRelevancyInterval(CharacterRange& range) const;

			static const CharacterRangeList WORD_CHARACTERS;
			static const CharacterRangeList NOT_WORD_CHARACTERS;
			static const CharacterRangeList WHITESPACE_CHARACTERS;
			static const CharacterRangeList UNICODE_DIGIT_CHARACTERS;
			static const CharacterRangeList UNICODE_WORD_CHARACTERS;
			static const CharacterRangeList UNICODE_WHITESPACE_CHARACTERS;

			static const CharacterRangeList HORIZONTAL_WHITESPACE_CHARACTERS;
			static const CharacterRangeList VERTICAL_WHITESPACE_CHARACTERS;
		};

//==========================================================================

		// Keep properties separate from literals when applying case folding.
		struct UnicodeProperty
		{
			struct Range { uint32_t min, max; };
			struct Data { uint32_t offset; uint16_t count, caselessIndex; };
			struct Name { const char* name; uint16_t index; };

			uint16_t index;
			bool negated;

			static bool Find(const char* normalizedName, UnicodeProperty& result);
			CharacterRangeList CreateRangeList(bool ignoreCase) const;

			static const Range RANGES[];
			static const Data DATA[];
			static const Name NAMES[];
			static const size_t NAME_COUNT;
		};

		struct Token
		{
			TokenType				type;

			union
			{
				Character			c;
				int					i;
				struct
				{
					uint16_t		optionSet;
					uint16_t		optionMask;
				};
			};

			Table<char>				stringData;
			CharacterRangeList		rangeList;
			Table<UnicodeProperty>	unicodeProperties;

			Token() { type = TokenType::End; }

			String GetString() const;

			void operator=(Token &&a);
		};

//==========================================================================
	} // namespace PatternInternal
} // namespace Javelin
//==========================================================================
