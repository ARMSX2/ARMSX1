// SPDX-License-Identifier: GPL-3.0-or-later
package com.armsx2.core;
import org.junit.Test;
import java.io.ByteArrayInputStream;
import java.io.InputStream;
import static org.junit.Assert.*;
public class Ps1BiosProbeTest {
    @Test public void rejectsOtherConsoleSizes() {
        assertFalse(Ps1BiosProbe.isCandidateSize(16*1024));
        assertFalse(Ps1BiosProbe.isCandidateSize(256*1024));
        assertFalse(Ps1BiosProbe.isCandidateSize(4*1024*1024));
        assertTrue(Ps1BiosProbe.isCandidateSize(512*1024));
        assertTrue(Ps1BiosProbe.isCandidateSize(-1));
        assertTrue(Ps1BiosProbe.isCandidateSize(0));
    }
    @Test public void rejectsShortAndOversizedStreams() throws Exception {
        assertNull(Ps1BiosProbe.probe(new ByteArrayInputStream(new byte[0])));
        assertNull(Ps1BiosProbe.probe(new ByteArrayInputStream(new byte[Ps1BiosProbe.IMAGE_SIZE-1])));
        assertNull(Ps1BiosProbe.probe(new ByteArrayInputStream(new byte[Ps1BiosProbe.IMAGE_SIZE+1])));
    }
    @Test public void sceMarkerDoesNotMakeRandomDataABios() throws Exception {
        byte[] junk=new byte[Ps1BiosProbe.IMAGE_SIZE];
        System.arraycopy(new byte[]{'S','C','E','A'},0,junk,100,4);
        assertNull(Ps1BiosProbe.probe(new ByteArrayInputStream(junk)));
    }
    @Test public void regionComesFromVerifiedIdentity() {
        assertEquals(0,Ps1BiosProbe.knownHash("8dd7d5296a650fac7319bce665a6a53c").region);
        assertEquals(1,Ps1BiosProbe.knownHash("490f666e1afb15b7362b406ed1cea246").region);
        assertEquals(2,Ps1BiosProbe.knownHash("32736f17079d0b2b7024407c39bd3050").region);
    }
    @Test public void excludesPs3AndUnknownHashes() {
        assertNull(Ps1BiosProbe.knownHash("c02a6fbb1b27359f84e92fae8bc21316"));
        assertNull(Ps1BiosProbe.knownHash("81bbe60ba7a3d1cea1d48c14cbcc647b"));
        assertNull(Ps1BiosProbe.knownHash("00000000000000000000000000000000"));
    }
    @Test public void recognizesCoreOpenBiosConvention() throws Exception {
        byte[] image=new byte[Ps1BiosProbe.IMAGE_SIZE];
        System.arraycopy(new byte[]{'O','p','e','n','B','I','O','S'},0,image,0x78,8);
        assertEquals("OpenBIOS (PS1)",Ps1BiosProbe.probe(new ByteArrayInputStream(image)).description);
        image[0x78]='X';
        assertNull(Ps1BiosProbe.probe(new ByteArrayInputStream(image)));
    }
    @Test public void neverReadsUnboundedUnknownSizeStream() throws Exception {
        final int[] reads={0};
        InputStream endless=new InputStream() {
            @Override public int read(){reads[0]++;return 0;}
            @Override public int read(byte[] out,int offset,int length) {
                java.util.Arrays.fill(out,offset,offset+length,(byte)0);reads[0]+=length;return length;
            }
        };
        assertNull(Ps1BiosProbe.probe(endless));
        assertEquals(Ps1BiosProbe.IMAGE_SIZE+1,reads[0]);
    }
    @Test public void handlesProviderReturningZeroLengthRead() throws Exception {
        InputStream provider=new ByteArrayInputStream(new byte[Ps1BiosProbe.IMAGE_SIZE]){
            boolean zero=true;
            @Override public synchronized int read(byte[] out,int offset,int length) {
                if(zero){zero=false;return 0;}return super.read(out,offset,length);
            }
        };
        assertNull(Ps1BiosProbe.probe(provider));
    }
}
