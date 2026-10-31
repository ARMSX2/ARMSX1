package dev.psxlab.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

/** Per-backend GameShark catalogue. Immutable selections are consumed once per guest frame. */
public final class Cheats {
    private static final int MAX_GROUPS=4096,MAX_LINES=65536;
    private static final Pattern CODE=Pattern.compile("^([0-9a-fA-F]{8})\\s*[:,]?\\s*([0-9a-fA-F]{1,8})(?:\\s*(?:#|;|//).*)?$");
    private record Code(int type,int address,int value){}
    private record Entry(String name,String description,List<Code> codes,boolean unsupported){}
    private volatile List<Entry> catalogue=List.of(),armed=List.of();
    private volatile String source="";
    private volatile int missing;
    public synchronized int load(String path)throws IOException {
        catalogue=armed=List.of();missing=0;source="";
        if(path==null||path.isBlank())return 0;
        Path file=Path.of(path);if(Files.size(file)>4*1024*1024)throw new IOException("Cheat catalogue exceeds 4 MiB");
        StringBuilder text=new StringBuilder();try(Reader input=Files.newBufferedReader(file,StandardCharsets.UTF_8)){char[] buffer=new char[8192];int n;while((n=input.read(buffer))!=-1){if(text.length()+n>4*1024*1024)throw new IOException("Cheat catalogue exceeds 4 MiB");text.append(buffer,0,n);}}
        List<Entry> entries=parse(text.toString());catalogue=entries;source=path;return entries.size();
    }
    public synchronized int loadText(String text)throws IOException {catalogue=armed=List.of();source="";missing=0;catalogue=parse(text);return catalogue.size();}
    private static boolean supported(int type){return type==0x30||type==0x80||type==0x50||type>=0xd0&&type<=0xd3||type>=0xe0&&type<=0xe3;}
    private static String clean(String value,int maximum){return value.replace('\u001e',' ').replace('\u001f',' ').substring(0,Math.min(maximum,value.length())).trim();}
    private static List<Entry> parse(String text)throws IOException {
        if(text.length()>4*1024*1024)throw new IOException("Cheat catalogue exceeds 4 MiB");
        List<Entry> result=new ArrayList<>();ArrayList<Code> codes=new ArrayList<>();String name=null,description="";boolean unsupported=false;int total=0;
        try(BufferedReader input=new BufferedReader(new StringReader(text))){
            String raw;while((raw=input.readLine())!=null){
                String line=raw.trim();if(line.startsWith("\ufeff"))line=line.substring(1).trim();
                if(line.isBlank()||line.startsWith("#")||line.startsWith(";")||line.startsWith("//"))continue;
                if(line.startsWith("[")){
                    if(name!=null)result.add(new Entry(name,description,List.copyOf(codes),unsupported));
                    if(result.size()>=MAX_GROUPS)throw new IOException("Too many cheat groups");
                    int end=line.lastIndexOf(']');name=clean(line.substring(1,end>0?end:line.length()),192);if(name.isEmpty())name="(unnamed)";
                    codes.clear();description="";unsupported=false;continue;
                }
                if(line.startsWith("@desc")){if(name!=null)description=clean(description+" "+line.substring(5).trim(),512);continue;}
                Matcher match=CODE.matcher(line);if(!match.matches())continue;
                if(++total>MAX_LINES)throw new IOException("Too many cheat code lines");
                if(name==null)name="Unnamed codes";
                int code=(int)Long.parseLong(match.group(1),16),value=(int)Long.parseLong(match.group(2),16),type=code>>>24;
                codes.add(new Code(type,code&0xffffff,value));unsupported|=!supported(type);
            }
        }
        if(name!=null)result.add(new Entry(name,description,List.copyOf(codes),unsupported));return List.copyOf(result);
    }
    public synchronized int arm(boolean master,String[] names){
        HashSet<String> selected=new HashSet<>();if(master&&names!=null)for(String name:names)if(name!=null)selected.add(name.trim().toLowerCase(Locale.ROOT));
        ArrayList<Entry> next=new ArrayList<>();HashSet<String> found=new HashSet<>();
        for(Entry entry:catalogue)if(selected.contains(entry.name.toLowerCase(Locale.ROOT))){next.add(entry);found.add(entry.name.toLowerCase(Locale.ROOT));}
        missing=selected.size()-found.size();armed=List.copyOf(next);return next.size();
    }
    public int armedCount(){return armed.size();}
    public Map<String,Object> diagnostic(){return Map.of("file",source,"catalogue_entries",catalogue.size(),"armed_entries",armed.size(),"missing_names",missing,"source","psxlab/app/src/main/java/dev/psxlab/core/Cheats.java#apply");}
    public String describe(){
        StringBuilder out=new StringBuilder();for(Entry entry:catalogue){if(out.length()!=0)out.append('\u001e');out.append(entry.name).append('\u001f').append(entry.codes.size()).append('\u001f').append(entry.unsupported?'1':'0').append('\u001f').append(entry.description);}return out.toString();
    }
    public static String[] selectedNames(String toml){
        if(toml==null||toml.isBlank())return new String[0];
        ArrayList<String> result=new ArrayList<>();StringBuilder token=new StringBuilder();boolean quoted=false,escape=false;
        for(int i=0;i<toml.length();i++){char c=toml.charAt(i);
            if(escape){token.append(switch(c){case 'n'->'\n';case 'r'->'\r';case 't'->'\t';default->c;});escape=false;}
            else if(quoted&&c=='\\')escape=true;
            else if(c=='"'){if(quoted){result.add(token.toString());token.setLength(0);}quoted=!quoted;}
            else if(quoted)token.append(c);
        }return result.toArray(new String[0]);
    }
    private static boolean valid(int address,int width){return address>=0&&address<=2*1024*1024-width;}
    private static int read(Bus bus,int address,int width){return bus.read8(address)|(width==2?bus.read8(address+1)<<8:0);}
    public void apply(Bus bus){
        List<Entry> current=armed;if(current.isEmpty())return;
        for(Entry entry:current){boolean skip=false;int repeats=0,step=0,delta=0;
            for(Code code:entry.codes){
                if(skip){skip=false;continue;}
                int type=code.type,address=code.address,value=code.value;
                if(type==0x50){repeats=(address>>>8)&255;step=address&255;delta=value;continue;}
                if(type==0x30||type==0x80){int width=type==0x30?1:2,count=repeats==0?1:repeats;
                    for(int i=0;i<count;i++){if(valid(address,width))bus.patchRam(address,value,width);if(repeats!=0){address+=step;value+=delta;}}
                    repeats=0;continue;
                }
                if(type>=0xd0&&type<=0xd3||type>=0xe0&&type<=0xe3){int width=type>=0xe0?1:2,expected=value&(width==1?255:65535);
                    if(!valid(address,width)){skip=true;continue;}int actual=read(bus,address,width);
                    skip=!switch(type&3){case 0->actual==expected;case 1->actual!=expected;case 2->actual<expected;default->actual>expected;};
                }
            }
        }
    }
}
