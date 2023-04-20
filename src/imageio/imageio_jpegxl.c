/*
    This file is part of darktable,
    Copyright (C) 2022-2023 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <inttypes.h>

#include <jxl/decode.h>
#include <jxl/resizable_parallel_runner.h>

#include "common/exif.h"
#include "common/image.h"
#include "imageio/imageio_common.h"

dt_imageio_retval_t dt_imageio_open_jpegxl(dt_image_t *img,
                                           const char *filename,
                                           dt_mipmap_buffer_t *mbuf)
{

  JxlDecoderStatus status;
  JxlBasicInfo basicinfo;
  size_t icc_size = 0;
  uint64_t exif_size = 0;
  uint8_t *exif_data = NULL;
  uint32_t num_threads;

  FILE* inputfile = g_fopen(filename, "rb");

  if(!inputfile)
  {
    dt_print(DT_DEBUG_ALWAYS, "[jpegxl_open] Cannot open file for read: %s\n", filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  fseek(inputfile, 0, SEEK_END);
  size_t inputFileSize = ftell(inputfile);
  fseek(inputfile, 0, SEEK_SET);

  void* read_buffer = malloc(inputFileSize);

  if(fread(read_buffer, 1, inputFileSize, inputfile) != inputFileSize)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[jpegxl_open] Failed to read %zu bytes: %s\n", inputFileSize, filename);
    free(read_buffer);
    fclose(inputfile);
    return DT_IMAGEIO_LOAD_FAILED;
  }
  fclose(inputfile);

  JxlSignature signature = JxlSignatureCheck(read_buffer, inputFileSize);

  if(signature != JXL_SIG_CODESTREAM && signature != JXL_SIG_CONTAINER)
  {
    // It's normal if this function is called for a non-jxl file, so we should fail silently.
    free(read_buffer);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  const JxlPixelFormat pixel_format =
  {
   4,                    // number of channels
   JXL_TYPE_FLOAT,       // channel depth
   JXL_NATIVE_ENDIAN,    // endianness
   0                     // align
  };

  JxlDecoder *decoder = JxlDecoderCreate(NULL);

  if(!decoder)
  {
    dt_print(DT_DEBUG_ALWAYS, "[jpegxl_open] ERROR: JxlDecoderCreate failed\n");
    free(read_buffer);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  JxlParallelRunner *runner = JxlResizableParallelRunnerCreate(NULL);
  if(!runner)
  {
    dt_print(DT_DEBUG_ALWAYS, "[jpegxl_open] ERROR: JxlResizableParallelRunnerCreate failed\n");
    JxlDecoderDestroy(decoder);
    free(read_buffer);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  if(JxlDecoderSetInput(decoder, read_buffer, inputFileSize) != JXL_DEC_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "[jpegxl_open] ERROR: JxlDecoderSetInput failed\n");
    JxlResizableParallelRunnerDestroy(runner);
    JxlDecoderDestroy(decoder);
    free(read_buffer);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  if(JxlDecoderSubscribeEvents(decoder,
                               JXL_DEC_BASIC_INFO |
                               JXL_DEC_COLOR_ENCODING |
                               JXL_DEC_BOX |
                               JXL_DEC_FULL_IMAGE)
     != JXL_DEC_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "[jpegxl_open] ERROR: JxlDecoderSubscribeEvents failed\n");
    JxlResizableParallelRunnerDestroy(runner);
    JxlDecoderDestroy(decoder);
    free(read_buffer);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  if(JxlDecoderSetParallelRunner(decoder, JxlResizableParallelRunner, runner) != JXL_DEC_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "[jpegxl_open] ERROR: JxlDecoderSetParallelRunner failed\n");
    JxlResizableParallelRunnerDestroy(runner);
    JxlDecoderDestroy(decoder);
    free(read_buffer);
    return DT_IMAGEIO_LOAD_FAILED;
  }


  // Grand Decoding Loop
  while(1)
  {
    status = JxlDecoderProcessInput(decoder);

    if(status == JXL_DEC_ERROR)
    {
      dt_print(DT_DEBUG_ALWAYS, "[jpegxl_open] ERROR: JXL decoding failed\n");
      JxlResizableParallelRunnerDestroy(runner);
      JxlDecoderDestroy(decoder);
      free(read_buffer);
      return DT_IMAGEIO_LOAD_FAILED;
    }

    if(status == JXL_DEC_NEED_MORE_INPUT)
    {
      dt_print(DT_DEBUG_ALWAYS, "[jpegxl_open] ERROR: JXL data incomplete\n");
      JxlResizableParallelRunnerDestroy(runner);
      JxlDecoderDestroy(decoder);
      free(read_buffer);
      return DT_IMAGEIO_LOAD_FAILED;
    }

    if(status == JXL_DEC_BASIC_INFO)
    {
      if(JxlDecoderGetBasicInfo(decoder, &basicinfo) != JXL_DEC_SUCCESS)
      {
        dt_print(DT_DEBUG_ALWAYS, "[jpegxl_open] ERROR: JXL basic info not available\n");
        JxlResizableParallelRunnerDestroy(runner);
        JxlDecoderDestroy(decoder);
        free(read_buffer);
        return DT_IMAGEIO_LOAD_FAILED;
      }

      // Unlikely to happen, but let there be a sanity check
      if(basicinfo.xsize == 0 || basicinfo.ysize == 0)
      {
        dt_print(DT_DEBUG_ALWAYS,"[jpegxl_open] ERROR: JXL image declares zero dimensions\n");
        JxlResizableParallelRunnerDestroy(runner);
        JxlDecoderDestroy(decoder);
        free(read_buffer);
        return DT_IMAGEIO_LOAD_FAILED;
      }


      num_threads = JxlResizableParallelRunnerSuggestThreads(basicinfo.xsize, basicinfo.ysize);
      JxlResizableParallelRunnerSetThreads(runner, num_threads);

      continue;    // go to next loop iteration to process rest of the input
    }

    if(status == JXL_DEC_BOX)
    {
      // There is no need for fallback reading of Exif data if Exiv2 has already done it for us
      if(img->exif_inited) continue;

      JxlBoxType type;

      // Calling JxlDecoderReleaseBoxBuffer when no buffer is set is not an error
      JxlDecoderReleaseBoxBuffer(decoder);

      // Box decompression is not yet supported due to the lack of test images
      // with brotli compressed Exif data.
      status = JxlDecoderGetBoxType(decoder, type, JXL_FALSE);
      if(status != JXL_DEC_SUCCESS) continue;

      // Initially we get the full size of a box (the content of the box will be less)
      status = JxlDecoderGetBoxSizeRaw(decoder, &exif_size);
      // If the size is too small, it doesn't even make sense to check the type. At least
      // 4 bytes are occupied by the box type and another 4 by the "offset of the start
      // of Exif data" field (if it was Exif). Therefore, the size of 8 bytes excludes
      // the presence of the data we are looking for.
      if((status != JXL_DEC_SUCCESS) || (exif_size <= 8)) continue;

      if(memcmp(type, "Exif", 4) == 0)
      {
        // To get the Exif payload size we need to subtract 4 bytes of the box type FourCC.
        // See also https://github.com/libjxl/libjxl/issues/2022 and
        // https://github.com/darktable-org/darktable/pull/13463
        // In short: we may be subtracting too little, but it is safer to do so
        // than to subtract too much.
        exif_size -= 4;
        exif_data = g_try_malloc0(exif_size);
        if(!exif_data) continue;
        status = JxlDecoderSetBoxBuffer(decoder, exif_data, exif_size);
      }
    }

    if(status == JXL_DEC_COLOR_ENCODING)
    {
      if(JxlDecoderGetICCProfileSize(decoder, &pixel_format, JXL_COLOR_PROFILE_TARGET_DATA, &icc_size)
         == JXL_DEC_SUCCESS)
      {
        if(icc_size)
        {
          img->profile_size = icc_size;
          img->profile = (uint8_t *)g_malloc0(icc_size);
          JxlDecoderGetColorAsICCProfile(decoder,
                                         &pixel_format,
                                         JXL_COLOR_PROFILE_TARGET_DATA,
                                         img->profile,
                                         icc_size);
        }
      } else
      {
        // According to libjxl docs, the only situation where an ICC profile is not available
        // is when the image has an unknown or XYB color space. But dt does not support such
        // images, so in this case we should refuse to import the image. If in the future dt
        // will support the XYB color space, we can add code here to handle that case.
        dt_print(DT_DEBUG_ALWAYS,
                 "[jpegxl_open] WARNING: the image '%s' has an unknown or xyb color space. "
                 "We do not import such images\n",
                 filename);
        JxlResizableParallelRunnerDestroy(runner);
        JxlDecoderDestroy(decoder);
        free(read_buffer);
        return DT_IMAGEIO_LOAD_FAILED;
      }
    continue;    // go to next iteration to process rest of the input
    }

    if(status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
    {
      img->width = basicinfo.xsize;
      img->height = basicinfo.ysize;
      img->buf_dsc.channels = 4;
      img->buf_dsc.datatype = TYPE_FLOAT;
      float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
      if(!mipbuf)
      {
        JxlResizableParallelRunnerDestroy(runner);
        JxlDecoderDestroy(decoder);
        g_free(read_buffer);
        dt_print(DT_DEBUG_ALWAYS,
                 "[jpegxl_open] could not alloc full buffer for image: %s\n",
                 img->filename);
        return DT_IMAGEIO_CACHE_FULL;
      }
      JxlDecoderSetImageOutBuffer(decoder,
                                  &pixel_format,
                                  mipbuf,
                                  basicinfo.xsize * basicinfo.ysize * 4 * 4);
      continue;    // go to next iteration to process rest of the input
    }

    // If the image is an animation, more full frames may be decoded. We do not
    // check and reject the image if it is an animation, but only read the first
    // frame. It hardly makes sense to process such an image, but perhaps the
    // user intends to use dt as a DAM for such images.
    if (status == JXL_DEC_FULL_IMAGE)
      break;    // Terminate processing

  } // end of processing loop

  if(!img->exif_inited)
  {
    JxlDecoderReleaseBoxBuffer(decoder);
    // First 4 bytes of Exif blob is an offset of the actual Exif data
    const uint32_t exif_offset = exif_data[0] << 24 |
                                 exif_data[1] << 16 |
                                 exif_data[2] <<  8 |
                                 exif_data[3];
    if(exif_size > 4 + exif_offset)
    {
      dt_exif_read_from_blob(img,
                             exif_data + 4 + exif_offset,
                             exif_size - 4 - exif_offset);
    }
    g_free(exif_data);
  }

  JxlResizableParallelRunnerDestroy(runner);
  JxlDecoderDestroy(decoder);

  // Set all needed type flags and make a record about the loader
  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->loader = LOADER_JPEGXL;

  // JXL can be LDR or HDR. But if channel width does not exceed 8 bit it must be LDR.
  if(basicinfo.bits_per_sample <= 8)
  {
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
  } else
  {
    img->flags &= ~DT_IMAGE_LDR;
    img->flags |= DT_IMAGE_HDR;
  }

  return DT_IMAGEIO_OK;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
