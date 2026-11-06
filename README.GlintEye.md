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
`libraw_unpack_thumb()` selects it, as it does a CR3's full-size JPEG. Stock
LibRaw returns `PRVW` and `THMB` as configuration boxes and coded pictures, which
no decoder reads, so here all three come back as complete HEIF files, without
re-encoding and carrying the photo's orientation. `libraw_dcraw_make_mem_thumb()`
still reports them as `LIBRAW_IMAGE_H265`.

## Tags

The changes are the commits after the upstream `0.22.2` tag on this branch;
release tags are named `0.22.2-glinteye.N`. This branch also tracks the generated
build files from LibRaw's release tarball, so a tag's source archive builds with
`./configure` alone. When moving to a new LibRaw release, take them from its tarball.

## License

Unchanged from LibRaw: LGPL 2.1 or CDDL 1.0, at your option. See `COPYRIGHT`,
