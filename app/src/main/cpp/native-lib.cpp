#include <jni.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <unistd.h>
#include <android/log.h>
#include <android/bitmap.h>
#include <avif/avif.h>

#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#define TINYEXR_USE_THREAD 1
#include "tinyexr.h"

#define DNG_LOG(...) __android_log_print(ANDROID_LOG_INFO, "DngRaw", __VA_ARGS__)

// Fast Float to Half conversion (approximate, sufficient for Android FP16)
static inline uint16_t float_to_half(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(float));
    uint32_t sign = (x >> 16) & 0x8000;
    uint32_t val = (x & 0x7fffffff) + 0x1000;
    if (val >= 0x47800000) {
        if ((x & 0x7fffffff) >= 0x47800000) {
            if (val < 0x7f800000) return sign | 0x7c00; // Infinity
            return sign | 0x7c00 | ((x >> 13) & 0x3ff); // NaN
        }
        return sign | 0x7bff; // Max positive normal
    }
    if (val >= 0x38800000) return sign | ((val - 0x38000000) >> 13); // Normal
    if (val < 0x33000000) return sign; // Zero
    val = (x & 0x7fffffff) >> 23;
    return sign | ( (((x & 0x7fffff) | 0x800000) + (0x800000 >> (val - 102))) >> (126 - val) ); // Subnormal
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_hdrviewer_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    std::string hello = "Hello from C++17! NDK skeleton is ready.";
    return env->NewStringUTF(hello.c_str());
}

extern "C" JNIEXPORT jshortArray JNICALL
Java_com_example_hdrviewer_HdrDecoder_decodeExrNative(
        JNIEnv* env,
        jclass /* clazz */,
        jbyteArray exrData,
        jint length,
        jintArray outDimens) {
        
    jbyte* memory = env->GetByteArrayElements(exrData, nullptr);

    float* out_rgba = nullptr;
    int width = 0;
    int height = 0;
    const char* err = nullptr;

    // Decode OpenEXR from RAM buffer using tinyexr
    int ret = LoadEXRFromMemory(&out_rgba, &width, &height, reinterpret_cast<const unsigned char*>(memory), length, &err);
    
    env->ReleaseByteArrayElements(exrData, memory, JNI_ABORT);

    if (ret != TINYEXR_SUCCESS) {
        if (err) {
            jclass ioException = env->FindClass("java/io/IOException");
            env->ThrowNew(ioException, err);
            FreeEXRErrorMessage(err);
        } else {
            jclass ioException = env->FindClass("java/io/IOException");
            env->ThrowNew(ioException, "Failed to parse OpenEXR");
        }
        return nullptr;
    }

    // Pass dimensions back to Java
    jint dimens[2] = { width, height };
    env->SetIntArrayRegion(outDimens, 0, 2, dimens);

    // Convert float32 RGBA to float16 (FP16/Half) natively for Android wide color gamut support
    int numPixels = width * height;
    std::vector<uint16_t> fp16Pixels(numPixels * 4);
    for (int i = 0; i < numPixels * 4; ++i) {
        fp16Pixels[i] = float_to_half(out_rgba[i]);
    }
    
    free(out_rgba); // allocated by tinyexr

    jshortArray result = env->NewShortArray(numPixels * 4);
    env->SetShortArrayRegion(result, 0, numPixels * 4, reinterpret_cast<const jshort*>(fp16Pixels.data()));

    return result;
}

// ---------------------------------------------------------------------------
// Native RAW DNG decoder.
//
// Android's stock RAW engine (ImageDecoder/BitmapFactory -> dng_sdk) refuses to
// render a headless CFA mosaic with no embedded preview, which is exactly what
// the camera app writes (it keeps the DNG as pure, maximal sensor data). So we
// decode the real Bayer mosaic ourselves: parse the TIFF, demosaic, apply
// black/white levels + white balance + the camera color matrix, and emit linear
// FP16 RGBA into the viewer's wide-gamut pipeline. The DNG is read-only; no
// sensor data is ever discarded.
// ---------------------------------------------------------------------------
namespace dngraw {

static inline uint16_t rdu16(const uint8_t* p, bool le) {
    return le ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)(p[1] | (p[0] << 8));
}
static inline uint32_t rdu32(const uint8_t* p, bool le) {
    return le ? (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24))
              : (uint32_t)(p[3] | (p[2] << 8) | (p[1] << 16) | ((uint32_t)p[0] << 24));
}
static inline uint32_t typeSize(uint16_t t) {
    switch (t) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: return 4;
        case 5: case 10: case 12: return 8;
        default: return 1;
    }
}

struct Entry { uint16_t tag, type; uint32_t count; const uint8_t* val; };

static const uint8_t* valuePtr(const uint8_t* base, size_t n, const uint8_t* e12, bool le) {
    uint16_t type = rdu16(e12 + 2, le);
    uint32_t count = rdu32(e12 + 4, le);
    uint64_t sz = (uint64_t)typeSize(type) * count;
    if (sz <= 4) return e12 + 8;
    uint32_t off = rdu32(e12 + 8, le);
    if ((uint64_t)off + sz > n) return nullptr;
    return base + off;
}

static bool parseIFD0(const uint8_t* d, size_t n, std::vector<Entry>& out, bool& le) {
    if (n < 8) return false;
    if (d[0] == 'I' && d[1] == 'I') le = true;
    else if (d[0] == 'M' && d[1] == 'M') le = false;
    else return false;
    if (rdu16(d + 2, le) != 42) return false;
    uint32_t off = rdu32(d + 4, le);
    if ((uint64_t)off + 2 > n) return false;
    uint16_t cnt = rdu16(d + off, le);
    if ((uint64_t)off + 2 + (uint64_t)cnt * 12 + 4 > n) return false;
    for (uint16_t i = 0; i < cnt; ++i) {
        const uint8_t* e = d + off + 2 + (size_t)i * 12;
        Entry en;
        en.tag = rdu16(e, le); en.type = rdu16(e + 2, le); en.count = rdu32(e + 4, le);
        en.val = valuePtr(d, n, e, le);
        if (en.val) out.push_back(en);
    }
    return true;
}

static const Entry* find(const std::vector<Entry>& es, uint16_t tag) {
    for (auto& e : es) if (e.tag == tag) return &e;
    return nullptr;
}

static double valAt(const Entry* e, bool le, uint32_t i) {
    if (i >= e->count) return 0.0;
    switch (e->type) {
        case 1: case 6: case 7: return (double)e->val[i];
        case 3: return (double)rdu16(e->val + i * 2, le);
        case 8: return (double)(int16_t)rdu16(e->val + i * 2, le);
        case 4: return (double)rdu32(e->val + i * 4, le);
        case 9: return (double)(int32_t)rdu32(e->val + i * 4, le);
        case 5: { uint32_t num = rdu32(e->val + i * 8, le), den = rdu32(e->val + i * 8 + 4, le);
                  return den ? (double)num / den : 0.0; }
        case 10:{ int32_t num = (int32_t)rdu32(e->val + i * 8, le), den = (int32_t)rdu32(e->val + i * 8 + 4, le);
                  return den ? (double)num / den : 0.0; }
        default: return 0.0;
    }
}

static void mat3_vec(const double m[9], const double v[3], double o[3]) {
    for (int r = 0; r < 3; ++r) o[r] = m[r*3]*v[0] + m[r*3+1]*v[1] + m[r*3+2]*v[2];
}
static void mat3_mul(const double a[9], const double b[9], double o[9]) {
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
        double s = 0; for (int k = 0; k < 3; ++k) s += a[r*3+k]*b[k*3+c];
        o[r*3+c] = s;
    }
}
static bool mat3_inv(const double m[9], double o[9]) {
    double det = m[0]*(m[4]*m[8]-m[5]*m[7]) - m[1]*(m[3]*m[8]-m[5]*m[6]) + m[2]*(m[3]*m[7]-m[4]*m[6]);
    if (std::fabs(det) < 1e-12) return false;
    double id = 1.0 / det;
    o[0]=(m[4]*m[8]-m[5]*m[7])*id; o[1]=(m[2]*m[7]-m[1]*m[8])*id; o[2]=(m[1]*m[5]-m[2]*m[4])*id;
    o[3]=(m[5]*m[6]-m[3]*m[8])*id; o[4]=(m[0]*m[8]-m[2]*m[6])*id; o[5]=(m[2]*m[3]-m[0]*m[5])*id;
    o[6]=(m[3]*m[7]-m[4]*m[6])*id; o[7]=(m[1]*m[6]-m[0]*m[7])*id; o[8]=(m[0]*m[4]-m[1]*m[3])*id;
    return true;
}

} // namespace dngraw

extern "C" JNIEXPORT jshortArray JNICALL
Java_com_example_hdrviewer_HdrDecoder_decodeDngNative(
        JNIEnv* env, jclass, jbyteArray dngData, jint length, jintArray outDimens) {
    using namespace dngraw;
    jbyte* mem = env->GetByteArrayElements(dngData, nullptr);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(mem);
    size_t n = (size_t)length;

    std::vector<Entry> es; bool le = true;
    if (!parseIFD0(d, n, es, le)) { env->ReleaseByteArrayElements(dngData, mem, JNI_ABORT); return nullptr; }

    const Entry* eW = find(es, 256), *eH = find(es, 257), *eStrip = find(es, 273), *eCfa = find(es, 33422);
    const Entry* eComp = find(es, 259), *ePhoto = find(es, 262), *eBits = find(es, 258);
    if (!eW || !eH || !eStrip || !eCfa) { env->ReleaseByteArrayElements(dngData, mem, JNI_ABORT); return nullptr; }

    int comp  = eComp  ? (int)valAt(eComp, le, 0)  : 1;
    int photo = ePhoto ? (int)valAt(ePhoto, le, 0) : 32803;
    int bits  = eBits  ? (int)valAt(eBits, le, 0)  : 16;
    // Only our own files: uncompressed 16-bit CFA. Anything else -> let Java fall back to Android.
    if (comp != 1 || photo != 32803 || bits != 16) {
        env->ReleaseByteArrayElements(dngData, mem, JNI_ABORT);
        DNG_LOG("not a plain CFA16 DNG (comp=%d photo=%d bits=%d) -> fallback", comp, photo, bits);
        return nullptr;
    }
    int W = (int)valAt(eW, le, 0), H = (int)valAt(eH, le, 0);
    if (W <= 0 || H <= 0 || (long long)W * H > 200000000LL) { env->ReleaseByteArrayElements(dngData, mem, JNI_ABORT); return nullptr; }
    uint32_t stripOff = (uint32_t)valAt(eStrip, le, 0);
    if ((uint64_t)stripOff + (uint64_t)W * H * 2 > n) { env->ReleaseByteArrayElements(dngData, mem, JNI_ABORT); return nullptr; }
    const uint8_t* mosaic = d + stripOff;

    int cfa[4]; for (int i = 0; i < 4; ++i) cfa[i] = (int)valAt(eCfa, le, i);   // 0=R 1=G 2=B
    double black[4] = {0,0,0,0};
    if (const Entry* eb = find(es, 50714)) {
        for (uint32_t i = 0; i < 4; ++i) black[i] = valAt(eb, le, std::min(i, eb->count - 1));
    }
    double white = 65535.0; if (const Entry* ew = find(es, 50717)) white = valAt(ew, le, 0);
    double neutral[3] = {1,1,1};
    if (const Entry* en = find(es, 50728)) for (int i = 0; i < 3 && (uint32_t)i < en->count; ++i) neutral[i] = valAt(en, le, i);
    for (int i = 0; i < 3; ++i) if (neutral[i] < 1e-6) neutral[i] = 1.0;

    // Build camera-RAW -> linear sRGB matrix. Prefer ForwardMatrix (maps the
    // white-balanced camera space to XYZ D50); fall back to inverse ColorMatrix.
    static const double XYZ2SRGB_D50[9] = {  // Bradford-adapted XYZ(D50) -> linear sRGB
        3.1338561,-1.6168667,-0.4906146, -0.9787684,1.9161415,0.0334540, 0.0719453,-0.2289914,1.4052427 };
    double cam2srgb[9]; bool haveColor = false;
    if (const Entry* efm = find(es, 50964); efm && efm->count >= 9) {
        double fm[9], cam2xyz[9];
        for (int i = 0; i < 9; ++i) fm[i] = valAt(efm, le, i);
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) cam2xyz[r*3+c] = fm[r*3+c] / neutral[c]; // * diag(1/neutral)
        mat3_mul(XYZ2SRGB_D50, cam2xyz, cam2srgb); haveColor = true;
    } else if (const Entry* ecm = find(es, 50721); ecm && ecm->count >= 9) {
        double cm[9], inv[9];
        for (int i = 0; i < 9; ++i) cm[i] = valAt(ecm, le, i);     // XYZ -> camera
        if (mat3_inv(cm, inv)) {
            static const double XYZ2SRGB_D65[9] = {
                3.2404542,-1.5371385,-0.4985314, -0.9692660,1.8760108,0.0415560, 0.0556434,-0.2040259,1.0572252 };
            mat3_mul(XYZ2SRGB_D65, inv, cam2srgb); haveColor = true;
        }
    }

    int orient = 1; if (const Entry* eo = find(es, 274)) orient = (int)valAt(eo, le, 0);
    int outW = W, outH = H; if (orient == 6 || orient == 8) { outW = H; outH = W; }

    size_t np = (size_t)outW * outH;
    std::vector<uint16_t> half(np * 4);
    const uint16_t HALF_ONE = float_to_half(1.0f);
    const double scale = (white - black[0] > 1.0) ? (white - black[0]) : (white > 1.0 ? white : 1023.0);
    const double wbR = neutral[1] / neutral[0], wbB = neutral[1] / neutral[2];

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // 3x3 neighbour-average demosaic (robust for any 2x2 CFA layout).
            double sum[3] = {0,0,0}; int cnt[3] = {0,0,0};
            for (int dy = -1; dy <= 1; ++dy) {
                int yy = y + dy; if (yy < 0 || yy >= H) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = x + dx; if (xx < 0 || xx >= W) continue;
                    int c = cfa[(yy & 1) * 2 + (xx & 1)];
                    const uint8_t* p = mosaic + ((size_t)yy * W + xx) * 2;
                    double v = (double)(p[0] | (p[1] << 8)) - black[(yy & 1) * 2 + (xx & 1)];
                    if (v < 0) v = 0; sum[c] += v; cnt[c]++;
                }
            }
            double cam[3];
            for (int c = 0; c < 3; ++c) cam[c] = (cnt[c] ? sum[c] / cnt[c] : 0.0) / scale;
            double out[3];
            if (haveColor) {
                mat3_vec(cam2srgb, cam, out);           // WB + colour folded into the matrix
            } else {
                out[0] = cam[0] * wbR; out[1] = cam[1]; out[2] = cam[2] * wbB;  // white balance only
            }
            int dx2, dy2;
            switch (orient) {
                case 3: dx2 = W - 1 - x; dy2 = H - 1 - y; break;
                case 6: dx2 = H - 1 - y; dy2 = x;         break;
                case 8: dx2 = y;         dy2 = W - 1 - x; break;
                default: dx2 = x;        dy2 = y;         break;
            }
            size_t di = ((size_t)dy2 * outW + dx2) * 4;
            half[di+0] = float_to_half((float)(out[0] < 0 ? 0 : out[0]));
            half[di+1] = float_to_half((float)(out[1] < 0 ? 0 : out[1]));
            half[di+2] = float_to_half((float)(out[2] < 0 ? 0 : out[2]));
            half[di+3] = HALF_ONE;
        }
    }
    env->ReleaseByteArrayElements(dngData, mem, JNI_ABORT);

    jint dims[2] = { outW, outH };
    env->SetIntArrayRegion(outDimens, 0, 2, dims);
    DNG_LOG("decoded RAW %dx%d -> %dx%d (orient=%d color=%d)", W, H, outW, outH, orient, (int)haveColor);

    jshortArray result = env->NewShortArray((jsize)(np * 4));
    env->SetShortArrayRegion(result, 0, (jsize)(np * 4), reinterpret_cast<const jshort*>(half.data()));
    return result;
}

// ---------------------------------------------------------------------------
// Ultra HDR (JPEG_R) export helper.
//
// The DNG is the lossless HDR master; this produces a *compatible* derivative:
// an SDR base + a per-channel gain map, which Android (API 34+) muxes into an
// Ultra HDR JPEG on Bitmap.compress(JPEG). The result opens as a normal JPEG
// everywhere and renders true HDR on capable displays. Inputs/outputs are
// Android Bitmaps locked directly (no big Java arrays). Returns 0 on success.
// ---------------------------------------------------------------------------
namespace uhdr {

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f, man = h & 0x3ff, f;
    if (exp == 0) {
        if (man == 0) { f = sign; }
        else { int e = -1; do { e++; man <<= 1; } while (!(man & 0x400)); man &= 0x3ff;
               f = sign | ((uint32_t)(127 - 15 - e) << 23) | (man << 13); }
    } else if (exp == 0x1f) { f = sign | 0x7f800000u | (man << 13); }
    else { f = sign | ((exp + 112) << 23) | (man << 13); }
    float o; std::memcpy(&o, &f, 4); return o;
}
static inline float srgb_oetf(float v) {
    if (v <= 0.f) return 0.f; if (v >= 1.f) return 1.f;
    return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

} // namespace uhdr

extern "C" JNIEXPORT jint JNICALL
Java_com_example_hdrviewer_HdrDecoder_buildUltraHdrNative(
        JNIEnv* env, jclass, jobject srcBmp, jobject sdrBmp, jobject gainBmp, jfloatArray metaOut) {
    using namespace uhdr;
    AndroidBitmapInfo si, di, gi;
    if (AndroidBitmap_getInfo(env, srcBmp, &si) < 0) return -1;
    if (AndroidBitmap_getInfo(env, sdrBmp, &di) < 0) return -2;
    if (AndroidBitmap_getInfo(env, gainBmp, &gi) < 0) return -3;
    if (si.format != ANDROID_BITMAP_FORMAT_RGBA_F16) return -4;
    if (di.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || gi.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return -5;
    int w = (int)si.width, h = (int)si.height;
    if ((int)di.width != w || (int)di.height != h || (int)gi.width != w || (int)gi.height != h) return -6;

    void *sp = nullptr, *dp = nullptr, *gp = nullptr;
    if (AndroidBitmap_lockPixels(env, srcBmp, &sp) < 0) return -7;
    if (AndroidBitmap_lockPixels(env, sdrBmp, &dp) < 0) { AndroidBitmap_unlockPixels(env, srcBmp); return -8; }
    if (AndroidBitmap_lockPixels(env, gainBmp, &gp) < 0) { AndroidBitmap_unlockPixels(env, srcBmp); AndroidBitmap_unlockPixels(env, sdrBmp); return -9; }

    // Pass 1: per-channel peak (the linear ratio the brightest pixel needs).
    float maxc[3] = {1.f, 1.f, 1.f};
    for (int y = 0; y < h; ++y) {
        const uint16_t* sr = (const uint16_t*)((const uint8_t*)sp + (size_t)y * si.stride);
        for (int x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) {
            float v = half_to_float(sr[x * 4 + c]); if (v > maxc[c]) maxc[c] = v;
        }
    }
    float logmax[3]; for (int c = 0; c < 3; ++c) logmax[c] = (maxc[c] > 1.f) ? log2f(maxc[c]) : 1.f;

    // Pass 2: SDR base (clamped, sRGB-encoded) + log-encoded gain map.
    for (int y = 0; y < h; ++y) {
        const uint16_t* sr = (const uint16_t*)((const uint8_t*)sp + (size_t)y * si.stride);
        uint8_t* dr = (uint8_t*)dp + (size_t)y * di.stride;
        uint8_t* gr = (uint8_t*)gp + (size_t)y * gi.stride;
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                float v = half_to_float(sr[x * 4 + c]); if (v < 0.f) v = 0.f;
                float sdrLin = v > 1.f ? 1.f : v;
                dr[x * 4 + c] = (uint8_t)(srgb_oetf(sdrLin) * 255.f + 0.5f);
                float g = 0.f;
                if (v > 1.f && maxc[c] > 1.f) { g = log2f(v) / logmax[c]; if (g < 0.f) g = 0.f; if (g > 1.f) g = 1.f; }
                gr[x * 4 + c] = (uint8_t)(g * 255.f + 0.5f);
            }
            dr[x * 4 + 3] = 255; gr[x * 4 + 3] = 255;
        }
    }
    AndroidBitmap_unlockPixels(env, gainBmp);
    AndroidBitmap_unlockPixels(env, sdrBmp);
    AndroidBitmap_unlockPixels(env, srcBmp);

    float meta[3] = {maxc[0], maxc[1], maxc[2]};
    env->SetFloatArrayRegion(metaOut, 0, 3, meta);
    DNG_LOG("UltraHDR built %dx%d ratioMax=%.2f/%.2f/%.2f", w, h, maxc[0], maxc[1], maxc[2]);
    return 0;
}

// ---------------------------------------------------------------------------
// Lossless HDR export: write the displayed RGBA_F16 bitmap as a 32-bit-float
// RGB TIFF (uncompressed, SampleFormat=IEEE float) straight to the output file
// descriptor. Truly lossless, preserves the full HDR range, and opens in
// Photoshop/Lightroom/Affinity/GIMP and most OS viewers. Streamed row-by-row so
// we never hold the (large) file in memory. The DNG remains the raw master;
// this is the lossless *rendered* derivative. Returns 0 on success.
// ---------------------------------------------------------------------------
namespace tiffw {
static bool write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf; size_t left = n;
    while (left) { ssize_t k = write(fd, p, left); if (k <= 0) return false; p += (size_t)k; left -= (size_t)k; }
    return true;
}
} // namespace tiffw

extern "C" JNIEXPORT jint JNICALL
Java_com_example_hdrviewer_HdrDecoder_encodeTiffToFdNative(JNIEnv* env, jclass, jobject srcBmp, jint fd) {
    AndroidBitmapInfo si;
    if (AndroidBitmap_getInfo(env, srcBmp, &si) < 0) return -1;
    if (si.format != ANDROID_BITMAP_FORMAT_RGBA_F16) return -2;
    int w = (int)si.width, h = (int)si.height;

    void* sp = nullptr;
    if (AndroidBitmap_lockPixels(env, srcBmp, &sp) < 0) return -3;

    // Little-endian TIFF: header(8) + IFD + BitsPerSample[3] + SampleFormat[3], then pixels.
    const uint32_t HDR = 8, NTAGS = 11;
    const uint32_t IFD_OFF = HDR;
    const uint32_t IFD_SIZE = 2 + NTAGS * 12 + 4;
    const uint32_t BPS_OFF = IFD_OFF + IFD_SIZE;
    const uint32_t SFMT_OFF = BPS_OFF + 6;
    const uint32_t STRIP_OFF = SFMT_OFF + 6;
    const uint64_t stripBytes = (uint64_t)w * h * 3 * 4;

    std::vector<uint8_t> head(STRIP_OFF, 0);
    auto pu16 = [&](uint32_t o, uint16_t v) { head[o] = v & 0xff; head[o + 1] = (v >> 8) & 0xff; };
    auto pu32 = [&](uint32_t o, uint32_t v) { head[o] = v & 0xff; head[o + 1] = (v >> 8) & 0xff; head[o + 2] = (v >> 16) & 0xff; head[o + 3] = (v >> 24) & 0xff; };
    head[0] = 'I'; head[1] = 'I'; pu16(2, 42); pu32(4, IFD_OFF);
    pu16(IFD_OFF, (uint16_t)NTAGS);
    uint32_t e = IFD_OFF + 2;
    auto tag = [&](uint16_t id, uint16_t type, uint32_t count, uint32_t value) {
        pu16(e, id); pu16(e + 2, type); pu32(e + 4, count); pu32(e + 8, value); e += 12;
    };
    tag(256, 4, 1, (uint32_t)w);            // ImageWidth
    tag(257, 4, 1, (uint32_t)h);            // ImageLength
    tag(258, 3, 3, BPS_OFF);                // BitsPerSample [32,32,32]
    tag(259, 3, 1, 1);                      // Compression = none
    tag(262, 3, 1, 2);                      // Photometric = RGB
    tag(273, 4, 1, STRIP_OFF);              // StripOffsets
    tag(277, 3, 1, 3);                      // SamplesPerPixel
    tag(278, 4, 1, (uint32_t)h);            // RowsPerStrip
    tag(279, 4, 1, (uint32_t)stripBytes);   // StripByteCounts
    tag(284, 3, 1, 1);                      // PlanarConfig = chunky
    tag(339, 3, 3, SFMT_OFF);               // SampleFormat [3,3,3] = IEEE float
    pu32(e, 0);                             // next IFD = 0
    pu16(BPS_OFF, 32); pu16(BPS_OFF + 2, 32); pu16(BPS_OFF + 4, 32);
    pu16(SFMT_OFF, 3); pu16(SFMT_OFF + 2, 3); pu16(SFMT_OFF + 4, 3);

    bool ok = tiffw::write_all((int)fd, head.data(), head.size());
    std::vector<uint8_t> row((size_t)w * 3 * 4);
    for (int y = 0; y < h && ok; ++y) {
        const uint16_t* sr = (const uint16_t*)((const uint8_t*)sp + (size_t)y * si.stride);
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                float v = uhdr::half_to_float(sr[x * 4 + c]);
                uint32_t bits; std::memcpy(&bits, &v, 4);
                uint8_t* p = &row[(size_t)(x * 3 + c) * 4];
                p[0] = bits & 0xff; p[1] = (bits >> 8) & 0xff; p[2] = (bits >> 16) & 0xff; p[3] = (bits >> 24) & 0xff;
            }
        }
        if (!tiffw::write_all((int)fd, row.data(), row.size())) ok = false;
    }
    AndroidBitmap_unlockPixels(env, srcBmp);
    DNG_LOG("TIFF32f %s %dx%d -> %llu bytes", ok ? "written" : "FAILED", w, h,
            (unsigned long long)(head.size() + stripBytes));
    return ok ? 0 : -4;
}

// ---------------------------------------------------------------------------
// Compact HDR export: encode the displayed RGBA_F16 bitmap as a 12-bit BT.2020
// PQ AVIF (AV1, 4:4:4) via libavif + libaom. Highest-quality/feature HDR tier
// between lossless TIFF and the lossy Ultra HDR JPEG. Streamed to the fd.
// Returns 0 on success.
// ---------------------------------------------------------------------------
namespace avifx {
static inline float pq_oetf(float L) {           // L = nits/10000 in [0,1] -> PQ code [0,1]
    if (L < 0.f) L = 0.f; if (L > 1.f) L = 1.f;
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float Lm1 = powf(L, m1);
    return powf((c1 + c2 * Lm1) / (1.0f + c3 * Lm1), m2);
}
// Rec.709 (sRGB) linear -> Rec.2020 linear (D65).
static const float R709_2020[9] = {
    0.627403896f, 0.329283038f, 0.043313066f,
    0.069097289f, 0.919540395f, 0.011362316f,
    0.016391439f, 0.088013308f, 0.895595253f };
} // namespace avifx

extern "C" JNIEXPORT jint JNICALL
Java_com_example_hdrviewer_HdrDecoder_encodeAvifToFdNative(
        JNIEnv* env, jclass, jobject srcBmp, jint fd, jint quality) {
    using namespace avifx;
    AndroidBitmapInfo si;
    if (AndroidBitmap_getInfo(env, srcBmp, &si) < 0) return -1;
    if (si.format != ANDROID_BITMAP_FORMAT_RGBA_F16) return -2;
    int w = (int)si.width, h = (int)si.height;

    void* sp = nullptr;
    if (AndroidBitmap_lockPixels(env, srcBmp, &sp) < 0) return -3;

    avifImage* image = avifImageCreate(w, h, 12, AVIF_PIXEL_FORMAT_YUV444);
    if (!image) { AndroidBitmap_unlockPixels(env, srcBmp); return -4; }
    image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
    image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084; // PQ
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
    image->yuvRange = AVIF_RANGE_FULL;

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, image);
    rgb.format = AVIF_RGB_FORMAT_RGB;   // no alpha
    rgb.depth = 12;
    if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
        avifImageDestroy(image); AndroidBitmap_unlockPixels(env, srcBmp); return -5;
    }

    const float scaleNits = 1000.0f;    // map linear 1.0 (sensor white) -> 1000 nits
    for (int y = 0; y < h; ++y) {
        const uint16_t* srow = (const uint16_t*)((const uint8_t*)sp + (size_t)y * si.stride);
        uint16_t* drow = (uint16_t*)(rgb.pixels + (size_t)y * rgb.rowBytes);
        for (int x = 0; x < w; ++x) {
            float r = uhdr::half_to_float(srow[x * 4 + 0]); if (r < 0) r = 0;
            float g = uhdr::half_to_float(srow[x * 4 + 1]); if (g < 0) g = 0;
            float b = uhdr::half_to_float(srow[x * 4 + 2]); if (b < 0) b = 0;
            float R = R709_2020[0]*r + R709_2020[1]*g + R709_2020[2]*b;
            float G = R709_2020[3]*r + R709_2020[4]*g + R709_2020[5]*b;
            float B = R709_2020[6]*r + R709_2020[7]*g + R709_2020[8]*b;
            drow[x * 3 + 0] = (uint16_t)(pq_oetf(R * scaleNits / 10000.0f) * 4095.f + 0.5f);
            drow[x * 3 + 1] = (uint16_t)(pq_oetf(G * scaleNits / 10000.0f) * 4095.f + 0.5f);
            drow[x * 3 + 2] = (uint16_t)(pq_oetf(B * scaleNits / 10000.0f) * 4095.f + 0.5f);
        }
    }
    AndroidBitmap_unlockPixels(env, srcBmp);

    avifResult ar = avifImageRGBToYUV(image, &rgb);
    avifRGBImageFreePixels(&rgb);
    if (ar != AVIF_RESULT_OK) { avifImageDestroy(image); DNG_LOG("RGBToYUV failed: %s", avifResultToString(ar)); return -6; }

    avifEncoder* enc = avifEncoderCreate();
    if (!enc) { avifImageDestroy(image); return -7; }
    enc->quality = (int)quality;        // 0..100 (100 = lossless)
    enc->speed = 6;
    enc->maxThreads = 8;

    avifRWData out = AVIF_DATA_EMPTY;
    ar = avifEncoderAddImage(enc, image, 1, AVIF_ADD_IMAGE_FLAG_SINGLE);
    if (ar == AVIF_RESULT_OK) ar = avifEncoderFinish(enc, &out);
    avifEncoderDestroy(enc);
    avifImageDestroy(image);
    if (ar != AVIF_RESULT_OK) { avifRWDataFree(&out); DNG_LOG("AVIF encode failed: %s", avifResultToString(ar)); return -8; }

    bool ok = tiffw::write_all((int)fd, out.data, out.size);
    DNG_LOG("AVIF %s %dx%d q=%d -> %zu bytes", ok ? "written" : "FAILED", w, h, (int)quality, out.size);
    avifRWDataFree(&out);
    return ok ? 0 : -9;
}
