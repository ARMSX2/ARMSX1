package dev.psxlab;
import dev.psxlab.core.ByteSource;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
public final class FileSource implements ByteSource {
    private final FileInputStream stream;
    public FileSource(FileInputStream stream){this.stream=stream;}
    @Override public byte[] read(long offset,int length)throws IOException{
        if(offset<0 || length<0 || length>4*1024*1024)throw new IOException("Invalid file range");
        ByteBuffer b=ByteBuffer.allocate(length);FileChannel c=stream.getChannel();
        while(b.hasRemaining()){int n=c.read(b,offset+b.position());if(n<=0)throw new EOFException("Truncated or non-seekable disc image");}
        return b.array();
    }
    @Override public void close()throws IOException{stream.close();}
    @Override public long length()throws IOException{return stream.getChannel().size();}
}
