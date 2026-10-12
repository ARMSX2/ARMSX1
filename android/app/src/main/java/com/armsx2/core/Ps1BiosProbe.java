// SPDX-License-Identifier: GPL-3.0-or-later
// New PS1-only detector. Recognition metadata adapted from libretro/swanstation
// dd4805619bbbfc6e0d1776372cb16377088b448e, src/core/bios.cpp (GPL-3.0).
// No BIOS image bytes are included. Filenames do not determine console or region.
package com.armsx2.core;

import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

public final class Ps1BiosProbe {
    public static final int IMAGE_SIZE = 512 * 1024;
    private static final Map<String, Info> KNOWN;
    public static final class Info {
        public final int version, region;
        public final String description;
        Info(int version, int region, String description) {
            this.version=version; this.region=region; this.description=description;
        }
        public String zone() {
            switch(region) { case 0: return "Japan"; case 1: return "USA"; case 2: return "Europe"; default: return "Region-free"; }
        }
    }
    static {
        Map<String, Info> known=new HashMap<>();
        known.put("239665b1a3dade1b5a52c06338011044", new Info(0x100, 0, "SCPH-1000, DTL-H1000 (v1.0)"));
        known.put("924e392ed05558ffdb115408c263dccf", new Info(0x214, 1, "SCPH-1001, 5003, DTL-H1201, H3001 (v2.2 12-04-95 A)"));
        known.put("54847e693405ffeb0359c6287434cbef", new Info(0x200, 2, "SCPH-1002, DTL-H1002 (v2.0 05-10-95 E)"));
        known.put("417b34706319da7cf001e76e40136c23", new Info(0x20a, 2, "SCPH-1002, DTL-H1102 (v2.1 07-17-95 E)"));
        known.put("e2110b8a2b97a8e0b857a45d32f7e187", new Info(0x214, 2, "SCPH-1002, DTL-H1202, H3002 (v2.2 12-04-95 E)"));
        known.put("ca5cfc321f916756e3f0effbfaeba13b", new Info(0x214, 0, "DTL-H1100 (v2.2 03-06-96 D)"));
        known.put("849515939161e62f6b866f6853006780", new Info(0x10a, 0, "SCPH-3000, DTL-H1000H (v1.1 01-22-95)"));
        known.put("dc2b9bf8da62ec93e868cfd29f0d067d", new Info(0x200, 1, "SCPH-1001, DTL-H1001 (v2.0 05-07-95 A)"));
        known.put("cba733ceeff5aef5c32254f1d617fa62", new Info(0x20a, 0, "SCPH-3500 (v2.1 07-17-95 J)"));
        known.put("da27e8b6dab242d8f91a9b25d80c63b8", new Info(0x20a, 1, "SCPH-1001, DTL-H1101 (v2.1 07-17-95 A)"));
        known.put("57a06303dfa9cf9351222dfcbb4a29d9", new Info(0x214, 0, "SCPH-5000, DTL-H1200, H3000 (v2.2 12-04-95 J)"));
        known.put("8dd7d5296a650fac7319bce665a6a53c", new Info(0x300, 0, "SCPH-5500 (v3.0 09-09-96 J)"));
        known.put("490f666e1afb15b7362b406ed1cea246", new Info(0x300, 1, "SCPH-5501, 5503, 7003 (v3.0 11-18-96 A)"));
        known.put("32736f17079d0b2b7024407c39bd3050", new Info(0x300, 2, "SCPH-5502, 5552 (v3.0 01-06-97 E)"));
        known.put("8e4c14f567745eff2f0408c8129f72a6", new Info(0x400, 0, "SCPH-7000, 7500, 9000 (v4.0 08-18-97 J)"));
        known.put("b84be139db3ee6cbd075630aa20a6553", new Info(0x40a, 0, "SCPH-7000W (v4.1 11-14-97 A)"));
        known.put("1e68c231d0896b7eadcad1d7d8e76129", new Info(0x40a, 1, "SCPH-7001, 7501, 7503, 9001, 9003, 9903 (v4.1 12-16-97 A)"));
        known.put("b9d9a0286c33dc6b7237bb13cd46fdee", new Info(0x40a, 2, "SCPH-7002, 7502, 9002 (v4.1 12-16-97 E)"));
        known.put("8abc1b549a4a80954addc48ef02c4521", new Info(0x41e, 0, "SCPH-100 (v4.3 03-11-00 J)"));
        known.put("9a09ab7e49b422c007e6d54d7c49b965", new Info(0x428, 1, "SCPH-101 (v4.4 03-24-00 A)"));
        known.put("6e3735ff4c7dc899ee98981385f6f3d0", new Info(0x432, 1, "SCPH-101 (v4.5 05-25-00 A)"));
        known.put("b10f5e0e3d9eb60e5159690680b1e774", new Info(0x428, 2, "SCPH-102 (v4.4 03-24-00 E)"));
        known.put("de93caec13d1a141a40a79f5c86168d6", new Info(0x432, 2, "SCPH-102 (v4.5 05-25-00 E)"));
        known.put("c53ca5908936d412331790f4426c6c33", new Info(0x432, 10, "PSP, SCPH-1000R (v4.5 05-25-00 J)"));
        known.put("476d68a94ccec3b9c8303bbd1daf2810", new Info(0x432, 0, "SCPH-1000R (v4.5 05-25-00 J)"));
        KNOWN=Collections.unmodifiableMap(known);
    }
    private Ps1BiosProbe() {}
    /** SAF providers may not report size; unknown-sized files still get bounded validation. */
    public static boolean isCandidateSize(long size) { return size <= 0 || size == IMAGE_SIZE; }
    /** Does not take ownership of the stream. Reads no more than one PS1 image plus one byte. */
    public static Info probe(InputStream stream) throws IOException {
        byte[] image=new byte[IMAGE_SIZE];
        int offset=0;
        while(offset<image.length) {
            int read=stream.read(image,offset,image.length-offset);
            if(read<0)return null;
            if(read==0) {
                int one=stream.read();if(one<0)return null;image[offset++]=(byte)one;
            } else offset+=read;
        }
        if(stream.read()!=-1)return null;
        if(matchesOpenBios(image))return new Info(0,10,"OpenBIOS (PS1)");
        try { return knownHash(hex(MessageDigest.getInstance("MD5").digest(image))); }
        catch(NoSuchAlgorithmException impossible) {throw new IllegalStateException("Android/JVM must provide MD5",impossible);}
    }
    static Info knownHash(String hash) { return KNOWN.get(hash); }
    private static boolean matchesOpenBios(byte[] image) {
        byte[] magic={'O','p','e','n','B','I','O','S'};
        for(int i=0;i<magic.length;i++)if(image[0x78+i]!=magic[i])return false;
        return true;
    }
    private static String hex(byte[] bytes) {
        char[] digits="0123456789abcdef".toCharArray(), result=new char[bytes.length*2];
        for(int i=0;i<bytes.length;i++){int value=bytes[i]&255;result[i*2]=digits[value>>>4];result[i*2+1]=digits[value&15];}
        return new String(result);
    }
}
