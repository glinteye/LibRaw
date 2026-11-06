# LibRaw for GlintEye

This branch is LibRaw 0.22.2 plus two changes used by
GlintEye, a photo culling app that reads only the previews embedded in RAW
files.

## `LIBRAW_RAWOPTIONS_ALLOW_THUMBNAIL_ONLY`

Stock LibRaw refuses to open a file when it has no decoder for the file's raw
data, even if it has already found the embedded previews. With this option set
before opening, such a file opens with `imgdata.idata.raw_count == 0`, so its
thumbnails can still be unpacked:

```c
libraw_data_t *lr = libraw_init(0);
lr->rawparams.options |= LIBRAW_RAWOPTIONS_ALLOW_THUMBNAIL_ONLY;
if (libraw_open_file(lr, path) == LIBRAW_SUCCESS) {
  /* lr->idata.raw_count == 0: previews only; libraw_unpack() refuses. */
  libraw_unpack_thumb_ex(lr, 0);
}
```

Behaviour without the option is unchanged.

## Full-size preview of Canon CR3 files shot with HDR PQ

These files have no JPEG. Their `THMB` and `PRVW` previews, which stock LibRaw
lists and returns as `LIBRAW_THUMBNAIL_H265`, are 320 and 1620 pixels wide. The
full-size image is a grid of HEVC tiles in the file's first track, and stock
LibRaw does not list it.

This branch lists it as one more thumbnail with its real size, and the default
`libraw_unpack_thumb()` selects it, as it does a CR3's full-size JPEG.
`libraw_dcraw_make_mem_thumb()` returns it as `LIBRAW_IMAGE_H265`, in the
layout of the `PRVW` data: the `GRID`, `CISZ`, `hvcC`, `colr` and `pixi`
boxes, then an `IMGD` box holding each tile as a 4-byte length followed by its
NAL units.

## Tags

The patches are the commits after the upstream `0.22.2` tag on this branch;
release tags are named `0.22.2-glinteye.N`.

## License

Unchanged from LibRaw: LGPL 2.1 or CDDL 1.0, at your option. See `COPYRIGHT`,
