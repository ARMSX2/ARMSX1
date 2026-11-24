"""Extract the bounded graphics attachment from an ArmSX1 text report."""
import base64
import hashlib
import re
import sys
from pathlib import Path

source, destination = map(Path, sys.argv[1:])
if source.stat().st_size > 20 * 1024 * 1024:
    raise SystemExit('Report exceeds the 20 MiB extraction limit')
text = source.read_text(encoding='utf-8', errors='replace')
begin = '-----BEGIN ARMSX1 GRAPHICS REPLAY-----'
end = '-----END ARMSX1 GRAPHICS REPLAY-----'
if text.count(begin) != 1 or text.count(end) != 1:
    raise SystemExit('Report must contain exactly one graphics replay attachment')
header, payload = text.split(begin, 1)
payload = ''.join(payload.split(end, 1)[0].split())
if len(payload) > 12 * 1024 * 1024:
    raise SystemExit('Graphics attachment exceeds size limit')
data = base64.b64decode(payload, validate=True)
matches = re.findall(r'Format: ArmSX1 graphics replay v[123], gzip/base64; bytes=(\d+); sha256=([0-9a-f]{64})', header)
if not matches:
    raise SystemExit('Missing graphics attachment checksum')
size, digest = matches[-1]
if len(data) != int(size) or hashlib.sha256(data).hexdigest() != digest:
    raise SystemExit('Graphics attachment checksum mismatch')
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_bytes(data)
print(f'Extracted {len(data)} bytes to {destination}')
