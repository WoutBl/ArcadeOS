#ifndef VGA_H
#define VGA_H

#include "types.h"

/* VGA Text Mode Configuration */
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000
#define VGA_CTRL_REGISTER 0x3D4
#define VGA_DATA_REGISTER 0x3D5

/* VGA Colors */
enum vga_color {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_BLUE = 1,
    VGA_COLOR_GREEN = 2,
    VGA_COLOR_CYAN = 3,
    VGA_COLOR_RED = 4,
    VGA_COLOR_MAGENTA = 5,
    VGA_COLOR_BROWN = 6,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_DARK_GREY = 8,
    VGA_COLOR_LIGHT_BLUE = 9,
    VGA_COLOR_LIGHT_GREEN = 10,
    VGA_COLOR_LIGHT_CYAN = 11,
    VGA_COLOR_LIGHT_RED = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN = 14,
    VGA_COLOR_WHITE = 15,
};

/* Color Themes */
typedef struct {
    const char* name;
    uint8_t text_fg;
    uint8_t text_bg;
    uint8_t prompt_fg;
    uint8_t prompt_bg;
    uint8_t header_fg;
    uint8_t header_bg;
    uint8_t error_fg;
    uint8_t error_bg;
    uint8_t success_fg;
    uint8_t success_bg;
    uint8_t dir_fg;
    uint8_t file_fg;
} color_theme_t;

/* Virtual Terminal System */
#define MAX_TERMINALS 4
#define TERMINAL_BUFFER_SIZE (VGA_WIDTH * VGA_HEIGHT)
#define SCROLLBACK_LINES 200
#define SCROLLBACK_BUFFER_SIZE (VGA_WIDTH * SCROLLBACK_LINES)

typedef struct {
    uint16_t screen_buffer[TERMINAL_BUFFER_SIZE];
    uint16_t scrollback_buffer[SCROLLBACK_BUFFER_SIZE];
    size_t scrollback_pos;
    size_t scroll_offset;
    size_t row;
    size_t column;
    uint8_t color;
    char command_buffer[CMD_BUFFER_SIZE];
    size_t command_length;
    char working_dir[MAX_PATH];
    int active;
} virtual_terminal_t;

/* Inline helpers */
static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return (uint8_t)fg | (uint8_t)bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t)uc | (uint16_t)color << 8;
}

/* Theme access */
extern color_theme_t themes[];
extern int current_theme;
#define NUM_THEMES 6

/* Terminal state (shared globals) */
extern uint16_t* terminal_buffer;
extern size_t terminal_row;
extern size_t terminal_column;
extern uint8_t terminal_color;
extern virtual_terminal_t terminals[];
extern int current_terminal;

/* Current working directory (shared with fs/shell) */
extern char current_dir[];

/* Command buffer (shared with shell) */
extern char command_buffer[];
extern size_t command_length;

/* Terminal API */
void terminal_initialize(void);
void terminal_setcolor(uint8_t color);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_writedec(uint32_t n);
void terminal_writehex(uint32_t n);
void terminal_update_cursor(void);
void terminal_enable_cursor(uint8_t cursor_start, uint8_t cursor_end);
void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);
void terminal_scroll(void);

/* Virtual terminal API */
void vterm_init_all(void);
void vterm_save_current(void);
void vterm_restore(int term_num);
void vterm_switch(int term_num);
void vterm_show_indicator(void);
void vterm_scroll_up(void);
void vterm_scroll_down(void);

#endif /* VGA_H */
