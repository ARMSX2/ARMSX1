#!/usr/bin/env python3
"""Audit bundled PS1 coverage. Reports data gaps; never guesses compatible codes."""
from collections import defaultdict
from pathlib import Path
import argparse
import json
from update_ps1_cheats import ASSETS


def audit(assets=ASSETS):
    discs = defaultdict(set)
    regions = defaultdict(set)
    for line in (assets / 'ps1-cover-titles.tsv').read_text(encoding='utf-8').splitlines():
        if line.startswith('#'):
            continue
        name, region, serial = line.split('\t')
        discs[serial].add(name)
        regions[region].add(serial)
    safe, related, unmatched = set(), set(), []
    catalogues = []
    for line in (assets / 'ps1-cheats-index.tsv').read_text(encoding='utf-8').splitlines():
        fields = line.split('\t')
        if len(fields) != 6 or fields[0].startswith('#'):
            continue
        catalogues.append(fields)
        safe.update(filter(None, fields[2].split(',')))
        related.update(filter(None, fields[3].split(',')))
        if not fields[3]:
            unmatched.append(fields[1])
    return {
        'catalogues': len(catalogues),
        'indexed_game_serials': len(discs),
        'serials_with_auto_candidate': len(safe),
        'serials_with_related_catalogue': len(related),
        'serials_without_related_catalogue': len(set(discs) - related),
        'catalogues_without_serial_mapping': len(unmatched),
        'region_coverage': {region: {'indexed': len(serials), 'related': len(serials & related),
                                   'auto_candidate': len(serials & safe)}
                            for region, serials in sorted(regions.items())},
        'unmapped_examples': unmatched[:45],
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, default=ASSETS)
    print(json.dumps(audit(parser.parse_args().assets), indent=2))
