package dev.psxlab;
import org.junit.Test;
import java.util.*;
import static org.junit.Assert.*;
public class CoverMatcherTest {
    private CoverMatcher catalog(){return new CoverMatcher(Arrays.asList("Crash Bandicoot (USA).png","Crash Bandicoot - Warped (USA).png","Crash Bandicoot 3 - Warped (Europe) (En,Fr,De).png","Crash Bandicoot 2 - Cortex Strikes Back (USA).png","Final Fantasy VII (USA) (Disc 1).png","Dino Crisis (USA).png","Dino Crisis 2 (USA).png","Resident Evil (USA).png","Resident Evil - Director's Cut (USA).png","Resident Evil - Survivor (USA).png"));}
    @Test public void caseRegionLanguagesAndDiscTagsMatch(){assertEquals("Final Fantasy VII (USA) (Disc 1).png",catalog().find("final fantasy 7 (europe) (disc 2).chd").get(0));}
    @Test public void warpedAliasesAndRegionalRanking(){assertEquals("Crash Bandicoot - Warped (USA).png",catalog().find("Crash Bandicoot - Warped (USA).chd").get(0));assertEquals("Crash Bandicoot 3 - Warped (Europe) (En,Fr,De).png",catalog().find("Crash Bandicoot 3 - Warped (Europe).chd").get(0));}
    @Test public void subtitleLookupDoesNotConfuseSequel(){assertEquals("Crash Bandicoot 2 - Cortex Strikes Back (USA).png",catalog().find("Crash Bandicoot 2.chd").get(0));assertEquals("Dino Crisis (USA).png",catalog().find("Dino Crisis.chd").get(0));assertTrue(catalog().find("Dino Crisis 3.chd").isEmpty());}
    @Test public void serialExtractionAcceptsBootPathsAndFileNames(){assertEquals("SCUS-94570",CoverMatcher.serial("\\SCUS_945.70;1"));assertEquals("SLUS-01279",CoverMatcher.serial("Dino [SLUS-01279].chd"));assertNull(CoverMatcher.serial("TEST-01279"));}
    @Test public void punctuationAndArticlesNormalize(){assertEquals(CoverMatcher.normalize("World Is Not Enough, The (USA).png"),CoverMatcher.normalize("The World Is Not Enough.chd"));assertEquals(CoverMatcher.normalize("Rock & Roll.png"),CoverMatcher.normalize("Rock and Roll.chd"));}
}
