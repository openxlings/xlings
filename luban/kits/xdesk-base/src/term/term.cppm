// A terminal's screen and the byte stream that paints it.
//
// Bytes in (what a program writes: text, control characters, escape
// sequences), a grid of cells out, and the replies a program expects back (a
// cursor position report, a device attributes answer) -- a shell that asks
// where the cursor is and gets no answer waits forever. What it does not speak
// is ignored and counted (`unhandled`), which is how a program using something
// missing here is told apart from a program drawing strangely.
//
// The subset is what shells and full-screen programs send: cursor motion,
// erase, insert and delete, scroll regions, the alternate screen, SGR in all
// three colour depths, wide characters, bracketed paste, mouse reporting, the
// title. xterm's meaning for each.
export module xdesk.term;

import std;

export namespace xdesk::term {

struct Color {
    enum class Kind : std::uint8_t { Default, Indexed, Rgb };
    Kind kind { Kind::Default };
    std::uint32_t value { 0 };     // Indexed: 0..255; Rgb: 0xRRGGBB
    bool operator==(const Color&) const = default;
};

enum Attr : std::uint16_t {
    Bold = 1, Faint = 2, Italic = 4, Underline = 8, Blink = 16, Inverse = 32, Hidden = 64, Strike = 128,
};

struct Cell {
    char32_t ch { U' ' };
    Color fg, bg;
    std::uint16_t attrs { 0 };
    bool continuation { false };   // the second column of a wide character
    bool operator==(const Cell&) const = default;
};

// How many columns a character takes: 0 for a combining mark, 2 for East
// Asian wide and emoji, 1 otherwise.
int width(char32_t c);

class Screen {
public:
    Screen(int rows, int cols);

    void resize(int rows, int cols);
    void feed(std::string_view bytes);

    // Replies the program expects (DSR, DA), drained.
    std::string take_replies();

    int rows() const { return rows_; }
    int cols() const { return cols_; }
    const Cell& at(int row, int col) const { return grid()[std::size_t(row) * cols_ + col]; }
    int cursor_row() const { return cy_; }
    int cursor_col() const { return std::min(cx_, cols_ - 1); }
    bool cursor_visible() const { return cursor_visible_; }

    const std::string& title() const { return title_; }
    bool alternate() const { return alt_active_; }
    bool bracketed_paste() const { return bracketed_paste_; }
    bool application_cursor() const { return app_cursor_; }
    // 0 none, 1000 buttons, 1002 drag, 1003 any motion; SGR encoding with 1006.
    int mouse_mode() const { return mouse_mode_; }
    bool mouse_sgr() const { return mouse_sgr_; }

    // The cursor's shape, as DECSCUSR set it.
    enum class CursorShape { Block, Underline, Bar };
    CursorShape cursor_shape() const { return cursor_shape_; }

    // What the program asked to put on the clipboard (OSC 52), drained. Whether
    // it reaches the host's clipboard is the embedder's grant to decide.
    std::optional<std::string> take_clipboard() { return std::exchange(clipboard_, std::nullopt); }

    // Lines scrolled off the primary screen, oldest first (bounded).
    std::size_t scrollback() const { return history_.size(); }
    const std::vector<Cell>& history_line(std::size_t i) const { return history_[i]; }

    // Set by every change, cleared by the reader.
    bool dirty { true };
    std::size_t unhandled { 0 };

private:
    const std::vector<Cell>& grid() const { return alt_active_ ? alt_ : main_; }
    std::vector<Cell>& grid() { return alt_active_ ? alt_ : main_; }
    Cell blank() const;
    void put(char32_t c);
    void newline();
    void index();          // down one, scrolling the region at its bottom
    void reverse_index();  // up one, scrolling the region at its top
    void scroll_up(int top, int bottom, int n);
    void scroll_down(int top, int bottom, int n);
    void erase(int row, int from, int to);   // [from, to)
    void csi(char final, std::string_view params, char prefix, char intermediate);
    void sgr(const std::vector<int>& p);
    void set_mode(int mode, bool on, bool priv);
    void osc(std::string_view body);
    void control(char c);
    void esc(char c, char intermediate);

    int rows_, cols_;
    std::vector<Cell> main_, alt_;
    std::deque<std::vector<Cell>> history_;
    int cx_ { 0 }, cy_ { 0 };
    bool wrap_pending_ { false };
    int top_ { 0 }, bottom_ { 0 };          // the scroll region, inclusive
    Cell pen_;
    struct Saved { int cx, cy; Cell pen; } saved_ { 0, 0, {} };
    bool cursor_visible_ { true };
    bool autowrap_ { true };
    bool insert_ { false };
    bool alt_active_ { false };
    bool bracketed_paste_ { false };
    bool app_cursor_ { false };
    int mouse_mode_ { 0 };
    bool mouse_sgr_ { false };
    std::string title_;
    std::string replies_;
    CursorShape cursor_shape_ { CursorShape::Block };
    std::optional<std::string> clipboard_;

    // The parser's state between feeds.
    enum class State { Ground, Escape, Csi, Osc, OscEscape, Charset } state_ { State::Ground };
    std::string seq_;
    bool osc_ { true };            // the string being read is an OSC (not a DCS, APC or PM)
    char intermediate_ { 0 };
    std::uint32_t utf8_ { 0 };
    int utf8_left_ { 0 };
};

// The bytes a key sends, xterm's: `key` is a name ("Up", "Enter", "F5",
// "Backspace", "Tab", "Escape", "Home", ..., or one character), `mods` the
// opensal modifier bits (Shift 1, Ctrl 2, Alt 4).
std::string encode_key(std::string_view key, std::uint32_t mods, bool application_cursor);

// What a paste sends: the text, inside the bracketed-paste markers when the
// program asked for them.
std::string encode_paste(std::string_view text, bool bracketed);

}  // namespace xdesk::term
