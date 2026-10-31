import struct
import unittest
from cheat_memory_research import exact_candidates


class MemoryResearchTests(unittest.TestCase):
    def test_repeated_observations_narrow_score_candidates(self):
        first=struct.pack('<4I',0,0,0,999)
        second=struct.pack('<4I',100,100,1,999)
        third=struct.pack('<4I',175,101,2,999)
        self.assertEqual([0,4],exact_candidates([(first,0),(second,100)]))
        self.assertEqual([0],exact_candidates([(first,0),(second,100),(third,175)]))

    def test_width_and_endian_are_explicit(self):
        self.assertEqual([2],exact_candidates([(bytes([0,0,0x34,0x12]),0x1234)],16))
        self.assertEqual([2],exact_candidates([(bytes([0,0,0x34,0x12]),0x34)],8))

    def test_mismatched_or_missing_snapshots_are_rejected(self):
        for samples in ([],[(b'1234',0),(b'12',0)]):
            with self.assertRaises(ValueError):exact_candidates(samples)
