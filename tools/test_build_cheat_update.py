import hashlib
import io
from pathlib import Path
import tempfile
import unittest
import zipfile
from build_cheat_update import FILES, ROOT, build, package_bytes


class CheatUpdatePublishingTests(unittest.TestCase):
    def test_real_package_is_reproducible_and_contains_only_licensed_database_assets(self):
        assets = ROOT / 'android/app/src/main/assets'
        data = package_bytes(assets)
        self.assertEqual(data, package_bytes(assets))
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            self.assertEqual(set(archive.namelist()), set(FILES) | {'format-version.txt', 'hashes.tsv'})
            self.assertEqual(archive.read('format-version.txt'), b'1\n')
            hashes = dict(line.split('\t') for line in archive.read('hashes.tsv').decode().splitlines())
            for name in FILES:
                self.assertEqual(archive.read(name), (assets / name).read_bytes())
                self.assertEqual(hashes[name], hashlib.sha256(archive.read(name)).hexdigest())
            with zipfile.ZipFile(io.BytesIO(archive.read('ps1-cheats.zip'))) as lists:
                self.assertNotIn('d35596600326aec0596d.cht', lists.namelist())
                self.assertFalse(any(b'experimental' in lists.read(name) for name in lists.namelist()))

    def test_generated_repository_package_is_current(self):
        self.assertEqual((ROOT / 'cheats/ps1-cheats-update.zip').read_bytes(),
                         package_bytes(ROOT / 'android/app/src/main/assets'))

    def test_missing_assets_do_not_replace_previous_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / 'published.zip'
            output.write_bytes(b'previous')
            with self.assertRaises(FileNotFoundError):
                build(root, output)
            self.assertEqual(output.read_bytes(), b'previous')


if __name__ == '__main__':
    unittest.main()
