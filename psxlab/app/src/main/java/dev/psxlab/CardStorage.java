package dev.psxlab;

import dev.psxlab.core.*;
import java.io.*;
import java.nio.file.*;
import java.util.concurrent.*;

/** Coalesced card persistence; the emulation thread never writes a file. */
final class CardStorage implements AutoCloseable {
    /** One guest save: SAVING at its first write, FAILED at most once, then SAVED when it is on disk. */
    static final int SAVING=0,SAVED=1,FAILED=2;
    interface Events{void card(int slot,int state,String detail);}
    /** A game writes a save as many 128-byte frames; it is over once nothing changed for this long. */
    static final long DEFAULT_QUIET_MILLIS=600;
    private final MemoryCard[] cards=new MemoryCard[2];
    private final File[] files=new File[2];
    private final boolean[] dirty=new boolean[2],saving=new boolean[2],failedThisSave=new boolean[2];
    private final long[] changedNanos=new long[2];
    private final ScheduledExecutorService writer=Executors.newSingleThreadScheduledExecutor(r->new Thread(r,"PSX-MemoryCards"));
    private final Events events;
    private final long quietNanos;
    private volatile IOException failure;
    CardStorage(File directory,Bus bus)throws IOException{this(directory,bus,(slot,state,detail)->{},DEFAULT_QUIET_MILLIS);}
    CardStorage(File directory,Bus bus,Events events,long quietMillis)throws IOException{
        this.events=events;this.quietNanos=TimeUnit.MILLISECONDS.toNanos(quietMillis);
        if(!directory.isDirectory() && !directory.mkdirs())throw new IOException("Cannot create memory card folder");
        try{for(int i=0;i<2;i++){final int slot=i;files[i]=new File(directory,"card"+(i+1)+".mcd");byte[] image=files[i].isFile()?Files.readAllBytes(files[i].toPath()):MemoryCard.formatted();if(image.length!=MemoryCard.SIZE)throw new IOException("Card "+(i+1)+" has an invalid size; existing file was preserved");cards[i]=new MemoryCard(image,()->mark(slot));bus.sio0.insertCard(i,cards[i]);if(!files[i].isFile())write(i);}
        }catch(IOException e){writer.shutdown();throw e;}
        writer.scheduleWithFixedDelay(this::flush,200,200,TimeUnit.MILLISECONDS);
    }
    /** Guest wrote a frame. Called on the emulation thread: no file work, and the event is a post. */
    void mark(int slot){
        boolean started;
        synchronized(this){dirty[slot]=true;changedNanos[slot]=System.nanoTime();started=!saving[slot];if(started){saving[slot]=true;failedThisSave[slot]=false;}}
        if(started)notify(slot,SAVING,"");
    }
    private void write(int slot)throws IOException{
        File temp=new File(files[slot].getPath()+".tmp");try(FileOutputStream out=new FileOutputStream(temp)){out.write(cards[slot].image());out.getFD().sync();}
        Files.move(temp.toPath(),files[slot].toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
    private void flush(){flush(false);}
    /** last: the session is ending, so a save still in progress is finished by this flush. */
    private void flush(boolean last){
        for(int i=0;i<2;i++){
            boolean save;synchronized(this){save=dirty[i];dirty[i]=false;}
            if(save)try{write(i);failure=null;}catch(IOException e){
                boolean report;synchronized(this){dirty[i]=true;report=saving[i]&&!failedThisSave[i];failedThisSave[i]=true;}failure=e;
                if(report)notify(i,FAILED,String.valueOf(e.getMessage()));
            }
            // SAVED only once the file holds every frame the game wrote and the game has stopped
            // writing, so a multi-frame save is announced once and never before it is on disk.
            boolean finished;
            // A retry that succeeds after a reported failure still ends the save as SAVED.
            synchronized(this){finished=saving[i]&&!dirty[i]&&(last||System.nanoTime()-changedNanos[i]>=quietNanos);if(finished)saving[i]=false;}
            if(finished)notify(i,SAVED,"");
        }
    }
    private void notify(int slot,int state,String detail){try{events.card(slot,state,detail);}catch(RuntimeException ignored){}}
    String error(){return failure==null?"":failure.getMessage();}
    @Override public void close(){boolean interrupted=Thread.interrupted();writer.shutdown();try{if(!writer.awaitTermination(5,TimeUnit.SECONDS))writer.shutdownNow();}catch(InterruptedException e){interrupted=true;writer.shutdownNow();}flush(true);if(interrupted)Thread.currentThread().interrupt();}
}
