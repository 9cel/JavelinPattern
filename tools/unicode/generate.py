#!/usr/bin/env python3
"""Generate Unicode 17.0.0 property tables.

Download the files in SHA256 below from https://www.unicode.org/Public/17.0.0/ucd/
into a directory, preserving subdirectories, then run:
  python3 tools/unicode/generate.py /path/to/ucd > \
    Javelin/Pattern/Internal/PatternUnicodeData.inc

The build uses the checked-in output and needs neither Python nor the UCD.
Unicode data is covered by tools/unicode/LICENSE.txt.
"""

from collections import defaultdict
import hashlib
import itertools
import pathlib
import sys

SHA256 = {
    'UnicodeData.txt': '2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c',
    'Scripts.txt': '9f5e50d3abaee7d6ce09480f325c706f485ae3240912527e651954d2d6b035bf',
    'ScriptExtensions.txt': 'ec2107e58825a1586acee8e0911ce18260394ac8b87e535ca325f1ccbeb06bc6',
    'PropertyAliases.txt': '4441f573caf952ffece1d7c892e7715bd7136dfc26f96eb6f268bf1e474715fb',
    'PropertyValueAliases.txt': '64e9a5f76f7a1e8b5a47d6a1f9a26522a251208f5276bdfa1559dac7cf2e827a',
    'PropList.txt': '130dcddcaadaf071008bdfce1e7743e04fdfbc910886f017d9f9ac931d8c64dd',
    'DerivedCoreProperties.txt': '24c7fed1195c482faaefd5c1e7eb821c5ee1fb6de07ecdbaa64b56a99da22c08',
    'emoji/emoji-data.txt': '2cb2bb9455cda83e8481541ecf5b6dfda66a3bb89efa3fa7c5297eccf607b72b',
    'extracted/DerivedBidiClass.txt': '4867b4b7f0731ed1bfcd34cc6251211ff1542541fce0734b6fbda139ee80b3a4',
    'extracted/DerivedBinaryProperties.txt': '13dd09d35a9377e33eb388a01e6581d4bfec6b2685316078c341982fa444071a',
}
MAX = 0x10ffff


def normalize(name):
    return ''.join(c.lower() for c in name if c not in '_- \t\n\v\f\r').replace(':', '=')


def records(text):
    for line in text.splitlines():
        line = line.split('#')[0].strip()
        if line:
            yield [field.strip() for field in line.split(';')]


def interval(text):
    parts = text.split('..')
    return int(parts[0], 16), int(parts[-1], 16)


def merge(ranges):
    result = []
    for start, end in sorted(ranges):
        if result and start <= result[-1][1] + 1:
            result[-1] = result[-1][0], max(end, result[-1][1])
        else:
            result.append((start, end))
    return result


def subtract(ranges, removed):
    result = []
    i = 0
    for start, end in merge(ranges):
        while i < len(removed) and removed[i][1] < start:
            i += 1
        j = i
        while j < len(removed) and removed[j][0] <= end:
            a, b = removed[j]
            if start < a:
                result.append((start, a - 1))
            start = max(start, b + 1)
            j += 1
        if start <= end:
            result.append((start, end))
    return result


def load_properties(directory):
    files = {}
    for name, digest in SHA256.items():
        data = (pathlib.Path(directory) / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'Expected unmodified Unicode 17.0.0 {name}')
        files[name] = data.decode('utf-8')

    property_aliases = {}
    for fields in records(files['PropertyAliases.txt']):
        for alias in fields:
            property_aliases[alias] = fields
    value_aliases = defaultdict(dict)
    for kind, *aliases in records(files['PropertyValueAliases.txt']):
        for alias in aliases:
            value_aliases[kind][alias] = aliases

    properties, names = {}, {}

    def add(key, ranges, aliases):
        properties[key] = merge(ranges)
        for alias in aliases:
            normalized = normalize(alias)
            assert normalized not in names or names[normalized] == key, alias
            names[normalized] = key

    def qualified(kind, value):
        return [f'{prefix}={alias}' for prefix in property_aliases[kind]
                for alias in value_aliases[kind][value]]

    categories = defaultdict(list)
    first = None
    for fields in records(files['UnicodeData.txt']):
        end = int(fields[0], 16)
        if fields[1].endswith(', First>'):
            first = end
            continue
        start = first if fields[1].endswith(', Last>') else end
        first = None
        categories[fields[2]].append((start, end))
    assigned = merge(itertools.chain.from_iterable(categories.values()))
    categories['Cn'] = subtract([(0, MAX)], assigned)
    for group in 'CLMNPSZ':
        categories[group] = merge(r for name, ranges in list(categories.items())
                                  if name.startswith(group) for r in ranges)
    categories['LC'] = merge(categories['Lu'] + categories['Ll'] + categories['Lt'])
    for name, ranges in categories.items():
        add('gc='+name, ranges, value_aliases['gc'][name] + qualified('gc', name))
    names['l&'] = 'gc=LC'

    scripts = defaultdict(list)
    for codes, script in records(files['Scripts.txt']):
        short = value_aliases['sc'][script][0]
        scripts[short].append(interval(codes))
    scripts['Zzzz'] = subtract([(0, MAX)], merge(itertools.chain.from_iterable(scripts.values())))
    extensions, overridden = defaultdict(list), []
    for codes, script_list in records(files['ScriptExtensions.txt']):
        r = interval(codes)
        overridden.append(r)
        for script in script_list.split():
            extensions[script].append(r)
    overridden = merge(overridden)
    # Script_Extensions replaces Script where explicitly specified; otherwise
    # it defaults to Script. Common/Inherited must not retain overridden points.
    for aliases in sorted(set(tuple(v) for v in value_aliases['sc'].values())):
        short = aliases[0]
        add('sc='+short, scripts[short], qualified('sc', short))
        ext_names = [f'{prefix}={alias}' for prefix in property_aliases['scx'] for alias in aliases]
        add('scx='+short, subtract(scripts[short], overridden) + extensions[short], list(aliases) + ext_names)

    for filename in ('PropList.txt', 'DerivedCoreProperties.txt', 'emoji/emoji-data.txt',
                     'extracted/DerivedBinaryProperties.txt'):
        binary = defaultdict(list)
        for fields in records(files[filename]):
            if len(fields) == 2:  # Exclude enumerated properties such as InCB.
                codes, name = fields
                binary[name].append(interval(codes))
        for name, ranges in binary.items():
            add(name, ranges, property_aliases[name])

    # Bidi defaults for unassigned code points depend on their block. Apply
    # @missing declarations in order, then the explicitly assigned values.
    bidi = ['L'] * (MAX+1)
    text = files['extracted/DerivedBidiClass.txt']
    defaults = [line.split('@missing:')[1] for line in text.splitlines() if '@missing:' in line]
    for codes, value in itertools.chain(records('\n'.join(defaults)), records(text)):
        start, end = interval(codes)
        bidi[start:end+1] = [value_aliases['bc'][value][0]] * (end-start+1)
    bidi_ranges = defaultdict(list)
    offset = 0
    for value, points in itertools.groupby(bidi):
        length = sum(1 for _ in points)
        bidi_ranges[value].append((offset, offset+length-1))
        offset += length
    for name, ranges in bidi_ranges.items():
        add('bc='+name, ranges, qualified('bc', name))

    add('Any', [(0, MAX)], ['Any'])
    add('ASCII', [(0, 127)], ['ASCII'])
    add('Assigned', assigned, ['Assigned'])
    add('Xan', categories['L'] + categories['N'], ['Xan'])
    add('Xwd', categories['L'] + categories['N'] + categories['Mn'] + categories['Pc'], ['Xwd'])
    # PCRE2's Perl/POSIX whitespace includes historical U+180E; Unicode's
    # White_Space property itself does not.
    add('Xsp', categories['Z'] + [(9, 13), (0x85, 0x85), (0x180e, 0x180e)], ['Xsp', 'Xps'])
    return properties, names


def generate(directory):
    properties, names = load_properties(directory)
    keys = sorted(properties)
    indices = {key: i for i, key in enumerate(keys)}
    assert len(keys) < 65536
    print('// Generated by tools/unicode/generate.py; do not edit.')
    print('// Unicode 17.0.0; https://www.unicode.org/Public/17.0.0/ucd/')
    print('// Source hashes and regeneration instructions are in the generator.')
    print('// See tools/unicode/LICENSE.txt for the Unicode data license.\n')

    # Share identical range sets, including scripts without extensions.
    ranges, offsets, data = [], {}, []
    for key in keys:
        values = tuple(properties[key])
        if values not in offsets:
            offsets[values] = len(ranges)
            ranges.extend(values)
        caseless = 'gc=LC' if key in ('gc=Lu', 'gc=Ll', 'gc=Lt') else key
        assert len(values) < 65536
        data.append((offsets[values], len(values), indices[caseless]))

    print('const UnicodeProperty::Range UnicodeProperty::RANGES[] = {')
    for start, end in ranges:
        print(f'  {{0x{start:04X}, 0x{end:04X}}},')
    print('};\n\nconst UnicodeProperty::Data UnicodeProperty::DATA[] = {')
    for key, (offset, count, caseless) in zip(keys, data):
        print(f'  {{{offset}, {count}, {caseless}}}, // {key}')
    print('};\n\nconst UnicodeProperty::Name UnicodeProperty::NAMES[] = {')
    for name, key in sorted(names.items()):
        print(f'  {{"{name}", {indices[key]}}},')
    print('};\n\nconst size_t UnicodeProperty::NAME_COUNT = sizeof(NAMES)/sizeof(NAMES[0]);\n')
    shorthands = [('DIGIT', 'gc=Nd'), ('WORD', 'Xwd'), ('WHITESPACE', 'Xsp')]
    for i, (shorthand, key) in enumerate(shorthands):
        if i:
            print()
        print(f'const CharacterRangeList CharacterRangeList::UNICODE_{shorthand}_CHARACTERS =')
        print(f'    UnicodeProperty{{{indices[key]}, false}}.CreateRangeList(false);')


if __name__ == '__main__':
    generate(sys.argv[1])
