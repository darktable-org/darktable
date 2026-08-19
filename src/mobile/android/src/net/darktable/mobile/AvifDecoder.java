package net.darktable.mobile;

import android.graphics.ImageFormat;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.util.Log;

import java.nio.ByteBuffer;

/**
 * Decodes a single AV1 frame from raw OBU data extracted from an AVIF file.
 * Called from C++ via JNI to bypass Android's HeifDecoderImpl, which does not
 * support AV1 Profile 2 (12-bit, YUV444, identity matrix).
 */
public class AvifDecoder {
    private static final String TAG = "AvifDecoder";

    /**
     * Decode one AV1 still-image frame.
     *
     * @param width   image width (from AVIF ispe box)
     * @param height  image height (from AVIF ispe box)
     * @param csd     AV1CodecConfigurationRecord from av1C box (CSD-0), may be empty
     * @param sample  raw AV1 OBU bitstream from the AVIF mdat
     * @return int[width*height] of ARGB_8888 pixels, or null on failure
     */
    public static int[] decode(int width, int height, byte[] csd, byte[] sample) {
        MediaCodec codec = null;
        try {
            codec = MediaCodec.createDecoderByType("video/av01");

            MediaFormat fmt = MediaFormat.createVideoFormat("video/av01", width, height);
            if (csd != null && csd.length > 0)
                fmt.setByteBuffer("csd-0", ByteBuffer.wrap(csd));
            codec.configure(fmt, null, null, 0);
            codec.start();

            // Feed the complete AV1 OBU stream as a single key-frame + EOS
            int inIdx = codec.dequeueInputBuffer(2_000_000L);
            if (inIdx < 0) {
                Log.e(TAG, "no input buffer");
                return null;
            }
            ByteBuffer inBuf = codec.getInputBuffer(inIdx);
            if (inBuf == null) return null;
            inBuf.clear();
            inBuf.put(sample);
            codec.queueInputBuffer(inIdx, 0, sample.length, 0,
                    MediaCodec.BUFFER_FLAG_KEY_FRAME | MediaCodec.BUFFER_FLAG_END_OF_STREAM);

            // Drain output — may receive INFO_OUTPUT_FORMAT_CHANGED first
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            for (int attempt = 0; attempt < 40; attempt++) {
                int outIdx = codec.dequeueOutputBuffer(info, 250_000L);
                if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) continue;
                if (outIdx == MediaCodec.INFO_TRY_AGAIN_LATER)       continue;
                if (outIdx < 0) break;

                Image image = codec.getOutputImage(outIdx);
                int[] pixels = null;
                if (image != null) {
                    pixels = imageToArgb(image);
                    image.close();
                }
                codec.releaseOutputBuffer(outIdx, false);
                return pixels;
            }
            Log.e(TAG, "no output frame produced");
            return null;

        } catch (Exception e) {
            Log.e(TAG, "decode failed: " + e, e);
            return null;
        } finally {
            if (codec != null) {
                try { codec.stop(); } catch (Exception ignored) {}
                codec.release();
            }
        }
    }

    private static int[] imageToArgb(Image image) {
        int w = image.getWidth(), h = image.getHeight();
        int fmt = image.getFormat();
        Image.Plane[] planes = image.getPlanes();
        Log.d(TAG, "output format=0x" + Integer.toHexString(fmt)
                + " size=" + w + "x" + h);

        if (fmt == ImageFormat.YUV_420_888)  return yuv420ToArgb(planes, w, h);
        if (fmt == ImageFormat.YUV_444_888)  return yuv444ToArgb(planes, w, h);
        if (fmt == 0x36 /* YCBCR_P010 */)   return p010ToArgb(planes, w, h);

        Log.e(TAG, "unhandled output format 0x" + Integer.toHexString(fmt));
        return null;
    }

    // ── YUV 4:2:0 flexible (8-bit) ────────────────────────────────────────────

    private static int[] yuv420ToArgb(Image.Plane[] planes, int w, int h) {
        ByteBuffer yBuf = planes[0].getBuffer();
        ByteBuffer uBuf = planes[1].getBuffer();
        ByteBuffer vBuf = planes[2].getBuffer();
        int yRow  = planes[0].getRowStride();
        int uvRow = planes[1].getRowStride();
        int uvPx  = planes[1].getPixelStride(); // 1=I420, 2=NV12/NV21
        int uLim  = uBuf.limit();
        int vLim  = vBuf.limit();

        int[] out = new int[w * h];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int Y = yBuf.get(y * yRow + x) & 0xFF;
                // Odd-dimension images may leave the last UV row/col outside the
                // buffer (dav1d allocates floor(dim/2) UV samples per axis).
                // Clamp so we never read past the buffer limit.
                int uvOff = (y >> 1) * uvRow + (x >> 1) * uvPx;
                int U = (uvOff < uLim) ? uBuf.get(uvOff) & 0xFF : 128;
                int V = (uvOff < vLim) ? vBuf.get(uvOff) & 0xFF : 128;
                out[y * w + x] = yuvToArgb(Y, U - 128, V - 128);
            }
        }
        return out;
    }

    // ── YUV 4:4:4 flexible (8-bit) ────────────────────────────────────────────
    // For AV1 identity matrix: planes[0]=G, planes[1]=B, planes[2]=R.
    // The yuvToArgb call still gives a useful image even if channels are swapped.

    private static int[] yuv444ToArgb(Image.Plane[] planes, int w, int h) {
        ByteBuffer yBuf = planes[0].getBuffer();
        ByteBuffer uBuf = planes[1].getBuffer();
        ByteBuffer vBuf = planes[2].getBuffer();
        int yRow = planes[0].getRowStride();
        int uRow = planes[1].getRowStride();
        int vRow = planes[2].getRowStride();
        int uPx  = planes[1].getPixelStride();
        int vPx  = planes[2].getPixelStride();

        int[] out = new int[w * h];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int Y = yBuf.get(y * yRow + x) & 0xFF;
                int U = uBuf.get(y * uRow + x * uPx) & 0xFF;
                int V = vBuf.get(y * vRow + x * vPx) & 0xFF;
                out[y * w + x] = yuvToArgb(Y, U - 128, V - 128);
            }
        }
        return out;
    }

    // ── YCBCR_P010 (10-bit semi-planar 4:2:0, API 31+) ───────────────────────

    private static int[] p010ToArgb(Image.Plane[] planes, int w, int h) {
        ByteBuffer yBuf = planes[0].getBuffer();
        ByteBuffer uBuf = planes[1].getBuffer();
        ByteBuffer vBuf = planes[2].getBuffer();
        int yRow  = planes[0].getRowStride();
        int uvRow = planes[1].getRowStride();
        int uPx   = planes[1].getPixelStride(); // 4 for P010 interleaved
        int vPx   = planes[2].getPixelStride();

        int[] out = new int[w * h];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int yOff = y * yRow + x * 2;
                // P010: 16-bit LE, 10-bit value in top bits [15:6]
                int Y10 = ((yBuf.get(yOff) & 0xFF) | ((yBuf.get(yOff + 1) & 0xFF) << 8)) >> 6;

                int uOff = (y >> 1) * uvRow + (x >> 1) * uPx;
                int vOff = (y >> 1) * uvRow + (x >> 1) * vPx;
                int U10 = ((uBuf.get(uOff) & 0xFF) | ((uBuf.get(uOff + 1) & 0xFF) << 8)) >> 6;
                int V10 = ((vBuf.get(vOff) & 0xFF) | ((vBuf.get(vOff + 1) & 0xFF) << 8)) >> 6;

                // Map 10-bit full-range [0..1023] → 8-bit [0..255]
                int Y8 = (Y10 * 255 + 511) / 1023;
                int U8 = (U10 * 255 + 511) / 1023 - 128;
                int V8 = (V10 * 255 + 511) / 1023 - 128;

                out[y * w + x] = yuvToArgb(Y8, U8, V8);
            }
        }
        return out;
    }

    // ── BT.709 full-range YCbCr → ARGB_8888 ─────────────────────────────────

    private static int yuvToArgb(int Y, int Cb, int Cr) {
        int R = clamp8(Math.round(Y + 1.5748f * Cr));
        int G = clamp8(Math.round(Y - 0.1873f * Cb - 0.4681f * Cr));
        int B = clamp8(Math.round(Y + 1.8556f * Cb));
        return 0xFF000000 | (R << 16) | (G << 8) | B;
    }

    private static int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
}
