package com.armsx2.companion;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/** Read-only decoder for the unmodified US retail CTR (SCUS-94426).
 * Layout references: CTR-tools/CTR-ModSDK namespace_Main.h,
 * namespace_Vehicle.h, namespace_Level.h and regionsEXE.h.
 * No game code, textures or ROM bytes are bundled.
 */
public final class CtrRaceReader {
    private static final String[] TRACKS = {"Dingo Canyon", "Dragon Mines", "Blizzard Bluff",
        "Crash Cove", "Tiger Temple", "Papu's Pyramid", "Roo's Tubes", "Hot Air Skyway",
        "Sewer Speedway", "Mystery Caves", "Cortex Castle", "N. Gin Labs", "Polar Pass",
        "Oxide Station", "Coco Park", "Tiny Arena", "Slide Coliseum", "Turbo Track"};
    private static final String[] CHARACTERS = {"Crash", "Cortex", "Tiny", "Coco", "N. Gin",
        "Dingodile", "Polar", "Pura", "Pinstripe", "Papu Papu", "Ripper Roo", "Komodo Joe",
        "N. Tropy", "Penta Penguin", "Fake Crash", "Oxide"};
    public static final class Point {
        public final float x, z;
        public Point(float x, float z) { this.x = x; this.z = z; }
    }
    public static final class Edge {
        public final Point from, to;
        Edge(Point from, Point to) { this.from = from; this.to = to; }
    }
    public static final class Racer {
        public final int id, position, lap, character;
        public final String name;
        public final Point point;
        Racer(int id, int position, int lap, int character, String name, Point point) {
            this.id = id; this.position = position; this.lap = lap;
            this.name = name; this.point = point;
            this.character = character;
        }
    }
    public static final class Race {
        public final String track;
        public final List<Edge> edges;
        public final List<Racer> racers;
        public final Hud hud;
        Race(String track, List<Edge> edges, List<Racer> racers, Hud hud) {
            this.track = track; this.edges = Collections.unmodifiableList(edges);
            this.racers = Collections.unmodifiableList(racers);
            this.hud = hud;
        }
    }
    public static final class Hud {
        public final int timeTicks, lap, totalLaps, wumpa, item, itemCount, character;
        public final int driftRoom, driftMax, speed, jump, boosts;
        public final boolean wrongWay;
        Hud(int timeTicks,int lap,int totalLaps,int wumpa,int item,int itemCount,int character,
            int driftRoom,int driftMax,int speed,int jump,int boosts,boolean wrongWay){
            this.timeTicks=timeTicks;this.lap=lap;this.totalLaps=totalLaps;this.wumpa=wumpa;
            this.item=item;this.itemCount=itemCount;this.character=character;
            this.driftRoom=driftRoom;this.driftMax=driftMax;this.speed=speed;
            this.jump=jump;this.boosts=boosts;this.wrongWay=wrongWay;
        }
        public String clock(){int ticks=Math.max(0,timeTicks);return String.format(Locale.ROOT,
            "%d:%02d.%02d",ticks/57600,(ticks/960)%60,(ticks%960)*100/960);}
        public int itemIcon(){
            if(item<0 || item>16 || item==15 || item==16)return -1;
            if(item==7 && ((0xc9>>character)&1)==0)return 0x32;
            if(wumpa==10 && (item==3||item==4||item==6))return item+0x11;
            return item+5;
        }
    }
    /** Bounded diagnostic metadata only; never contains a RAM dump or modifies guest data. */
    public static final class Result {
        public final Race race;
        public final String code, reason, detail;
        Result(Race race, String code, String reason, String detail) {
            this.race = race; this.code = code; this.reason = reason; this.detail = detail;
        }
    }
    private static Result reject(String code, String reason, String detail) {
        return new Result(null, code, reason, detail);
    }
    private static String hex(int value) { return String.format(Locale.ROOT, "0x%08X", value); }
    public static boolean supported(String serial) {
        return "SCUS94426".equals(normalize(serial));
    }
    public static boolean isCtr(String serial) {
        String s = normalize(serial);
        return s.equals("SCUS94426") || s.equals("SCES02105") || s.equals("SCES02834") || s.equals("SCPS10118");
    }
    private static String normalize(String serial) {
        return serial == null ? "" : serial.trim().toUpperCase(Locale.ROOT)
            .replace("-", "").replace("_", "").replace(".", "");
    }
    /** Null means menu/loading/unsupported/corrupt data. Never guess a RAM layout. */
    public static Race read(String serial, byte[] ram) {
        return diagnose(serial, ram).race;
    }
    /** Same strict decoder as read(), with the exact failed check exposed for testing. */
    public static Result diagnose(String serial, byte[] ram) {
        if (!supported(serial)) return reject("REGION", "USA SCUS-94426 required", "serial=" + normalize(serial));
        if (ram == null || ram.length < 2 * 1024 * 1024)
            return reject("SNAPSHOT", "No complete guest RAM snapshot", "bytes=" + (ram == null ? 0 : ram.length));
        String stage = "Game tracker";
        String detail = "RAM bytes=" + ram.length;
        try {
            Memory m = new Memory(ram);
            int rawGt = m.i32(0x8d2ac);
            detail = "gGT=" + hex(rawGt);
            int gt = m.address(rawGt, 0x2534);
            int mode = m.i32(gt);
            detail += " mode=" + hex(mode);
            if ((mode & (0x20 | 0x2000 | 0x100000 | 0x20000000 | 0x40000000)) != 0)
                return reject("RACE_STATE", "Menu, hub, battle, cutscene or loading flag", detail);
            int track = m.i32(gt + 0x1a10);
            detail += " track=" + track;
            if (track < 0 || track >= TRACKS.length)
                return reject("TRACK", "Circuit track ID outside 0-17", detail);
            int players = m.u8(gt + 0x1ca8);
            int bots = m.u8(gt + 0x1caa);
            detail += " players=" + players + " bots=" + bots;
            if (players < 1 || players > 4)
                return reject("RACER_COUNT", "Invalid configured player count", detail);
            // Live US Arcade can report bots=0 while all seven AI drivers are active.
            // The indexed driver table is authoritative, not that configured bot counter.
            // Validate every occupied slot before using it to require a complete rank table.
            stage = "Indexed driver table";
            int[] drivers = new int[8];
            int expectedRacers = 0;
            for (int id = 0; id < drivers.length; id++) {
                int raw = m.i32(gt + 0x24ec + id * 4);
                drivers[id] = raw;
                if (raw == 0) continue;
                int driver = m.address(raw, 0x638);
                if (m.u8(driver + 0x4a) != id)
                    return reject("DRIVER_TABLE", "Indexed slot and driver ID disagree",
                        detail + " slot=" + id + " driver=" + hex(raw));
                expectedRacers++;
            }
            detail += " active=" + expectedRacers;
            if (expectedRacers < players)
                return reject("RACER_COUNT", "Active drivers do not include all players", detail);
            for (int id = 0; id < players; id++) {
                if (drivers[id] == 0)
                    return reject("DRIVER_TABLE", "Player missing from indexed driver table",
                        detail + " slot=" + id);
            }
            stage = "Level/checkpoint pointers";
            int level = m.pointer(gt + 0x160, 0x150);
            int count = m.i32(level + 0x148);
            detail += " nodes=" + count;
            if (count < 3 || count > 255)
                return reject("NODE_COUNT", "Checkpoint count outside 3-255", detail);
            int nodes = m.pointer(level + 0x14c, count * 12);
            stage = "Checkpoint route";
            List<Point> points = new ArrayList<>();
            for (int i = 0; i < count; i++)
                points.add(new Point(m.i16(nodes + i * 12), m.i16(nodes + i * 12 + 4)));
            List<Edge> edges = new ArrayList<>();
            for (int i = 0; i < count; i++) {
                // Explicit forward/left/right links preserve shortcuts; don't connect array order.
                for (int offset : new int[]{8, 9, 11}) {
                    int next = m.u8(nodes + i * 12 + offset);
                    if (next < count && next != i) edges.add(new Edge(points.get(i), points.get(next)));
                }
            }
            if (edges.isEmpty()) return reject("ROUTE", "No valid checkpoint route links", detail);
            List<Racer> racers = new ArrayList<>();
            boolean[] seen = new boolean[8];
            for (int rank = 0; rank < 8; rank++) {
                int raw = m.i32(gt + 0x250c + rank * 4);
                if (raw == 0) continue;
                stage = "Rank " + (rank + 1) + " driver";
                String driverDetail = detail + " rank=" + (rank + 1) + " driver=" + hex(raw);
                int driver = m.address(raw, 0x638);
                int id = m.u8(driver + 0x4a);
                driverDetail += " id=" + id;
                if (id >= 8 || seen[id])
                    return reject("DRIVER_ID", "Invalid or duplicate driver ID", driverDetail);
                // Validate against the indexed driver table to reject stale loading data.
                int indexed = drivers[id];
                if (indexed != raw)
                    return reject("DRIVER_TABLE", "Rank and indexed driver tables disagree",
                        driverDetail + " indexed=" + hex(indexed));
                seen[id] = true;
                int character = m.i16(0x86e84 + id * 2);
                driverDetail += " char=" + character;
                if (character < 0 || character >= CHARACTERS.length)
                    return reject("CHARACTER", "Character ID outside 0-15", driverDetail);
                int lap = m.u8(driver + 0x44);
                driverDetail += " lap=" + lap;
                if (lap > 127) return reject("LAP", "Lap value rejected by the current layout", driverDetail);
                float x = m.i32(driver + 0x2d4) / 256f;
                float z = m.i32(driver + 0x2dc) / 256f;
                if (Math.abs(x) > 65536 || Math.abs(z) > 65536)
                    return reject("COORDINATES", "Racer coordinates outside expected bounds",
                        driverDetail + " x=" + x + " z=" + z);
                racers.add(new Racer(id, rank + 1, lap + 1, character, CHARACTERS[character], new Point(x, z)));
            }
            if (racers.size() != expectedRacers)
                return reject("RANK_COUNT", "Rank table does not contain all active drivers",
                    detail + " decoded=" + racers.size());
            int player=m.address(drivers[0],0x638);
            Hud hud=new Hud(m.i32(player+0x514),m.u8(player+0x44)+1,m.u8(gt+0x1d33),
                m.u8(player+0x30),m.u8(player+0x36),m.u8(player+0x37),m.i16(0x86e84),
                m.i16(player+0x3dc),m.u8(player+0x476)*Math.max(0,Math.min(128,m.i32(gt+0x1d04))),
                m.i16(player+0x38e),m.i16(player+0x46),m.i16(player+0x4e),
                (m.i32(player+0x2c8)&0x100)!=0 && m.i32(player+0x490)>=501);
            return new Result(new Race(TRACKS[track], edges, racers, hud), "OK", "Race decoded",
                "track=" + track + " racers=" + racers.size() + " nodes=" + count);
        } catch (IllegalArgumentException malformed) {
            return reject("POINTER", "Invalid guest pointer/data at " + stage,
                detail + " " + malformed.getMessage());
        }
    }
    private static final class Memory {
        final byte[] bytes;
        Memory(byte[] bytes) { this.bytes = bytes; }
        int address(int raw, int length) {
            // Only guest RAM addresses, not hardware registers or ROM aliases.
            int region = raw >>> 24;
            if (region != 0x80 && region != 0xa0) throw new IllegalArgumentException("pointer=" + hex(raw));
            int offset = raw & 0x1fffffff;
            if ((offset & 3) != 0 || offset < 0 || offset > Math.min(bytes.length, 2 * 1024 * 1024) - length)
                throw new IllegalArgumentException("pointer=" + hex(raw) + " length=" + length);
            return offset;
        }
        int pointer(int offset, int length) { return address(i32(offset), length); }
        int u8(int offset) { check(offset, 1); return bytes[offset] & 255; }
        int i16(int offset) { return (short)(u8(offset) | u8(offset + 1) << 8); }
        int i32(int offset) { return u8(offset) | u8(offset+1)<<8 | u8(offset+2)<<16 | u8(offset+3)<<24; }
        void check(int offset, int length) {
            if (offset < 0 || offset > bytes.length - length)
                throw new IllegalArgumentException("offset=" + hex(offset) + " length=" + length);
        }
    }
}
