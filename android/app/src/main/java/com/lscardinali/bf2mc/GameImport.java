package com.lscardinali.bf2mc;

import java.io.File;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.List;

// Extracts the Battlefield 2: Modern Combat disc image into <files>/game: default.xex first (its SHA-256 must be the
// one the game was recompiled from, see README), then the rest, all into <files>/game.import, renamed to game only
// once complete, so an interrupted import never looks like an installed game. What only multiplayer or the console
// uses stays on the disc. Plain Java (no Android classes) so it also runs on the PC.
public final class GameImport {
    public static final String GAME = "game";
    public static final String TEMP = "game.import";
    public static final String XEX = "default.xex";
    private static final String SHA256 = "a2e791b578ef15420f3737b48999ca4285fdf85c85cb6732aac0e465ed9d5484";
    private static final String[] UNUSED = {"bf2mc_mp", "mpdefault.xex", "$SystemUpdate"};
    private static final long SPARE = 64L << 20;  // free space to leave beyond the game

    public interface Listener {
        // a file started or advanced; done/total are bytes over the whole import. returns false to cancel
        boolean onProgress(String file, int index, int count, long done, long total);
    }

    public static final class WrongDiscException extends IOException {
        public final String sha256;

        WrongDiscException(String msg, String sha256) {
            super(msg);
            this.sha256 = sha256;
        }
    }

    public static final class NoSpaceException extends IOException {
        public final long needed, free;

        NoSpaceException(long needed, long free) {
            super("not enough free space: " + needed + " bytes needed, " + free + " free");
            this.needed = needed;
            this.free = free;
        }
    }

    // true if a game folder is ready in dir (the same check app_main.cpp makes)
    public static boolean installed(File dir) {
        return dir != null && new File(new File(dir, GAME), XEX).isFile();
    }

    public static void deleteTree(File f) {
        File[] kids = f.isDirectory() ? f.listFiles() : null;
        if (kids != null)
            for (File k : kids) deleteTree(k);
        f.delete();
    }

    // returns false if cancelled (the temp folder is removed); throws on a bad image or I/O error
    public static boolean run(FileChannel image, File filesDir, Listener listener) throws IOException {
        File temp = new File(filesDir, TEMP);
        deleteTree(temp);
        try {
            if (!extractAll(image, temp, filesDir, listener)) {
                deleteTree(temp);
                return false;
            }
        } catch (IOException | RuntimeException e) {
            deleteTree(temp);
            throw e;
        }
        File game = new File(filesDir, GAME);
        deleteTree(game);  // not a complete game (no default.xex), or we would not be importing
        if (!temp.renameTo(game)) throw new IOException("can't rename " + temp + " to " + game);
        return true;
    }

    private static boolean used(Xdvdfs.Entry e) {
        String top = e.path.contains("/") ? e.path.substring(0, e.path.indexOf('/')) : e.path;
        for (String u : UNUSED)
            if (top.equalsIgnoreCase(u)) return false;
        return true;
    }

    private static boolean extractAll(FileChannel image, File temp, File filesDir, Listener listener)
            throws IOException {
        Xdvdfs fs = new Xdvdfs(image);
        List<Xdvdfs.Entry> entries = new ArrayList<>();
        Xdvdfs.Entry xex = null;
        long total = 0;
        int count = 0;
        for (Xdvdfs.Entry e : fs.list()) {
            if (!used(e)) continue;
            entries.add(e);
            if (e.dir) continue;
            total += e.size;
            count++;
            if (e.path.equalsIgnoreCase(XEX)) xex = e;
        }
        if (xex == null) throw new WrongDiscException("no default.xex on this disc", null);
        long free = filesDir.getUsableSpace();
        if (total + SPARE > free) throw new NoSpaceException(total + SPARE, free);
        if (!temp.mkdirs()) throw new IOException("can't create " + temp);

        final long[] done = {0};
        final long fTotal = total;
        final int fCount = count;
        final int[] index = {1};
        final String[] name = {XEX};
        Xdvdfs.Progress progress = n -> {
            done[0] += n;
            return listener == null || listener.onProgress(name[0], index[0], fCount, done[0], fTotal);
        };

        // default.xex first: a wrong disc fails in a second, not after copying gigabytes
        MessageDigest md;
        try {
            md = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        if (listener != null && !listener.onProgress(XEX, 1, count, 0, total)) return false;
        if (!fs.extract(xex, new File(temp, XEX), md, progress)) return false;
        String sha = hex(md.digest());
        if (!sha.equals(SHA256)) throw new WrongDiscException("default.xex is not the expected release's", sha);

        for (Xdvdfs.Entry e : entries) {
            File dst = new File(temp, e.path);
            if (e.dir) {
                if (!dst.isDirectory() && !dst.mkdirs()) throw new IOException("can't create " + dst);
                continue;
            }
            if (e == xex) continue;
            index[0]++;
            name[0] = e.path;
            if (listener != null && !listener.onProgress(e.path, index[0], count, done[0], total)) return false;
            File parent = dst.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                throw new IOException("can't create " + parent);
            if (!fs.extract(e, dst, null, progress)) return false;
        }
        return true;
    }

    private static String hex(byte[] b) {
        StringBuilder s = new StringBuilder(b.length * 2);
        for (byte x : b) s.append(String.format("%02x", x & 0xFF));
        return s.toString();
    }
}
