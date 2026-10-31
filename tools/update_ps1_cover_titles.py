#!/usr/bin/env python3
"""Rebuild the cover-only filename index from pinned Libretro/Redump metadata.

The generated asset is CC BY-SA 4.0; it does not contain ROM data or artwork.
"""
from pathlib import Path
import re
import urllib.request

COMMIT = 'd5bae90b22018ce3c9c5a8eff4141a4768c8dd5f'
SOURCE = f'https://raw.githubusercontent.com/libretro/libretro-database/{COMMIT}'
ASSETS = Path(__file__).resolve().parents[1] / 'android/app/src/main/assets'

def download(path):
    return urllib.request.urlopen(SOURCE + '/' + urllib.parse.quote(path), timeout=30).read().decode('utf-8')

def main():
    data = download('metadat/redump/Sony - PlayStation.dat')
    rows = set()
    for block in re.findall(r'^game \(\n(.*?)^\)', data, re.M | re.S):
        fields = dict(re.findall(r'^\s*(name|region|serial) "([^"]*)"', block, re.M))
        name, region, serial = (fields.get(key, '') for key in ('name', 'region', 'serial'))
        if not re.fullmatch(r'[A-Z]{4}-\d{5}', serial) or not region:
            continue
        if re.search(r'\([^)]*\b(?:Demo|Beta|Proto|Sample|Trade Demo|Video)\b', name, re.I):
            continue
        rows.add((name, region, serial))
    header = ('# Cover-only title/region/serial index, derived from Libretro/Redump.\n'
              '# License: CC BY-SA 4.0. See ps1-cover-titles-NOTICE.txt.\n'
              f'# Source commit: {COMMIT}\n')
    (ASSETS / 'ps1-cover-titles.tsv').write_text(header + ''.join('\t'.join(row) + '\n' for row in sorted(rows)), encoding='utf-8')
    (ASSETS / 'ps1-cover-titles-LICENSE.txt').write_text(download('LICENSE'), encoding='utf-8')
    (ASSETS / 'ps1-cover-titles-NOTICE.txt').write_text(
        'PS1 cover title index\n\n'
        'Attribution: Libretro database contributors and Redump contributors.\n'
        'Source: https://github.com/libretro/libretro-database\n'
        f'Commit: {COMMIT}\n'
        'Source file: metadat/redump/Sony - PlayStation.dat\n'
        'License: Creative Commons Attribution-ShareAlike 4.0 International\n'
        'https://creativecommons.org/licenses/by-sa/4.0/\n\n'
        'Changes: extracted title, region and serial fields; omitted demo/prototype\n'
        'entries and nonstandard serials; deduplicated into a tab-separated asset.\n'
        'This derived data remains CC BY-SA 4.0, independent of the application license.\n'
        'Rebuild with tools/update_ps1_cover_titles.py.\n', encoding='utf-8')
    print(f'Generated {len(rows)} cover-title records.')

if __name__ == '__main__':
    main()
