package dev.psxlab;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import dev.psxlab.core.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** One owning mount path for standalone launch, artwork and format verification. */
public final class DiscMedia implements AutoCloseable {
    private final AutoCloseable owner;
    public final IsoDisc disc;
    public final byte[] executable;
    public final String bootName;
    private DiscMedia(AutoCloseable owner,IsoDisc disc,byte[] executable,String bootName){this.owner=owner;this.disc=disc;this.executable=executable;this.bootName=bootName;}
    public static boolean supportedName(String name){return name!=null&&name.toLowerCase(Locale.ROOT).matches(".*\\.(chd|iso|bin|img|exe|cue|pbp)$");}
    public static DiscMedia open(Context context,Uri uri,String name)throws IOException{return open(context,uri,name,null);}
    public static DiscMedia open(Context context,Uri uri,String name,Uri parent)throws IOException{
        if(CueDisc.isCueName(name)){
            String text=cueText(context,uri);Uri directory=directory(context,uri,parent);
            CueDisc cue=new CueDisc(text,track->source(context,child(context,directory,track),false));
            try{IsoDisc disc=new IsoDisc((SectorSource)cue);byte[] executable=disc.bootExecutable();return new DiscMedia(cue,disc,executable,disc.bootPath);}
            catch(IOException|RuntimeException error){closeAfterFailure(cue,error);throw error;}
        }
        ByteSource bytes=source(context,uri);
        try{
            if(new String(bytes.read(0,8),StandardCharsets.US_ASCII).equals("PS-X EXE")){
                byte[] header=bytes.read(0,2048);int size=ExeLoader.le(header,28);
                if(size<4||size>2*1024*1024)throw new IOException("Invalid executable size");
                return new DiscMedia(bytes,null,bytes.read(0,size+2048),name);
            }
            IsoDisc disc=bytes instanceof PbpDisc?new IsoDisc((SectorSource)bytes):new IsoDisc(bytes);
            byte[] executable=disc.bootExecutable();return new DiscMedia(bytes,disc,executable,disc.bootPath);
        }catch(IOException|RuntimeException error){closeAfterFailure(bytes,error);throw error;}
    }
    private static void closeAfterFailure(AutoCloseable owner,Exception error){try{owner.close();}catch(Exception closing){error.addSuppressed(closing);}}
    private static ByteSource source(Context context,Uri uri)throws IOException{
        return source(context,uri,true);
    }
    private static ByteSource source(Context context,Uri uri,boolean containers)throws IOException{
        Uri target=uri.getScheme()==null?Uri.fromFile(new File(uri.toString())):uri;
        try(ParcelFileDescriptor fd=context.getContentResolver().openFileDescriptor(target,"r")){
            if(fd==null)throw new FileNotFoundException("Cannot open disc image: "+target);
            FileSource raw=new FileSource(new ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())));
            try{
                if(!containers)return raw;
                byte[] magic=raw.read(0,8);
                if(new String(magic,StandardCharsets.US_ASCII).equals("MComprHD")){raw.close();return new ChdSource(fd);}
                return PbpDisc.hasMagic(magic)?new PbpDisc(raw):raw;
            }catch(IOException|RuntimeException error){closeAfterFailure(raw,error);throw error;}
        }
    }
    private static String cueText(Context context,Uri uri)throws IOException{
        Uri target=uri.getScheme()==null?Uri.fromFile(new File(uri.toString())):uri;
        try(InputStream input=context.getContentResolver().openInputStream(target)){
            if(input==null)throw new FileNotFoundException("Cannot open CUE file");
            ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] block=new byte[8192];int count;
            while((count=input.read(block))!=-1){if(out.size()+count>1024*1024)throw new IOException("CUE text exceeds 1 MiB");out.write(block,0,count);}
            return out.toString(StandardCharsets.UTF_8.name());
        }
    }
    /** Track images are not independent library games when a valid CUE references them. */
    public static Set<Uri> referencedTracks(Context context,Uri cue,Uri parent)throws IOException{
        List<String> names=CueDisc.referencedFiles(cueText(context,cue));Uri directory=directory(context,cue,parent);Set<Uri> tracks=new HashSet<>();
        for(String name:names)tracks.add(child(context,directory,name));return tracks;
    }
    private record Document(String id,String name,boolean folder){}
    private static List<Document> children(Context context,Uri directory)throws IOException{
        Uri query=DocumentsContract.buildChildDocumentsUriUsingTree(directory,DocumentsContract.getDocumentId(directory));List<Document> result=new ArrayList<>();
        try(Cursor cursor=context.getContentResolver().query(query,new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID,DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE},null,null,null)){
            if(cursor==null)throw new IOException("Game folder is unavailable");
            while(cursor.moveToNext()){if(result.size()>=20000)throw new IOException("Choose a smaller game folder");result.add(new Document(cursor.getString(0),cursor.getString(1),DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))));}
        }catch(SecurityException error){throw new IOException("Add the game's folder to the library to grant access to its BIN tracks",error);}
        return result;
    }
    private static Uri directory(Context context,Uri cue,Uri supplied)throws IOException{
        if(cue.getScheme()==null||"file".equals(cue.getScheme())){
            File file=new File(cue.getScheme()==null?cue.toString():cue.getPath()).getCanonicalFile();return Uri.fromFile(file.getParentFile());
        }
        if(supplied!=null)return supplied;
        if(!DocumentsContract.isTreeUri(cue))throw new IOException("Add the game's folder to the library so the CUE and BIN tracks can be opened together");
        String wanted=DocumentsContract.getDocumentId(cue),root=DocumentsContract.getTreeDocumentId(cue);ArrayDeque<String> pending=new ArrayDeque<>();Set<String> visited=new HashSet<>();pending.add(root);int seen=0;
        // Document IDs are provider-defined: never assume they are filesystem paths.
        while(!pending.isEmpty()){
            if(Thread.currentThread().isInterrupted())throw new IOException("Cancelled");String id=pending.removeFirst();if(!visited.add(id))continue;Uri folder=DocumentsContract.buildDocumentUriUsingTree(cue,id);
            for(Document doc:children(context,folder)){
                if(++seen>20000)throw new IOException("Choose a smaller game folder");
                if(doc.id.equals(wanted))return folder;if(doc.folder)pending.add(doc.id);
            }
        }
        throw new FileNotFoundException("The CUE's parent folder could not be found; add the folder again");
    }
    private static Uri child(Context context,Uri directory,String relative)throws IOException{
        String name=CueDisc.relativeName(relative);
        if("file".equals(directory.getScheme())){
            File parent=new File(directory.getPath()).getCanonicalFile(),target=new File(parent,name).getCanonicalFile();
            if(!target.toPath().startsWith(parent.toPath()))throw new IOException("CUE track is outside its folder");return Uri.fromFile(target);
        }
        String[] parts=name.split("/");Uri current=directory;
        for(int index=0;index<parts.length;index++){
            if(parts[index].equals("."))continue;Document found=null,folded=null;boolean ambiguous=false;
            for(Document entry:children(context,current)){
                if(parts[index].equals(entry.name)){found=entry;break;}
                if(parts[index].equalsIgnoreCase(entry.name)){if(folded!=null)ambiguous=true;folded=entry;}
            }
            if(found==null&&!ambiguous)found=folded;
            if(found==null)throw new FileNotFoundException("CUE track not found: "+name);
            if(index<parts.length-1&&!found.folder)throw new IOException("CUE track path crosses a file: "+name);
            current=DocumentsContract.buildDocumentUriUsingTree(directory,found.id);
        }
        return current;
    }
    @Override public void close()throws IOException{try{owner.close();}catch(IOException error){throw error;}catch(Exception error){throw new IOException("Could not close disc image",error);}}
}
