// xdesk.native.png: libpng's C header, read here and nowhere else -- what
// xdesk.draw reads an application's icon with (its simplified API). Macros
// are constants here (tools/lint_c_headers.sh).
module;

#include <png.h>

export module xdesk.native.png;

export using ::png_image;
export using ::png_image_begin_read_from_file;
export using ::png_image_finish_read;
export using ::png_image_free;

export namespace xdesk::native::png {
inline constexpr unsigned version = PNG_IMAGE_VERSION;
// Premultiplied is xdesk.draw's; libpng gives straight BGRA (ARGB in a little-endian word).
inline constexpr unsigned format_bgra = PNG_FORMAT_BGRA;
inline bool failed(const png_image& img) { return PNG_IMAGE_FAILED(img); }
inline ::size_t size(const png_image& img) { return PNG_IMAGE_SIZE(img); }
}
