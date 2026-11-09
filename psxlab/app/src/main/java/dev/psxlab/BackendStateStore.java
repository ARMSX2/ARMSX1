package dev.psxlab;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

/** Disk metadata and machine state are committed together; engine validation happens before load. */
public final class BackendStateStore {
    private static final int MAGIC = 0x50534c41, VERSION_IDENTITY = 3, VERSION_DISC_INDEX = 4;
    private static final int MAXIMUM_PAYLOAD = 24 * 1024 * 1024;
    private static final java.util.regex.Pattern SLOT=java.util.regex.Pattern.compile("^[0-9a-f]{64}-(autosave|[0-9])\\.psxlabstate$",java.util.regex.Pattern.CASE_INSENSITIVE);
    record Stored(String game,String disc,String identity,int discIndex,byte[] payload) {}
    public record Metadata(String game,String disc,int slot,String identity) {}
    public interface IdentityResolver { String identify(String source)throws Exception; }
    /** Disc ID plus boot-content fingerprint: independent of name, URI and container format. */
    public static String identity(String boot,byte[] executable){
        java.util.regex.Matcher match=java.util.regex.Pattern.compile("(?i)([A-Z]{4})[_-]([0-9]{3})[.]?([0-9]{2})").matcher(boot);
        String serial=match.find()?match.group(1).toUpperCase(java.util.Locale.ROOT)+"-"+match.group(2)+match.group(3):"EXE";
        return "psx:"+serial+":"+MigrationBackup.digest(executable);
    }
    static boolean validIdentity(String identity){return identity.matches("psx:(?:[A-Z]{4}-[0-9]{5}|EXE):[0-9a-f]{64}");}
    public static String serial(String identity){if(!validIdentity(identity))return "";String serial=identity.split(":")[1];return serial.equals("EXE")?"":serial;}
    private static File sourceIndex(File states,String source){return new File(states,"source-"+MigrationBackup.digest(normalize(source).getBytes(StandardCharsets.UTF_8))+".id");}
    private static String rememberedIdentity(File state,String source){
        File index=sourceIndex(state.getParentFile(),source);
        try{if(index.length()>128)return "";String value=new String(Files.readAllBytes(index.toPath()),StandardCharsets.UTF_8);return validIdentity(value)?value:"";}catch(IOException e){return "";}
    }
    public static Integer slot(String name){java.util.regex.Matcher m=SLOT.matcher(name);return m.matches()?m.group(1).equalsIgnoreCase("autosave")?-1:Integer.parseInt(m.group(1)):null;}
    public static String normalize(String game){
        if(game.startsWith("psx:")&&validIdentity(game))return game;
        if(game.isEmpty()||game.contains("://")&&!game.startsWith("file:"))return game;
        try{return (game.startsWith("file:")?new File(java.net.URI.create(game)):new File(game)).getCanonicalPath();}
        catch(IOException|IllegalArgumentException e){return game;}
    }
    public static boolean sameGame(String saved,String game,String runtime){
        if(normalize(saved).equals(normalize(game))||!runtime.isEmpty()&&normalize(saved).equals(normalize(runtime)))return true;
        if(saved.contains("/cache/saf-launch/")&&game.startsWith("content://")&&new File(saved).getName().matches("game-"+MigrationBackup.digest(game.getBytes(StandardCharsets.UTF_8))+"\\.[a-z0-9]+"))return true;
        return saved.contains("/cache/saf-launch/")&&runtime.contains("/cache/saf-launch/")&&new File(saved).getName().matches("game-[0-9a-f]{64}\\.[a-z0-9]+")&&new File(saved).getName().equals(new File(runtime).getName());
    }
    public static Metadata metadata(File file)throws IOException{
        Integer slot=slot(file.getName());if(slot==null)throw new IOException("Not an own-engine slot");
        try(DataInputStream in=new DataInputStream(new FileInputStream(file))){
            if(in.readInt()!=MAGIC)throw new IOException("Not an own-engine state");int version=in.readInt();
            if(version>=2&&version<=VERSION_DISC_INDEX){
                String game=in.readUTF(),disc=in.readUTF(),identity=version>=VERSION_IDENTITY?in.readUTF():rememberedIdentity(file,game);
                readDiscIndex(in,version);
                int size=in.readInt();
                if(game.isEmpty()||version>=VERSION_IDENTITY&&!validIdentity(identity)||size<1||size>MAXIMUM_PAYLOAD||size!=in.available())throw new IOException("Invalid backend state length");
                return new Metadata(game,disc,slot,identity);
            }
            if(version!=1)throw new IOException("Unsupported backend state version");
        }
        Stored stored=read(file);return new Metadata(stored.game(),stored.disc(),slot,stored.identity());
    }
    static File find(File root,String game,String runtime,int slot){
        File target=file(root,game,slot);if(target.isFile())return target;File[] files=target.getParentFile().listFiles();if(files==null)return target;
        File best=null;for(File candidate:files)if(Integer.valueOf(slot).equals(slot(candidate.getName())))try{Metadata meta=metadata(candidate);if(sameGame(meta.game(),game,runtime)&&(best==null||candidate.lastModified()>best.lastModified()))best=candidate;}catch(IOException ignored){}
        return best==null?target:best;
    }
    static File find(File root,String identity,String game,String runtime,int slot){
        if(identity.isEmpty())return find(root,game,runtime,slot);
        File target=file(root,identity,slot);if(target.isFile())try{if(identity.equals(metadata(target).identity()))return target;}catch(IOException ignored){}File[] files=target.getParentFile().listFiles();if(files==null)return target;
        File best=null;for(File candidate:files)if(Integer.valueOf(slot).equals(slot(candidate.getName())))try{
            Metadata meta=metadata(candidate);boolean matches=meta.identity().isEmpty()?sameGame(meta.game(),game,runtime):identity.equals(meta.identity());
            if(matches&&(best==null||candidate.lastModified()>best.lastModified()))best=candidate;
        }catch(IOException ignored){}
        return best==null?target:best;
    }
    /** Run once before emulation starts. Preserve originals and choose the newest legacy slot. */
    static int adoptLegacy(File root,String identity,String game,String runtime,java.util.Collection<String> aliases,IdentityResolver resolver)throws IOException{
        File directory=new File(root,"states");File[] files=directory.listFiles();if(files==null)return 0;
        java.util.Map<Integer,File> newest=new java.util.HashMap<>();java.util.Map<String,String> resolved=new java.util.HashMap<>();java.util.List<File> originals=new java.util.ArrayList<>();
        for(File file:files)if(slot(file.getName())!=null)try{
            Metadata meta=metadata(file);String id=meta.identity();
            if(id.isEmpty()){
                String source=meta.game();id=resolved.get(source);
                if(id==null){
                    boolean alias=sameGame(source,game,runtime)||aliases.stream().anyMatch(a->sameGame(source,a,""));
                    try{id=alias?identity:resolver.identify(source);}catch(Exception ignored){id="";}
                    if(id==null||!validIdentity(id))id="";resolved.put(source,id);
                    if(!id.isEmpty())atomic(sourceIndex(directory,source),id.getBytes(StandardCharsets.UTF_8));
                }
            }
            if(identity.equals(id)&&!file.equals(file(root,identity,meta.slot()))){originals.add(file);File previous=newest.get(meta.slot());if(previous==null||file.lastModified()>previous.lastModified())newest.put(meta.slot(),file);}
        }catch(IOException ignored){}
        int adopted=0;for(var entry:newest.entrySet()){
            File target=file(root,identity,entry.getKey());if(target.exists())continue;
            File original=entry.getValue();Stored state=read(original);write(target,state.game(),state.disc(),identity,state.discIndex(),state.payload());
            File preview=new File(original+".png");if(preview.isFile())atomic(new File(target+".png"),Files.readAllBytes(preview.toPath()));
            target.setLastModified(original.lastModified());adopted++;
        }
        // Keep every conflicting/original slot, outside discovery, so deleting a
        // migrated slot cannot resurrect an older copy on the next launch.
        if(!originals.isEmpty()){
            File archive=new File(directory,"legacy/"+System.currentTimeMillis()+"-"+java.util.UUID.randomUUID());Files.createDirectories(archive.toPath());
            for(File original:originals){
                File target=file(root,identity,slot(original.getName()));try{if(!identity.equals(metadata(target).identity()))continue;}catch(IOException invalid){continue;}
                Files.move(original.toPath(),new File(archive,original.getName()).toPath());File preview=new File(original+".png");if(preview.isFile())Files.move(preview.toPath(),new File(archive,preview.getName()).toPath());
            }
        }
        return adopted;
    }

    public static File file(File root, String game, int slot) {
        if (slot < -1 || slot > 9 || game.isEmpty()) throw new IllegalArgumentException("No game or invalid slot");
        String key = MigrationBackup.digest(normalize(game).getBytes(StandardCharsets.UTF_8));
        return new File(new File(root, "states"), key + "-" + (slot == -1 ? "autosave" : slot) + ".psxlabstate");
    }

    static void write(File file, String disc, byte[] payload) throws IOException {
        write(file,disc,disc,payload);
    }
    static void write(File file,String game,String disc,byte[] payload)throws IOException{
        write(file,game,disc,"",payload);
    }
    static void write(File file,String game,String disc,String identity,byte[] payload)throws IOException{
        write(file,game,disc,identity,0,payload);
    }
    static void write(File file,String game,String disc,String identity,int discIndex,byte[] payload)throws IOException{
        if (payload.length < 1 || payload.length > MAXIMUM_PAYLOAD) throw new IOException("Invalid backend state length");
        if(!identity.isEmpty()&&!validIdentity(identity))throw new IOException("Invalid game identity");
        if(discIndex<0||discIndex>4||discIndex!=0&&identity.isEmpty())throw new IOException("Invalid embedded disc index");
        try (ByteArrayOutputStream bytes = new ByteArrayOutputStream(); DataOutputStream out = new DataOutputStream(bytes)) {
            out.writeInt(MAGIC);
            out.writeInt(discIndex!=0?VERSION_DISC_INDEX:identity.isEmpty()?2:VERSION_IDENTITY);
            out.writeUTF(game);
            out.writeUTF(disc);
            if(!identity.isEmpty())out.writeUTF(identity);
            if(discIndex!=0)out.writeByte(discIndex);
            out.writeInt(payload.length);
            out.write(payload);
            out.flush();
            atomic(file, bytes.toByteArray());
        }
    }
    private static int readDiscIndex(DataInputStream in,int version)throws IOException{
        if(version<VERSION_DISC_INDEX)return 0;
        int index=in.readUnsignedByte();
        if(index>4)throw new IOException("Invalid embedded disc index");
        return index;
    }

    static Stored read(File file) throws IOException {
        if (file.length() > MAXIMUM_PAYLOAD + 2*65536L + 256) throw new IOException("Save state exceeds size limit");
        try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
            if (in.readInt() != MAGIC)
                throw new IOException("This is not a PSX Lab backend state. Use SwanStation for SwanStation states.");
            int version=in.readInt();if(version<1||version>VERSION_DISC_INDEX)throw new IOException("Unsupported backend state version");
            String game=version>=2?in.readUTF():null;
            String disc = in.readUTF();
            String identity=version>=VERSION_IDENTITY?in.readUTF():"";if(version>=VERSION_IDENTITY&&!validIdentity(identity))throw new IOException("Invalid game identity");
            int discIndex=readDiscIndex(in,version);
            int length = in.readInt();
            if (length < 1 || length > MAXIMUM_PAYLOAD) throw new IOException("Invalid backend state length");
            byte[] state = new byte[length];
            in.readFully(state);
            if (in.read() != -1) throw new IOException("Unexpected backend state data");
            if(game==null){try{game=dev.psxlab.core.SaveState.identity(state);}catch(IOException e){game=disc;}}
            return new Stored(game,disc,version>=VERSION_IDENTITY?identity:rememberedIdentity(file,game),discIndex,state);
        }
    }

    static void atomic(File target, byte[] bytes) throws IOException {
        File directory = target.getParentFile();
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot create save directory");
        File temporary = new File(target + ".tmp");
        try (FileOutputStream out = new FileOutputStream(temporary)) {
            out.write(bytes);
            out.getFD().sync();
        }
        Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }
}
