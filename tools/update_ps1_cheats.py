#!/usr/bin/env python3
"""Build an offline PS1 catalogue from a local, pinned Libretro checkout.

Upstream data remains CC BY-SA 4.0, separately from the application license.
Additional numeric supplements have per-source provenance in cheat-supplements/NOTICE.md.
Only complete, supported GameShark groups are included; never run partial cheats.
Usage: python tools/update_ps1_cheats.py build/libretro-cheat-source
Attributed regional supplements in tools/cheat-supplements are included too.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import unicodedata
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'android/app/src/main/assets'
TAGS = re.compile(r'\(([^)]*)\)|\[([^]]*)\]')
SUPPORTED = {0x30, 0x80, 0x50, 0xD0, 0xD1, 0xD2, 0xD3, 0xE0, 0xE1, 0xE2, 0xE3}
REGIONS = {'usa', 'europe', 'japan', 'germany', 'france', 'italy', 'spain',
           'australia', 'uk', 'korea', 'asia', 'canada', 'netherlands', 'sweden'}
REGION_SUFFIX = re.compile(r'\s+(USA|Europe|Japan|NTSC-U|NTSC-J|PAL)$', re.I)
# Discovery aliases only: these do NOT establish revision/code compatibility.
DISCOVERY_ALIASES = {
    'battlearenatoshinden4': 'Toshinden 4',
    'boardgametopshop': 'Top Shop',
    'chippokeralphnodaibouken': 'Adventure of Little Ralph, The',
    'finalfantasyixblankandamarant': 'Final Fantasy IX',
    'finalfantasyixcinnaandquina': 'Final Fantasy IX',
    'finalfantasyoriginsfinalfantasymiscellaneous': 'Final Fantasy Origins - Final Fantasy',
    'finalfantasyoriginsfinalfantasyiimiscellaneous': 'Final Fantasy Origins - Final Fantasy II',
    'pacmanworld': 'Pac-Man World - 20th Anniversary',
    'puzzlestarsweep': 'Star Sweep',
    'tigerwoodspgatourgold': 'Tiger Woods PGA Tour Golf',
}

def catalogue_paths(source, supplements=ROOT / 'tools/cheat-supplements'):
    files = list((source / 'cht/Sony - PlayStation').glob('*.cht')) + list(supplements.glob('*.cht'))
    names = set()
    for file in files:
        if file.name in names:
            raise ValueError(f'Duplicate upstream/supplement catalogue: {file.name}')
        names.add(file.name)
    return sorted(files, key=lambda file: file.name.casefold())

def title_key(name):
    text = TAGS.sub(' ', name)
    text = re.sub(r'\s+\(?Game\s*(?:Buster|Shark)\)?\s*$', '', text, flags=re.I)
    text = REGION_SUFFIX.sub('', text.strip()).replace('&', ' and ').replace('_', ' and ')
    text = unicodedata.normalize('NFKD', text).lower()
    return re.sub('[^a-z0-9]', '', text)

def region_tags(name):
    regions = {part.strip().lower() for tag in TAGS.findall(name)
               for part in (tag[0] or tag[1]).split(',') if part.strip().lower() in REGIONS}
    suffix = REGION_SUFFIX.search(TAGS.sub(' ', name).strip())
    if suffix:
        region = suffix[1].lower()
        regions.add({'ntsc-u': 'usa', 'ntsc-j': 'japan', 'pal': 'europe'}.get(region, region))
    return regions

def release_kind(name):
    tags = ' '.join(tag[0] or tag[1] for tag in TAGS.findall(name)).lower()
    return next((kind for kind in ('demo', 'prototype', 'beta', 'sample', 'trial', 'taikenban', 'otameshi', 'cancelled')
                 if re.search(r'\b' + kind + r'\b', tags)), 'retail')

def disc_number(name):
    match = re.search(r'[\[(]Disc\s+(\d+)[\])]', name, re.I)
    return match[1] if match else None

def match_metadata(name, by_title, serial_revisions):
    key = title_key(name)
    direct = by_title.get(key, [])
    alias = DISCOVERY_ALIASES.get(key)
    rows = direct + (by_title.get(title_key(alias), []) if alias else [])
    regions, safe, related = region_tags(name), set(), set()
    for disc_name, region, serial in rows:
        if regions and region not in regions:
            continue
        if release_kind(name) != release_kind(disc_name):
            continue
        if disc_number(name) and disc_number(name) != disc_number(disc_name):
            continue
        related.add(serial)
        # Aliases aid discovery only. World/revision ambiguities still need confirmation.
        if ((disc_name, region, serial) in direct and regions and
                revision(name) == revision(disc_name) and len(serial_revisions[serial]) == 1):
            safe.add(serial)
    return safe, related

def revision(name):
    return tuple(tag[0].lower() for tag in TAGS.findall(name)
                 if re.search(r'\b(rev|v\d|version|edition|hits|best)\b', tag[0], re.I))

def convert(text, source_name):
    fields = {}
    for match in re.finditer(r'^\s*cheat(\d+)_(desc|code|handler|author)\s*=\s*(.*?)\s*$', text, re.M):
        value = match[3].strip()
        if value.startswith('"') and value.endswith('"'):
            value = value[1:-1].replace('\\"', '"')
        fields.setdefault(int(match[1]), {})[match[2]] = value
    groups, skipped, names = [], 0, set()
    for number, row in sorted(fields.items()):
        tokens = re.split(r'[+\s]+', row.get('code', '').strip())
        valid = row.get('handler', '0') == '0' and len(tokens) >= 2 and len(tokens) % 2 == 0
        codes = []
        if valid:
            for address, value in zip(tokens[::2], tokens[1::2]):
                if not re.fullmatch(r'[0-9a-fA-F]{8}', address) or not re.fullmatch(r'[0-9a-fA-F]{1,8}', value):
                    valid = False
                    break
                opcode = int(address[:2], 16)
                if opcode not in SUPPORTED or int(value, 16) > 0xFFFF:
                    valid = False
                    break
                codes.append(f'{address.upper()} {int(value, 16):04X}')
            # A dangling compare/slide would not implement the published cheat.
            if codes and int(codes[-1][:2], 16) not in {0x30, 0x80}:
                valid = False
            for i, code in enumerate(codes):
                if code.startswith('50') and (i + 1 >= len(codes) or codes[i + 1][:2] not in {'30', '80'}):
                    valid = False
        if not valid:
            skipped += 1
            continue
        name = re.sub(r'[\[\]\r\n\t]', ' ', row.get('desc', '')).strip() or f'Cheat {number + 1}'
        # Core names are byte-bounded. Keep names unique and identical in both parsers.
        name = name.encode('utf-8')[:150].decode('utf-8', errors='ignore')
        if name.lower() in names:
            name += f' (code {number + 1})'
        names.add(name.lower())
        author = re.sub(r'[\r\n\t]', ' ', row.get('author', '')).strip()
        attribution = f'Author: {author}' if author else f'Libretro: {source_name}'
        groups.append(f'[{name}]\n@desc {attribution}\n' + '\n'.join(codes) + '\n')
    comments = '\n'.join(line for line in text.splitlines() if line.lstrip().startswith(('#', ';')))
    return comments + '\n\n' + '\n'.join(groups), len(groups), skipped

def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('source', type=Path)
    source = args.parse_args().source.resolve()
    commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    metadata = []
    for line in (ASSETS / 'ps1-cover-titles.tsv').read_text(encoding='utf-8').splitlines():
        if not line.startswith('#'):
            name, region, serial = line.split('\t')
            metadata.append((name, region.lower(), serial))
    by_title = {}
    for row in metadata:
        by_title.setdefault(title_key(row[0]), []).append(row)
    # Serial alone cannot distinguish pressings which reuse the same executable ID.
    serial_revisions = {}
    for name, _, serial in metadata:
        serial_revisions.setdefault(serial, set()).add(revision(name))
    index, entries = [], []
    for file in catalogue_paths(source):
        name = file.stem
        body, count, skipped = convert(file.read_text(encoding='utf-8-sig'), name)
        if not count:
            continue
        identifier = hashlib.sha256(file.name.encode()).hexdigest()[:20]
        safe, related = match_metadata(name, by_title, serial_revisions)
        clean_name = name.replace('\t', ' ').replace('\n', ' ')
        index.append('\t'.join((identifier, clean_name, ','.join(sorted(safe)),
                                ','.join(sorted(related)), str(count), str(skipped))))
        entries.append((identifier, body))
    with zipfile.ZipFile(ASSETS / 'ps1-cheats.zip', 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for identifier, body in entries:
            entry = zipfile.ZipInfo(identifier + '.cht', date_time=(2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, body.encode('utf-8'))
    (ASSETS / 'ps1-cheats-index.tsv').write_text('# Libretro PS1 cheats, CC BY-SA 4.0\n' + '\n'.join(index) + '\n', encoding='utf-8')
    (ASSETS / 'ps1-cheats-LICENSE.txt').write_bytes((source / 'LICENSE').read_bytes())
    (ASSETS / 'ps1-cheats-NOTICE.txt').write_text(
        'PS1 cheat catalogue\n\nAttribution: Libretro database contributors, Redump contributors, and original code authors.\n'
        'Source: https://github.com/libretro/libretro-database\n'
        f'Commit: {commit}\nSource directory: cht/Sony - PlayStation\n'
        'License: Creative Commons Attribution-ShareAlike 4.0 International\n'
        'https://creativecommons.org/licenses/by-sa/4.0/\n\n'
        'Changes: converted supported, complete GameShark groups to ARMSX .cht format;\n'
        'omitted unsupported/placeholder groups; added conservative title/region/serial index.\n'
        'Discovery aliases are confirmation-only; demo/retail and explicit disc mismatches are excluded.\n'
        'Serial metadata is derived from the bundled ps1-cover-titles.tsv; its source commit\n'
        'and attribution are recorded in ps1-cover-titles-NOTICE.txt. No inferred ROM identity\n'
        'is used: this index maps known serials to cheat lists, not filenames to serials.\n'
        'Upstream data is supplied without warranty and remains CC BY-SA 4.0, separately from the app.\n'
        'Additional numeric supplements have per-source provenance and annotation notices below.\n'
        'Rebuild with tools/update_ps1_cheats.py and the pinned source checkout.\n', encoding='utf-8')
    with (ASSETS / 'ps1-cheats-NOTICE.txt').open('a', encoding='utf-8') as notice:
        notice.write('\nRegional supplements: tools/cheat-supplements.\n'
            'Crash Bandicoot - Warped (USA), SCUS-94244: six groups derived from the licensed\n'
            'Libretro Warped GameShark file and independently checked against USA listings.\n'
            'Original authors: CodeBoy (lives, powers, gems, sapphires, crystals); SonicBlueTM (platinum relics).\n'
            'USA verification: https://gamehacking.org/game/88639?hacker=CodeBoy\n'
            'USA verification: https://gamehacking.org/game/88639?hacker=SonicBlueTM\n'
            'European catalogues are retained separately. See tools/cheat-supplements/NOTICE.md.\n')
    print(f'Bundled {len(entries)} catalogues; {sum(int(row.split(chr(9))[4]) for row in index)} complete cheats.')
    from build_cheat_update import build
    print(f'GitHub download package SHA-256: {build()}')

if __name__ == '__main__':
    main()
