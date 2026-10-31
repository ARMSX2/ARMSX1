package dev.psxlab.core;

import java.io.*;
import java.security.*;
import java.util.*;
import java.util.zip.*;

/** Complete machine snapshot at a paused core boundary. Host queues are recreated on load. */
public final class SaveState {
    private static final int MAGIC=0x50535853,VERSION=1,MAXIMUM=24*1024*1024;
    private static byte[] hash(byte[] bytes){try{return MessageDigest.getInstance("SHA-256").digest(bytes);}catch(NoSuchAlgorithmException e){throw new AssertionError(e);}}
    private static Object[] roots(Cpu cpu,Bus bus,Gpu gpu){return new Object[]{cpu,bus,gpu,bus.dma,bus.mdec,bus.spu,bus.timers,bus.cdrom,bus.sio0,cpu.gte};}
    /** Validate every device against a disposable machine while the real session is paused. */
    public static void validate(byte[] bytes,String game,byte[] bios)throws IOException{
        try(Gpu gpu=new Gpu(false)){Bus bus=new Bus(gpu);bus.loadBios(bios);for(int i=0;i<2;i++)bus.sio0.insertCard(i,new MemoryCard(MemoryCard.formatted(),null));restore(bytes,new Cpu(bus),bus,gpu,game);}
    }
    public static byte[] capture(Cpu cpu,Bus bus,Gpu gpu,String game)throws IOException{
        bus.prepareSnapshot();bus.spu.flushAudio();gpu.prepareSnapshot();
        try{
            ByteArrayOutputStream raw=new ByteArrayOutputStream();DataOutputStream data=new DataOutputStream(raw);data.writeUTF(game);data.write(hash(bus.bios));data.writeBoolean(cpu.gte.nativeExecution());
            byte[] spu=bus.spu.snapshotNative();data.writeInt(spu.length);data.write(spu);
            if(cpu.gte.nativeExecution())for(int value:cpu.gte.nativeState())data.writeInt(value);
            for(Object root:roots(cpu,bus,gpu))StateCodec.write(data,StateCodec.capture(root));data.flush();byte[] payload=raw.toByteArray();
            if(payload.length>MAXIMUM)throw new IOException("Save state exceeds size limit");
            ByteArrayOutputStream file=new ByteArrayOutputStream();DataOutputStream header=new DataOutputStream(file);header.writeInt(MAGIC);header.writeInt(VERSION);header.writeUTF(System.getProperty("os.arch",""));header.writeInt(payload.length);header.write(hash(payload));header.flush();
            try(GZIPOutputStream zip=new GZIPOutputStream(file)){zip.write(payload);}return file.toByteArray();
        }catch(ReflectiveOperationException e){throw new IOException("Could not capture device state",e);}
    }
    public static void restore(byte[] file,Cpu cpu,Bus bus,Gpu gpu,String game)throws IOException{
        if(file.length>MAXIMUM)throw new IOException("Save state exceeds size limit");
        DataInputStream header=new DataInputStream(new ByteArrayInputStream(file));if(header.readInt()!=MAGIC || header.readInt()!=VERSION)throw new IOException("Unsupported save state version");
        if(!header.readUTF().equals(System.getProperty("os.arch","")))throw new IOException("State was created for a different CPU architecture");int length=header.readInt();if(length<0||length>MAXIMUM)throw new IOException("Invalid state length");byte[] checksum=new byte[32];header.readFully(checksum);
        byte[] payload=new byte[length];try(GZIPInputStream zip=new GZIPInputStream(header)){int at=0,n;while(at<length && (n=zip.read(payload,at,length-at))>=0)at+=n;if(at!=length || zip.read()!=-1)throw new IOException("State payload length mismatch");}
        if(!MessageDigest.isEqual(checksum,hash(payload)))throw new IOException("Save state checksum mismatch");
        DataInputStream data=new DataInputStream(new ByteArrayInputStream(payload));if(!data.readUTF().equals(game))throw new IOException("Save state belongs to a different game");byte[] bios=new byte[32];data.readFully(bios);if(!MessageDigest.isEqual(bios,hash(bus.bios)))throw new IOException("Save state requires the BIOS used when it was saved");
        boolean nativeGte=data.readBoolean();if(nativeGte!=cpu.gte.nativeExecution())throw new IOException("Geometry engine differs from saved state");int spuSize=data.readInt();if(spuSize!=bus.spu.snapshotNative().length)throw new IOException("Sound engine differs from saved state");byte[] spu=new byte[spuSize];data.readFully(spu);int[] gte=new int[64];if(nativeGte)for(int i=0;i<64;i++)gte[i]=data.readInt();
        Object[] roots=roots(cpu,bus,gpu);StateCodec.Node[] nodes=new StateCodec.Node[roots.length];int[] budget={250000};for(int i=0;i<nodes.length;i++)nodes[i]=StateCodec.read(data,0,budget);if(data.read()!=-1)throw new IOException("Unexpected state data");
        // Callers load into a fresh machine before starting audio. No live host
        // AudioTrack, file descriptor, renderer handle or compiled-code pointer is restored.
        cpu.closeNative();gpu.prepareSnapshot();
        try{for(int i=0;i<roots.length;i++)StateCodec.apply(nodes[i],roots[i]);if(nativeGte)System.arraycopy(gte,0,cpu.gte.nativeState(),0,64);bus.spu.restoreNative(spu);bus.sio0.restored();bus.restoreSnapshotMemory();gpu.restoreSnapshot();}
        catch(ReflectiveOperationException | IllegalArgumentException e){throw new IOException("Save state is incompatible with this engine",e);}
    }
}
