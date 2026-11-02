package com.lscardinali.bf2mc;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

// XDVDFS (the Xbox disc filesystem) reader: finds the volume descriptor at
// the start of the image (trimmed XISO) or at the game partition of a full disc dump, walks the directory trees and
// copies files out. Plain Java on a FileChannel (no Android classes), so it also runs on the PC.
public final class Xdvdfs {
    public static final int SECTOR = 2048;
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
    private static final long[] PARTITION_OFFSETS = {0, 0x0FD90000L, 0x02080000L, 0x18300000L};
    private static final int MAX_TABLE = 16 << 20;  // a directory table larger than this is a corrupt image
    private static final int CHUNK = 1 << 20;

    public static final class Entry {
        public final String path;  // relative, '/' separated
        public final long sector;
        public final long size;
        public final boolean dir;

        Entry(String path, long sector, long size, boolean dir) {
            this.path = path;
            this.sector = sector;
            this.size = size;
            this.dir = dir;
        }
    }

    public interface Progress {
        // bytes copied of the current file; returns false to cancel
        boolean onBytes(long n);
    }

    public static final class NotXboxDiscException extends IOException {
        NotXboxDiscException(String msg) {
            super(msg);
        }
    }

    private final FileChannel ch;
    public final long base;
    private final long rootSector;
    private final long rootSize;

    public Xdvdfs(FileChannel ch) throws IOException {
        this.ch = ch;
        for (long b : PARTITION_OFFSETS) {
            ByteBuffer head = read(b + 32L * SECTOR, SECTOR, false);
            if (head == null || !magicAt(head, 0) || !magicAt(head, 0x7EC)) continue;
            base = b;
            rootSector = head.getInt(20) & 0xFFFFFFFFL;
            rootSize = head.getInt(24) & 0xFFFFFFFFL;
            return;
        }
        throw new NotXboxDiscException("no XDVDFS volume descriptor found");
    }

    private static boolean magicAt(ByteBuffer b, int off) {
        for (int i = 0; i < MAGIC.length; i++)
            if (b.get(off + i) != MAGIC[i]) return false;
        return true;
    }

    // reads n bytes at pos; null on a short read when !strict
    private ByteBuffer read(long pos, int n, boolean strict) throws IOException {
        ByteBuffer b = ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN);
        while (b.hasRemaining()) {
            int r = ch.read(b, pos + b.position());
            if (r < 0) {
                if (strict) throw new IOException("short read at 0x" + Long.toHexString(pos));
                return null;
            }
        }
        b.flip();
        return b;
    }

    // every directory and file, parents before children
    public List<Entry> list() throws IOException {
        List<Entry> out = new ArrayList<>();
        walk(rootSector, rootSize, "", out, new HashSet<>(), 0);
        return out;
    }

    private void walk(long sector, long size, String path, List<Entry> out, Set<Long> tables, int depth)
            throws IOException {
        if (size == 0) return;
        if (size > MAX_TABLE || depth > 32 || !tables.add(sector))
            throw new NotXboxDiscException("corrupt directory table at sector " + sector);
        ByteBuffer table = read(base + sector * SECTOR, (int) size, true);
        Set<Integer> seen = new HashSet<>();
        ArrayList<Integer> stack = new ArrayList<>();
        stack.add(0);
        while (!stack.isEmpty()) {
            int off = stack.remove(stack.size() - 1);
            if (off + 14 > table.limit() || !seen.add(off)) continue;
            int left = table.getShort(off) & 0xFFFF;
            int right = table.getShort(off + 2) & 0xFFFF;
            if (left == 0xFFFF && right == 0xFFFF) continue;  // sector padding
            long start = table.getInt(off + 4) & 0xFFFFFFFFL;
            long length = table.getInt(off + 8) & 0xFFFFFFFFL;
            int attr = table.get(off + 12) & 0xFF;
            int nameLen = table.get(off + 13) & 0xFF;
            if (off + 14 + nameLen > table.limit()) continue;
            byte[] raw = new byte[nameLen];
            for (int i = 0; i < nameLen; i++) raw[i] = table.get(off + 14 + i);
            String name = new String(raw, StandardCharsets.ISO_8859_1);
            if (left != 0) stack.add(left * 4);
            if (right != 0) stack.add(right * 4);
            if (name.isEmpty() || name.equals(".") || name.equals("..") || name.indexOf('/') >= 0
                    || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0)
                throw new NotXboxDiscException("bad file name in directory table at sector " + sector);
            String rel = path.isEmpty() ? name : path + "/" + name;
            boolean dir = (attr & 0x10) != 0;
            out.add(new Entry(rel, start, length, dir));
            if (dir) walk(start, length, rel, out, tables, depth + 1);
        }
    }

    // copies one file to dst (through dst.part, renamed when complete); md, if set, digests its bytes.
    // returns false if progress cancelled it
    public boolean extract(Entry e, File dst, MessageDigest md, Progress progress) throws IOException {
        File part = new File(dst.getPath() + ".part");
        ByteBuffer buf = ByteBuffer.allocateDirect((int) Math.min(CHUNK, Math.max(e.size, 1)));
        long pos = base + e.sector * SECTOR;
        long left = e.size;
        boolean cancelled = false;
        try (FileOutputStream os = new FileOutputStream(part); FileChannel out = os.getChannel()) {
            while (left > 0 && !cancelled) {
                buf.clear();
                if (left < buf.capacity()) buf.limit((int) left);
                int r = ch.read(buf, pos);
                if (r < 0) throw new IOException("short read in " + e.path);
                buf.flip();
                if (md != null) {
                    md.update(buf);
                    buf.rewind();
                }
                while (buf.hasRemaining()) out.write(buf);
                pos += r;
                left -= r;
                if (progress != null && !progress.onBytes(r)) cancelled = true;
            }
        }
        if (cancelled) {
            part.delete();
            return false;
        }
        if (!part.renameTo(dst)) throw new IOException("can't rename " + part);
        return true;
    }
}
