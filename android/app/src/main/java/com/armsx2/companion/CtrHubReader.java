package com.armsx2.companion;

/** USA Adventure hubs: original map textures + UI_Map_GetIconPos transform.
 * References: CTR-ModSDK namespace_Level.h and UI_10_Map_GetIconPos.c.
 * No memory writes or map/ROM bytes included. */
public final class CtrHubReader {
    private static final String[] HUBS={"Gemstone Valley","N. Sanity Beach","The Lost Ruins","Glacier Park","Citadel City"};
    public static final class Hub {
        public final int level,character,width,height,topWidth,topHeight,bottomWidth,bottomHeight;
        public final float x,y;
        public final boolean showTop;
        public final String name,identity;
        Hub(int level,int character,int tw,int th,int bw,int bh,boolean showTop,float sx,float sy,String identity) {
            this.level=level;this.character=character;this.name=HUBS[level-25];this.identity=identity;
            this.topWidth=tw;this.topHeight=th;this.bottomWidth=bw;this.bottomHeight=bh;
            this.showTop=showTop;width=Math.max(showTop?tw:0,bw);height=(showTop?th:0)+bh;
            x=(sx-(500-width))/width;y=(sy-(195-height))/height;
        }
    }
    public static Hub read(String serial,byte[] ram) {
        if(!CtrRaceReader.supported(serial)||ram==null||ram.length<2*1024*1024)return null;
        try {
            Memory m=new Memory(ram);int gt=m.ptr(0x8d2ac,0x2534);
            if(gt!=0x96b20)return null;
            int mode=m.i32(gt);
            if((mode&0x100000)==0 || (mode&(0x20|0x2000|0x20000000|0x40000000))!=0)return null;
            int level=m.i32(gt+0x1a10);
            if(level<25||level>29||m.u8(gt+0x1ca8)!=1)return null;
            int lev=m.ptr(gt+0x160,0x138),spawn=m.ptr(lev+0x134,8);
            int count=m.i32(spawn);if(count<1||count>16)return null;
            int map=m.ptr(spawn+4,20);
            int rangeX=m.i16(map)-m.i16(map+4),rangeZ=m.i16(map+2)-m.i16(map+6);
            int sizeX=m.i16(map+8),sizeY=m.i16(map+10),rotation=m.i16(map+16),half=m.i16(map+18);
            if(rangeX==0||rangeZ==0||sizeX<1||sizeX>512||sizeY<1||sizeY>256||rotation<0||rotation>3||half<0||half>1)return null;
            int player=m.ptr(gt+0x24ec,0x638),character=m.i16(0x86e84);
            if(m.u8(player+0x4a)!=0||character<0||character>15)return null;
            int wx=m.i32(player+0x2d4)>>8,wz=m.i32(player+0x2dc)>>8;
            if(Math.abs((long)wx)>65536||Math.abs((long)wz)>65536)return null;
            // Same integer projection as retail, without changing the player's coordinates.
            long addX,addY;
            if(rotation==0){addX=(long)wx*sizeX/rangeX;addY=(long)wz*sizeY*2/rangeZ;}
            else if(rotation==1){addX=-(long)wz*sizeX/rangeZ;addY=(long)wx*sizeY*2/rangeX;}
            else if(rotation==2){addX=-(long)wx*sizeX/rangeX;addY=-(long)wz*sizeY*2/rangeZ;}
            else {addX=(long)wz*sizeX/rangeZ;addY=-(long)wx*sizeY*2/rangeX;}
            int topRaw=m.i32(gt+0x1eec+3*4),bottomRaw=m.i32(gt+0x1eec+4*4);
            int bottom=m.address(bottomRaw,32),bw=m.u8(bottom+0x18)-m.u8(bottom+0x14),bh=m.u8(bottom+0x1d)-m.u8(bottom+0x15);
            int tw=0,th=0;
            if(half==0){int top=m.address(topRaw,32);tw=m.u8(top+0x18)-m.u8(top+0x14);th=m.u8(top+0x1d)-m.u8(top+0x15);}
            if(bw<1||bh<1||bw>255||bh>255||(half==0&&(tw<1||th<1||tw>255||th>255)))return null;
            float sx=m.i16(map+12)+addX,sy=m.i16(map+14)+addY-16;
            String identity=level+":"+lev+":"+map+":"+topRaw+":"+bottomRaw;
            return new Hub(level,character,tw,th,bw,bh,half==0,sx,sy,identity);
        } catch(IllegalArgumentException invalid){return null;}
    }
    private static final class Memory {
        final byte[] b;Memory(byte[] b){this.b=b;}
        int address(int p,int size){int r=p>>>24,o=p&0x1fffffff;
            if((r!=0x80&&r!=0xa0)||(o&3)!=0||o>2*1024*1024-size)throw new IllegalArgumentException();return o;}
        int ptr(int p,int size){return address(i32(p),size);}
        int u8(int p){if(p<0||p>=2*1024*1024)throw new IllegalArgumentException();return b[p]&255;}
        int i16(int p){return (short)(u8(p)|(u8(p+1)<<8));}
        int i32(int p){return u8(p)|(u8(p+1)<<8)|(u8(p+2)<<16)|(u8(p+3)<<24);}
    }
}
