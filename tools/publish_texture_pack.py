"""Validate a redistribution-approved, normalized PNG pack and update the GitHub catalogue.

Uploads/releases are deliberate owner actions, not performed by this script.
"""
import argparse
import binascii
import hashlib
import io
from pathlib import Path
import re
import struct
from urllib.parse import urlsplit
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CATALOGUE = ROOT / 'texture-packs/catalogue.tsv'
ASSET = ROOT / 'android/app/src/main/assets/ps1-texture-packs.tsv'
HEADER = '# EmuCorePSX texture packs v1'
HOSTS = {'github.com', 'raw.githubusercontent.com', 'objects.githubusercontent.com', 'release-assets.githubusercontent.com'}
TEXTURE = re.compile(r'replacements/vram-write-[a-f0-9]{32}\.png\Z')
MAX_DOWNLOAD = 2 * 1024**3
MAX_EXPANDED = 8 * 1024**3


def https(url, download=False):
    uri = urlsplit(url)
    if (uri.scheme != 'https' or not uri.hostname or uri.username is not None or uri.password is not None
            or uri.fragment or uri.port not in (None, 443) or (download and uri.hostname not in HOSTS)):
        raise ValueError('Unsupported HTTPS URL')


def validate_png(data):
    stream = io.BytesIO(data)
    if stream.read(8) != b'\x89PNG\r\n\x1a\n':
        raise ValueError('Invalid PNG signature')
    header = pixels = False
    for _ in range(100_000):
        raw_length = stream.read(4)
        if len(raw_length) != 4:
            raise ValueError('Truncated PNG')
        length, = struct.unpack('>I', raw_length)
        kind = stream.read(4)
        if length > 64 * 1024**2 or not re.fullmatch(b'[A-Za-z]{4}', kind):
            raise ValueError('Invalid PNG chunk')
        if not header and (kind != b'IHDR' or length != 13):
            raise ValueError('Missing PNG header')
        payload, crc = stream.read(length), stream.read(4)
        if len(payload) != length or len(crc) != 4 or struct.unpack('>I', crc)[0] != binascii.crc32(kind + payload):
            raise ValueError('Invalid PNG checksum')
        if kind == b'IHDR':
            if header:
                raise ValueError('Duplicate PNG header')
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', payload)
            depths = {0: {1, 2, 4, 8, 16}, 2: {8, 16}, 3: {1, 2, 4, 8}, 4: {8, 16}, 6: {8, 16}}
            if (not 1 <= width <= 8192 or not 1 <= height <= 8192 or width * height > 16_777_216
                    or depth not in depths.get(color, set()) or compression or filtering or interlace not in (0, 1)):
                raise ValueError('Unsupported PNG dimensions or header')
            header = True
        elif kind == b'IDAT' and length:
            pixels = True
        elif kind == b'IEND':
            if length or not pixels or stream.read(1):
                raise ValueError('Invalid PNG end')
            return
    raise ValueError('Excessive PNG chunks')


def inspect_pack(path):
    size = path.stat().st_size
    if not 1 <= size <= MAX_DOWNLOAD:
        raise ValueError('Invalid pack download size')
    names, expanded, images = set(), 0, 0
    with zipfile.ZipFile(path) as archive:
        for entry in archive.infolist():
            if entry.filename in names or len(names) >= 100_002:
                raise ValueError('Duplicate/excessive pack entries')
            names.add(entry.filename)
            if entry.is_dir():
                if entry.filename != 'replacements/':
                    raise ValueError('Unsupported directory')
                continue
            image = TEXTURE.fullmatch(entry.filename)
            if not image and entry.filename not in {'LICENSE.txt', 'NOTICE.txt'}:
                raise ValueError('Unsupported pack path/format')
            limit = 64 * 1024**2 if image else 128 * 1024
            with archive.open(entry) as stream:
                data = stream.read(limit + 1)
            if not 0 < len(data) <= limit or len(data) != entry.file_size:
                raise ValueError('Invalid entry size')
            expanded += len(data)
            if expanded > MAX_EXPANDED:
                raise ValueError('Pack exceeds expanded size limit')
            if image:
                validate_png(data)
                images += 1
            elif not data.decode('utf-8').strip() or b'\0' in data:
                raise ValueError('Invalid notice/license text')
    if not images or 'LICENSE.txt' not in names:
        raise ValueError('Pack needs textures and a license')
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return digest, size, expanded


def publish(path, identifier, title, serials, version, author, license_label, source_url, download_url,
            catalogue=CATALOGUE, asset=ASSET):
    if not re.fullmatch('[a-z0-9][a-z0-9-]{0,63}', identifier):
        raise ValueError('Invalid pack ID')
    serials = sorted(set(serials.split(',')))
    if not 1 <= len(serials) <= 128 or any(not re.fullmatch('[A-Z]{4}-[0-9]{5}', serial) for serial in serials):
        raise ValueError('Explicit normalized serials required')
    https(source_url); https(download_url, download=True)
    digest, size, expanded = inspect_pack(path)
    values = [identifier, title, ','.join(serials), version, author, license_label, source_url, download_url,
              digest, str(size), str(expanded)]
    if any(not value.strip() or len(value) > 2048 or any(char in value for char in '\t\r\n\0') for value in values):
        raise ValueError('Invalid catalogue field')
    existing = catalogue.read_text(encoding='utf-8')
    if existing.splitlines()[0] != HEADER:
        raise ValueError('Unknown catalogue version')
    lines = [line for line in existing.splitlines()[1:] if line and not line.startswith('#') and line.split('\t')[0] != identifier]
    if len(lines) >= 2000:
        raise ValueError('Catalogue entry limit reached')
    lines.append('\t'.join(values))
    text = HEADER + '\n' + '\n'.join(sorted(lines)) + '\n'
    if len(text.encode('utf-8')) > 1024**2:
        raise ValueError('Catalogue exceeds app size limit')
    catalogue.write_text(text, encoding='utf-8')
    asset.write_text(text, encoding='utf-8')
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', required=True, type=Path)
    for name in ('id', 'title', 'serials', 'version', 'author', 'license', 'source-url', 'download-url'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--confirm-redistribution', action='store_true', required=True,
                        help='Confirm you have the rights to distribute this pack; preserve original notices.')
    args = parser.parse_args()
    digest = publish(args.pack, args.id, args.title, args.serials, args.version, args.author, args.license,
                     args.source_url, args.download_url)
    print(f'Catalogue updated: {args.id}, SHA-256 {digest}. Upload the exact validated ZIP and push the catalogue; no APK rebuild is needed.')


if __name__ == '__main__':
    main()
