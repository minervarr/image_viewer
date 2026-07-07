package com.example.hdrviewer;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.Gainmap;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;

import java.io.OutputStream;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.Toast;

import java.io.InputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class MainActivity extends Activity {
    
    private ImageView imageView;
    private ExecutorService executor = Executors.newSingleThreadExecutor();
    private Bitmap currentBitmap;
    private String currentBaseName = "image";

    // Used to load the 'hdrviewer' library on application startup.
    static {
        System.loadLibrary("hdrviewer");
    }

    public native String stringFromJNI();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        // Request Wide Color Gamut & HDR support natively from the window manager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            getWindow().setColorMode(ActivityInfo.COLOR_MODE_WIDE_COLOR_GAMUT);
        }
        if (Build.VERSION.SDK_INT >= 34) { // Build.VERSION_CODES.UPSIDE_DOWN_CAKE
            getWindow().setColorMode(ActivityInfo.COLOR_MODE_HDR);
        }
        
        FrameLayout rootLayout = new FrameLayout(this);
        rootLayout.setBackgroundColor(0xFF000000); // Black background
        
        imageView = new ImageView(this);
        imageView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        
        FrameLayout.LayoutParams imageParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 
                ViewGroup.LayoutParams.MATCH_PARENT);
        rootLayout.addView(imageView, imageParams);
        
        LinearLayout buttonLayout = new LinearLayout(this);
        buttonLayout.setOrientation(LinearLayout.HORIZONTAL);
        buttonLayout.setGravity(Gravity.CENTER);
        
        Button btnLeft = new Button(this);
        btnLeft.setText("Rotate Left");
        // Hardware accelerated rotation via the GPU compositor (blasting fast)
        btnLeft.setOnClickListener(v -> imageView.setRotation(imageView.getRotation() - 90));
        
        Button btnRight = new Button(this);
        btnRight.setText("Rotate Right");
        btnRight.setOnClickListener(v -> imageView.setRotation(imageView.getRotation() + 90));

        Button btnSave = new Button(this);
        btnSave.setText("Save HDR");
        btnSave.setOnClickListener(v -> startSaveUltraHdr());

        Button btnExr = new Button(this);
        btnExr.setText("Save TIFF");
        btnExr.setOnClickListener(v -> startSaveTiff());

        Button btnAvif = new Button(this);
        btnAvif.setText("Save AVIF");
        btnAvif.setOnClickListener(v -> startSaveAvif());

        // Add some margin between the buttons
        LinearLayout.LayoutParams btnParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        btnParams.setMargins(20, 0, 20, 0);

        buttonLayout.addView(btnLeft, btnParams);
        buttonLayout.addView(btnRight, btnParams);
        buttonLayout.addView(btnSave, btnParams);
        buttonLayout.addView(btnExr, btnParams);
        buttonLayout.addView(btnAvif, btnParams);
        
        FrameLayout.LayoutParams buttonParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 
                ViewGroup.LayoutParams.WRAP_CONTENT);
        buttonParams.gravity = Gravity.BOTTOM;
        buttonParams.bottomMargin = 80;
        
        rootLayout.addView(buttonLayout, buttonParams);
        
        setContentView(rootLayout);
        
        // Show JNI string as toast just to prove C++ 17 is working
        Toast.makeText(this, "C++ says: " + stringFromJNI(), Toast.LENGTH_LONG).show();

        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, 1);
    }
    
    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == 1 && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) {
                String name = uri.getLastPathSegment();
                if (name != null) {
                    int slash = name.lastIndexOf('/');
                    if (slash >= 0) name = name.substring(slash + 1);
                    int dot = name.lastIndexOf('.');
                    if (dot > 0) name = name.substring(0, dot);
                    if (!name.isEmpty()) currentBaseName = name;
                }
                loadHdr(uri);
            }
        } else if (requestCode == 2 && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) saveUltraHdr(uri);
        } else if (requestCode == 3 && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) saveTiff(uri);
        } else if (requestCode == 4 && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) saveAvif(uri);
        }
    }

    private void loadHdr(Uri uri) {
        Toast.makeText(this, "Loading Native HDR...", Toast.LENGTH_SHORT).show();
        executor.execute(() -> {
            try {
                long start = System.currentTimeMillis();
                Bitmap bitmap = HdrDecoder.decode(MainActivity.this, uri);
                long end = System.currentTimeMillis();
                currentBitmap = bitmap;
                runOnUiThread(() -> {
                    imageView.setRotation(0); // Reset rotation for new image
                    imageView.setImageBitmap(bitmap);
                    Toast.makeText(MainActivity.this, "Loaded " + bitmap.getConfig() + " in " + (end - start) + "ms", Toast.LENGTH_SHORT).show();
                });
            } catch (Exception e) {
                e.printStackTrace();
                runOnUiThread(() -> Toast.makeText(MainActivity.this, "Error: " + e.getMessage(), Toast.LENGTH_LONG).show());
            }
        });
    }

    // Let the user pick a destination, then write a lossless 32-bit-float TIFF.
    private void startSaveTiff() {
        if (currentBitmap == null) {
            Toast.makeText(this, "Load an image first", Toast.LENGTH_SHORT).show();
            return;
        }
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/tiff");
        intent.putExtra(Intent.EXTRA_TITLE, currentBaseName + ".tiff");
        startActivityForResult(intent, 3);
    }

    private void saveTiff(Uri uri) {
        final Bitmap src = currentBitmap;
        if (src == null) return;
        Toast.makeText(this, "Saving lossless TIFF...", Toast.LENGTH_SHORT).show();
        executor.execute(() -> {
            ParcelFileDescriptor pfd = null;
            try {
                if (src.getConfig() != Bitmap.Config.RGBA_F16)
                    throw new java.io.IOException("Image is not HDR (RGBA_F16)");
                pfd = getContentResolver().openFileDescriptor(uri, "w");
                if (pfd == null) throw new java.io.IOException("Could not open file descriptor");
                int rc = HdrDecoder.encodeTiffToFdNative(src, pfd.getFd());
                if (rc != 0) throw new java.io.IOException("TIFF encode failed (rc=" + rc + ")");
                runOnUiThread(() -> Toast.makeText(MainActivity.this,
                        "Saved lossless 32-bit TIFF", Toast.LENGTH_LONG).show());
            } catch (Throwable t) {
                t.printStackTrace();
                runOnUiThread(() -> Toast.makeText(MainActivity.this, "TIFF save failed: " + t.getMessage(), Toast.LENGTH_LONG).show());
            } finally {
                if (pfd != null) try { pfd.close(); } catch (Exception ignored) {}
            }
        });
    }

    // Let the user pick a destination, then write a 12-bit BT.2020 PQ AVIF there.
    private void startSaveAvif() {
        if (currentBitmap == null) {
            Toast.makeText(this, "Load an image first", Toast.LENGTH_SHORT).show();
            return;
        }
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/avif");
        intent.putExtra(Intent.EXTRA_TITLE, currentBaseName + ".avif");
        startActivityForResult(intent, 4);
    }

    private void saveAvif(Uri uri) {
        final Bitmap src = currentBitmap;
        if (src == null) return;
        Toast.makeText(this, "Encoding AVIF (HDR)… this can take a bit", Toast.LENGTH_SHORT).show();
        executor.execute(() -> {
            ParcelFileDescriptor pfd = null;
            try {
                if (src.getConfig() != Bitmap.Config.RGBA_F16)
                    throw new java.io.IOException("Image is not HDR (RGBA_F16)");
                pfd = getContentResolver().openFileDescriptor(uri, "w");
                if (pfd == null) throw new java.io.IOException("Could not open file descriptor");
                long t0 = System.currentTimeMillis();
                int rc = HdrDecoder.encodeAvifToFdNative(src, pfd.getFd(), 90);
                if (rc != 0) throw new java.io.IOException("AVIF encode failed (rc=" + rc + ")");
                final long ms = System.currentTimeMillis() - t0;
                runOnUiThread(() -> Toast.makeText(MainActivity.this,
                        "Saved 12-bit HDR AVIF (" + ms + " ms)", Toast.LENGTH_LONG).show());
            } catch (Throwable t) {
                t.printStackTrace();
                runOnUiThread(() -> Toast.makeText(MainActivity.this, "AVIF save failed: " + t.getMessage(), Toast.LENGTH_LONG).show());
            } finally {
                if (pfd != null) try { pfd.close(); } catch (Exception ignored) {}
            }
        });
    }

    // Let the user pick a destination, then write an Ultra HDR JPEG there.
    private void startSaveUltraHdr() {
        if (currentBitmap == null) {
            Toast.makeText(this, "Load an image first", Toast.LENGTH_SHORT).show();
            return;
        }
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/jpeg");
        intent.putExtra(Intent.EXTRA_TITLE, currentBaseName + "_hdr.jpg");
        startActivityForResult(intent, 2);
    }

    private void saveUltraHdr(Uri uri) {
        final Bitmap src = currentBitmap;
        if (src == null) return;
        Toast.makeText(this, "Saving Ultra HDR...", Toast.LENGTH_SHORT).show();
        executor.execute(() -> {
            Bitmap sdr = null, gain = null;
            try {
                int w = src.getWidth(), h = src.getHeight();
                Bitmap toSave = src;
                if (Build.VERSION.SDK_INT >= 34 && src.getConfig() == Bitmap.Config.RGBA_F16) {
                    sdr = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                    gain = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                    float[] meta = new float[3];
                    int rc = HdrDecoder.buildUltraHdrNative(src, sdr, gain, meta);
                    if (rc == 0) {
                        Gainmap gm = new Gainmap(gain);
                        gm.setRatioMin(1f, 1f, 1f);
                        gm.setRatioMax(Math.max(1f, meta[0]), Math.max(1f, meta[1]), Math.max(1f, meta[2]));
                        gm.setGamma(1f, 1f, 1f);
                        gm.setEpsilonSdr(0f, 0f, 0f);
                        gm.setEpsilonHdr(0f, 0f, 0f);
                        float fullHdr = Math.max(meta[0], Math.max(meta[1], meta[2]));
                        gm.setDisplayRatioForFullHdr(Math.max(1.0001f, fullHdr));
                        gm.setMinDisplayRatioForHdrTransition(1f);
                        sdr.setGainmap(gm);
                        toSave = sdr;
                    }
                }
                OutputStream os = getContentResolver().openOutputStream(uri);
                if (os == null) throw new java.io.IOException("Could not open output stream");
                boolean ok = toSave.compress(Bitmap.CompressFormat.JPEG, 95, os);
                os.flush();
                os.close();
                final boolean hdr = (toSave != src);
                runOnUiThread(() -> Toast.makeText(MainActivity.this,
                        ok ? (hdr ? "Saved Ultra HDR JPEG" : "Saved JPEG (SDR)") : "Save failed",
                        Toast.LENGTH_LONG).show());
            } catch (Throwable t) {
                t.printStackTrace();
                runOnUiThread(() -> Toast.makeText(MainActivity.this, "Save failed: " + t.getMessage(), Toast.LENGTH_LONG).show());
            } finally {
                if (sdr != null && sdr != currentBitmap) { /* keep sdr if saved? it's separate; recycle gain */ }
                if (gain != null) gain.recycle();
            }
        });
    }
}
