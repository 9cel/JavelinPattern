#### About this fork

This is a fork of Jeffrey Lim's [JavelinPattern](https://github.com/jthlim/JavelinPattern).

It differs from the upstream repository in the following ways:

- Fixes some build errors that I ran into trying to build the library on x86_64 Ubuntu 24.04.
- Fixes some bugs (memory safety, floating point UB, incorrect matching) that I discovered
  while testing/benchmarking and doing some fuzzing (though some of these may have been
  introduced by my other changes).
- Extends Unicode support by recognizing `\p{...}`/`\P{...}` and introducing a new
  option (`JP_OPTION_UCP`) which makes `\w`, `\W`, `\s`, `\S`, `\b`, and `\B` Unicode-aware.
- Adds two new functions to the public API (`jp_scan()` and `jp_scan_captures()`) for iterating
  matches without most of the overhead incurred by repeatedly restarting the scan on each call
  to `jp_partial_match()`.
- Introduces some vibecoded optimizations (e.g. literal prefilter) and adjusts some of the
  defaults to support some large/complex patterns that previously wouldn't compile at all or
  would take very long (>10s) to compile.

Despite the vibecoding, the library has been tested pretty extensively in its current state
on x86_64 using a fairly wide range of challenging patterns and haystacks, including under
LLVM's LibFuzzer and ASan to catch memory safety bugs and against PCRE2 to verify correctness.

# JavelinPattern v0.1

JavelinPattern is a regular expression engine which aims to be fast _and_ feature rich.

There is also a pre-built OSX binary for [ag (the_silver_searcher)](https://github.com/jthlim/the_silver_searcher) that uses JavelinPattern.

## Features

- JavelinPattern is _fast_
- Multiple internal processing engines
- Supports sub-match capturing
- Supports minimal/maximal/possessive quantifiers
- Supports unicode case folding
- JavelinPattern really is _fast_...
- Supports offline pattern compilation
- Supports positive and negative lookaheads, lookbehinds
- Supports conditional and recursive regexes
- JIT for back tracking x64 and arm64 engine
- Automatic stack guarding for x64 (configurable auto-stack growth or fail match)
- Threadsafe -- patterns can be used on multiple threads concurrently
- Support for glob syntax patterns
- ... did I mention it is _fast_ ?

## Performance

JavelinPattern has a number of internal engines that it uses depending on the pattern
provided. To be feature rich, it has a back-tracking engine that provides look-aheads,
look-behinds, conditional regexes and other features. If you use one of these features,
the back tracking engine will automatically be chosen. In other situations, the pattern
compiler will try and determine the most effective engine to handle the supplied pattern.

Test machine: Macbook Pro Retina Mid 2012, 2.6 GHz Intel Core i7, OSX El Capitan 10.11.4 using code modified from here: [Performance comparison of regular expression engines](http://sljit.sourceforge.net/regex_perf.html)

Javelin-BT is the performance when the `JP_OPTION_PREFER_BACK_TRACKING` flag is set. The following table is the time taken (in milliseconds) to scan [mtent12.txt](http://www.gutenberg.org/files/3200/old/mtent12.zip) -- a text file approximately 20MB in size.

|                                        |  PCRE-DFA | PCRE-JIT |     ONIG |      RE2 | JAVELIN-BT | JAVELIN |
| -------------------------------------- | --------: | -------- | -------: | -------: | ---------: | ------: |
| Twain                                  |        15 | 15       |       16 |        2 |          1 |       1 |
| (?i)Twain                              |       106 | 16       |      120 |       84 |          2 |       2 |
| [a-z]shing                             |       881 | 15       |       15 |      121 |          3 |       4 |
| Huck[a-zA-Z]+\|Saw[a-zA-Z]+            |        56 | 3        |       45 |       73 |          2 |       2 |
| \b\w+nn\b                              |      1240 | 101      |      970 |       72 |          1 |       1 |
| [a-q][^u-z]{13}x                       |      2515 | 2        |       56 |     4175 |          1 |       2 |
| Tom\|Sawyer\|Huckleberry\|Finn         |        62 | 27       |       52 |       74 |          2 |       2 |
| (?i)Tom\|Sawyer\|Huckleberry\|Finn     |       451 | 77       |      562 |      109 |          3 |       3 |
| .{0,2}(Tom\|Sawyer\|Huckleberry\|Finn) |      4726 | 334      |      118 |       85 |        114 |      12 |
| .{2,4}(Tom\|Sawyer\|Huckleberry\|Finn) |      5905 | 367      |      118 |       80 |        124 |      13 |
| Tom.{10,25}river\|river.{10,25}Tom     |       119 | 14       |       85 |       81 |          3 |       6 |
| [a-zA-Z]+ing                           |      1978 | 73       |     1090 |      138 |          5 |       7 |
| \s[a-zA-Z]{0,12}ing\s                  |       911 | 105      |       77 |      100 |         72 |      45 |
| ([A-Za-z]awyer\|[A-Za-z]inn)\s         |      1336 | 33       |      201 |      116 |          2 |       2 |
| ["'][^"']{0,30}[?!\.]["']              |       130 | 13       |       94 |       77 |          5 |       8 |
| ([0-9]+)-([0-9]+)-([0-9]+)             |        50 | 17       |       41 |      167 |          1 |       1 |
| **TOTAL**                              | **20479** | **1210** | **3661** | **5554** |    **342** | **109** |

![Pattern Performance Graph](PatternPerformance.png)

Note: Graph based on µs resolution measurements

## API

Check [JavelinPattern.h](JavelinPattern.h) to see the list of available functions (It's very brief)

Example code snippet:

```c
#include "JavelinPattern.h"

{
  jp_pattern_t pattern;
  int result = jp_pattern_compile(&pattern, "test(\\w+)", JP_OPTION_IGNORE_CASE | JP_OPTION_UTF8);
  if(result != 0) ...

  const void* captures[4];
  if(jp_partial_match(pattern, data, strlen(data), captures, 0))
  {
     // captures[0] contains start of match (inclusive)
	 // captures[1] contains end of match (exclusive)
	 // captures[2] contains start of \w+ subgroup match
	 // captures[3] contains end of \w+ subgroup match
  }

  jp_pattern_free(pattern);
}

```

## Supported Patterns

### Unicode character classes

Pass `JP_OPTION_UCP` (`Javelin::Pattern::UCP` in C++) to enable Unicode property
classification for `\d`, `\w`, `\s`, `\b`, and their uppercase complements.
It applies inside character classes too, including mixed and negated classes.
`[\b]` remains the backspace character.

UCP is independent of encoding and case folding. Use `JP_OPTION_UTF8 |
JP_OPTION_UCP` for UTF-8 input. With UCP alone, each input byte is classified
as its Latin-1 code point. `JP_OPTION_UNICODE_CASE` retains its separate role
in case-insensitive matching. Without UCP, the existing ASCII shorthand and
word-boundary behavior is preserved (including the legacy omission of vertical
tab from `\s`).

The property tables use Unicode **17.0.0** and PCRE2 10.43+ class definitions:

| Escape | UCP definition |
| --- | --- |
| `\d` | General category `Nd` (decimal digits) |
| `\w` | Categories `L`, `N`, `Mn`, and `Pc` |
| `\s` | Categories `Z`, plus TAB–CR, NEL, and U+180E |
| `\b` | A transition between word and non-word characters; input edges count as non-word |

`\D`, `\W`, and `\S` complement their corresponding character sets; `\B`
asserts the absence of a word boundary. In UTF-8 mode boundaries occur only
between encoded characters. These are regex word boundaries, not Unicode
text-segmentation rules. UCP does not change POSIX classes, `\h`, or `\v`.

Explicit `\p{property}` and `\P{property}` escapes match one character
with or without the named property, independently of UCP. With `JP_OPTION_UTF8`,
they decode UTF-8; without it, they classify each byte as its Latin-1 code point
(U+0000–U+00FF), as PCRE2 does. They can be mixed with literals and
other properties inside brackets, for example `[\p{L}\p{Nd}_]` and
`[^\p{White_Space}]`. A property cannot be a character-range endpoint.

Supported property names use Unicode 17.0.0 data and aliases:

| Property | Examples |
| --- | --- |
| General categories, including aggregates | `\p{L}`, `\p{Letter}`, `\p{Nd}`, `\p{gc=Decimal_Number}`, `\p{General_Category:Lu}`, `\p{LC}`, `\p{L&}` |
| Scripts | `\p{sc=Greek}`, `\p{Script:Grek}` |
| Script extensions | `\p{Greek}`, `\p{scx=Grek}`, `\p{Script_Extensions=Katakana}` |
| Binary properties | `\p{Alphabetic}`, `\p{Alpha}`, `\p{White_Space}`, `\p{Emoji}`, `\p{XID_Start}` |
| Bidirectional classes | `\p{bc=AL}`, `\p{Bidi_Class:Arabic_Letter}` |
| Additional sets | `Any`, `ASCII`, `Assigned`, `Xan` (L or N), `Xwd` (UCP word), `Xsp`/`Xps` (UCP whitespace) |

Names ignore ASCII case, whitespace, underscores, and hyphens; `:` and `=`
are equivalent separators. One-letter categories can omit braces (`\pL`,
`\PN`). A leading `^` inside braces negates the property: `\p{^L}` means
`\P{L}`, and `\P{^L}` means `\p{L}`. Unknown names and malformed property
escapes are compile errors. Blocks, ages, string properties, and `Is`/`In`
prefixes are not supported. Binary properties come from `PropList.txt`,
`DerivedCoreProperties.txt`, `DerivedBinaryProperties.txt`, and `emoji-data.txt`.

As in PCRE2 10.45+, caseless matching treats `Lu`, `Ll`, and `Lt` (and their
aliases) as `LC`; other properties keep their membership. This applies with
`JP_OPTION_IGNORE_CASE` or inline `(?i)`, independently of UCP and
`JP_OPTION_UNICODE_CASE`. Literal class members retain the ordinary case-folding
rules. `\p{White_Space}` uses the Unicode definition, which excludes U+180E;
`\p{Xsp}` and UCP `\s` include it for PCRE2 compatibility.

Bare script names mean Script_Extensions. Explicit script-extension assignments
replace the primary script's default membership, including for Common and
Inherited. This follows Unicode's data; PCRE2 instead treats `scx=Common` and
`scx=Inherited` as primary-script queries. Surrogates are not UTF-8 characters:
`\p{Cs}` and `\P{Any}` match nothing; their complements match Unicode scalar
values in UTF-8 mode, or any byte in byte mode.

Malformed property syntax and unknown names report
`JP_RESULT_MALFORMED_UNICODE_PROPERTY` and `JP_RESULT_UNKNOWN_UNICODE_PROPERTY`,
respectively. The C++ API has matching `PatternException::Type` values.

Unicode classes use the ordinary bytecode engines. UCP word boundaries currently
lower to fixed-width lookarounds and therefore select the backtracking engine,
including its x64/ARM64 JIT. This can affect performance when enabling the option
on existing patterns.

The generated tables are checked in; builds do not download Unicode data.
See [the generator](tools/unicode/generate.py) for regeneration instructions and
[the Unicode license](tools/unicode/LICENSE.txt).

### Syntax

| Pattern         | BT Only | Meaning                                                   |
| --------------- | ------- | --------------------------------------------------------- |
| (...)           |         | Capture                                                   |
| (?:...)         |         | Cluster                                                   |
| (?_x_)          |         | Option change ('i', 's', 'm', 'u', 'U')                   |
| (?#...)         |         | Comment                                                   |
| (?=...)         | Y       | Positive look-ahead                                       |
| (?!...)         | Y       | Negative look-ahead                                       |
| (?\<=...)       | Y       | Positive look-behind                                      |
| (?\<!...)       | Y       | Negative look-behind                                      |
| (?\>...)        | Y       | Atomic group                                              |
| (?(_c_)_t_:_f_) | Y       | Conditional regex                                         |
| (?R)            | Y       | Recursive regex                                           |
| (?1)            | Y       | Recursive to capture group 1                              |
| (?-_n_)         | Y       | Recursive to relative capture group                       |
| (?+_n_)         | Y       | Recursive to relative capture group                       |
| \|              |         | Alternation                                               |
| .               |         | Any character                                             |
| ^               |         | Start of line                                             |
| $               |         | End of line                                               |
| \\a             |         | Alarm (ASCII 0x07)                                        |
| \\A             |         | Start of input                                            |
| \\b             |         | Word boundary                                             |
| \\B             |         | Not word boundary                                         |
| \\c*X*          |         | Control-_X_                                               |
| \\C             |         | Any byte                                                  |
| \\d             |         | Digit                                                     |
| \\D             |         | Not digit                                                 |
| \\e             |         | Escape (ASCII 0x1b)                                       |
| \\f             |         | Form feed (ASCII 0x0c)                                    |
| \\G             |         | Start of search                                           |
| \\h             |         | Horizontal whitespace                                     |
| \\K             |         | Reset capture                                             |
| \\n             |         | Newline (ASCII 0x0a)                                      |
| \\p{_property_} |         | Unicode property (byte or UTF8 mode; independent of UCP)  |
| \\P{_property_} |         | Complement of a Unicode property                          |
| \\Q...\\E       |         | Treat ... after \\Q as a literal until \\E is encountered |
| \\r             |         | Carriage Return (ASCII 0x0d)                              |
| \\s             |         | Whitespace                                                |
| \\S             |         | Not whitespace                                            |
| \\t             |         | Horizontal tab (ASCII 0x09)                               |
| \\u*####*       |         | Unicode 4-hex digits                                      |
| \\u{_###_}      |         | Unicode hex                                               |
| \\v             |         | Vertical tab (ASCII 0x0b)                                 |
| \\w             |         | Word character                                            |
| \\W             |         | Not word character                                        |
| \\x##           |         | 2 hex digits                                              |
| \\x{_###_}      |         | Unicode hex                                               |
| \\z             |         | End of input                                              |
| \\1             | Y       | Back reference                                            |
| \\\\            |         | Backslash                                                 |
| [...]           |         | Character set (ranges supported)                          |
| [^...]          |         | Not character set (ranges supported)                      |
| [[:*xxx*:]]     |         | POSIX character class _xxx_, eg `lower`, `alpha`          |

| Quantifiers | BT Only | Meaning                          |
| ----------- | ------- | -------------------------------- |
| \*          |         | 0 or more maximal                |
| \*?         |         | 0 or more minimal                |
| \*+         | Y       | 0 or more possessive             |
| \+          |         | 1 or more maximal                |
| \+?         |         | 1 or more minimal                |
| \++         | Y       | 1 or more possessive             |
| ?           |         | 0 or 1 maximal                   |
| ??          |         | 0 or 1 minimal                   |
| ?+          | Y       | 0 or 1 possessive                |
| {###}       |         | Exactly ### times                |
| {M,N}       |         | Between M and N times maximal    |
| {M,N}?      |         | Between M and N times minimal    |
| {M,N}+      | Y       | Between M and N times possessive |
| {M,}        |         | At least M times maximal         |
| {M,}?       |         | At least M times minimal         |
| {M,}+       | Y       | At least M times possessive`     |
| {,N}        |         | At most M times maximal          |
| {,N}?       |         | At most M times minimal          |
| {,N}+       | Y       | At most M times possessive`      |
