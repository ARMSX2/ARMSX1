// SPDX-License-Identifier: GPL-3.0-or-later
package com.armsx2.core;

import java.io.File;
import java.io.FileInputStream;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** Resolves a console model using verified BIOS identities, never filenames. */
public final class Ps1BiosSelection {
    private Ps1BiosSelection() {}

    public static boolean matches(Ps1BiosProbe.Info info, String model) {
        if (info == null || model == null) return false;
        String wanted = model.toUpperCase(Locale.ROOT).replace("SCPH-", "");
        // The old model grid distinguishes the two known PAL PSone revisions.
        if (wanted.equals("102A")) return info.region == 2 && info.version == 0x428;
        if (wanted.equals("102B")) return info.region == 2 && info.version == 0x432;
        String consoles = info.description.split("\\(", 2)[0];
        for (String part : consoles.split(",")) {
            String name = part.trim().toUpperCase(Locale.ROOT).replace("SCPH-", "");
            if (name.equals(wanted)) return true;
        }
        return false;
    }

    public static File resolve(String model, Iterable<File> directories, Iterable<File> legacyFiles) {
        return resolve(model, directories, legacyFiles, file -> {
            try (FileInputStream input = new FileInputStream(file)) {
                return Ps1BiosProbe.probe(input);
            }
        });
    }

    interface Probe { Ps1BiosProbe.Info read(File file) throws java.io.IOException; }

    static File resolve(String model, Iterable<File> directories, Iterable<File> legacyFiles, Probe probe) {
        Set<File> candidates = new LinkedHashSet<>();
        for (File directory : directories) {
            if (directory == null) continue;
            File[] files;
            try { files = directory.listFiles(); } catch (SecurityException ignored) { continue; }
            if (files == null) continue;
            List<File> ordered = new ArrayList<>();
            java.util.Collections.addAll(ordered, files);
            ordered.sort(Comparator.comparing(File::getAbsolutePath));
            candidates.addAll(ordered);
        }
        // Keep old imports available, but they have no priority over the selected model.
        for (File file : legacyFiles) if (file != null) candidates.add(file);
        for (File file : candidates) {
            try {
                if (!file.isFile() || file.length() != Ps1BiosProbe.IMAGE_SIZE) continue;
                if (matches(probe.read(file), model)) return file;
            } catch (java.io.IOException | SecurityException ignored) {
                // A stale/unreadable import must not stop the remaining choices being checked.
            }
        }
        return null; // No matching dump: let SwanStation's region-aware auto-selector run.
    }
}
