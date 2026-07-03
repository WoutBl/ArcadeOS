/*
 * ArcadeOS – Boot Console Driver & Virtual Terminals
 *
 * Historically this was a VGA text-mode (0xB8000) driver. ArcadeOS boots
 * into a linear framebuffer, so the same terminal_* API now renders
 * glyphs onto the framebuffer through console_gfx. When no framebuffer
 * is available (GRUB fallback) it degrades to classic VGA text mode.
 *
 * Every character is also mirrored to the COM1 serial port so the
 * console can be debugged headless.
 */

#include "vga.h"
#include "fb.h"
#include "console_gfx.h"
#include "serial.h"

/* ──────── Global state ──────── */
uint16_t* terminal_buffer;
size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;

virtual_terminal_t terminals[MAX_TERMINALS];
int current_terminal = 0;

char current_dir[MAX_PATH] = "/";
char command_buffer[CMD_BUFFER_SIZE];
size_t command_length = 0;

static uint8_t current_text_color;
static uint8_t current_bg_color;

/* Shadow text buffer used when rendering to the framebuffer */
static uint16_t fb_shadow[VGA_WIDTH * VGA_HEIGHT];

/* Classic 16-color VGA palette in 0x00RRGGBB */
static const uint32_t vga_palette[16] = {
    0x000000, /* BLACK */        0x0000AA, /* BLUE */
    0x00AA00, /* GREEN */        0x00AAAA, /* CYAN */
    0xAA0000, /* RED */          0xAA00AA, /* MAGENTA */
    0xAA5500, /* BROWN */        0xAAAAAA, /* LIGHT_GREY */
    0x555555, /* DARK_GREY */    0x5555FF, /* LIGHT_BLUE */
    0x55FF55, /* LIGHT_GREEN */  0x55FFFF, /* LIGHT_CYAN */
    0xFF5555, /* LIGHT_RED */    0xFF55FF, /* LIGHT_MAGENTA */
    0xFFFF55, /* LIGHT_BROWN */  0xFFFFFF, /* WHITE */
};

/* ──────── Theme definitions ──────── */
int current_theme = 0;

color_theme_t themes[] = {
    /* Dark Mode (default) */
    {
        "dark",
        VGA_COLOR_WHITE, VGA_COLOR_BLACK,
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK,
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK,
        VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK,
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK,
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_LIGHT_BLUE
    },
    /* Light Mode */
    {
        "light",
        VGA_COLOR_BLACK, VGA_COLOR_WHITE,
        VGA_COLOR_BLUE, VGA_COLOR_WHITE,
        VGA_COLOR_CYAN, VGA_COLOR_WHITE,
        VGA_COLOR_RED, VGA_COLOR_WHITE,
        VGA_COLOR_GREEN, VGA_COLOR_WHITE,
        VGA_COLOR_BLUE, VGA_COLOR_CYAN
    },
    /* Matrix */
    {
        "matrix",
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK,
        VGA_COLOR_GREEN, VGA_COLOR_BLACK,
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK,
        VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK,
        VGA_COLOR_WHITE, VGA_COLOR_BLACK,
        VGA_COLOR_GREEN, VGA_COLOR_LIGHT_GREEN
    },
    /* Ocean */
    {
        "ocean",
        VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE,
        VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLUE,
        VGA_COLOR_WHITE, VGA_COLOR_BLUE,
        VGA_COLOR_LIGHT_RED, VGA_COLOR_BLUE,
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLUE,
        VGA_COLOR_LIGHT_BLUE, VGA_COLOR_CYAN
    },
    /* Fire */
    {
        "fire",
        VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED,
        VGA_COLOR_LIGHT_RED, VGA_COLOR_RED,
        VGA_COLOR_WHITE, VGA_COLOR_RED,
        VGA_COLOR_BLACK, VGA_COLOR_LIGHT_RED,
        VGA_COLOR_LIGHT_BROWN, VGA_COLOR_RED,
        VGA_COLOR_LIGHT_BROWN, VGA_COLOR_LIGHT_RED
    },
    /* Retro */
    {
        "retro",
        VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK,
        VGA_COLOR_BROWN, VGA_COLOR_BLACK,
        VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK,
        VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK,
        VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK,
        VGA_COLOR_BROWN, VGA_COLOR_LIGHT_BROWN
    }
};

/* ──────── Framebuffer glyph rendering ──────── */

/* Render one shadow-buffer cell to the framebuffer (8x8 glyph) */
static void fbcon_render_cell(size_t x, size_t y) {
    uint16_t entry = terminal_buffer[y * VGA_WIDTH + x];
    char     c     = (char)(entry & 0xFF);
    uint8_t  color = (uint8_t)(entry >> 8);
    gfx_front_char((int)(x * 8), (int)(y * 8),
                   c, vga_palette[color & 0x0F], vga_palette[(color >> 4) & 0x0F]);
}

/* Re-render the whole console (after vterm restore / scrollback view) */
static void fbcon_render_all(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            fbcon_render_cell(x, y);
}

/* ──────── Terminal init ──────── */
void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    current_text_color = themes[current_theme].text_fg;
    current_bg_color = themes[current_theme].text_bg;
    terminal_color = vga_entry_color(current_text_color, current_bg_color);

    /* Framebuffer mode renders into a shadow buffer;
     * text mode writes straight to VGA memory. */
    terminal_buffer = fb_available() ? fb_shadow : (uint16_t*)VGA_MEMORY;

    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
        }
    }
    if (fb_available())
        gfx_front_clear(vga_palette[current_bg_color & 0x0F]);
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

void terminal_update_cursor(void) {
    if (fb_available()) return;   /* No hardware cursor in framebuffer mode */
    uint16_t position = (uint16_t)(terminal_row * VGA_WIDTH + terminal_column);
    outb(VGA_CTRL_REGISTER, 0x0F);
    outb(VGA_DATA_REGISTER, (uint8_t)(position & 0xFF));
    outb(VGA_CTRL_REGISTER, 0x0E);
    outb(VGA_DATA_REGISTER, (uint8_t)((position >> 8) & 0xFF));
}

void terminal_enable_cursor(uint8_t cursor_start, uint8_t cursor_end) {
    if (fb_available()) return;
    outb(VGA_CTRL_REGISTER, 0x0A);
    outb(VGA_DATA_REGISTER, (inb(VGA_DATA_REGISTER) & 0xC0) | cursor_start);
    outb(VGA_CTRL_REGISTER, 0x0B);
    outb(VGA_DATA_REGISTER, (inb(VGA_DATA_REGISTER) & 0xE0) | cursor_end);
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    terminal_buffer[y * VGA_WIDTH + x] = vga_entry((unsigned char)c, color);
    if (fb_available())
        fbcon_render_cell(x, y);
}

void terminal_scroll(void) {
    virtual_terminal_t* term = &terminals[current_terminal];

    if (term->scroll_offset == 0) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            term->scrollback_buffer[term->scrollback_pos * VGA_WIDTH + x] = terminal_buffer[x];
        }
        term->scrollback_pos = (term->scrollback_pos + 1) % SCROLLBACK_LINES;
    }

    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[y * VGA_WIDTH + x] = terminal_buffer[(y + 1) * VGA_WIDTH + x];
        }
    }
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
    }
    terminal_row = VGA_HEIGHT - 1;

    if (fb_available()) {
        /* Scroll the pixel rows to match the shadow buffer (8 px per line) */
        gfx_front_scroll(8, vga_palette[(terminal_color >> 4) & 0x0F]);
    }
    terminal_update_cursor();
}

void terminal_putchar(char c) {
    serial_putc(c);   /* Mirror everything to COM1 for headless debugging */

    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_scroll();
        terminal_update_cursor();
        return;
    }
    if (c == '\b') {
        if (terminal_column > 0) {
            terminal_column--;
            terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
            terminal_update_cursor();
        }
        return;
    }
    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_scroll();
    }
    terminal_update_cursor();
}

void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
    terminal_write(data, strlen(data));
}

void terminal_writedec(uint32_t n) {
    if (n == 0) { terminal_putchar('0'); return; }
    char buf[12];
    int i = 0;
    while (n > 0) { buf[i++] = '0' + (char)(n % 10); n /= 10; }
    while (i > 0) terminal_putchar(buf[--i]);
}

void terminal_writehex(uint32_t n) {
    const char* hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--)
        terminal_putchar(hex[(n >> (i * 4)) & 0xF]);
}

/* ──────── Virtual terminal functions ──────── */
void vterm_init_all(void) {
    for (int i = 0; i < MAX_TERMINALS; i++) {
        terminals[i].row = 0;
        terminals[i].column = 0;
        terminals[i].color = vga_entry_color(themes[current_theme].text_fg, themes[current_theme].text_bg);
        terminals[i].command_length = 0;
        terminals[i].active = (i == 0) ? 1 : 0;
        terminals[i].scrollback_pos = 0;
        terminals[i].scroll_offset = 0;
        strcpy(terminals[i].working_dir, "/");
        memset(terminals[i].command_buffer, 0, CMD_BUFFER_SIZE);

        for (size_t j = 0; j < TERMINAL_BUFFER_SIZE; j++)
            terminals[i].screen_buffer[j] = vga_entry(' ', terminals[i].color);

        for (size_t j = 0; j < SCROLLBACK_BUFFER_SIZE; j++)
            terminals[i].scrollback_buffer[j] = vga_entry(' ', terminals[i].color);
    }
}

void vterm_save_current(void) {
    for (size_t i = 0; i < TERMINAL_BUFFER_SIZE; i++)
        terminals[current_terminal].screen_buffer[i] = terminal_buffer[i];
    terminals[current_terminal].row = terminal_row;
    terminals[current_terminal].column = terminal_column;
    terminals[current_terminal].color = terminal_color;
    strcpy(terminals[current_terminal].working_dir, current_dir);
    terminals[current_terminal].command_length = command_length;
    for (size_t i = 0; i < command_length; i++)
        terminals[current_terminal].command_buffer[i] = command_buffer[i];
}

void vterm_restore(int term_num) {
    for (size_t i = 0; i < TERMINAL_BUFFER_SIZE; i++)
        terminal_buffer[i] = terminals[term_num].screen_buffer[i];
    terminal_row = terminals[term_num].row;
    terminal_column = terminals[term_num].column;
    terminal_color = terminals[term_num].color;
    strcpy(current_dir, terminals[term_num].working_dir);
    command_length = terminals[term_num].command_length;
    for (size_t i = 0; i < terminals[term_num].command_length; i++)
        command_buffer[i] = terminals[term_num].command_buffer[i];
    if (fb_available())
        fbcon_render_all();
    terminal_update_cursor();
}

void vterm_switch(int term_num) {
    if (term_num < 0 || term_num >= MAX_TERMINALS) return;
    if (term_num == current_terminal) return;

    vterm_save_current();
    current_terminal = term_num;

    if (!terminals[term_num].active) {
        terminals[term_num].active = 1;
        terminal_initialize();
        terminal_setcolor(vga_entry_color(themes[current_theme].header_fg, themes[current_theme].text_bg));
        terminal_writestring("=== Virtual Terminal ");
        char term_char[2] = { (char)('0' + term_num + 1), '\0' };
        terminal_writestring(term_char);
        terminal_writestring(" ===\n");
        terminal_setcolor(vga_entry_color(themes[current_theme].text_fg, themes[current_theme].text_bg));
        terminal_writestring("Type 'help' for available commands\n");
        terminal_writestring("Use Ctrl+1-4 to switch terminals\n\n");
        vterm_save_current();
    } else {
        vterm_restore(term_num);
    }
}

void vterm_show_indicator(void) {
    virtual_terminal_t* term = &terminals[current_terminal];
    size_t indicator_pos = VGA_WIDTH - 5;

    terminal_buffer[indicator_pos]     = vga_entry('[', vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_buffer[indicator_pos + 1] = vga_entry('T', vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_buffer[indicator_pos + 2] = vga_entry((unsigned char)('0' + current_terminal + 1), vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    terminal_buffer[indicator_pos + 3] = vga_entry(']', vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));

    if (term->scroll_offset > 0) {
        size_t scroll_pos = VGA_WIDTH - 14;
        static const char scroll_txt[] = "[SCROLL]";
        for (size_t i = 0; i < sizeof(scroll_txt) - 1; i++)
            terminal_buffer[scroll_pos + i] = vga_entry((unsigned char)scroll_txt[i], vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    }

    if (fb_available()) {
        for (size_t x = VGA_WIDTH - 14; x < VGA_WIDTH; x++)
            fbcon_render_cell(x, 0);
    }
}

void vterm_scroll_up(void) {
    virtual_terminal_t* term = &terminals[current_terminal];

    if (term->scroll_offset < SCROLLBACK_LINES - VGA_HEIGHT) {
        if (term->scroll_offset == 0)
            vterm_save_current();

        term->scroll_offset += 5;
        if (term->scroll_offset > SCROLLBACK_LINES - VGA_HEIGHT)
            term->scroll_offset = SCROLLBACK_LINES - VGA_HEIGHT;

        size_t start_line = (term->scrollback_pos + SCROLLBACK_LINES - term->scroll_offset - VGA_HEIGHT) % SCROLLBACK_LINES;
        for (size_t y = 0; y < VGA_HEIGHT; y++) {
            size_t src_line = (start_line + y) % SCROLLBACK_LINES;
            for (size_t x = 0; x < VGA_WIDTH; x++)
                terminal_buffer[y * VGA_WIDTH + x] = term->scrollback_buffer[src_line * VGA_WIDTH + x];
        }
        if (fb_available())
            fbcon_render_all();
        vterm_show_indicator();
    }
}

void vterm_scroll_down(void) {
    virtual_terminal_t* term = &terminals[current_terminal];

    if (term->scroll_offset > 0) {
        if (term->scroll_offset <= 5) {
            term->scroll_offset = 0;
            vterm_restore(current_terminal);
            vterm_show_indicator();
        } else {
            term->scroll_offset -= 5;
            size_t start_line = (term->scrollback_pos + SCROLLBACK_LINES - term->scroll_offset - VGA_HEIGHT) % SCROLLBACK_LINES;
            for (size_t y = 0; y < VGA_HEIGHT; y++) {
                size_t src_line = (start_line + y) % SCROLLBACK_LINES;
                for (size_t x = 0; x < VGA_WIDTH; x++)
                    terminal_buffer[y * VGA_WIDTH + x] = term->scrollback_buffer[src_line * VGA_WIDTH + x];
            }
            if (fb_available())
                fbcon_render_all();
            vterm_show_indicator();
        }
    }
}
