package dev.psxlab;

import java.io.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;
import java.util.zip.*;

/** Verified, non-destructive archive of saves. Never includes games or a BIOS. */
public final class MigrationBackup {
    private MigrationBackup() {}
    public static File create(File root, File destination, String... folders) throws IOException {
        if (!destination.isDirectory() && !destination.mkdirs()) throw new IOException("Cannot create backup directory");
        File target = new File(destination, "saves-" + System.currentTimeMillis() + "-" + UUID.randomUUID() + ".zip");
        File temporary = new File(target + ".tmp");
        StringBuilder manifest = new StringBuilder("SHA-256\tbytes\tpath\n");
        Set<String> names = new HashSet<>();
        try (FileOutputStream file = new FileOutputStream(temporary); ZipOutputStream zip = new ZipOutputStream(file)) {
            for (String folder : folders) add(root.getCanonicalFile(), new File(root, folder), zip, manifest, names);
            zip.putNextEntry(new ZipEntry("MANIFEST.tsv")); zip.write(manifest.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8)); zip.closeEntry();
            zip.finish(); file.getFD().sync();
        } catch (IOException e) { temporary.delete(); throw e; }
        try (ZipFile zip = new ZipFile(temporary)) {
            for (String row : manifest.toString().split("\n")) {
                if (row.startsWith("SHA-256")) continue;
                String[] fields = row.split("\t", 3);
                try (InputStream in = zip.getInputStream(zip.getEntry(fields[2]))) {
                    ByteArrayOutputStream copy = new ByteArrayOutputStream(); byte[] block = new byte[8192]; int count;
                    while ((count = in.read(block)) != -1) copy.write(block, 0, count);
                    byte[] bytes = copy.toByteArray();
                    if (bytes.length != Long.parseLong(fields[1]) || !digest(bytes).equals(fields[0])) throw new IOException("Backup verification failed");
                }
            }
        }
        Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE);
        return target;
    }
    private static void add(File root, File file, ZipOutputStream zip, StringBuilder manifest, Set<String> names) throws IOException {
        if (!file.exists()) return;
        File resolved = file.getCanonicalFile();
        if (!resolved.toPath().startsWith(root.toPath()) || Files.isSymbolicLink(file.toPath())) throw new IOException("Unsafe backup path");
        if (file.isDirectory()) {
            File[] children = file.listFiles(); if (children == null) throw new IOException("Cannot enumerate saves");
            Arrays.sort(children, Comparator.comparing(File::getName));
            for (File child : children) add(root, child, zip, manifest, names);
        } else {
            if (file.length() > 64L * 1024 * 1024) throw new IOException("Save file exceeds backup limit");
            String name = root.toPath().relativize(resolved.toPath()).toString().replace('\\', '/');
            if (!names.add(name)) return;
            byte[] bytes = Files.readAllBytes(file.toPath());
            zip.putNextEntry(new ZipEntry(name)); zip.write(bytes); zip.closeEntry();
            manifest.append(digest(bytes)).append('\t').append(bytes.length).append('\t').append(name).append('\n');
        }
    }
    public static String digest(byte[] bytes) {
        try {
            byte[] hash = MessageDigest.getInstance("SHA-256").digest(bytes); StringBuilder text = new StringBuilder();
            for (byte b : hash) text.append(String.format(Locale.ROOT, "%02x", b & 255)); return text.toString();
        } catch (NoSuchAlgorithmException e) { throw new AssertionError(e); }
    }
}
