#ifndef GD_UI_TYPES_H
#define GD_UI_TYPES_H

#include "gdb.h"
#include <ncurses.h>
#include <sys/stat.h>

typedef struct {
    char **lines;
    int count;
    char path[GD_PATH_MAX];
    time_t mtime;
    struct stat metadata;
    bool debug_lines;
} Source;
typedef enum {
    VIEW_MAIN,
    VIEW_HELP,
    VIEW_BREAKS,
    VIEW_OUTPUT,
    VIEW_ADDRESS,
    VIEW_REGISTER,
    VIEW_FUNCTIONS,
    VIEW_OPEN_MENU,
    VIEW_OPEN_FILES,
    VIEW_EDIT_VALUE,
    VIEW_FORCE_RETURN,
    VIEW_CATCH_MENU,
    VIEW_CATCH_SEARCH
} View;
typedef enum { MODE_VIM, MODE_GDB } InputMode;
typedef enum { PANE_SOURCE, PANE_VARIABLES, PANE_REGISTERS, PANE_STACK } PaneFocus;
typedef enum { CODE_SOURCE, CODE_ASSEMBLY } CodeView;
typedef enum { FUNCTION_BREAKPOINT, FUNCTION_OPEN } FunctionAction;
typedef enum { EDIT_VARIABLE, EDIT_REGISTER } EditKind;
typedef enum { LANGUAGE_AUTO, LANGUAGE_JA, LANGUAGE_EN } Language;
enum { COLOR_BREAK = 1, COLOR_COND, COLOR_WATCH, COLOR_ERROR, COLOR_INFO, COLOR_MUTED };

#define UI_MAX_EXPANSIONS 16
#define UI_MAX_CHILDREN 32
#define UI_MAX_VAR_ROWS 256
#define UI_HISTORY_MAX 64
#define UI_MAX_ADDRESS_MATCHES 32
typedef struct {
    char path[GD_PATH_MAX];
    int cursor;
    int column;
    int top;
    int hscroll;
    CodeView code_view;
    bool debug_lines;
    char address[128];
    char function[256];
    int asm_selected;
    int asm_top;
} SourceLocation;

typedef struct {
    SourceLocation items[UI_HISTORY_MAX];
    int count;
    int index;
} SourceHistory;

typedef struct {
    char expression[512];
    GdbChild children[UI_MAX_CHILDREN];
    int child_count;
    unsigned long revision;
} VarExpansion;

typedef struct {
    char expression[512];
    char label[128];
    char value[GD_TEXT_MAX];
    char type[256];
    int depth;
    bool header;
    bool expandable;
    bool expanded;
    unsigned long revision;
} VarRow;

typedef struct {
    GdbAddressInfo info;
    char matches[UI_MAX_ADDRESS_MATCHES][512];
    int match_count;
} AddressPanel;

typedef struct {
    char name[32];
    char value[256];
    unsigned long long numeric;
    bool numeric_available;
} RegisterPanel;

typedef struct {
    char input[512];
    GdbFunction candidates[GD_MAX_FUNCTIONS];
    int count;
    int selected;
    int top;
    FunctionAction action;
    GdbFunction base[GD_MAX_FUNCTIONS];
    char base_filter[512];
    int base_count;
    unsigned long symbol_revision;
    bool complete;
} FunctionDialog;

#define UI_MAX_SOURCE_FILES 512
typedef struct {
    char path[GD_PATH_MAX];
    char canonical[GD_PATH_MAX];
    char display[GD_PATH_MAX];
    bool debug_lines;
} SourceFileCandidate;

#define UI_MAX_SOURCE_DIRS 1024
typedef struct {
    char path[GD_PATH_MAX];
    struct stat metadata;
    unsigned long long signature;
    size_t entries;
} SourceDirectory;

typedef struct {
    char input[512];
    SourceFileCandidate all[UI_MAX_SOURCE_FILES];
    int all_count;
    int matches[UI_MAX_SOURCE_FILES];
    int count;
    int selected;
    int top;
    SourceDirectory directories[UI_MAX_SOURCE_DIRS];
    int directory_count;
    char root[GD_PATH_MAX];
    pid_t owner;
    unsigned long symbol_revision;
    bool catalogue_valid;
} FileDialog;

typedef struct {
    EditKind kind;
    char expression[512];
    char label[128];
    char type[256];
    char current[GD_TEXT_MAX];
    char input[GD_TEXT_MAX];
    bool confirm;
} EditDialog;

typedef struct {
    char function[256];
    char return_type[256];
    char input[GD_TEXT_MAX];
    bool is_void;
    bool type_known;
    bool confirm;
} ForceReturnDialog;

typedef struct {
    char event[32];
    char input[128];
    char candidates[GD_MAX_CATCH_CANDIDATES][128];
    int count;
    int selected;
    int top;
} CatchDialog;

#endif
