package dev.psxlab;
import android.content.*;
import android.database.*;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;

/** Read-only synthetic document provider, deliberately using opaque IDs. Test APK only. */
public final class DiscFixtureProvider extends ContentProvider {
    public static final String AUTHORITY="dev.psxlab.test.media-fixtures";
    private record Item(String name,String parent,byte[] data){}
    private final Map<String,Item> items=new LinkedHashMap<>();
    @Override public boolean onCreate(){
        items.put("root",new Item("Fixtures",null,null));items.put("folder-5",new Item("Tracks","root",null));
        items.put("cue-17",new Item("game.cue","root",DiscFixtureData.cue().getBytes(StandardCharsets.UTF_8)));
        items.put("cue-18",new Item("nested.cue","root","FILE tracks/data.bin BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n".getBytes(StandardCharsets.UTF_8)));
        items.put("data-30",new Item("Track One.bin","root",DiscFixtureData.raw()));items.put("audio-31",new Item("MusicA.bin","root",new byte[2352*2]));items.put("audio-32",new Item("MusicB.bin","root",new byte[2352*2]));
        items.put("nested-50",new Item("DATA.BIN","folder-5",DiscFixtureData.raw()));items.put("pbp-88",new Item("EBOOT.PBP","root",DiscFixtureData.pbp()));items.put("iso-15",new Item("single.iso","root",DiscFixtureData.iso()));items.put("exe-33",new Item("demo.exe","root",DiscFixtureData.executable()));
        items.put("bad-77",new Item("missing.cue","root","FILE missing.bin BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n".getBytes(StandardCharsets.UTF_8)));return true;
    }
    private String id(Uri uri){List<String> path=uri.getPathSegments();int at=path.indexOf("document");if(at<0||at+1>=path.size())throw new IllegalArgumentException("No document ID");return path.get(at+1);}
    private String mime(Item item){return item.data==null?DocumentsContract.Document.MIME_TYPE_DIR:"application/octet-stream";}
    @Override public Cursor query(Uri uri,String[] projection,String selection,String[] arguments,String order){
        String[] columns=projection==null?new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE,DocumentsContract.Document.COLUMN_SIZE}:projection;MatrixCursor result=new MatrixCursor(columns);String parent=id(uri);boolean children="children".equals(uri.getLastPathSegment());
        for(var entry:items.entrySet()){Item item=entry.getValue();if(children?!Objects.equals(parent,item.parent):!entry.getKey().equals(parent))continue;Object[] values=new Object[columns.length];for(int i=0;i<columns.length;i++)values[i]=switch(columns[i]){case DocumentsContract.Document.COLUMN_DOCUMENT_ID->entry.getKey();case DocumentsContract.Document.COLUMN_DISPLAY_NAME->item.name;case DocumentsContract.Document.COLUMN_MIME_TYPE->mime(item);case DocumentsContract.Document.COLUMN_SIZE->item.data==null?0:item.data.length;default->null;};result.addRow(values);}return result;
    }
    @Override public ParcelFileDescriptor openFile(Uri uri,String mode)throws FileNotFoundException{String id=id(uri);Item item=items.get(id);if(!"r".equals(mode)||item==null||item.data==null)throw new FileNotFoundException("Read-only fixture unavailable");try{File file=new File(getContext().getCacheDir(),"media-fixture-"+id);if(file.length()!=item.data.length)Files.write(file.toPath(),item.data);return ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY);}catch(IOException error){throw new FileNotFoundException(error.getMessage());}}
    @Override public String getType(Uri uri){Item item=items.get(id(uri));return item==null?null:mime(item);}
    @Override public Uri insert(Uri uri,ContentValues values){throw new UnsupportedOperationException();}
    @Override public int update(Uri uri,ContentValues values,String selection,String[] arguments){throw new UnsupportedOperationException();}
    @Override public int delete(Uri uri,String selection,String[] arguments){throw new UnsupportedOperationException();}
}
