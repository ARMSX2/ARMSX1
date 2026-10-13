"""Publishable data-only package for the in-app Download cheats button.

Run after updating/rebuilding the bundled catalogue. This never scans research
directories or includes ROMs, BIOS files, user cheats, settings or APKs.
"""
import argparse
import hashlib
import io
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FILES = (
    'ps1-cheats.zip', 'ps1-cheats-index.tsv', 'ps1-cheats-NOTICE.txt',
    'ps1-cheats-LICENSE.txt', 'ps1-cover-titles.tsv',
    'ps1-cover-titles-NOTICE.txt', 'ps1-cover-titles-LICENSE.txt',
)


def package_bytes(assets):
    payload = {name: (assets / name).read_bytes() for name in FILES}
    if any(not body for body in payload.values()):
        raise ValueError('Empty catalogue component')
    payload['format-version.txt'] = b'1\n'
    payload['hashes.tsv'] = ''.join(
        f'{name}\t{hashlib.sha256(payload[name]).hexdigest()}\n' for name in FILES
    ).encode('utf-8')
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w') as archive:
        for name, body in sorted(payload.items()):
            entry = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, body, compresslevel=9)
    result = output.getvalue()
    if len(result) > 12 * 1024 * 1024:
        raise ValueError('Catalogue exceeds app download limit')
    return result


def build(assets=ROOT / 'android/app/src/main/assets', output=ROOT / 'cheats/ps1-cheats-update.zip'):
    data = package_bytes(assets)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, default=ROOT / 'android/app/src/main/assets')
    parser.add_argument('--output', type=Path, default=ROOT / 'cheats/ps1-cheats-update.zip')
    args = parser.parse_args()
    print(f'{args.output}: SHA-256 {build(args.assets, args.output)}')


if __name__ == '__main__':
    main()
