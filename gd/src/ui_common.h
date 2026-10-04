#ifndef GD_UI_COMMON_H
#define GD_UI_COMMON_H

#include "ui_types.h"

const char *tui_text(const char *english, const char *japanese);
bool parse_language(const char *value, Language *result);
bool locale_is_utf8(void);
bool language_is_japanese(Language language);
int display_width(const char *text);
const char *state_name(GdbState s);
void add_clipped(WINDOW *window, int y, int x, const char *text, int width);
void clipped(int y, int x, int width, const char *fmt, ...);
const char *focus_name(PaneFocus focus);
PaneFocus cycle_focus(PaneFocus focus, CodeView code_view, int direction);
void layout_heights(int rows, int *source_h, int *variables_h, int *stack_h);
int execution_context_top(int cursor, int source_h);
void line_rule(int y, const char *title);
void centered_title(int y, int x, int width, const char *title);
WINDOW *create_popup(const char *title, int wanted_height, int wanted_width);
bool prompt_text(const char *label, char *out, size_t size);
bool confirm_quit(const Gdb *g);
bool ui_is_japanese(void);
void ui_set_japanese(bool japanese);

#endif
