package dev.psxlab.core;

import java.io.*;
import java.lang.reflect.*;
import java.util.*;

/** Versioned field snapshots, restricted to our device state. No Java object deserialization. */
final class StateCodec {
    private static final Map<String,Set<String>> SKIP=new HashMap<>();
    static {
        skip(Cpu.class,"nativeCpu","bus","gte");
        skip(Bus.class,"nativeMemory","instructionWords","instructionValid","gpu","dma","mdec","spu","timers","cdrom","sio0","nativeCores","bios");
        skip(Gpu.class,"journal","worker","raster","hardware","hardwareDrawing","internalScale","presentationMode","renderer","failure","diagnosticsEnabled","diagnosticQueueWait","diagnosticDraw","diagnosticScanout","vulkan","readSynchronized","softwareVramRevision","vulkanFrames");
        skip(Spu.class,"nativeCore","sink","output");
        skip(Gte.class,"nativeCore");
        skip(CdRom.class,"disc","xaSink","recentCommands","recentWrites","lastMediaError");
        skip(SerialPad.class,"state");
        skip(MemoryCard.class,"data","changed");
    }
    private static void skip(Class<?> type,String... names){SKIP.put(type.getName(),new HashSet<>(Arrays.asList(names)));}
    private static List<Field> fields(Class<?> c){
        ArrayList<Field> result=new ArrayList<>();
        for(Field f:c.getDeclaredFields())if(!Modifier.isStatic(f.getModifiers()) && !f.isSynthetic() && !f.getName().equals("bus") && !f.getName().equals("traceSink") && !SKIP.getOrDefault(c.getName(),Collections.emptySet()).contains(f.getName())){f.setAccessible(true);result.add(f);}
        result.sort(Comparator.comparing(Field::getName));return result;
    }
    static final class Node {final int kind;String type;Object scalar;ArrayList<Node> items;TreeMap<String,Node> fields;Node(int k){kind=k;}}
    static Node capture(Object value)throws ReflectiveOperationException,IOException{
        if(value==null)return new Node(0);
        Node n;
        if(value instanceof Integer){n=new Node(1);n.scalar=value;}else if(value instanceof Long){n=new Node(2);n.scalar=value;}else if(value instanceof Boolean){n=new Node(3);n.scalar=value;}
        else if(value instanceof Byte){n=new Node(4);n.scalar=value;}else if(value instanceof Short){n=new Node(5);n.scalar=value;}else if(value instanceof String){n=new Node(6);n.scalar=value;}
        else if(value.getClass().isArray()){n=new Node(7);Class<?> component=value.getClass().getComponentType();n.type=component.getName();if(component.isPrimitive()){n.scalar=Array.newInstance(component,Array.getLength(value));System.arraycopy(value,0,n.scalar,0,Array.getLength(value));}else{n.items=new ArrayList<>();for(int i=0;i<Array.getLength(value);i++)n.items.add(capture(Array.get(value,i)));}}
        else if(value instanceof ArrayDeque<?>){n=new Node(8);n.items=new ArrayList<>();for(Object item:(ArrayDeque<?>)value)n.items.add(capture(item));}
        else{
            if(!value.getClass().getName().startsWith("dev.psxlab.core."))throw new IOException("Unsupported state object "+value.getClass().getName());
            n=new Node(9);n.type=value.getClass().getName();n.fields=new TreeMap<>();for(Field f:fields(value.getClass()))n.fields.put(f.getName(),capture(f.get(value)));
        }
        return n;
    }
    static void write(DataOutput out,Node n)throws IOException{
        out.writeByte(n.kind);
        switch(n.kind){case 0:break;case 1:out.writeInt((int)n.scalar);break;case 2:out.writeLong((long)n.scalar);break;case 3:out.writeBoolean((boolean)n.scalar);break;case 4:out.writeByte((byte)n.scalar);break;case 5:out.writeShort((short)n.scalar);break;case 6:out.writeUTF((String)n.scalar);break;
            case 7:out.writeUTF(n.type);if(n.scalar!=null){out.writeInt(Array.getLength(n.scalar));switch(n.type){case "byte":out.write((byte[])n.scalar);break;case "int":for(int v:(int[])n.scalar)out.writeInt(v);break;case "long":for(long v:(long[])n.scalar)out.writeLong(v);break;case "short":for(short v:(short[])n.scalar)out.writeShort(v);break;case "boolean":for(boolean v:(boolean[])n.scalar)out.writeBoolean(v);break;default:throw new IOException("Unsupported primitive array");}break;} // fall through
            case 8:out.writeInt(n.items.size());for(Node item:n.items)write(out,item);break;
            case 9:out.writeUTF(n.type);out.writeInt(n.fields.size());for(Map.Entry<String,Node> f:n.fields.entrySet()){out.writeUTF(f.getKey());write(out,f.getValue());}break;
            default:throw new IOException("Unknown state node");}
    }
    static Node read(DataInput in,int depth,int[] budget)throws IOException{
        if(depth>32 || --budget[0]<0)throw new IOException("State exceeds structural limit");Node n=new Node(in.readUnsignedByte());
        switch(n.kind){case 0:break;case 1:n.scalar=in.readInt();break;case 2:n.scalar=in.readLong();break;case 3:n.scalar=in.readBoolean();break;case 4:n.scalar=in.readByte();break;case 5:n.scalar=in.readShort();break;case 6:n.scalar=in.readUTF();break;
            case 7:n.type=in.readUTF();Class<?> component=primitive(n.type);if(component!=null){int count=in.readInt();if(count<0||count>3*1024*1024)throw new IOException("Invalid state array");n.scalar=Array.newInstance(component,count);switch(n.type){case "byte":in.readFully((byte[])n.scalar);break;case "int":{int[] a=(int[])n.scalar;for(int i=0;i<count;i++)a[i]=in.readInt();break;}case "long":{long[] a=(long[])n.scalar;for(int i=0;i<count;i++)a[i]=in.readLong();break;}case "short":{short[] a=(short[])n.scalar;for(int i=0;i<count;i++)a[i]=in.readShort();break;}case "boolean":{boolean[] a=(boolean[])n.scalar;for(int i=0;i<count;i++)a[i]=in.readBoolean();break;}}break;} // fall through
            case 8:{int count=in.readInt();if(count<0 || count>3*1024*1024 || count>budget[0])throw new IOException("Invalid state array");n.items=new ArrayList<>(count);for(int i=0;i<count;i++)n.items.add(read(in,depth+1,budget));break;}
            case 9:{n.type=in.readUTF();int count=in.readInt();if(count<0||count>256)throw new IOException("Invalid state object");n.fields=new TreeMap<>();for(int i=0;i<count;i++){String key=in.readUTF();if(n.fields.put(key,read(in,depth+1,budget))!=null)throw new IOException("Duplicate state field");}break;}
            default:throw new IOException("Unknown state node");}
        return n;
    }
    private static int integer(Node n,String key){return (int)n.fields.get(key).scalar;}
    private static Class<?> primitive(String type){switch(type){case "byte":return byte.class;case "int":return int.class;case "long":return long.class;case "short":return short.class;case "boolean":return boolean.class;default:return null;}}
    private static Object construct(Node n)throws ReflectiveOperationException,IOException{
        Class<?> type;
        switch(n.type){
            case "dev.psxlab.core.CdRom$Event":type=Class.forName(n.type);return make(type,new Class[]{int.class,int.class,int[].class},integer(n,"ticks"),integer(n,"irq"),apply(n.fields.get("bytes"),null));
            case "dev.psxlab.core.Mdec$Output":type=Class.forName(n.type);return make(type,new Class[]{int[].class,int.class,int.class},apply(n.fields.get("words"),null),integer(n,"block"),integer(n,"depth"));
            case "dev.psxlab.core.Dma$OutputTransfer":type=Class.forName(n.type);return make(type,new Class[]{int.class,int.class,int.class},integer(n,"address"),integer(n,"words"),integer(n,"control"));
            case "dev.psxlab.core.Dma$Chopped":type=Class.forName(n.type);return make(type,new Class[]{int.class,int.class,int.class,int.class},integer(n,"address"),integer(n,"remaining"),integer(n,"control"),integer(n,"wordCycles"));
            default:throw new IOException("State object cannot be constructed: "+n.type);
        }
    }
    private static Object make(Class<?> c,Class<?>[] types,Object... args)throws ReflectiveOperationException{Constructor<?> ctor=c.getDeclaredConstructor(types);ctor.setAccessible(true);return ctor.newInstance(args);}
    @SuppressWarnings({"unchecked","rawtypes"})
    static Object apply(Node n,Object existing)throws ReflectiveOperationException,IOException{
        if(n.kind==0)return null;if(n.kind<=6)return n.scalar;
        if(n.kind==7){
            if(n.scalar!=null){int count=Array.getLength(n.scalar);Object out=existing!=null && existing.getClass()==n.scalar.getClass() && Array.getLength(existing)==count?existing:Array.newInstance(n.scalar.getClass().getComponentType(),count);System.arraycopy(n.scalar,0,out,0,count);return out;}
            Class<?> component;
            if(existing!=null && existing.getClass().isArray() && existing.getClass().getComponentType().getName().equals(n.type))component=existing.getClass().getComponentType();
            else switch(n.type){case "int":component=int.class;break;case "byte":component=byte.class;break;case "short":component=short.class;break;case "boolean":component=boolean.class;break;case "long":component=long.class;break;default:throw new IOException("Invalid state array type "+n.type);}
            Object out=existing!=null && Array.getLength(existing)==n.items.size()?existing:Array.newInstance(component,n.items.size());
            for(int i=0;i<n.items.size();i++)Array.set(out,i,apply(n.items.get(i),out==existing?Array.get(existing,i):null));return out;
        }
        if(n.kind==8){ArrayDeque out=existing instanceof ArrayDeque?(ArrayDeque)existing:new ArrayDeque();ArrayList<Object> items=new ArrayList<>();for(Node item:n.items)items.add(apply(item,null));out.clear();out.addAll(items);return out;}
        if(existing==null)existing=construct(n);
        if(!existing.getClass().getName().equals(n.type))throw new IOException("State object type mismatch");
        // Version-1 states made before drive mechanics had no operation in flight.
        if(existing instanceof CdRom){
            for(String key:new String[]{"driveTicks","seekTicks","seekTarget","readAfterSeek","explicitSeek","seekReady"})if(!n.fields.containsKey(key)){boolean flag=key.equals("readAfterSeek")||key.equals("explicitSeek")||key.equals("seekReady");Node value=new Node(flag?3:1);value.scalar=flag?Boolean.FALSE:Integer.valueOf(0);n.fields.put(key,value);}
        }
        List<Field> fs=fields(existing.getClass());if(fs.size()!=n.fields.size())throw new IOException("State schema differs from this engine");
        // Records are immutable; their constructor already consumed every field.
        if(n.type.equals("dev.psxlab.core.Mdec$Output"))return existing;
        for(Field f:fs){Node value=n.fields.get(f.getName());if(value==null)throw new IOException("Missing state field "+f.getName());Object old=f.get(existing);if(Modifier.isFinal(f.getModifiers()) && old!=null && old.getClass().isArray() && value.kind==7 && Array.getLength(old)!=(value.scalar!=null?Array.getLength(value.scalar):value.items.size()))throw new IOException("Fixed state array length mismatch");Object restored=apply(value,old);if(old!=restored || f.getType().isPrimitive()){
            if(Modifier.isFinal(f.getModifiers()) && f.getType().isPrimitive()){if(!Objects.equals(old,restored))throw new IOException("Immutable state mismatch "+f.getName());}
            else f.set(existing,restored);
        }}return existing;
    }
}
