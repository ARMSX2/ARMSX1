import unittest
import tempfile
from pathlib import Path
from update_ps1_cheats import catalogue_paths, convert, revision, title_key, ROOT, match_metadata, region_tags

class ConversionTests(unittest.TestCase):
    def test_complete_multi_line_group(self):
        body, count, omitted = convert('cheat0_desc = "Infinite Health"\ncheat0_code = "D0000000+0001+80000002+0064"', 'Example (USA)')
        self.assertEqual((count, omitted), (1, 0))
        self.assertIn('[Infinite Health]\n', body)
        self.assertIn('D0000000 0001\n80000002 0064', body)

    def test_entire_group_is_removed_if_any_line_is_unsupported(self):
        body, count, omitted = convert('cheat0_desc = "Broken"\ncheat0_code = "80000000+0001+B0000000+0002"', 'Example')
        self.assertEqual((count, omitted), (0, 1))
        self.assertNotIn('80000000', body)

    def test_placeholders_odd_pairs_and_dangling_comparisons_are_removed(self):
        for codes in ('80000000+????', '80000000+0001+80000002', 'D0000000+0001', '50000102+0001', '50000102+0001+D0000000+0001+80000000+0002'):
            with self.subTest(codes=codes):
                self.assertEqual(convert(f'cheat0_code = "{codes}"', 'Example')[1:], (0, 1))

    def test_native_retroarch_memory_handlers_are_not_gameshark(self):
        self.assertEqual(convert('cheat0_code = "80000000+0001"\ncheat0_handler = 1', 'Example')[1:], (0, 1))

    def test_names_are_unique_and_byte_bounded(self):
        name = 'é' * 200
        body, count, omitted = convert(f'cheat0_desc = "{name}"\ncheat0_code = "80000000+0001"\ncheat1_desc = "{name}"\ncheat1_code = "80000002+0002"', 'Example')
        names = [line for line in body.splitlines() if line.startswith('[')]
        self.assertEqual((count, omitted), (2, 0))
        self.assertEqual(len(set(names)), 2)
        self.assertTrue(all(len(name.encode('utf-8')) < 192 for name in names))

    def test_title_and_revision_are_separate(self):
        self.assertEqual(title_key('Example (USA) (Rev 1)'), title_key('Example (Europe)'))
        self.assertNotEqual(revision('Example (USA) (Rev 1)'), revision('Example (USA)'))
        self.assertEqual(title_key('Command _ Conquer (USA)'), title_key('Command & Conquer (USA)'))
        self.assertEqual(title_key('Command and Conquer (USA)'), title_key('Command & Conquer (USA)'))

    def test_warped_usa_supplement_has_six_complete_groups(self):
        file = ROOT / 'tools/cheat-supplements/Crash Bandicoot - Warped (USA) (GameShark).cht'
        body, count, omitted = convert(file.read_text(encoding='utf-8'), file.stem)
        self.assertEqual((count, omitted), (6, 0))
        self.assertIn('SCUS-94244', body)
        self.assertIn('[Have All Platinum Relics]', body)
        self.assertNotIn('[Hyper Mode]', body)

    def test_supplements_are_included_without_replacing_upstream(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            upstream = root / 'cht/Sony - PlayStation'
            upstream.mkdir(parents=True)
            supplements = root / 'supplements'
            supplements.mkdir()
            first = upstream / 'Europe.cht'
            second = supplements / 'USA.cht'
            first.touch()
            second.touch()
            self.assertEqual(catalogue_paths(root, supplements), [first, second])
            (supplements / 'Europe.cht').touch()
            with self.assertRaises(ValueError):
                catalogue_paths(root, supplements)

    def test_title_aliases_are_discovery_only_and_region_specific(self):
        usa = ('Top Shop (USA)', 'usa', 'SLUS-01333')
        europe = ('Top Shop (Europe)', 'europe', 'SLES-01333')
        safe, related = match_metadata('Board Game - Top Shop (USA) (GameShark)',
            {title_key('Top Shop'): [usa, europe]}, {'SLUS-01333': {()}, 'SLES-01333': {()}})
        self.assertEqual(safe, set())
        self.assertEqual(related, {'SLUS-01333'})

    def test_trailing_region_is_recognised_without_guessing_revision(self):
        self.assertEqual(region_tags('Gran Turismo 2 USA (v1.0)'), {'usa'})
        row = ('Gran Turismo 2 (USA) (Disc 1)', 'usa', 'SCUS-94455')
        safe, related = match_metadata('Gran Turismo 2 USA (v1.0)',
            {title_key('Gran Turismo 2'): [row]}, {'SCUS-94455': {()}})
        self.assertEqual(safe, set())
        self.assertEqual(related, {'SCUS-94455'})

    def test_demo_codes_are_not_assigned_to_retail_game(self):
        row = ('Example (USA)', 'usa', 'SLUS-00123')
        self.assertEqual(match_metadata('Example (Demo) (USA)',
            {title_key('Example'): [row]}, {'SLUS-00123': {()}}), (set(), set()))

    def test_disc_specific_codes_do_not_match_another_disc(self):
        row = ('Example (USA) (Disc 2)', 'usa', 'SLUS-00123')
        self.assertEqual(match_metadata('Example (USA) (Disc 1)',
            {title_key('Example'): [row]}, {'SLUS-00123': {()}}), (set(), set()))

if __name__ == '__main__':
    unittest.main()
