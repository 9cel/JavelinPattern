//============================================================================

#include "Javelin/Pattern/Internal/PatternTokenizer.h"
#include "Javelin/Pattern/Pattern.h"
#include "Javelin/Template/Utility.h"
#include "Javelin/Type/Exception.h"

//============================================================================

using namespace Javelin;
using namespace Javelin::PatternInternal;

//============================================================================

// PCRE2's limit applies to each numeric quantifier bound, including exact
// counts and the lower bound of an unbounded repetition.
static const int MAXIMUM_REPETITION_COUNT = 65535;

//============================================================================

Tokenizer::Tokenizer(Utf8Pointer aP, Utf8Pointer aEnd, bool aUseUtf8, bool aUseUnicodeProperties)
{
	useUtf8 = aUseUtf8;
	useUnicodeProperties = aUseUnicodeProperties;
	phase	= Phase::General;
	p		= aP;
	end 	= aEnd;

	ProcessTokens();
}

//============================================================================

Character Tokenizer::GetEscapedCharacter()
{
	JPATTERN_VERIFY(p < end, UnexpectedEndOfPattern, nullptr);

	unsigned char c;
	c = *pUC++;
	switch(c)
	{
	case 'a':
		return Character('\a');

	case 'b':
		return Character('\b');

	case 'p':
	case 'P':
		// A character range endpoint must be a single character.
		JPATTERN_ERROR(UnexpectedToken, pUC-1);

	case 'e':
		return Character('\e');

	case 'f':
		return Character('\f');

	case 'v':
		return Character('\v');

	case 'n':
		return Character('\n');

	case 'r':
		return Character('\r');

	case 't':
		return Character('\t');

	case 'c':
		JPATTERN_VERIFY(p < end, UnexpectedEndOfPattern, nullptr);
		c = *pUC;
		JPATTERN_VERIFY(32 <= c && c <= 126, UnexpectedControlCharacter, pUC);
		++pUC;
		return Character::ToUpper(c) ^ 0x40;

	case 'u':
		if(*pUC == '{') goto BraceHexCode;
		else
		{
			uint32_t v = 0;
			for(int i = 0; i < 4; ++i)
			{
				uint32_t x = *pUC;
				JPATTERN_VERIFY(Character::IsHexCharacter(x), UnexpectedHexCharacter, pUC);
				++pUC;
				v = v<<4 | Character::GetHexValue(x);
			}
			return Character(v);
		}

	case 'x':
		if(*pUC == '{') goto BraceHexCode;
		else
		{
			uint32_t v = 0;
			for(int i = 0; i < 2; ++i)
			{
				uint32_t x = *pUC;
				JPATTERN_VERIFY(Character::IsHexCharacter(x), UnexpectedHexCharacter, pUC);
				++pUC;
				v = v<<4 | Character::GetHexValue(x);
			}
			return Character(v);
		}

	BraceHexCode:
		{
			++pUC;
			uint32_t v = 0;
			while(*pUC != '}')
			{
				uint32_t x = *pUC;
				JPATTERN_VERIFY(Character::IsHexCharacter(x), UnexpectedHexCharacter, pUC);
				++pUC;
				v = v<<4 | Character::GetHexValue(x);
			}
			++pUC;
			return Character(v);
		}

	default:
		if(useUtf8 && c >= 128)
		{
			--pUC;
			return GetUtf8Character();
		}
		return Character(c);
	}
}

void Tokenizer::AddUnicodeProperty()
{
	UnicodeProperty property{0, *pUC++ == 'P'};
	Table<char> name;
	if(pUC < pUCEnd && *pUC == '{')
	{
		++pUC;
		bool canNegate = true;
		while(pUC < pUCEnd && *pUC != '}')
		{
			unsigned char c = *pUC++;
			if(c == ' ' || (c >= '\t' && c <= '\r') || c == '_' || c == '-') continue;
			if(canNegate && c == '^')
			{
				property.negated = !property.negated;
				canNegate = false;
				continue;
			}
			canNegate = false;
			if(c >= 'A' && c <= 'Z') c += 'a' - 'A';
			if(c == ':') c = '=';
			JPATTERN_VERIFY((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
							|| c == '=' || c == '&', MalformedUnicodeProperty, pUC-1);
			name.Append(c);
		}
		JPATTERN_VERIFY(pUC < pUCEnd && !name.IsEmpty(), MalformedUnicodeProperty, pUC);
		++pUC;
	}
	else
	{
		// The one-letter general categories also allow the Perl form \pL.
		JPATTERN_VERIFY(pUC < pUCEnd, MalformedUnicodeProperty, pUC);
		unsigned char c = *pUC++;
		if(c >= 'A' && c <= 'Z') c += 'a' - 'A';
		JPATTERN_VERIFY(c >= 'a' && c <= 'z', MalformedUnicodeProperty, pUC-1);
		name.Append(c);
	}
	name.Append('\0');
	JPATTERN_VERIFY(UnicodeProperty::Find(name.GetData(), property), UnknownUnicodeProperty, pUC);
	currentToken.unicodeProperties.Append(property);
}

Character Tokenizer::GetUtf8Character()
{
	JPATTERN_VERIFY(p < end, UnexpectedEndOfPattern, nullptr);
	size_t byteCount = p->GetNumberOfBytes();
	JPATTERN_VERIFY(byteCount <= size_t(pUCEnd - pUC), UnexpectedEndOfPattern, pUC);
	return *p++;
}

Character Tokenizer::GetCharacter()
{
	JPATTERN_VERIFY(p < end, UnexpectedEndOfPattern, nullptr);

	Character c;
	if(useUtf8)
	{
		c = GetUtf8Character();
		if(c != '\\') return c;
	}
	else
	{
		c = *pUC++;
		if(c != '\\') return c;
	}

	return GetEscapedCharacter();
}

JINLINE char Tokenizer::PeekCharacter()
{
	return p < end ? *pUC : '\0';
}

JINLINE void Tokenizer::ConsumeCharacter()
{
	if(useUtf8) ++p;
	else ++pUC;
}

template<size_t N> JINLINE bool Tokenizer::ConsumeIfMatch(const char (&s)[N])
{
	constexpr size_t length = N-1;
	if(size_t(pUCEnd - pUC) < length || memcmp(pUC, s, length) != 0) return false;
	pUC += length;
	return true;
}

//============================================================================

void Tokenizer::ProcessTokens()
{
	currentToken.unicodeProperties.SetCount(0);
	while(1)
	{
	Loop:
		switch(phase)
		{
		case Phase::General:
			switch(PeekCharacter())
			{
			case '\0':
				currentToken.type = TokenType::End;
				return;

			case '+':
				ConsumeCharacter();
				if(PeekCharacter() == '?')
				{
					ConsumeCharacter();
					currentToken.type = TokenType::OneOrMoreMinimal;
				}
				else if(PeekCharacter() == '+')
				{
					ConsumeCharacter();
					currentToken.type = TokenType::OneOrMorePossessive;
				}
				else currentToken.type = TokenType::OneOrMoreMaximal;
				return;

			case '*':
				ConsumeCharacter();
				if(PeekCharacter() == '?')
				{
					ConsumeCharacter();
					currentToken.type = TokenType::ZeroOrMoreMinimal;
				}
				else if(PeekCharacter() == '+')
				{
					ConsumeCharacter();
					currentToken.type = TokenType::ZeroOrMorePossessive;
				}
				else currentToken.type = TokenType::ZeroOrMoreMaximal;
				return;

			case '?':
				ConsumeCharacter();
				if(PeekCharacter() == '?')
				{
					ConsumeCharacter();
					currentToken.type = TokenType::ZeroOrOneMinimal;
				}
				else if(PeekCharacter() == '+')
				{
					ConsumeCharacter();
					currentToken.type = TokenType::ZeroOrOnePossessive;
				}
				else currentToken.type = TokenType::ZeroOrOneMaximal;
				return;

			case '.':
				ConsumeCharacter();
				currentToken.type = TokenType::Wildcard;
				return;

			case '^':
				ConsumeCharacter();
				currentToken.type = TokenType::StartOfLine;
				return;

			case '$':
				ConsumeCharacter();
				currentToken.type = TokenType::EndOfLine;
				return;

			case '|':
				ConsumeCharacter();
				currentToken.type = TokenType::Alternate;
				return;

			case '(':
				ConsumeCharacter();
				switch(PeekCharacter())
				{
				case '*':
					ConsumeCharacter();
					if(ConsumeIfMatch("ACCEPT)"))
					{
						currentToken.type = TokenType::Accept;
						return;
					}
					if(ConsumeIfMatch("FAIL)"))
					{
						currentToken.type = TokenType::Fail;
						return;
					}
					if(ConsumeIfMatch("F)"))
					{
						currentToken.type = TokenType::Fail;
						return;
					}
					JPATTERN_ERROR(UnableToParseGroupType, pUC);

				case '?':
					ConsumeCharacter();

					if(PeekCharacter() == '#')
					{
						ConsumeCharacter();
						while(GetCharacter() != ')') { }
						continue;
					}
					else
					{
						bool turnOn = true;
						bool signSpecified = false;
						currentToken.optionSet  = 0;
						currentToken.optionMask = 0;

						for(;;)
						{
							switch(PeekCharacter())
							{
							case ':':
								ConsumeCharacter();
								currentToken.type = TokenType::ClusterStart;
								return;

							case '=':
								ConsumeCharacter();
								currentToken.type = TokenType::PositiveLookAhead;
								return;

							case '!':
								ConsumeCharacter();
								currentToken.type = TokenType::NegativeLookAhead;
								return;

							case '<':
								ConsumeCharacter();
								switch(PeekCharacter())
								{
								case '=':
									ConsumeCharacter();
									currentToken.type = TokenType::PositiveLookBehind;
									return;

								case '!':
									ConsumeCharacter();
									currentToken.type = TokenType::NegativeLookBehind;
									return;

								default:
									JPATTERN_ERROR(UnexpectedLookBehindType, pUC);
								}
								break;

							case 'i':
								if(turnOn) currentToken.optionSet |= Pattern::IGNORE_CASE;
								currentToken.optionMask |= Pattern::IGNORE_CASE;
								ConsumeCharacter();
								break;

							case 'm':
								if(turnOn) currentToken.optionSet |= Pattern::MULTILINE;
								currentToken.optionMask |= Pattern::MULTILINE;
								ConsumeCharacter();
								break;

							case 's':
								if(turnOn) currentToken.optionSet |= Pattern::DOTALL;
								currentToken.optionMask |= Pattern::DOTALL;
								ConsumeCharacter();
								break;

							case 'u':
								if(turnOn) currentToken.optionSet |= Pattern::UNICODE_CASE;
								currentToken.optionMask |= Pattern::UNICODE_CASE;
								ConsumeCharacter();
								break;

							case 'U':
								if(turnOn) currentToken.optionSet |= Pattern::UNGREEDY;
								currentToken.optionMask |= Pattern::UNGREEDY;
								ConsumeCharacter();
								break;

							case '-':
								turnOn = false;
								signSpecified = true;
								ConsumeCharacter();
								break;

							case '+':
								turnOn = true;
								signSpecified = true;
								ConsumeCharacter();
								break;

							case '(':
								JPATTERN_VERIFY(currentToken.optionSet == 0 && currentToken.optionMask == 0 && signSpecified == false, UnexpectedGroupOptions, pUC);
								currentToken.type = TokenType::Conditional;
								return;

							case '>':
								ConsumeCharacter();
								currentToken.type = TokenType::AtomicGroup;
								return;

							case ')':
								ConsumeCharacter();
								currentToken.type = TokenType::OptionChange;
								return;

							case 'R':
								JPATTERN_VERIFY(currentToken.optionSet == 0 && currentToken.optionMask == 0 && signSpecified == false, UnexpectedGroupOptions, pUC);
								ConsumeCharacter();
								currentToken.type = TokenType::Recurse;
								currentToken.i = 0;
								JPATTERN_VERIFY(GetCharacter() == ')', ExpectedCloseGroup, pUC);
								return;

							case '0':
							case '1':
							case '2':
							case '3':
							case '4':
							case '5':
							case '6':
							case '7':
							case '8':
							case '9':
								// Recurse reference
								JPATTERN_VERIFY(currentToken.optionSet == 0 && currentToken.optionMask == 0, UnexpectedGroupOptions, pUC);
								currentToken.type = TokenType::Recurse;
								currentToken.i = PeekCharacter() - '0';
								ConsumeCharacter();

								while(1)
								{
									char c = PeekCharacter();
									if(c < '0' || c > '9') break;

									currentToken.i = currentToken.i * 10 + (c - '0');
									ConsumeCharacter();
								}
								if(signSpecified)
								{
									currentToken.type = TokenType::RecurseRelative;
									if(!turnOn) currentToken.i = -currentToken.i;
								}
								JPATTERN_VERIFY(GetCharacter() == ')', ExpectedCloseGroup, pUC);
								return;

							default:
								JPATTERN_ERROR(UnableToParseGroupType, pUC);
							}
						}
					}
					break;

				default:
					currentToken.type = TokenType::CaptureStart;
					break;
				}
				return;

			case '[':
				ConsumeCharacter();
				if(PeekCharacter() == '^')
				{
					ConsumeCharacter();
					currentToken.type = TokenType::NotRange;
				}
				else
				{
					currentToken.type = TokenType::Range;
				}
				currentToken.rangeList.SetCount(0);
				if(PeekCharacter() == '-' || PeekCharacter() == ']')
				{
					currentToken.rangeList.Append(PeekCharacter());
					ConsumeCharacter();
				}
				while(PeekCharacter() != ']')
				{
					JPATTERN_VERIFY(pUC < pUCEnd, UnexpectedEndOfPattern, pUC);
					if(pUCEnd - pUC >= 2 && pUC[0] == '[' && pUC[1] == ':')
					{
						if(ConsumeIfMatch("[:alnum:]"))
						{
							currentToken.rangeList.Add('0', '9');
							currentToken.rangeList.Add('A', 'Z');
							currentToken.rangeList.Add('a', 'z');
						}
						else if(ConsumeIfMatch("[:alpha:]"))
						{
							currentToken.rangeList.Add('A', 'Z');
							currentToken.rangeList.Add('a', 'z');
						}
						else if(ConsumeIfMatch("[:blank:]"))
						{
							currentToken.rangeList.Add('\t');
							currentToken.rangeList.Add(' ');
						}
						else if(ConsumeIfMatch("[:cntrl:]"))
						{
							currentToken.rangeList.Add('\x00', '\x1f');
							currentToken.rangeList.Add('\x7f');
						}
						else if(ConsumeIfMatch("[:digit:]"))
						{
							currentToken.rangeList.Add('0', '9');
						}
						else if(ConsumeIfMatch("[:graph:]"))
						{
							currentToken.rangeList.Add('!', '~');
						}
						else if(ConsumeIfMatch("[:lower:]"))
						{
							currentToken.rangeList.Add('a', 'z');
						}
						else if(ConsumeIfMatch("[:print:]"))
						{
							currentToken.rangeList.Add(' ', '~');
						}
						else if(ConsumeIfMatch("[:punct:]"))
						{
							currentToken.rangeList.Add('!', '/');
							currentToken.rangeList.Add(':', '@');
							currentToken.rangeList.Add('\\');
							currentToken.rangeList.Add('[', '`');
							currentToken.rangeList.Add('{', '~');
						}
						else if(ConsumeIfMatch("[:space:]"))
						{
							currentToken.rangeList.Add('\t', '\r');
							currentToken.rangeList.Add(' ');
						}
						else if(ConsumeIfMatch("[:upper:]"))
						{
							currentToken.rangeList.Add('A', 'Z');
						}
						else if(ConsumeIfMatch("[:xdigit:]"))
						{
							currentToken.rangeList.Add('0', '9');
							currentToken.rangeList.Add('A', 'F');
							currentToken.rangeList.Add('a', 'f');
						}
						else
						{
							JPATTERN_ERROR(UnknownPosixCharacterClass, pUC);
						}
					}
					else
					{
						CharacterRange interval;

						JVERIFY(PeekCharacter() != '\0');

						if(pUC[0] == '\\')
						{
							++pUC;
							switch(*pUC)
							{
							case 'p':
							case 'P':
								AddUnicodeProperty();
								JPATTERN_VERIFY(pUC == pUCEnd || *pUC != '-' ||
												(pUC+1 < pUCEnd && pUC[1] == ']'), UnexpectedToken, pUC);
								continue;

							case 'd':
								++pUC;
								if(useUnicodeProperties)
								{
									for(const auto& range : CharacterRangeList::UNICODE_DIGIT_CHARACTERS) currentToken.rangeList.Add(range);
								}
								else currentToken.rangeList.Add('0', '9');
								continue;

							case 's':
								++pUC;
								for(const CharacterRange& range : (useUnicodeProperties ? CharacterRangeList::UNICODE_WHITESPACE_CHARACTERS : CharacterRangeList::WHITESPACE_CHARACTERS))
								{
									currentToken.rangeList.Add(range);
								}
								continue;

							case 'w':
								++pUC;
								for(const CharacterRange& range : (useUnicodeProperties ? CharacterRangeList::UNICODE_WORD_CHARACTERS : CharacterRangeList::WORD_CHARACTERS))
								{
									currentToken.rangeList.Add(range);
								}
								continue;

							case 'D':
								++pUC;
								if(useUnicodeProperties)
								{
									for(const auto& range : CharacterRangeList::UNICODE_DIGIT_CHARACTERS.CreateComplement()) currentToken.rangeList.Add(range);
								}
								else
								{
									currentToken.rangeList.Add(0, '0'-1);
									currentToken.rangeList.Add('9'+1, Character::Maximum());
								}
								continue;

							case 'S':
								++pUC;
								for(const CharacterRange& range : (useUnicodeProperties ? CharacterRangeList::UNICODE_WHITESPACE_CHARACTERS : CharacterRangeList::WHITESPACE_CHARACTERS).CreateComplement())
								{
									currentToken.rangeList.Add(range);
								}
								continue;

							case 'W':
								++pUC;
								for(const CharacterRange& range : (useUnicodeProperties ? CharacterRangeList::UNICODE_WORD_CHARACTERS : CharacterRangeList::WORD_CHARACTERS).CreateComplement())
								{
									currentToken.rangeList.Add(range);
								}
								continue;

							default:
								interval.min = GetEscapedCharacter();
								break;
							}
						}
						else interval.min = GetCharacter();
						if(PeekCharacter() == '-' && pUC+1 < pUCEnd && pUC[1] != ']')
						{
							ConsumeCharacter();
							interval.max = GetCharacter();
						}
						else
						{
							interval.max = interval.min;
						}
						currentToken.rangeList.Add(interval);
					}
				}

				if(currentToken.rangeList.GetCount() == 1 && currentToken.unicodeProperties.IsEmpty())
				{
					if(currentToken.type == TokenType::Range
					   && currentToken.rangeList[0].GetSize() == 0
					   && currentToken.rangeList[0].min < 128)
					{
						currentToken.type = TokenType::Character;
						currentToken.c = currentToken.rangeList[0].min;
					}
				}
				else
				{
					currentToken.rangeList.Sort();
				}
				ConsumeCharacter();
				return;

			case ')':
				ConsumeCharacter();
				currentToken.type = TokenType::CloseBracket;
				return;

			case '{':
				ConsumeCharacter();
				currentToken.type = TokenType::CounterStart;
				phase = Phase::Counter;
				return;

			case '}':
				ConsumeCharacter();
				currentToken.type = TokenType::CounterEnd;
				return;

			case '\\':
				ConsumeCharacter();

				switch(PeekCharacter())
				{
				case '.':
				case '^':
				case '$':
				case '\\':
				case '|':
				case '+':
				case '?':
				case '(':
				case ')':
				case '<':
				case '>':
				case '[':
				case ']':
				case '{':
				case '*':
				case '}':
					currentToken.type = TokenType::Character;
					currentToken.c = PeekCharacter();
					ConsumeCharacter();
					return;

				case 'A':
					ConsumeCharacter();
					currentToken.type = TokenType::StartOfInput;
					return;

				case 'a':
					ConsumeCharacter();
					currentToken.type = TokenType::Character;
					currentToken.c = '\a';
					return;

				case 'b':
					ConsumeCharacter();
					currentToken.type = TokenType::WordBoundary;
					return;

				case 'B':
					ConsumeCharacter();
					currentToken.type = TokenType::NotWordBoundary;
					return;

				case 'C':
					ConsumeCharacter();
					currentToken.type = TokenType::AnyByte;
					return;

				case 'd':
					ConsumeCharacter();
					currentToken.type = TokenType::Range;
					currentToken.rangeList.SetCount(0);
					if(useUnicodeProperties) currentToken.rangeList = CharacterRangeList::UNICODE_DIGIT_CHARACTERS;
					else currentToken.rangeList.Append('0', '9');
					return;

				case 'p':
				case 'P':
					currentToken.type = TokenType::Range;
					currentToken.rangeList.SetCount(0);
					AddUnicodeProperty();
					return;

				case 'c':
				case 'x':
				case 'u':
					{
						currentToken.c = GetEscapedCharacter();
						if(useUtf8 && currentToken.c >= 128)
						{
							currentToken.type = TokenType::Range;
							currentToken.rangeList.SetCount(0);
							currentToken.rangeList.Append(currentToken.c);
						}
						else
						{
							currentToken.type = TokenType::Character;
						}
					}
					return;

				case 'D':
					ConsumeCharacter();
					currentToken.type = TokenType::NotRange;
					currentToken.rangeList.SetCount(0);
					if(useUnicodeProperties) currentToken.rangeList = CharacterRangeList::UNICODE_DIGIT_CHARACTERS;
					else currentToken.rangeList.Append('0', '9');
					return;

				case 'e':
					ConsumeCharacter();
					currentToken.type = TokenType::Character;
					currentToken.c = '\e';
					return;

				case 'f':
					ConsumeCharacter();
					currentToken.type = TokenType::Character;
					currentToken.c = '\f';
					return;

				case 'G':
					ConsumeCharacter();
					currentToken.type = TokenType::StartOfSearch;
					return;

				case 'h':
					ConsumeCharacter();
					currentToken.type = TokenType::Range;
					currentToken.rangeList.SetCount(0);
					currentToken.rangeList.Append('\t');
					currentToken.rangeList.Append(' ');
					return;

				case 'K':
					ConsumeCharacter();
					currentToken.type = TokenType::ResetCapture;
					return;

				case 'n':
					ConsumeCharacter();
					currentToken.type = TokenType::Character;
					currentToken.c = '\n';
					return;

				case 'Q':
					ConsumeCharacter();
					phase = Phase::Literal;
					goto Loop;

				case 'r':
					ConsumeCharacter();
					currentToken.type = TokenType::Character;
					currentToken.c = '\r';
					return;

				case 't':
					ConsumeCharacter();
					currentToken.type = TokenType::Character;
					currentToken.c = '\t';
					return;

				case 'v':
					ConsumeCharacter();
					currentToken.type = TokenType::Character;
					currentToken.c = '\v';
					return;

				case 'w':
					ConsumeCharacter();
					currentToken.type = TokenType::WordCharacter;
					return;

				case 'W':
					ConsumeCharacter();
					currentToken.type = TokenType::NotWordCharacter;
					return;

				case 's':
					ConsumeCharacter();
					currentToken.type = TokenType::WhitespaceCharacter;
					return;

				case 'S':
					ConsumeCharacter();
					currentToken.type = TokenType::NotWhitespaceCharacter;
					return;

				case 'z':
					ConsumeCharacter();
					currentToken.type = TokenType::EndOfInput;
					return;

				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
					// Backreference
					currentToken.type = TokenType::BackReference;
					currentToken.i = PeekCharacter() - '0';
					ConsumeCharacter();

					while(1)
					{
						char c = PeekCharacter();
						if(c < '0' || c > '9') break;

						currentToken.i = currentToken.i * 10 + (c - '0');
						ConsumeCharacter();
					}

					if(currentToken.i == 0)
					{
						currentToken.type = TokenType::Character;
						currentToken.c = '\0';
					}
					return;

				default:
					// Non-alphanumeric escapes quote the following literal,
					// including punctuation, whitespace and UTF-8 characters.
					if(PeekCharacter() != '\0'
					   && !('A' <= PeekCharacter() && PeekCharacter() <= 'Z')
					   && !('a' <= PeekCharacter() && PeekCharacter() <= 'z'))
						goto LiteralCharacter;
					break;
				}
				JPATTERN_ERROR(UnknownEscape, pUC);

			default:
			LiteralCharacter:
				if(useUtf8)
				{
					bool singleByte = p->GetNumberOfBytes() == 1;
					Character c = GetUtf8Character();
					if(singleByte)
					{
						currentToken.type = TokenType::Character;
						currentToken.c = c;
					}
					else
					{
						currentToken.type = TokenType::Range;
						currentToken.rangeList.SetCount(0);
						currentToken.rangeList.Append(c);
					}
				}
				else
				{
					currentToken.type = TokenType::Character;
					currentToken.c = *pUC++;
				}
				return;
			}
			JPATTERN_ERROR(InternalError, pUC);

		case Phase::Counter:
			switch(PeekCharacter())
			{
			case '\0':
				currentToken.type = TokenType::End;
				return;

			case ',':
				ConsumeCharacter();
				currentToken.type = TokenType::CounterSeparator;
				return;

			case '}':
				ConsumeCharacter();
				if(PeekCharacter() == '?')
				{
					ConsumeCharacter();
					currentToken.type = TokenType::CounterEndMinimal;
				}
				else if(PeekCharacter() == '+')
				{
					ConsumeCharacter();
					currentToken.type = TokenType::CounterEndPossessive;
				}
				else currentToken.type = TokenType::CounterEnd;
				phase = Phase::General;
				return;

			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				currentToken.type = TokenType::CounterValue;
				currentToken.i = PeekCharacter() - '0';
				ConsumeCharacter();

				while(1)
				{
					char c = PeekCharacter();
					if(c < '0' || c > '9') break;

					currentToken.i = currentToken.i * 10 + (c - '0');
					// Checking each digit also prevents integer overflow.
					JPATTERN_VERIFY(currentToken.i <= MAXIMUM_REPETITION_COUNT, MaximumRepetitionCountExceeded, pUC);
					ConsumeCharacter();
				}
				return;

			default:
				JPATTERN_ERROR(UnableToParseRepetition, pUC);
			}
			break;

		case Phase::Literal:
			switch(PeekCharacter())
			{
			case '\0':
				currentToken.type = TokenType::End;
				return;

			case '\\':
				ConsumeCharacter();
				if(PeekCharacter() == 'E')
				{
					ConsumeCharacter();
					phase = Phase::General;
					goto Loop;
				}
				currentToken.type = TokenType::Character;
				currentToken.c = '\\';
				return;

			default:
				currentToken.type = TokenType::Character;
				currentToken.c = *pUC++;
				return;
			}
		}
	}
}

//============================================================================
