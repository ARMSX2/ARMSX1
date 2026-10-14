package com.armsx2.companion;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Original HUD pictures from the user's live USA game. No bundled art or disk writes.
 * Layout references: CTR-ModSDK namespace_Main.h (ptrIcons), namespace_Decal.h
 * (Icon/TextureLayout), UI_DrawRankedDrivers (character-to-icon mapping).
 */
public final class CtrPortraitReader {
    public static final int RAM_BYTES = 2 * 1024 * 1024;
    public static final int VRAM_BYTES = 1024 * 512 * 2;
    public static final int WUMPA = 3000;
    public static final int HUB_TOP=4000,HUB_BOTTOM=4001;
    public static int glyphKey(char glyph, boolean small) { return (small ? 2000 : 1000) + glyph; }
    private static final String HUD_GLYPHS = "0123456789:./TIME LAPX";
    private static final int[] ICON_IDS = {0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
        0x2b,0x29,0x28,0x2a,0x2c,0x35,0x36,0x37};
    private static final String[] ICON_NAMES = {"crash","cortex","tiny","coco","ngin","dingo","polar","pura",
        "pinstripe","papu","roo","komodojoe","ntropy","penguin","fakecrash","oxide"};
    public static final class Portrait {
        public final int character, width, height;
        public final int[] pixels;
        Portrait(int character, int width, int height, int[] pixels) {
            this.character=character; this.width=width; this.height=height; this.pixels=pixels;
        }
    }
    public static List<Portrait> read(String serial, byte[] snapshot) {
        List<Portrait> result=new ArrayList<>();
        if (!CtrRaceReader.supported(serial) || snapshot==null || snapshot.length!=RAM_BYTES+VRAM_BYTES)
            return result;
        try {
            int gt=address(i32(snapshot,0x8d2ac),0x2534);
            int mode=i32(snapshot,gt);
            if ((mode & (0x20|0x2000|0x20000000|0x40000000))!=0) return result;
            int track=i32(snapshot,gt+0x1a10);
            boolean hub=(mode&0x100000)!=0 && track>=25 && track<=29;
            if (!hub && ((mode&0x100000)!=0 || track<0 || track>17)) return result;
            // 0..15 are characters; 100+icon ID are original inventory pictures.
            for (int slot=0;slot<ICON_IDS.length+0x2f;slot++) {
                try {
                    boolean characterSlot=slot<ICON_IDS.length;
                    int iconId=characterSlot?ICON_IDS[slot]:5+slot-ICON_IDS.length;
                    if(!characterSlot && iconId>0x1f && iconId!=0x32)continue;
                    int character=characterSlot?slot:100+iconId;
                    int raw=i32(snapshot,gt+0x1eec+iconId*4);
                    if(raw==0)continue;
                    int icon=address(raw,0x20);
                    StringBuilder name=new StringBuilder();
                    for(int i=0;i<16 && u8(snapshot,icon+i)!=0;i++)name.append((char)u8(snapshot,icon+i));
                    if(characterSlot ? !name.toString().toLowerCase(Locale.ROOT).equals(ICON_NAMES[character]) :
                        name.length()==0)continue;
                    add(result, decode(snapshot,icon,character));
                } catch(IllegalArgumentException invalidIcon) { /* Wait/retry this slot; never guess. */ }
            }
            if(hub)for(int part=0;part<2;part++) {
                try {
                    int icon=address(i32(snapshot,gt+0x1eec+(3+part)*4),32);
                    StringBuilder name=new StringBuilder();
                    for(int i=0;i<16&&u8(snapshot,icon+i)!=0;i++)name.append((char)u8(snapshot,icon+i));
                    if(name.toString().toLowerCase(Locale.ROOT).startsWith("map"))
                        add(result,decode(snapshot,icon,HUB_TOP+part));
                } catch(IllegalArgumentException unavailableMap) { }
            }
            // Retail font mapping and group IDs, not hard-coded glyph texture coordinates.
            // The same glyph shapes draw TIME, LAP and their live numeric values in CTR.
            for (int font=1;font<=2;font++) {
                try {
                    int groupId=u16(snapshot,0x82358+font*2);
                    if(groupId!=(font==1?4:5))continue;
                    int group=group(snapshot,gt,groupId,font==1?"largefont":"middlefont");
                    int count=u16(snapshot,group+0x12);
                    for(char glyph:HUD_GLYPHS.toCharArray()) {
                        if(glyph==' ')continue;
                        try {
                            int id=u8(snapshot,0x82378+glyph-0x21);
                            if(id>=count)continue;
                            int icon=address(i32(snapshot,group+0x14+id*4),0x20);
                            add(result,decode(snapshot,icon,glyphKey(glyph,font==2)));
                        } catch(IllegalArgumentException unavailableGlyph) { }
                    }
                } catch(IllegalArgumentException unavailableFont) { }
            }
            try {
                int group=group(snapshot,gt,0xb,"wumpaposter");
                int icon=address(i32(snapshot,group+0x14),0x20);
                add(result,decode(snapshot,icon,WUMPA));
            } catch(IllegalArgumentException unavailableFruit) { }
        } catch(IllegalArgumentException invalidGame) { return new ArrayList<>(); }
        return result;
    }
    private static void add(List<Portrait> list,Portrait image){if(image!=null)list.add(image);}
    private static int group(byte[] b,int gt,int id,String expected) {
        int p=address(i32(b,gt+0x2114+id*4),0x14);
        StringBuilder name=new StringBuilder();
        for(int i=0;i<16 && u8(b,p+i)!=0;i++)name.append((char)u8(b,p+i));
        int count=u16(b,p+0x12);
        if(!expected.equalsIgnoreCase(name.toString()) || count<1 || count>256)
            throw new IllegalArgumentException();
        address(i32(b,gt+0x2114+id*4),0x14+count*4);
        return p;
    }
    private static Portrait decode(byte[] snapshot,int icon,int key) {
        int u=u8(snapshot,icon+0x14),v=u8(snapshot,icon+0x15);
        int width=u8(snapshot,icon+0x18)-u,height=u8(snapshot,icon+0x1d)-v;
        int clut=u16(snapshot,icon+0x16),page=u16(snapshot,icon+0x1a),depth=(page>>7)&3;
        int limit=key==HUB_TOP||key==HUB_BOTTOM?255:128;
        if(width<1 || height<1 || width>limit || height>limit || depth==3)return null;
        int[] pixels=new int[width*height];boolean visible=false;
        for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
            int tx=u+x,ty=v+y;
            int packed=word(snapshot,((page&15)*64)+(tx>>(2-depth)),(((page>>4)&1)*256)+ty);
            int color=packed;
            if(depth!=2) {
                int index=depth==0?((packed>>((tx&3)*4))&15):((packed>>((tx&1)*8))&255);
                color=word(snapshot,(clut&63)*16+index,(clut>>6)&511);
            }
            int pixel=color==0?0:0xff000000|expand(color&31)<<16|expand((color>>5)&31)<<8|expand((color>>10)&31);
            pixels[y*width+x]=pixel;visible|=pixel!=0;
        }
        return visible?new Portrait(key,width,height,pixels):null;
    }
    private static int expand(int value) { return (value<<3)|(value>>2); }
    private static int word(byte[] bytes,int x,int y) {
        if(x<0 || x>=1024 || y<0 || y>=512)throw new IllegalArgumentException();
        return u16(bytes,RAM_BYTES+(y*1024+x)*2);
    }
    private static int address(int raw,int length) {
        int region=raw>>>24,offset=raw&0x1fffffff;
        if((region!=0x80 && region!=0xa0) || (offset&3)!=0 || offset>RAM_BYTES-length)
            throw new IllegalArgumentException();
        return offset;
    }
    private static int u8(byte[] bytes,int offset) {
        if(offset<0 || offset>=bytes.length)throw new IllegalArgumentException();
        return bytes[offset]&255;
    }
    private static int u16(byte[] bytes,int offset) { return u8(bytes,offset)|(u8(bytes,offset+1)<<8); }
    private static int i32(byte[] bytes,int offset) { return u16(bytes,offset)|(u16(bytes,offset+2)<<16); }
}
