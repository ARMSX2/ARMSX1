import unittest
from update_ps1_cheats import convert, revision, title_key

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

if __name__ == '__main__':
    unittest.main()
