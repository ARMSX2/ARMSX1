package dev.psxlab.core;

/** Original bounded software rasterizer. Integer edge coverage with affine interpolation.
 * Subpixel gradient precision, texture cache quirks and interlaced drawing remain approximate. */
final class SoftwareRasterizer {
    private final int[] vram;
    private final boolean nativeExecution;
    long primitives,pixels;
    SoftwareRasterizer(int[] vram){this(vram,true);}
    SoftwareRasterizer(int[] vram,boolean nativeExecution){this.vram=vram;this.nativeExecution=nativeExecution && NativeRasterizer.AVAILABLE;}
    static int signed11(int value){return (value<<21)>>21;}
    private static final int[] DITHER={-4,0,-3,1,2,-2,3,-1,-3,1,-4,0,3,-1,2,-2};
    private static final class Vertex {
        int x,y,r,g,b,u,v;
        Vertex(int xy,int color,int uv,int offset){x=signed11(xy)+signed11(offset);y=signed11(xy>>>16)+signed11(offset>>>11);r=color&255;g=(color>>>8)&255;b=(color>>>16)&255;u=uv&255;v=(uv>>>8)&255;}
    }
    void draw(int[] packet,int[] env){
        int op=packet[0]>>>24;primitives++;
        if(nativeExecution){pixels+=NativeRasterizer.draw(vram,packet,env);return;}
        if(op<0x40)polygon(packet,env);
        else if(op<0x60)line(packet,env);
        else rectangle(packet,env);
    }
    void drawBatch(int[] commands,int count){
        if(nativeExecution){primitives+=count;pixels+=NativeRasterizer.drawBatch(vram,commands);return;}
        for(int at=0;at<commands.length;){int n=commands[at++];int[] p=java.util.Arrays.copyOfRange(commands,at,at+n);at+=n;int[] state=java.util.Arrays.copyOfRange(commands,at,at+7);at+=7;draw(p,state);}
    }
    private void polygon(int[] p,int[] e){
        int op=p[0]>>>24,n=(op&8)!=0?4:3,at=1,color=p[0],clut=0;
        boolean shaded=(op&16)!=0,textured=(op&4)!=0;
        Vertex[] vertices=new Vertex[n];
        for(int i=0;i<n;i++){
            if(i>0 && shaded)color=p[at++];int xy=p[at++],uv=textured?p[at++]:0;
            if(textured && i==0)clut=uv>>>16;
            vertices[i]=new Vertex(xy,color,uv,e[5]);
        }
        triangle(vertices[0],vertices[1],vertices[2],op,clut,e);
        if(n==4)triangle(vertices[1],vertices[2],vertices[3],op,clut,e);
    }
    private static long edge(Vertex a,Vertex b,int x,int y){return (long)(b.x-a.x)*(y-a.y)-(long)(b.y-a.y)*(x-a.x);}
    private static boolean topLeft(Vertex a,Vertex b){return b.y<a.y || b.y==a.y && b.x>a.x;}
    private static int gradientX(Vertex a,Vertex b,Vertex c,int va,int vb,int vc,long area){return (int)((((long)(vb-va)*(c.y-a.y)-(long)(vc-va)*(b.y-a.y))<<16)/area);}
    private static int gradientY(Vertex a,Vertex b,Vertex c,int va,int vb,int vc,long area){return (int)((((long)(b.x-a.x)*(vc-va)-(long)(c.x-a.x)*(vb-va))<<16)/area);}
    // Polygon attributes begin at half an attribute unit; sprites keep exact UVs.
    private static int start(long wa,long wb,long wc,int a,int b,int c,long area){return (int)(((wa*a+wb*b+wc*c)<<16)/area)+32768;}
    private void triangle(Vertex a,Vertex b,Vertex c,int op,int clut,int[] e){
        int minX=Math.min(a.x,Math.min(b.x,c.x)),maxX=Math.max(a.x,Math.max(b.x,c.x));
        int minY=Math.min(a.y,Math.min(b.y,c.y)),maxY=Math.max(a.y,Math.max(b.y,c.y));
        if(maxX-minX>=1024 || maxY-minY>=512)return;
        long area=edge(a,b,c.x,c.y);if(area==0)return;
        if(area<0){Vertex swap=b;b=c;c=swap;area=-area;}
        minX=Math.max(minX,e[3]&1023);maxX=Math.min(maxX-1,e[4]&1023);
        minY=Math.max(minY,(e[3]>>>10)&511);maxY=Math.min(maxY-1,(e[4]>>>10)&511);
        boolean ta=topLeft(b,c),tb=topLeft(c,a),tc=topLeft(a,b);
        boolean texture=(op&4)!=0,shaded=(op&16)!=0;
        boolean dither=(e[1]&512)!=0 && (shaded || texture && (op&1)==0);
        long aw=edge(b,c,minX,minY),bw=edge(c,a,minX,minY),cw=edge(a,b,minX,minY);
        int rx=shaded?gradientX(a,b,c,a.r,b.r,c.r,area):0,ry=shaded?gradientY(a,b,c,a.r,b.r,c.r,area):0;
        int gx=shaded?gradientX(a,b,c,a.g,b.g,c.g,area):0,gy=shaded?gradientY(a,b,c,a.g,b.g,c.g,area):0;
        int bx=shaded?gradientX(a,b,c,a.b,b.b,c.b,area):0,by=shaded?gradientY(a,b,c,a.b,b.b,c.b,area):0;
        int ux=texture?gradientX(a,b,c,a.u,b.u,c.u,area):0,uy=texture?gradientY(a,b,c,a.u,b.u,c.u,area):0;
        int vx=texture?gradientX(a,b,c,a.v,b.v,c.v,area):0,vy=texture?gradientY(a,b,c,a.v,b.v,c.v,area):0;
        int rRow=shaded?start(aw,bw,cw,a.r,b.r,c.r,area):a.r<<16,gRow=shaded?start(aw,bw,cw,a.g,b.g,c.g,area):a.g<<16,bRow=shaded?start(aw,bw,cw,a.b,b.b,c.b,area):a.b<<16;
        int uRow=texture?start(aw,bw,cw,a.u,b.u,c.u,area):0,vRow=texture?start(aw,bw,cw,a.v,b.v,c.v,area):0;
        for(int y=minY;y<=maxY;y++,rRow+=ry,gRow+=gy,bRow+=by,uRow+=uy,vRow+=vy){
            long wa=edge(b,c,minX,y),wb=edge(c,a,minX,y),wc=edge(a,b,minX,y);
            int r=rRow,g=gRow,blue=bRow,u=uRow,v=vRow;
            for(int x=minX;x<=maxX;x++,wa-=c.y-b.y,wb-=a.y-c.y,wc-=b.y-a.y,r+=rx,g+=gx,blue+=bx,u+=ux,v+=vx){
                if((wa>0 || wa==0 && ta) && (wb>0 || wb==0 && tb) && (wc>0 || wc==0 && tc))pixel(x,y,r>>16,g>>16,blue>>16,u>>16,v>>16,op,clut,e,dither);
            }
        }
    }

    private void rectangle(int[] p,int[] e){
        int op=p[0]>>>24,size=(op>>>3)&3;boolean textured=(op&4)!=0;
        int uv=textured?p[2]:0,wh=size==0?p[textured?3:2]:0;
        int w=size==0?wh&1023:size==1?1:size==2?8:16,h=size==0?(wh>>>16)&511:size==1?1:size==2?8:16;
        Vertex a=new Vertex(p[1],p[0],uv,e[5]);
        int minX=Math.max(a.x,e[3]&1023),maxX=Math.min(a.x+w-1,e[4]&1023),minY=Math.max(a.y,(e[3]>>>10)&511),maxY=Math.min(a.y+h-1,(e[4]>>>10)&511);
        int dx=(e[1]&4096)!=0?-1:1,dy=(e[1]&8192)!=0?-1:1;
        for(int y=minY;y<=maxY;y++)for(int x=minX;x<=maxX;x++)pixel(x,y,a.r,a.g,a.b,a.u+(x-a.x)*dx,a.v+(y-a.y)*dy,op,uv>>>16,e,false);
    }
    private void line(int[] p,int[] e){
        int op=p[0]>>>24;boolean shaded=(op&16)!=0;
        Vertex a=new Vertex(p[1],p[0],0,e[5]),b=new Vertex(p[shaded?3:2],shaded?p[2]:p[0],0,e[5]);
        int dx=b.x-a.x,dy=b.y-a.y,steps=Math.max(Math.abs(dx),Math.abs(dy));
        if(Math.abs(dx)>=1024 || Math.abs(dy)>=512)return;
        for(int i=0;i<=steps;i++){
            int x=steps==0?a.x:a.x+(int)Math.floorDiv((long)dx*i+steps/2,steps),y=steps==0?a.y:a.y+(int)Math.floorDiv((long)dy*i+steps/2,steps);
            if(x<(e[3]&1023) || x>(e[4]&1023) || y<((e[3]>>>10)&511) || y>((e[4]>>>10)&511))continue;
            int r=steps==0?a.r:a.r+(b.r-a.r)*i/steps,g=steps==0?a.g:a.g+(b.g-a.g)*i/steps,blue=steps==0?a.b:a.b+(b.b-a.b)*i/steps;
            // Line opcode bit2 is unused, never texture-enable.
            pixel(x,y,r,g,blue,0,0,op&~4,0,e,(e[1]&512)!=0);
        }
    }
    private int texture(int u,int v,int clut,int[] e){
        int window=e[2],maskX=(window&31)*8,maskY=((window>>>5)&31)*8;
        u=((u&255)&~maskX)|(((window>>>10)&31)*8&maskX);v=((v&255)&~maskY)|(((window>>>15)&31)*8&maskY);
        int page=e[1],x=(page&15)*64,y=((page>>>4)&1)*256+v,depth=(page>>>7)&3;
        if(depth>=2)return vram[(y&511)*1024+((x+u)&1023)];
        int packed=vram[(y&511)*1024+((x+(u>>(depth==0?2:1)))&1023)];
        int index=depth==0?(packed>>>((u&3)*4))&15:(packed>>>((u&1)*8))&255;
        return vram[((clut>>>6)&511)*1024+(((clut&63)*16+index)&1023)];
    }
    private void pixel(int x,int y,int r,int g,int b,int u,int v,int op,int clut,int[] e,boolean dither){
        int address=y*1024+x,back=vram[address];if((e[6]&2)!=0 && (back&0x8000)!=0)return;
        boolean textured=(op&4)!=0,semi=(op&2)!=0;int mask=0;
        if(textured){
            int texel=texture(u,v,clut,e);if(texel==0)return;mask=texel&0x8000;semi&=mask!=0;
            if((op&1)!=0){r=(texel&31)*8;g=((texel>>>5)&31)*8;b=((texel>>>10)&31)*8;}
            else{r=((texel&31)*r)>>4;g=(((texel>>>5)&31)*g)>>4;b=(((texel>>>10)&31)*b)>>4;}
        }
        int delta=dither?DITHER[(y&3)*4+(x&3)]:0;
        r=quantize(r+delta);g=quantize(g+delta);b=quantize(b+delta);
        if(semi){int mode=(e[1]>>>5)&3;r=blend(back&31,r,mode);g=blend((back>>>5)&31,g,mode);b=blend((back>>>10)&31,b,mode);}
        vram[address]=r|(g<<5)|(b<<10)|mask|((e[6]&1)<<15);pixels++;
    }
    private static int quantize(int color){return Math.max(0,Math.min(31,color>>3));}
    private static int blend(int back,int front,int mode){return switch(mode){case 0->(back>>1)+(front>>1);case 1->Math.min(31,back+front);case 2->Math.max(0,back-front);default->Math.min(31,back+(front>>2));};}
}
