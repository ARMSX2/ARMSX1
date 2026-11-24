package dev.psxlab;

import dev.psxlab.core.*;
import java.io.*;
import java.nio.*;
import java.util.concurrent.*;
import java.util.zip.Deflater;

/**
 * Developer-only rolling trace of guest state, enabled by a {@value #FLAG} file in the log folder.
 * Every {@link #INTERVAL} guest fields it records main RAM, the CPU registers and the pad buttons:
 * a full (deflated) snapshot every {@link #BASE_EVERY} records and only the changed words between,
 * so a developer can see what a stuck game is waiting on. The VM thread only copies memory; a
 * background thread diffs, compresses and writes. Records are flushed one at a time so the file
 * can be pulled over adb while the game runs; it rotates to deeptrace-previous.bin past
 * {@link #ROTATE_BYTES}.
 *
 * File: "ARMSXDT1", then records: int type (1 base, 2 delta), long field, long guestClocks, int pc,
 * int[32] registers, int padButtons, then base: int length + deflated 2 MiB RAM, delta: int count +
 * count x (int wordIndex, int value). All little-endian.
 */
final class DeepTrace implements AutoCloseable {
    static final String FLAG="deeptrace.on",FILE="deeptrace-current.bin",PREVIOUS="deeptrace-previous.bin";
    static final int INTERVAL=15,BASE_EVERY=20;
    static final long ROTATE_BYTES=48L*1024*1024;
    private static final byte[] MAGIC="ARMSXDT1".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
    private record Snapshot(long field,long clocks,int pc,int[] registers,int pad,byte[] ram){}
    private final File directory;
    private final ExecutorService writer=Executors.newSingleThreadExecutor(r->{Thread t=new Thread(r,"PSX-DeepTrace");t.setDaemon(true);return t;});
    private final Semaphore inFlight=new Semaphore(4);
    private long lastField=Long.MIN_VALUE;
    private volatile long dropped;
    // Writer-thread state.
    private OutputStream out;
    private byte[] previous;
    private int records;

    private DeepTrace(File directory){this.directory=directory;}

    /** A tracer when the developer flag file exists, otherwise null (normal play pays nothing). */
    static DeepTrace openIfRequested(File directory){
        return new File(directory,FLAG).isFile()?new DeepTrace(directory):null;
    }

    /** VM thread, after each slice of emulation. Copies state only on every INTERVAL-th field. */
    void onVm(long field,Cpu cpu,Bus bus,int padButtons){
        if(lastField!=Long.MIN_VALUE&&field-lastField<INTERVAL)return;
        lastField=field;
        if(!inFlight.tryAcquire()){dropped++;return;}
        Snapshot s=new Snapshot(field,cpu.instructions,cpu.pc,cpu.r.clone(),padButtons,bus.snapshotRam());
        writer.execute(()->{try{write(s);}catch(IOException|RuntimeException e){android.util.Log.w("PSXDeepTrace","trace write failed",e);}finally{inFlight.release();}});
    }

    long dropped(){return dropped;}

    private void write(Snapshot s)throws IOException{
        if(out==null||new File(directory,FILE).length()>ROTATE_BYTES)rotate();
        boolean base=previous==null||records%BASE_EVERY==0;
        ByteBuffer head=ByteBuffer.allocate(4+8+8+4+32*4+4).order(ByteOrder.LITTLE_ENDIAN);
        head.putInt(base?1:2).putLong(s.field()).putLong(s.clocks()).putInt(s.pc());for(int v:s.registers())head.putInt(v);head.putInt(s.pad());
        out.write(head.array());
        if(base){
            Deflater deflater=new Deflater(1);deflater.setInput(s.ram());deflater.finish();
            ByteArrayOutputStream packed=new ByteArrayOutputStream(512*1024);byte[] chunk=new byte[65536];
            while(!deflater.finished()){int n=deflater.deflate(chunk);packed.write(chunk,0,n);}deflater.end();
            writeInt(packed.size());packed.writeTo(out);
        }else{
            ByteBuffer cur=ByteBuffer.wrap(s.ram()).order(ByteOrder.LITTLE_ENDIAN),old=ByteBuffer.wrap(previous).order(ByteOrder.LITTLE_ENDIAN);
            ByteArrayOutputStream changes=new ByteArrayOutputStream();int count=0;ByteBuffer pair=ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
            for(int i=0;i<s.ram().length/4;i++){int v=cur.getInt(i*4);if(v!=old.getInt(i*4)){pair.clear();pair.putInt(i).putInt(v);changes.write(pair.array(),0,8);count++;}}
            writeInt(count);changes.writeTo(out);
        }
        out.flush();previous=s.ram();records++;
    }
    private void writeInt(int v)throws IOException{out.write(v);out.write(v>>>8);out.write(v>>>16);out.write(v>>>24);}
    private void rotate()throws IOException{
        if(out!=null)out.close();
        File current=new File(directory,FILE),old=new File(directory,PREVIOUS);
        if(current.isFile()){if(old.isFile()&&!old.delete())throw new IOException("Cannot rotate deep trace");if(!current.renameTo(old))throw new IOException("Cannot rotate deep trace");}
        out=new BufferedOutputStream(new FileOutputStream(current),1<<16);out.write(MAGIC);previous=null;records=0;
    }
    @Override public void close(){
        writer.shutdown();
        try{writer.awaitTermination(5,TimeUnit.SECONDS);}catch(InterruptedException e){Thread.currentThread().interrupt();}
        try{if(out!=null)out.close();}catch(IOException ignored){}
    }
}
