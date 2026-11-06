/* -*- C++ -*-
 * Copyright 2019-2025 LibRaw LLC (info@libraw.org)
 *
 LibRaw is free software; you can redistribute it and/or modify
 it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

#include "../../internal/libraw_cxx_defs.h"
#include <vector>

namespace
{
// Canon CR3 files shot with HDR PQ have no JPEG, only HEVC previews. The camera
// stores those as configuration boxes and coded pictures, which no decoder reads,
// so they are put into a HEIF file here, without re-encoding.
typedef std::vector<unsigned char> HeifBytes;

struct HeifSpan
{
  const unsigned char *data;
  size_t size;
};

unsigned read_be32(const unsigned char *p)
{
  return (unsigned(p[0]) << 24) | (unsigned(p[1]) << 16) | (unsigned(p[2]) << 8) | p[3];
}

// The content of the first `kind` box, looking until something is not a box.
bool find_box(const unsigned char *boxes, size_t length, const char *kind, HeifSpan *found)
{
  while (length >= 8)
  {
    const size_t size = read_be32(boxes);
    if (size < 8 || size > length)
      return false;
    if (!memcmp(boxes + 4, kind, 4))
    {
      found->data = boxes + 8;
      found->size = size - 8;
      return true;
    }
    boxes += size;
    length -= size;
  }
  return false;
}

void put_u8(HeifBytes &out, unsigned value) { out.push_back((unsigned char)value); }

void put_u16(HeifBytes &out, unsigned value)
{
  put_u8(out, (value >> 8) & 0xff);
  put_u8(out, value & 0xff);
}

void put_u32(HeifBytes &out, unsigned value)
{
  put_u16(out, value >> 16);
  put_u16(out, value & 0xffff);
}

void put_bytes(HeifBytes &out, const void *bytes, size_t size)
{
  const unsigned char *start = (const unsigned char *)bytes;
  out.insert(out.end(), start, start + size);
}

void set_u32(HeifBytes &out, size_t at, unsigned value)
{
  out[at] = (unsigned char)(value >> 24);
  out[at + 1] = (unsigned char)(value >> 16);
  out[at + 2] = (unsigned char)(value >> 8);
  out[at + 3] = (unsigned char)value;
}

size_t open_box(HeifBytes &out, const char *kind)
{
  const size_t start = out.size();
  put_u32(out, 0);
  put_bytes(out, kind, 4);
  return start;
}

size_t open_full_box(HeifBytes &out, const char *kind, unsigned version, unsigned flags)
{
  const size_t start = open_box(out, kind);
  put_u32(out, (version << 24) | flags);
  return start;
}

void close_box(HeifBytes &out, size_t start) { set_u32(out, start, unsigned(out.size() - start)); }

void put_box(HeifBytes &out, const char *kind, const HeifSpan &content)
{
  const size_t box = open_box(out, kind);
  put_bytes(out, content.data, content.size);
  close_box(out, box);
}

void put_image_size(HeifBytes &out, unsigned width, unsigned height)
{
  const size_t box = open_full_box(out, "ispe", 0, 0);
  put_u32(out, width);
  put_u32(out, height);
  close_box(out, box);
}

void put_item_info(HeifBytes &out, unsigned item, const char *kind, unsigned flags)
{
  const size_t box = open_full_box(out, "infe", 2, flags);
  put_u16(out, item);
  put_u16(out, 0);
  put_bytes(out, kind, 4);
  put_u8(out, 0);
  close_box(out, box);
}

void put_item_location(HeifBytes &out, unsigned item, unsigned method, unsigned offset, unsigned size)
{
  put_u16(out, item);
  put_u16(out, method);
  put_u16(out, 0); // data reference index
  put_u16(out, 1); // extent count
  put_u32(out, offset);
  put_u32(out, size);
}

void put_associations(HeifBytes &out, unsigned item, const unsigned char *properties, unsigned count)
{
  put_u16(out, item);
  put_u8(out, count);
  put_bytes(out, properties, count);
}

// `display_*` is the preview's size, from the top left of the coded picture; a grid
// carries its own size. `flip` is sizes.flip. False means the data cannot be used.
bool canon_hevc_to_heif(const unsigned char *data, size_t length, unsigned display_width,
                        unsigned display_height, int flip, HeifBytes &heif)
{
  HeifSpan cisz, hvcc, colr, pixi, imgd, grid;
  if (!find_box(data, length, "CISZ", &cisz) || !find_box(data, length, "hvcC", &hvcc) ||
      !find_box(data, length, "colr", &colr) || !find_box(data, length, "pixi", &pixi) ||
      !find_box(data, length, "IMGD", &imgd) || cisz.size < 12)
    return false;
  const unsigned coded_width = read_be32(cisz.data + 4);
  const unsigned coded_height = read_be32(cisz.data + 8);

  // IMGD holds each coded picture as a 4-byte length and its NAL units.
  std::vector<size_t> starts, sizes;
  for (size_t at = 0; at < imgd.size;)
  {
    if (imgd.size - at < 4)
      return false;
    const size_t size = read_be32(imgd.data + at);
    at += 4;
    if (size == 0 || size > imgd.size - at)
      return false;
    starts.push_back(at);
    sizes.push_back(size);
    at += size;
  }

  // flip 3, 5 and 6 are EXIF orientations 3, 8 and 6, the only ones a camera writes.
  // HEIF rotates anticlockwise in quarter turns.
  unsigned quarter_turns;
  switch (flip)
  {
  case 0: quarter_turns = 0; break;
  case 3: quarter_turns = 2; break;
  case 5: quarter_turns = 1; break;
  case 6: quarter_turns = 3; break;
  default: return false;
  }

  const bool is_grid = find_box(data, length, "GRID", &grid);
  if (is_grid)
  {
    // Version and flags, tile count, then rows and columns less one in the last two
    // bytes of a word, then the 16-bit output size.
    if (grid.size < 16 || (grid.data[10] + 1u) * (grid.data[11] + 1u) != starts.size() ||
        starts.size() > 0xfffe)
      return false;
  }
  else if (starts.size() != 1 || !display_width || !display_height ||
           display_width > coded_width || display_height > coded_height)
    return false;

  const unsigned picture_count = unsigned(starts.size());
  const unsigned primary = is_grid ? picture_count + 1 : 1;
  const unsigned char coded_properties[] = {0x81, 2, 3, 4}; // hvcC (essential), ispe, colr, pixi
  heif.reserve(imgd.size + 1024 + 64 * starts.size());

  size_t box = open_box(heif, "ftyp");
  put_bytes(heif, "heix", 4);
  put_u32(heif, 0);
  put_bytes(heif, "mif1", 4);
  put_bytes(heif, "heix", 4);
  close_box(heif, box);

  const size_t meta = open_full_box(heif, "meta", 0, 0);
  box = open_full_box(heif, "hdlr", 0, 0);
  put_u32(heif, 0);
  put_bytes(heif, "pict", 4);
  for (int i = 0; i < 13; i++)
    put_u8(heif, 0);
  close_box(heif, box);

  box = open_full_box(heif, "pitm", 0, 0);
  put_u16(heif, primary);
  close_box(heif, box);

  // Picture offsets are relative to the mdat content until its position is known.
  std::vector<size_t> offsets_to_fix;
  box = open_full_box(heif, "iloc", 1, 0);
  put_u8(heif, 0x44); // 4-byte offsets and lengths
  put_u8(heif, 0);    // no base offset, no extent index
  put_u16(heif, picture_count + (is_grid ? 1 : 0));
  for (unsigned i = 0; i < picture_count; i++)
  {
    put_item_location(heif, i + 1, 0, unsigned(starts[i]), unsigned(sizes[i]));
    offsets_to_fix.push_back(heif.size() - 8);
  }
  if (is_grid)
    put_item_location(heif, primary, 1, 0, 8); // construction method 1: within idat
  close_box(heif, box);

  box = open_full_box(heif, "iinf", 0, 0);
  put_u16(heif, picture_count + (is_grid ? 1 : 0));
  for (unsigned i = 0; is_grid && i < picture_count; i++)
    put_item_info(heif, i + 1, "hvc1", 1); // flag 1: hidden, shown only through the grid
  put_item_info(heif, primary, is_grid ? "grid" : "hvc1", 0);
  close_box(heif, box);

  if (is_grid)
  {
    box = open_full_box(heif, "iref", 0, 0);
    const size_t references = open_box(heif, "dimg");
    put_u16(heif, primary);
    put_u16(heif, picture_count);
    for (unsigned i = 0; i < picture_count; i++)
      put_u16(heif, i + 1);
    close_box(heif, references);
    close_box(heif, box);
  }

  unsigned char primary_properties[8];
  unsigned primary_property_count = 0;
  unsigned next_property = 5;
  const size_t iprp = open_box(heif, "iprp");
  const size_t ipco = open_box(heif, "ipco");
  put_box(heif, "hvcC", hvcc);
  put_image_size(heif, coded_width, coded_height);
  put_box(heif, "colr", colr);
  put_box(heif, "pixi", pixi);
  if (is_grid)
  {
    put_image_size(heif, (grid.data[12] << 8) | grid.data[13], (grid.data[14] << 8) | grid.data[15]);
    primary_properties[primary_property_count++] = (unsigned char)next_property++;
    primary_properties[primary_property_count++] = 3;
    primary_properties[primary_property_count++] = 4;
  }
  else
  {
    memcpy(primary_properties, coded_properties, sizeof(coded_properties));
    primary_property_count = sizeof(coded_properties);
    if (display_width != coded_width || display_height != coded_height)
    {
      // A clean aperture is positioned by its centre's offset from the picture's centre.
      box = open_box(heif, "clap");
      put_u32(heif, display_width);
      put_u32(heif, 1);
      put_u32(heif, display_height);
      put_u32(heif, 1);
      put_u32(heif, unsigned(int(display_width) - int(coded_width)));
      put_u32(heif, 2);
      put_u32(heif, unsigned(int(display_height) - int(coded_height)));
      put_u32(heif, 2);
      close_box(heif, box);
      primary_properties[primary_property_count++] = (unsigned char)(0x80 | next_property++);
    }
  }
  if (quarter_turns)
  {
    box = open_box(heif, "irot");
    put_u8(heif, quarter_turns);
    close_box(heif, box);
    primary_properties[primary_property_count++] = (unsigned char)(0x80 | next_property++);
  }
  close_box(heif, ipco);

  box = open_full_box(heif, "ipma", 0, 0);
  put_u32(heif, is_grid ? picture_count + 1 : 1);
  for (unsigned i = 0; is_grid && i < picture_count; i++)
    put_associations(heif, i + 1, coded_properties, sizeof(coded_properties));
  put_associations(heif, primary, primary_properties, primary_property_count);
  close_box(heif, box);
  close_box(heif, iprp);

  if (is_grid)
  {
    box = open_box(heif, "idat");
    put_u16(heif, 0); // version and flags: 16-bit output size
    put_u8(heif, grid.data[10]);
    put_u8(heif, grid.data[11]);
    put_bytes(heif, grid.data + 12, 4);
    close_box(heif, box);
  }
  close_box(heif, meta);

  box = open_box(heif, "mdat");
  const size_t mdat_content = heif.size();
  put_bytes(heif, imgd.data, imgd.size);
  close_box(heif, box);
  for (size_t i = 0; i < offsets_to_fix.size(); i++)
    set_u32(heif, offsets_to_fix[i], read_be32(&heif[offsets_to_fix[i]]) + unsigned(mdat_content));
  return true;
}
} // namespace

#ifndef NO_JPEG
struct jpegErrorManager
{
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
};

static void jpegErrorExit(j_common_ptr cinfo)
{
  jpegErrorManager *myerr = (jpegErrorManager *)cinfo->err;
  longjmp(myerr->setjmp_buffer, 1);
}
#endif

int LibRaw::unpack_thumb_ex(int idx)
{
	if (idx < 0 || idx >= imgdata.thumbs_list.thumbcount || idx >= LIBRAW_THUMBNAIL_MAXCOUNT)
		return LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL;

	// Set from thumb-list
    libraw_internal_data.internal_data.toffset = imgdata.thumbs_list.thumblist[idx].toffset;
    imgdata.thumbnail.tlength = imgdata.thumbs_list.thumblist[idx].tlength;
    libraw_internal_data.unpacker_data.thumb_format = imgdata.thumbs_list.thumblist[idx].tformat; 
    imgdata.thumbnail.twidth = imgdata.thumbs_list.thumblist[idx].twidth;
    imgdata.thumbnail.theight = imgdata.thumbs_list.thumblist[idx].theight;
	libraw_internal_data.unpacker_data.thumb_misc = imgdata.thumbs_list.thumblist[idx].tmisc;
	int rc = unpack_thumb();
    imgdata.progress_flags &= ~LIBRAW_PROGRESS_THUMB_LOAD;

	return rc;
}


int LibRaw::unpack_thumb(void)
{
  CHECK_ORDER_LOW(LIBRAW_PROGRESS_IDENTIFY);
  CHECK_ORDER_BIT(LIBRAW_PROGRESS_THUMB_LOAD);

#define THUMB_SIZE_CHECKT(A) \
  do { \
    if (INT64(A) > 1024LL * 1024LL * LIBRAW_MAX_THUMBNAIL_MB) return LIBRAW_UNSUPPORTED_THUMBNAIL; \
    if (INT64(A) > 0 &&  INT64(A) < 64LL)        return LIBRAW_NO_THUMBNAIL; \
  } while (0)

#define THUMB_SIZE_CHECKTNZ(A) \
  do { \
    if (INT64(A) > 1024LL * 1024LL * LIBRAW_MAX_THUMBNAIL_MB) return LIBRAW_UNSUPPORTED_THUMBNAIL; \
    if (INT64(A) < 64LL)        return LIBRAW_NO_THUMBNAIL; \
  } while (0)


#define THUMB_SIZE_CHECKWH(W,H) \
  do { \
    if (INT64(W)*INT64(H) > 1024ULL * 1024ULL * LIBRAW_MAX_THUMBNAIL_MB) return LIBRAW_UNSUPPORTED_THUMBNAIL; \
    if (INT64(W)*INT64(H) < 64ULL)        return LIBRAW_NO_THUMBNAIL; \
  } while (0)

#define Tformat libraw_internal_data.unpacker_data.thumb_format

  try
  {
    if (!libraw_internal_data.internal_data.input)
      return LIBRAW_INPUT_CLOSED;

    int t_colors = libraw_internal_data.unpacker_data.thumb_misc >> 5 & 7;
    int t_bytesps = (libraw_internal_data.unpacker_data.thumb_misc & 31) / 8;

    if (!ID.toffset && !(imgdata.thumbnail.tlength > 0 &&
                         load_raw == &LibRaw::broadcom_load_raw)  // RPi
#ifdef USE_6BY9RPI
        && !(imgdata.thumbnail.tlength > 0 && libraw_internal_data.unpacker_data.load_flags & 0x4000
            && (load_raw == &LibRaw::rpi_load_raw8 || load_raw == &LibRaw::nokia_load_raw ||
           load_raw == &LibRaw::rpi_load_raw12 || load_raw == &LibRaw::rpi_load_raw14))
#endif
    )
    {
      return LIBRAW_NO_THUMBNAIL;
    }
	else if (Tformat == LIBRAW_INTERNAL_THUMBNAIL_DNG_YCBCR)
	{
	  try
	  {
        dng_ycbcr_thumb_loader();
        T.tformat = LIBRAW_THUMBNAIL_BITMAP;
        T.tcolors = 3;
        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
	  }
	  catch (...)
	  {
        return LIBRAW_NO_THUMBNAIL;
	  }
      return 0;

	}
    else if ((Tformat >= LIBRAW_INTERNAL_THUMBNAIL_KODAK_THUMB)
		&& ((Tformat <= LIBRAW_INTERNAL_THUMBNAIL_KODAK_RGB)))
    {
		try {
          kodak_thumb_loader();
          T.tformat = LIBRAW_THUMBNAIL_BITMAP;
          SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
		}
		catch (...)
		{
          return LIBRAW_NO_THUMBNAIL;
		}
      return 0;
    }
    else
    {
#ifdef USE_X3FTOOLS
	if (Tformat == LIBRAW_INTERNAL_THUMBNAIL_X3F)
      {
        INT64 tsize = x3f_thumb_size();
        if (tsize < 2048 || INT64(ID.toffset) + tsize < 1)
          return LIBRAW_NO_THUMBNAIL;

        if (INT64(ID.toffset) + tsize > ID.input->size() + THUMB_READ_BEYOND)
          return LIBRAW_NO_THUMBNAIL;

        THUMB_SIZE_CHECKT(tsize);
      }
#else
	if (0) {}
#endif
      else
      {
        if (INT64(ID.toffset) + INT64(T.tlength) < 1)
          return LIBRAW_NO_THUMBNAIL;

        if (INT64(ID.toffset) + INT64(T.tlength) >
            ID.input->size() + THUMB_READ_BEYOND)
          return LIBRAW_NO_THUMBNAIL;
      }

      ID.input->seek(ID.toffset, SEEK_SET);
      if (Tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEG || Tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEGXL)
      {
        // An earlier Canon HEVC unpack left tlength as the HEIF's size, not the stored one.
        if (T.tformat == LIBRAW_THUMBNAIL_H265)
          for (int idx = 0; idx < imgdata.thumbs_list.thumbcount; idx++)
            if (imgdata.thumbs_list.thumblist[idx].toffset == ID.toffset)
            {
              T.tlength = imgdata.thumbs_list.thumblist[idx].tlength;
              break;
            }
        THUMB_SIZE_CHECKTNZ(T.tlength);
        if (T.thumb)
          free(T.thumb);
#ifdef LIBRAW_CALLOC_RAWSTORE
        T.thumb = (char *)calloc(T.tlength,1);
#else
        T.thumb = (char *)malloc(T.tlength);
#endif
		unsigned config_length = libraw_internal_data.unpacker_data.cr3_hevc_config_length;
		const bool hevc_track = config_length && ID.toffset == libraw_internal_data.unpacker_data.cr3_hevc_sample_offset;
		if (hevc_track)
		{
			// The CR3 sample holds only image data; its configuration is in the
			// track header. Join them in the layout of the H.265 PRVW data:
			// configuration boxes, then the image data in an IMGD box.
			unsigned imgd_length = T.tlength - config_length;
			unsigned sample_length = imgd_length - 8;
			unsigned char *imgd = (unsigned char *)T.thumb + config_length;
			ID.input->seek(libraw_internal_data.unpacker_data.cr3_hevc_config_offset, SEEK_SET);
			if (ID.input->read(T.thumb, 1, config_length) != int(config_length))
				return LIBRAW_IO_ERROR;
			imgd[0] = imgd_length >> 24;
			imgd[1] = imgd_length >> 16;
			imgd[2] = imgd_length >> 8;
			imgd[3] = imgd_length;
			memcpy(imgd + 4, "IMGD", 4);
			ID.input->seek(ID.toffset, SEEK_SET);
			if (ID.input->read(imgd + 8, 1, sample_length) != int(sample_length))
				return LIBRAW_IO_ERROR;
		}
		else
			ID.input->read(T.thumb, 1, T.tlength);
		unsigned char *tthumb = (unsigned char *)T.thumb;
		if (Tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEGXL)
		{
          T.tformat = LIBRAW_THUMBNAIL_JPEGXL;
          SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
          return 0;
		}
		// Check if it is canon H.265 preview:  CISZ at bytes 4-6, CISZ prefix is 000n
		if (hevc_track || (load_raw == &LibRaw::crxLoadRaw && T.tlength > 0xE0 && tthumb[0] == 0 &&
		                   tthumb[1] == 0 && tthumb[2] == 0 && !memcmp(tthumb + 4, "CISZ", 4)))
		{
			unsigned display_width = 0, display_height = 0;
			for (int idx = 0; idx < imgdata.thumbs_list.thumbcount; idx++)
				if (imgdata.thumbs_list.thumblist[idx].toffset == ID.toffset)
				{
					display_width = imgdata.thumbs_list.thumblist[idx].twidth;
					display_height = imgdata.thumbs_list.thumblist[idx].theight;
					break;
				}
			HeifBytes heif;
			if (!canon_hevc_to_heif(tthumb, T.tlength, display_width, display_height, S.flip, heif))
				return LIBRAW_UNSUPPORTED_THUMBNAIL;
			free(T.thumb);
			T.thumb = (char *)malloc(heif.size());
			memcpy(T.thumb, &heif[0], heif.size());
			T.tlength = unsigned(heif.size());
			T.tformat = LIBRAW_THUMBNAIL_H265;
			SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
			return 0;
		}
        tthumb[0] = 0xff;
        tthumb[1] = 0xd8;
#ifdef NO_JPEG
        T.tcolors = 3;
#else
        {
          jpegErrorManager jerr;
          struct jpeg_decompress_struct cinfo;
          cinfo.err = jpeg_std_error(&jerr.pub);
          jerr.pub.error_exit = jpegErrorExit;
          if (setjmp(jerr.setjmp_buffer))
          {
          err2:
            // Error in original JPEG thumb, read it again because
            // original bytes 0-1 was damaged above
            jpeg_destroy_decompress(&cinfo);
            T.tcolors = 3;
            T.tformat = LIBRAW_THUMBNAIL_UNKNOWN;
            ID.input->seek(ID.toffset, SEEK_SET);
            ID.input->read(T.thumb, 1, T.tlength);
            SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
            return 0;
          }
          jpeg_create_decompress(&cinfo);
          jpeg_mem_src(&cinfo, (unsigned char *)T.thumb, T.tlength);
          int rc = jpeg_read_header(&cinfo, TRUE);
          if (rc != 1)
            goto err2;
          T.tcolors = (cinfo.num_components > 0 && cinfo.num_components <= 3)
                          ? cinfo.num_components
                          : 3;
          jpeg_destroy_decompress(&cinfo);
        }
#endif
        T.tformat = LIBRAW_THUMBNAIL_JPEG;
        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
        return 0;
      }
      else if (Tformat == LIBRAW_INTERNAL_THUMBNAIL_LAYER)
      {
        int colors = libraw_internal_data.unpacker_data.thumb_misc >> 5 & 7;
        if (colors != 1 && colors != 3)
          return LIBRAW_UNSUPPORTED_THUMBNAIL;

        THUMB_SIZE_CHECKWH(T.twidth, T.theight);

        int tlength = T.twidth * T.theight;
        if (T.thumb)
          free(T.thumb);
        T.thumb = (char *)calloc(colors, tlength);
        unsigned char *tbuf = (unsigned char *)calloc(colors, tlength);
        // Avoid OOB of tbuf, should use tlength
        ID.input->read(tbuf, colors, tlength);
        if (libraw_internal_data.unpacker_data.thumb_misc >> 8 &&
            colors == 3) // GRB order
          for (int i = 0; i < tlength; i++)
          {
            T.thumb[i * 3] = tbuf[i + tlength];
            T.thumb[i * 3 + 1] = tbuf[i];
            T.thumb[i * 3 + 2] = tbuf[i + 2 * tlength];
          }
        else if (colors == 3) // RGB or 1-channel
          for (int i = 0; i < tlength; i++)
          {
            T.thumb[i * 3] = tbuf[i];
            T.thumb[i * 3 + 1] = tbuf[i + tlength];
            T.thumb[i * 3 + 2] = tbuf[i + 2 * tlength];
          }
        else if (colors == 1)
        {
          free(T.thumb);
          T.thumb = (char *)tbuf;
          tbuf = 0;
        }
        if (tbuf)
          free(tbuf);
        T.tcolors = colors;
        T.tlength = colors * tlength;
        T.tformat = LIBRAW_THUMBNAIL_BITMAP;
        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
        return 0;
      }
      else if (Tformat == LIBRAW_INTERNAL_THUMBNAIL_ROLLEI)
      {
        int i;
        THUMB_SIZE_CHECKWH(T.twidth, T.theight);
        int tlength = T.twidth * T.theight;
        if (T.thumb)
          free(T.thumb);
        T.tcolors = 3;
        T.thumb = (char *)calloc(T.tcolors, tlength);
        unsigned short *tbuf = (unsigned short *)calloc(2, tlength);
		try {
  		  read_shorts(tbuf, tlength);
          for (i = 0; i < tlength; i++)
          {
            T.thumb[i * 3] = (tbuf[i] << 3) & 0xff;
            T.thumb[i * 3 + 1] = (tbuf[i] >> 5 << 2) & 0xff;
            T.thumb[i * 3 + 2] = (tbuf[i] >> 11 << 3) & 0xff;
          }
          free(tbuf);
          T.tlength = T.tcolors * tlength;
          T.tformat = LIBRAW_THUMBNAIL_BITMAP;
          SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
		}
		catch (...)
		{
			free(tbuf);
			return LIBRAW_NO_THUMBNAIL;
		}
        return 0;
      }
      else if (Tformat == LIBRAW_INTERNAL_THUMBNAIL_PPM)
      {
        if (t_bytesps > 1)
          return LIBRAW_NO_THUMBNAIL;  // 8-bit thumb, but parsed for more
                                             // bits
        THUMB_SIZE_CHECKWH(T.twidth, T.theight);
        int t_length = T.twidth * T.theight * t_colors;

        if (T.tlength &&
            (int)T.tlength < t_length) // try to find tiff ifd with needed offset
        {
          int pifd = find_ifd_by_offset(libraw_internal_data.internal_data.toffset);
          if (pifd >= 0 && tiff_ifd[pifd].strip_offsets_count &&
              tiff_ifd[pifd].strip_byte_counts_count)
          {
            // We found it, calculate final size
            INT64 total_size = 0;
            for (int i = 0; i < tiff_ifd[pifd].strip_byte_counts_count 
				&& i < tiff_ifd[pifd].strip_offsets_count; i++)
              total_size += tiff_ifd[pifd].strip_byte_counts[i];
            if (total_size != (unsigned)t_length) // recalculate colors
            {
              if (total_size == T.twidth * T.theight * 3)
                T.tcolors = 3;
              else if (total_size == T.twidth * T.theight)
                T.tcolors = 1;
            }
            T.tlength = unsigned(total_size);
            THUMB_SIZE_CHECKTNZ(T.tlength);
            if (T.thumb)
              free(T.thumb);
#ifdef LIBRAW_CALLOC_RAWSTORE
            T.thumb = (char *)calloc(T.tlength,1);
#else
            T.thumb = (char *)malloc(T.tlength);
#endif
            char *dest = T.thumb;
            INT64 pos = ID.input->tell();
            INT64 remain = T.tlength;

            for (int i = 0; i < tiff_ifd[pifd].strip_byte_counts_count &&
                            i < tiff_ifd[pifd].strip_offsets_count;
                 i++)
            {
              int sz = tiff_ifd[pifd].strip_byte_counts[i];
              INT64 off = tiff_ifd[pifd].strip_offsets[i];
              if (off >= 0 && off + sz <= ID.input->size() && sz > 0 && INT64(sz) <= remain)
              {
                ID.input->seek(off, SEEK_SET);
                ID.input->read(dest, sz, 1);
                remain -= sz;
                dest += sz;
              }
            }
            ID.input->seek(pos, SEEK_SET);
            T.tformat = LIBRAW_THUMBNAIL_BITMAP;
            SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
            return 0;
          }
        }

        if (!T.tlength)
          T.tlength = t_length;

		THUMB_SIZE_CHECKTNZ(T.tlength);

		if (T.thumb)
          free(T.thumb);

#ifdef LIBRAW_CALLOC_RAWSTORE
        T.thumb = (char *)calloc(T.tlength,1);
#else
        T.thumb = (char *)malloc(T.tlength);
#endif
        if (!T.tcolors)
          T.tcolors = t_colors;

        ID.input->read(T.thumb, 1, T.tlength);

        T.tformat = LIBRAW_THUMBNAIL_BITMAP;
        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
        return 0;
      }
      else if (Tformat == LIBRAW_INTERNAL_THUMBNAIL_PPM16)
      {
        if (t_bytesps > 2)
			return LIBRAW_NO_THUMBNAIL; // 16-bit thumb, but parsed for
                                             // more bits
        int o_bps = (imgdata.rawparams.options & LIBRAW_RAWOPTIONS_USE_PPM16_THUMBS) ? 2 : 1;

		THUMB_SIZE_CHECKWH(T.twidth, (INT64(T.theight)*INT64(o_bps)));

		int o_length = T.twidth * T.theight * t_colors * o_bps;
        int i_length = T.twidth * T.theight * t_colors * 2;

		THUMB_SIZE_CHECKTNZ(o_length);
        THUMB_SIZE_CHECKTNZ(i_length);

        ushort *t_thumb = (ushort *)calloc(i_length, 1);
        ID.input->read(t_thumb, 1, i_length);
        if ((libraw_internal_data.unpacker_data.order == 0x4949) ==
            (ntohs(0x1234) == 0x1234))
          libraw_swab(t_thumb, i_length);

        if (T.thumb)
          free(T.thumb);
        if ((imgdata.rawparams.options & LIBRAW_RAWOPTIONS_USE_PPM16_THUMBS))
        {
          T.thumb = (char *)t_thumb;
          T.tformat = LIBRAW_THUMBNAIL_BITMAP16;
          T.tlength = i_length;
        }
        else
        {
#ifdef LIBRAW_CALLOC_RAWSTORE
          T.thumb = (char *)calloc(o_length,1);
#else
          T.thumb = (char *)malloc(o_length);
#endif
          for (int i = 0; i < o_length; i++)
            T.thumb[i] = t_thumb[i] >> 8;
          free(t_thumb);
          T.tformat = LIBRAW_THUMBNAIL_BITMAP;
          T.tlength = o_length;
        }
        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
        return 0;
      }
#ifdef USE_X3FTOOLS
	  else if (Tformat == LIBRAW_INTERNAL_THUMBNAIL_X3F)
      {
        x3f_thumb_loader(); // errors already catched in this call
        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
		if (!T.twidth && !T.theight)
			return LIBRAW_NO_THUMBNAIL;
        return 0;
      }
#endif
      else
      {
        return LIBRAW_UNSUPPORTED_THUMBNAIL;
      }
    }
    // last resort
    return LIBRAW_UNSUPPORTED_THUMBNAIL; /* warned as unreachable*/
  }
  catch (const LibRaw_exceptions& err)
  {
    EXCEPTION_HANDLER(err);
  }
}
