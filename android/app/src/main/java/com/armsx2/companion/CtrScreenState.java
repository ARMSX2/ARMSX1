package com.armsx2.companion;

/** Read-only presentation labels. Never infers a specific menu selection. */
public final class CtrScreenState {
    public enum Kind { BOOT, MENU, LOADING, INTRO, HUB, RESULTS, BATTLE, RACE, UNSUPPORTED }
    public final Kind kind;
    public final String title, subtitle;
    private CtrScreenState(Kind kind,String title,String subtitle) {
        this.kind=kind;this.title=title;this.subtitle=subtitle;
    }
    private static CtrScreenState state(Kind k,String t,String s){return new CtrScreenState(k,t,s);}
    public static CtrScreenState read(String serial,byte[] ram) {
        if(!CtrRaceReader.supported(serial)) return state(Kind.UNSUPPORTED,"CTR companion",
            "Live maps and race HUD require the USA version (SCUS-94426).");
        if(ram==null || ram.length<2*1024*1024 || word(ram,0x8d2ac)!=0x80096b20)
            return state(Kind.BOOT,"Starting CTR","Your second-screen companion is ready.");
        int mode=word(ram,0x96b20);
        if((mode&0x40000000)!=0)return state(Kind.LOADING,"Loading","The map will return when the level is ready.");
        if((mode&0x2000)!=0)return state(Kind.MENU,"Choose your adventure","Select your mode, character and course on the top screen.");
        if((mode&0x20000000)!=0)return state(Kind.INTRO,"Get ready","Follow the introduction on the top screen.");
        if((mode&0x100000)!=0)return state(Kind.HUB,"Adventure map","Explore the hub and drive up to a race portal.");
        if((mode&0x200000)!=0)return state(Kind.RESULTS,"Race complete","Results and the next-race options are on the top screen.");
        if((mode&0x20)!=0)return state(Kind.BATTLE,"Battle arena","Keep the action on the top screen.");
        return state(Kind.RACE,"Preparing race view","The live map and eight positions appear when race data is ready.");
    }
    private static int word(byte[] b,int p){return (b[p]&255)|((b[p+1]&255)<<8)|((b[p+2]&255)<<16)|(b[p+3]<<24);}
}
