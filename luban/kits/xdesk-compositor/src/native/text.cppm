// xdesk.native.text: FreeType's and HarfBuzz's C headers, read here and
// nowhere else -- what xdesk.draw shapes and renders text with. Macros are
// constants here. A name xdesk.draw needs is added to this list, not
// #included where it is used (tools/lint_c_headers.sh).
module;

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

export module xdesk.native.text;

// FreeType
export using ::FT_Library;
export using ::FT_Face;
export using ::FT_Bitmap;
export using ::FT_UInt;
export using ::FT_Render_Mode;
export using ::FT_Init_FreeType;
export using ::FT_Done_FreeType;
export using ::FT_New_Face;
export using ::FT_Done_Face;
export using ::FT_Set_Pixel_Sizes;
export using ::FT_Get_Char_Index;
export using ::FT_Load_Glyph;
export using ::FT_Render_Glyph;
export using ::FT_RENDER_MODE_LIGHT;

// HarfBuzz
export using ::hb_font_t;
export using ::hb_buffer_t;
export using ::hb_glyph_info_t;
export using ::hb_glyph_position_t;
export using ::hb_buffer_create;
export using ::hb_buffer_destroy;
export using ::hb_buffer_clear_contents;
export using ::hb_buffer_add_utf8;
export using ::hb_buffer_guess_segment_properties;
export using ::hb_buffer_get_glyph_infos;
export using ::hb_buffer_get_glyph_positions;
export using ::hb_shape;
export using ::hb_font_destroy;
export using ::hb_ft_font_create_referenced;
export using ::hb_ft_font_set_load_flags;

export namespace xdesk::native::ft {
// FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT: hinted lightly, as text on screen is.
inline constexpr int load_light = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;
}
