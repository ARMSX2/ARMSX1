package dev.psxlab.core;
import java.io.IOException;
public interface ByteSource extends AutoCloseable {
    byte[] read(long offset,int length) throws IOException;
    default long length() throws IOException { return -1; }
    @Override void close() throws IOException;
}
