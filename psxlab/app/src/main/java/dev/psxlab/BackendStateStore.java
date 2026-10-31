package dev.psxlab;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

/** Disk metadata and machine state are committed together; engine validation happens before load. */
final class BackendStateStore {
    private static final int MAGIC = 0x50534c41, VERSION = 1;
    private static final int MAXIMUM_PAYLOAD = 24 * 1024 * 1024;
    record Stored(String disc, byte[] payload) {}

    static File file(File root, String game, int slot) {
        if (slot < -1 || slot > 9 || game.isEmpty()) throw new IllegalArgumentException("No game or invalid slot");
        String key = MigrationBackup.digest(game.getBytes(StandardCharsets.UTF_8));
        return new File(new File(root, "states"), key + "-" + (slot == -1 ? "autosave" : slot) + ".psxlabstate");
    }

    static void write(File file, String disc, byte[] payload) throws IOException {
        if (payload.length < 1 || payload.length > MAXIMUM_PAYLOAD) throw new IOException("Invalid backend state length");
        try (ByteArrayOutputStream bytes = new ByteArrayOutputStream(); DataOutputStream out = new DataOutputStream(bytes)) {
            out.writeInt(MAGIC);
            out.writeInt(VERSION);
            out.writeUTF(disc);
            out.writeInt(payload.length);
            out.write(payload);
            out.flush();
            atomic(file, bytes.toByteArray());
        }
    }

    static Stored read(File file) throws IOException {
        if (file.length() > MAXIMUM_PAYLOAD + 65536L + 16) throw new IOException("Save state exceeds size limit");
        try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
            if (in.readInt() != MAGIC || in.readInt() != VERSION)
                throw new IOException("This is not a PSX Lab backend state. Use SwanStation for SwanStation states.");
            String disc = in.readUTF();
            int length = in.readInt();
            if (length < 1 || length > MAXIMUM_PAYLOAD) throw new IOException("Invalid backend state length");
            byte[] state = new byte[length];
            in.readFully(state);
            if (in.read() != -1) throw new IOException("Unexpected backend state data");
            return new Stored(disc, state);
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
