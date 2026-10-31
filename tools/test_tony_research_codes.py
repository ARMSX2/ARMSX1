"""Regression checks for experimental profiles. Actual effects need device tests."""
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[1]
FILE=ROOT/'tools/cheat-research/Tony Hawk USA - experimental.cht'
PROFILES=(0x801F2638,0x801EE6CC,0x801F2694,0x801EE728)


def groups():
    text=FILE.read_text(encoding='utf-8')
    return {name: [(int(address,16),int(value,16)) for address,value in
                   re.findall(r'^([0-9A-F]{8}) ([0-9A-F]{4})$',body,re.M)]
            for name,body in re.findall(r'^\[([^]]+)\]\n(.*?)(?=^\[|\Z)',text,re.M|re.S)}


def model_writes(codes,pointer):
    """Each authored write has two consecutive equality guards, not a global guard."""
    result={}
    for first,second,write in zip(codes[::3],codes[1::3],codes[2::3]):
        if first[1]==pointer&65535 and second[1]==pointer>>16:
            result[write[0]&0xffffff]=write[1]
    return result


class TonyResearchCodesTests(unittest.TestCase):
    def test_every_write_has_full_observed_player_pointer_guard(self):
        for name,codes in groups().items():
            self.assertEqual(0,len(codes)%3,name)
            for first,second,write in zip(codes[::3],codes[1::3],codes[2::3]):
                self.assertEqual(0xD00D17C8,first[0])
                self.assertEqual(0xD00D17CA,second[0])
                self.assertIn((second[1]<<16)|first[1],PROFILES)
                self.assertEqual(0x80,write[0]>>24)

    def test_career_and_single_session_allocations_write_only_their_own_fields(self):
        codes=groups()
        for pointer in PROFILES:
            base=pointer&0x1fffff
            score=model_writes(codes['Score 999,999 (experimental)'],pointer)
            self.assertEqual({base+0x928:999999&65535,base+0x92a:999999>>16},score)
            special=model_writes(codes['Full special meter (experimental)'],pointer)
            self.assertEqual({base+0x250:750,base+0x252:0,base+0x254:750,base+0x256:0},special)

    def test_single_session_profiles_are_not_omitted_by_career_only_guard(self):
        for pointer in (0x801F2694,0x801EE728):
            for codes in groups().values():
                self.assertTrue(model_writes(codes,pointer))
                self.assertEqual({},model_writes(codes,pointer^0x10000))

    def test_no_player_or_unknown_allocation_has_no_writes(self):
        for pointer in (0,0x801F2639,0x801E2638,0x801FE6CC,0x801D0000):
            for codes in groups().values():self.assertEqual({},model_writes(codes,pointer))

    def test_research_file_is_not_in_bundled_supplement_inputs(self):
        from update_ps1_cheats import catalogue_paths
        self.assertNotIn(FILE,catalogue_paths(ROOT/'build/libretro-cheat-source'))
        self.assertEqual(2,len(groups()))


if __name__=='__main__':unittest.main()
