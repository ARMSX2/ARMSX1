package dev.psxlab;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

/** Disk metadata and machine state are committed together; engine validation happens before load. */
public final class BackendStateStore {
    private static final int MAGIC = 0x50534c41, VERSION = 2;
    private static final int MAXIMUM_PAYLOAD = 24 * 1024 * 1024;
    private static final java.util.regex.Pattern SLOT=java.util.regex.Pattern.compile("^[0-9a-f]{64}-(autosave|[0-9])\\.psxlabstate$",java.util.regex.Pattern.CASE_INSENSITIVE);
    record Stored(String game,String disc, byte[] payload) {}
    public record Metadata(String game,String disc,int slot) {}
    public static Integer slot(String name){java.util.regex.Matcher m=SLOT.matcher(name);return m.matches()?m.group(1).equalsIgnoreCase("autosave")?-1:Integer.parseInt(m.group(1)):null;}
    public static String normalize(String game){
        if(game.isEmpty()||game.contains("://")&&!game.startsWith("file:"))return game;
        try{return (game.startsWith("file:")?new File(java.net.URI.create(game)):new File(game)).getCanonicalPath();}
        catch(IOException|IllegalArgumentException e){return game;}
    }
    public static boolean sameGame(String saved,String game,String runtime){
        if(normalize(saved).equals(normalize(game))||!runtime.isEmpty()&&normalize(saved).equals(normalize(runtime)))return true;
        return saved.contains("/cache/saf-launch/")&&runtime.contains("/cache/saf-launch/")&&new File(saved).getName().matches("game-[0-9a-f]{64}\\.[a-z0-9]+")&&new File(saved).getName().equals(new File(runtime).getName());
    }
    public static Metadata metadata(File file)throws IOException{
        Integer slot=slot(file.getName());if(slot==null)throw new IOException("Not an own-engine slot");
        try(DataInputStream in=new DataInputStream(new FileInputStream(file))){
            if(in.readInt()!=MAGIC)throw new IOException("Not an own-engine state");int version=in.readInt();
            if(version==VERSION){String game=in.readUTF(),disc=in.readUTF();int size=in.readInt();if(game.isEmpty()||size<1||size>MAXIMUM_PAYLOAD||size!=in.available())throw new IOException("Invalid backend state length");return new Metadata(game,disc,slot);}
            if(version!=1)throw new IOException("Unsupported backend state version");
        }
        Stored stored=read(file);return new Metadata(stored.game(),stored.disc(),slot);
    }
    static File find(File root,String game,String runtime,int slot){
        File target=file(root,game,slot);if(target.isFile())return target;File[] files=target.getParentFile().listFiles();if(files==null)return target;
        File best=null;for(File candidate:files)if(Integer.valueOf(slot).equals(slot(candidate.getName())))try{Metadata meta=metadata(candidate);if(sameGame(meta.game(),game,runtime)&&(best==null||candidate.lastModified()>best.lastModified()))best=candidate;}catch(IOException ignored){}
        return best==null?target:best;
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
        if (payload.length < 1 || payload.length > MAXIMUM_PAYLOAD) throw new IOException("Invalid backend state length");
        try (ByteArrayOutputStream bytes = new ByteArrayOutputStream(); DataOutputStream out = new DataOutputStream(bytes)) {
            out.writeInt(MAGIC);
            out.writeInt(VERSION);
            out.writeUTF(game);
            out.writeUTF(disc);
            out.writeInt(payload.length);
            out.write(payload);
            out.flush();
            atomic(file, bytes.toByteArray());
        }
    }

    static Stored read(File file) throws IOException {
        if (file.length() > MAXIMUM_PAYLOAD + 2*65536L + 16) throw new IOException("Save state exceeds size limit");
        try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
            if (in.readInt() != MAGIC)
                throw new IOException("This is not a PSX Lab backend state. Use SwanStation for SwanStation states.");
            int version=in.readInt();if(version!=1&&version!=VERSION)throw new IOException("Unsupported backend state version");
            String game=version==VERSION?in.readUTF():null;
            String disc = in.readUTF();
            int length = in.readInt();
            if (length < 1 || length > MAXIMUM_PAYLOAD) throw new IOException("Invalid backend state length");
            byte[] state = new byte[length];
            in.readFully(state);
            if (in.read() != -1) throw new IOException("Unexpected backend state data");
            if(game==null){try{game=dev.psxlab.core.SaveState.identity(state);}catch(IOException e){game=disc;}}
            return new Stored(game,disc, state);
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
