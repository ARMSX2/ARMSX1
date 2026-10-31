package dev.psxlab;

import dev.psxlab.core.*;
import java.io.*;
import java.nio.file.*;
import java.util.concurrent.*;

/** Coalesced card persistence; the emulation thread never writes a file. */
final class CardStorage implements AutoCloseable {
    private final MemoryCard[] cards=new MemoryCard[2];
    private final File[] files=new File[2];
    private final boolean[] dirty=new boolean[2];
    private final ScheduledExecutorService writer=Executors.newSingleThreadScheduledExecutor(r->new Thread(r,"PSX-MemoryCards"));
    private volatile IOException failure;
    CardStorage(File directory,Bus bus)throws IOException{
        if(!directory.isDirectory() && !directory.mkdirs())throw new IOException("Cannot create memory card folder");
        try{for(int i=0;i<2;i++){final int slot=i;files[i]=new File(directory,"card"+(i+1)+".mcd");byte[] image=files[i].isFile()?Files.readAllBytes(files[i].toPath()):MemoryCard.formatted();if(image.length!=MemoryCard.SIZE)throw new IOException("Card "+(i+1)+" has an invalid size; existing file was preserved");cards[i]=new MemoryCard(image,()->mark(slot));bus.sio0.insertCard(i,cards[i]);if(!files[i].isFile())write(i);}
        }catch(IOException e){writer.shutdown();throw e;}
        writer.scheduleWithFixedDelay(this::flush,200,200,TimeUnit.MILLISECONDS);
    }
    private synchronized void mark(int slot){dirty[slot]=true;}
    private void write(int slot)throws IOException{
        File temp=new File(files[slot].getPath()+".tmp");try(FileOutputStream out=new FileOutputStream(temp)){out.write(cards[slot].image());out.getFD().sync();}
        Files.move(temp.toPath(),files[slot].toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
    private void flush(){for(int i=0;i<2;i++){boolean save;synchronized(this){save=dirty[i];dirty[i]=false;}if(save)try{write(i);failure=null;}catch(IOException e){synchronized(this){dirty[i]=true;}failure=e;}}}
    String error(){return failure==null?"":failure.getMessage();}
    @Override public void close(){boolean interrupted=Thread.interrupted();writer.shutdown();try{if(!writer.awaitTermination(5,TimeUnit.SECONDS))writer.shutdownNow();}catch(InterruptedException e){interrupted=true;writer.shutdownNow();}flush();if(interrupted)Thread.currentThread().interrupt();}
}
