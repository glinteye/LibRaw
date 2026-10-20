# LibRaw for GlintEye

This branch is LibRaw 0.22.2 plus one change used by
GlintEye, a photo culling app that reads only the JPEG previews embedded in
RAW files.

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

Behaviour without the option is unchanged. The patch is the commit after the
upstream `0.22.2` tag on this branch; release tags are named
`0.22.2-glinteye.N`.

## License

Unchanged from LibRaw: LGPL 2.1 or CDDL 1.0, at your option. See `COPYRIGHT`,
