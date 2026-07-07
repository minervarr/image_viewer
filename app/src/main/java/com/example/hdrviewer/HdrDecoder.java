package com.example.hdrviewer;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ColorSpace;
import android.graphics.ImageDecoder;
import android.net.Uri;
import android.os.Build;
import android.util.Half;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.ShortBuffer;

public class HdrDecoder {
    
    static final short[] VALUE_TABLE = new short[256 * 256];
    static final short HALF_ONE;
    
    static {
        HALF_ONE = Half.toHalf(1.0f);
        for (int e = 0; e < 256; e++) {
            float f = (float) Math.pow(2.0, e - 128 - 8);
            int eShift = e << 8;
            for (int val = 0; val < 256; val++) {
                VALUE_TABLE[eShift | val] = Half.toHalf(val * f);
            }
        }
    }
    
    static class ExposedBuffer {
        byte[] data;
        int length;
        ExposedBuffer(byte[] d, int l) { data = d; length = l; }
    }
    
    public static ExposedBuffer readAllBytes(InputStream is) throws IOException {
        int capacity = Math.max(1024 * 1024, is.available()); // Start with 1MB or available
        byte[] buf = new byte[capacity];
        int count = 0;
        int n;
        while (true) {
            if (count == buf.length) {
                byte[] newBuf = new byte[buf.length + (buf.length >> 1)];
                System.arraycopy(buf, 0, newBuf, 0, count);
                buf = newBuf;
            }
            n = is.read(buf, count, buf.length - count);
            if (n == -1) break;
            count += n;
        }
        return new ExposedBuffer(buf, count);
    }
    
    private static class FastReader {
        byte[] data;
        int length;
        int pos;
        
        FastReader(ExposedBuffer eb) {
            this.data = eb.data;
            this.length = eb.length;
            pos = 0;
        }
        
        int read() {
            if (pos >= length) return -1;
            return data[pos++] & 0xFF;
        }
        
        String readLine() {
            StringBuilder sb = new StringBuilder();
            int c;
            while ((c = read()) != -1) {
                if (c == '\n') break;
                if (c != '\r') sb.append((char) c);
            }
            return sb.toString();
        }
        
        int getPos() { return pos; }
        void setPos(int p) { pos = p; }
    }
    
    public static native short[] decodeExrNative(byte[] exrData, int length, int[] outDimens) throws IOException;

    // Decodes a raw CFA-mosaic DNG (our camera's output) directly: demosaic +
    // white balance + colour, returning linear FP16 RGBA. Returns null for DNGs
    // we don't handle (e.g. compressed / preview-only), so callers can fall back.
    public static native short[] decodeDngNative(byte[] dngData, int length, int[] outDimens);

    // Builds an Ultra HDR gain map from the displayed RGBA_F16 bitmap: fills the
    // SDR base (ARGB_8888) and gain map (ARGB_8888) bitmaps, returns per-channel
    // ratioMax in meta[0..2]. Returns 0 on success. Caller attaches a Gainmap and
    // compresses to JPEG (API 34+) to produce the Ultra HDR file.
    public static native int buildUltraHdrNative(Bitmap srcF16, Bitmap sdrOut, Bitmap gainOut, float[] meta);

    // Writes the displayed RGBA_F16 bitmap as a lossless 32-bit-float RGB TIFF
    // straight to the given file descriptor. Returns 0 on success.
    public static native int encodeTiffToFdNative(Bitmap srcF16, int fd);

    // Encodes the displayed RGBA_F16 bitmap as a 12-bit BT.2020 PQ AVIF (AV1)
    // straight to the file descriptor. quality 0..100 (100 = lossless). Returns 0
    // on success.
    public static native int encodeAvifToFdNative(Bitmap srcF16, int fd, int quality);
    
    public static Bitmap decode(Context context, Uri uri) throws IOException {
        String filename = uri.getLastPathSegment();
        boolean isDng = filename != null && filename.toLowerCase().endsWith(".dng");
        
        // Fast path for DNG files to avoid allocating byte buffers
        if (isDng) {
            return decodeDng(context, uri);
        }
        
        InputStream is = context.getContentResolver().openInputStream(uri);
        if (is == null) throw new IOException("Could not open InputStream for URI");
        ExposedBuffer eb = readAllBytes(is);
        is.close();
        
        boolean isExr = false;
        if (filename != null && filename.toLowerCase().endsWith(".exr")) {
            isExr = true;
        } else if (eb.length > 4 && eb.data[0] == 0x76 && eb.data[1] == 0x2f && eb.data[2] == 0x31 && eb.data[3] == 0x01) {
            isExr = true; // OpenEXR magic number
        } else if (eb.length > 4 && 
                  ((eb.data[0] == 0x49 && eb.data[1] == 0x49 && eb.data[2] == 0x2a && eb.data[3] == 0x00) ||
                   (eb.data[0] == 0x4d && eb.data[1] == 0x4d && eb.data[2] == 0x00 && eb.data[3] == 0x2a))) {
            // TIFF/DNG magic number fallback
            return decodeDng(context, uri);
        }
        
        if (isExr) {
            int[] dimens = new int[2];
            short[] fp16Pixels = decodeExrNative(eb.data, eb.length, dimens);
            if (fp16Pixels == null) throw new IOException("EXR native decode returned null");
            
            ShortBuffer buffer = ShortBuffer.wrap(fp16Pixels);
            Bitmap bitmap = Bitmap.createBitmap(dimens[0], dimens[1], Bitmap.Config.RGBA_F16, true, ColorSpace.get(ColorSpace.Named.LINEAR_EXTENDED_SRGB));
            bitmap.copyPixelsFromBuffer(buffer);
            return bitmap;
        }
        
        FastReader reader = new FastReader(eb);
        String magic = reader.readLine();
        if (!magic.startsWith("#?RADIANCE") && !magic.startsWith("#?")) {
            throw new IOException("Not a valid HDR, EXR, or DNG file");
        }
        
        while (true) {
            String line = reader.readLine();
            if (line.isEmpty()) break;
        }
        
        String resString = reader.readLine();
        String[] parts = resString.trim().split("\\s+");
        if (parts.length < 4) throw new IOException("Invalid HDR resolution format");
        
        int height = Integer.parseInt(parts[1]);
        int width = Integer.parseInt(parts[3]);
        
        ShortBuffer buffer = ByteBuffer.allocateDirect(width * height * 8)
                .order(ByteOrder.nativeOrder()).asShortBuffer();
                
        byte[] scanline = new byte[width * 4];
        
        for (int y = 0; y < height; y++) {
            int savedPos = reader.getPos();
            int r = reader.read();
            int g = reader.read();
            int b = reader.read();
            int e = reader.read();
            
            if (r == 2 && g == 2 && width >= 8 && width <= 32767) {
                for (int i = 0; i < 4; i++) {
                    int x = 0;
                    while (x < width) {
                        int code = reader.read();
                        if (code > 128) {
                            int runLen = code & 127;
                            int val = reader.read();
                            for (int j = 0; j < runLen; j++) {
                                scanline[i * width + x++] = (byte) val;
                            }
                        } else {
                            for (int j = 0; j < code; j++) {
                                scanline[i * width + x++] = (byte) reader.read();
                            }
                        }
                    }
                }
                
                int destY = y * width * 4;
                for (int x = 0; x < width; x++) {
                    rgbeToFP16(
                        scanline[x] & 0xFF,
                        scanline[width + x] & 0xFF,
                        scanline[2 * width + x] & 0xFF,
                        scanline[3 * width + x] & 0xFF,
                        buffer, destY + x * 4
                    );
                }
            } else {
                reader.setPos(savedPos);
                int destY = y * width * 4;
                for (int x = 0; x < width; x++) {
                    rgbeToFP16(reader.read(), reader.read(), reader.read(), reader.read(), buffer, destY + x * 4);
                }
            }
        }
        
        buffer.rewind();
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.RGBA_F16, true, ColorSpace.get(ColorSpace.Named.LINEAR_EXTENDED_SRGB));
        bitmap.copyPixelsFromBuffer(buffer);
        return bitmap;
    }
    
    private static Bitmap decodeDng(Context context, Uri uri) throws IOException {
        // First: decode the real CFA mosaic ourselves. Android's RAW engine won't
        // render a headless (preview-less) CFA DNG, which is what our camera writes.
        // The file is read-only here; no sensor data is modified or discarded.
        try {
            InputStream rawIs = context.getContentResolver().openInputStream(uri);
            if (rawIs != null) {
                ExposedBuffer eb = readAllBytes(rawIs);
                rawIs.close();
                int[] dimens = new int[2];
                short[] fp16 = decodeDngNative(eb.data, eb.length, dimens);
                if (fp16 != null && dimens[0] > 0 && dimens[1] > 0) {
                    ShortBuffer buffer = ShortBuffer.wrap(fp16);
                    Bitmap bmp = Bitmap.createBitmap(dimens[0], dimens[1], Bitmap.Config.RGBA_F16,
                            true, ColorSpace.get(ColorSpace.Named.LINEAR_EXTENDED_SRGB));
                    bmp.copyPixelsFromBuffer(buffer);
                    return bmp;
                }
            }
        } catch (Throwable t) {
            // Fall through to Android's decoder (e.g. for Samsung DNGs with a preview).
            t.printStackTrace();
        }

        Bitmap bitmap = null;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            try {
                ImageDecoder.Source source = ImageDecoder.createSource(context.getContentResolver(), uri);
                bitmap = ImageDecoder.decodeBitmap(source, (decoder, info, src) -> {
                    decoder.setTargetColorSpace(ColorSpace.get(ColorSpace.Named.LINEAR_EXTENDED_SRGB));
                });
            } catch (Exception e) {
                e.printStackTrace();
            }
        }
        
        if (bitmap == null) {
            InputStream dngIs = context.getContentResolver().openInputStream(uri);
            if (dngIs != null) {
                BitmapFactory.Options options = new BitmapFactory.Options();
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    options.inPreferredConfig = Bitmap.Config.RGBA_F16;
                    options.inPreferredColorSpace = ColorSpace.get(ColorSpace.Named.LINEAR_EXTENDED_SRGB);
                }
                bitmap = BitmapFactory.decodeStream(dngIs, null, options);
                dngIs.close();
            }
        }
        
        if (bitmap == null) {
            throw new IOException("Failed to decode DNG natively. The file may be corrupt or unsupported by Android's RAW engine.");
        }
        return bitmap;
    }
    
    private static void rgbeToFP16(int r, int g, int b, int e, ShortBuffer buffer, int index) {
        if (e == 0) {
            buffer.put(index, (short) 0);
            buffer.put(index + 1, (short) 0);
            buffer.put(index + 2, (short) 0);
            buffer.put(index + 3, HALF_ONE);
            return;
        }
        
        int eShift = e << 8;
        buffer.put(index, VALUE_TABLE[eShift | r]);
        buffer.put(index + 1, VALUE_TABLE[eShift | g]);
        buffer.put(index + 2, VALUE_TABLE[eShift | b]);
        buffer.put(index + 3, HALF_ONE);
    }
}
