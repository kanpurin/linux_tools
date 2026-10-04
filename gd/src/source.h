#ifndef GD_SOURCE_H
#define GD_SOURCE_H

#include "ui_types.h"

void source_free(Source *s);
int source_load(Source *s, const char *path);
const char *path_name(const char *path);
void project_root_init(Gdb *g, char *out, size_t size);
bool project_path(const char *path, const char *root);
bool same_source_path(const char *a, const char *b);
void filter_file_dialog(FileDialog *d);
void load_file_dialog(Gdb *g, const char *project_root, FileDialog *d);
void history_update(SourceHistory *h, Source *s, int cursor, int column, int top, int hscroll);
void history_update_assembly(SourceHistory *h, int selected, int top);
void history_push(SourceHistory *h, const char *path, int cursor, int column, int top, int hscroll,
                  bool debug_lines);
void history_push_assembly(SourceHistory *h, const char *address, const char *function,
                           int selected, int top);
bool source_navigate(Source *s, SourceHistory *h, const char *path, int line, int *cursor,
                     int *column, int *top, int *hscroll, bool debug_lines, bool record);
bool history_move(SourceHistory *h, Source *s, Gdb *g, int direction, CodeView *code_view,
                  int *cursor, int *column, int *top, int *hscroll, int *asm_selected,
                  int *asm_top);
int source_search(Source *s, int cursor, const char *needle, int direction);
int text_column(const char *line, const char *needle, int direction);
bool text_search_position(Source *s, int *line, int *column, const char *needle, int direction,
                          bool whole_word);
bool word_char(int c);
void clamp_column(Source *s, int line, int *column);
bool word_under_cursor(Source *s, int line, int column, char *out, size_t size, int *start_out);
bool search_word(Source *s, int *line, int *column, const char *word, int direction);
void word_motion(Source *s, int *line, int *column, int direction, bool to_end);
bool goto_declaration(Source *s, int *line, int *column, bool global);
bool match_bracket(Source *s, int *line, int *column);
void paragraph_motion(Source *s, int *line, int direction);

#endif
