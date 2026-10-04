#ifndef GD_RENDER_H
#define GD_RENDER_H

#include "ui_types.h"

void draw_main(Gdb *g, Source *s, int cursor, int column, int top, int hscroll, InputMode mode,
               CodeView code_view, PaneFocus focus, VarRow *var_rows, int var_count, int var_top,
               int var_selected, int reg_selected, int reg_top, int asm_selected, int asm_top,
               int stack_selected, const char *search_text, bool search_whole_word,
               const char *project_root, int history_index, int history_count);
void draw_help_popup(void);
void draw_output(Gdb *g);
void draw_address(const AddressPanel *p);
void build_register_panel(const GdbRegister *r, RegisterPanel *p);
void draw_register_panel(const RegisterPanel *p);
void draw_function_dialog(Gdb *g, FunctionDialog *d);
void draw_open_menu(int selected);
void draw_file_dialog(Gdb *g, FileDialog *d);
void draw_edit_dialog(EditDialog *d);
void draw_force_return_dialog(ForceReturnDialog *d);
void draw_catch_menu(int selected);
void draw_catch_search(Gdb *g, CatchDialog *d);
void draw_breaks(Gdb *g, int selected, const char *project_root);

#endif
