# Eternal Eyes: unused PBP block-table entries

The Eternal Eyes report from 0.3.21 on Samsung SM-A556E ends at frame zero with `Invalid or missing PBP data block 10118`. The exception is thrown while opening the PBP index, before a VM or audio stream starts. It is separate from the Mizzurna Falls guest memory-access failure.

The reader required every block covered by the TOC to be stored. Some PBP conversions have empty entries in the declared disc tail. An unrequested empty entry therefore rejected an otherwise readable boot image. The reference PBP reader records empty entries and fails when a missing block is requested: <https://github.com/stenzek/duckstation/blob/master/src/util/cd_image_pbp.cpp>.

PSX Lab now permits unpopulated noninitial index entries while mounting. It retains the declared TOC/track positions and still rejects an empty first block, nonempty blocks outside the file, malformed/encrypted containers and invalid compression. Reading an absent block raises an explicit block/sector-range error; the cache is invalidated before that check and never returns earlier decoded audio/data. No missing sectors are silently filled, and no disc length is guessed from the index.

An original fixture with a valid boot executable and an empty third block fails to mount against the old code and mounts/boots after the change. Reading that absent block still fails, followed by a successful reread of a populated block. `CueLaunchIntegrationTest` also exercises the actual adapter mount path with a generated PBP having an unused empty tail. All 422 core tests pass. The reporting user's actual Eternal Eyes PBP is not on Thor; complete-game verification of that image remains necessary.
