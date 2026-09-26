#ifndef GD_GDB_H
#define GD_GDB_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define GD_PATH_MAX 4096
#define GD_TEXT_MAX 1024
#define GD_MAX_VARS 64
#define GD_MAX_FRAMES 64
#define GD_MAX_BREAKS 128
#define GD_MAX_CHILDREN 64
#define GD_MAX_INSTRUCTIONS 256
#define GD_MAX_REGISTERS 32
#define GD_MAX_FUNCTIONS 256
#define GD_MAX_CATCH_CANDIDATES 512

typedef enum {
    GDB_NOT_STARTED,
    GDB_RUNNING,
    GDB_STOPPED,
    GDB_EXITED,
    GDB_FAILED
} GdbState;

typedef struct { char name[128]; char value[GD_TEXT_MAX]; char type[256]; } GdbVar;
typedef struct {
    char expression[512];
    char name[128];
    char value[GD_TEXT_MAX];
    char type[256];
    int numchild;
} GdbChild;
typedef struct {
    int level;
    char func[256];
    char file[GD_PATH_MAX];
    char address[128];
    int line;
} GdbFrame;
typedef struct {
    char address[128];
    char function[256];
    char instruction[512];
    char call_target[256];
    bool current;
    bool call;
} GdbInstruction;
typedef struct {
    int number;
    int argument;
    char name[32];
    char value[256];
    char previous[256];
    bool changed;
} GdbRegister;
typedef struct {
    char name[512];
    char module[GD_PATH_MAX];
    char address[128];
    char source[GD_PATH_MAX];
    int line;
    bool debug_symbol;
} GdbFunction;
typedef struct {
    char file[GD_PATH_MAX];
    char fullname[GD_PATH_MAX];
} GdbSourceFile;
typedef struct {
    int number;
    bool enabled;
    bool watchpoint;
    bool catchpoint;
    char file[GD_PATH_MAX];
    char address[128];
    char function[512];
    char original_location[512];
    int line;
    char condition[512];
    bool function_breakpoint;
    bool pending;
    char catch_type[64];
    char catch_target[256];
} GdbBreakpoint;

typedef struct {
    char expression[512];
    char type[256];
    char value[GD_TEXT_MAX];
    char address[128];
    char points_to[GD_TEXT_MAX];
    bool pointer;
    bool is_null;
    bool optimized_out;
    bool address_available;
    bool points_to_available;
} GdbAddressInfo;

typedef struct {
    pid_t pid;
    int to_gdb;
    int from_gdb;
    int token;
    char readbuf[65536];
    size_t readlen;
    GdbState state;
    char executable[GD_PATH_MAX];
    char fullname[GD_PATH_MAX];
    char file[GD_PATH_MAX];
    char function[256];
    int line;
    char pc[128];
    char disassembly_function[256];
    bool source_available;
    bool amd64_sysv;
    int exit_code;
    GdbVar args[GD_MAX_VARS];
    int arg_count;
    GdbVar locals[GD_MAX_VARS];
    int local_count;
    GdbFrame frames[GD_MAX_FRAMES];
    int frame_count;
    int selected_frame;
    GdbInstruction instructions[GD_MAX_INSTRUCTIONS];
    int instruction_count;
    int current_instruction;
    GdbRegister registers[GD_MAX_REGISTERS];
    int register_count;
    GdbBreakpoint breaks[GD_MAX_BREAKS];
    int break_count;
    char output[32768];
    size_t output_len;
    char message[GD_TEXT_MAX];
    char stop_reason[128];
    char watch_old[GD_TEXT_MAX];
    char watch_new[GD_TEXT_MAX];
    int stop_breakpoint;
    char catch_event[64];
    char catch_target[256];
    char catch_phase[32];
    char catch_detail[512];
    char stop_thread[64];
    bool japanese;
    bool changed;
} Gdb;

int gdb_start(Gdb *g, const char *program, char *const program_argv[]);
void gdb_shutdown(Gdb *g);
int gdb_poll(Gdb *g, int timeout_ms);
int gdb_run(Gdb *g);
int gdb_continue(Gdb *g);
int gdb_next(Gdb *g);
int gdb_step(Gdb *g);
int gdb_next_instruction(Gdb *g);
int gdb_step_instruction(Gdb *g);
int gdb_finish(Gdb *g);
int gdb_select_frame(Gdb *g, int level);
int gdb_toggle_breakpoint(Gdb *g, const char *file, int line);
int gdb_set_cond_breakpoint(Gdb *g, const char *file, int line, const char *condition);
int gdb_set_function_breakpoint(Gdb *g, const char *function);
int gdb_set_catchpoint(Gdb *g, const char *event, const char *target);
int gdb_list_catch_candidates(Gdb *g, const char *event, const char *filter,
                              char candidates[][128], int max_candidates,
                              int *candidate_count);
int gdb_toggle_address_breakpoint(Gdb *g, const char *address);
int gdb_list_functions(Gdb *g, const char *filter, GdbFunction *functions,
                       int max_functions, int *function_count);
int gdb_list_source_files(Gdb *g, GdbSourceFile *files, int max_files,
                          int *file_count);
int gdb_disassemble_at(Gdb *g, const char *address);
int gdb_watch(Gdb *g, const char *expression);
int gdb_print(Gdb *g, const char *expression, char *out, size_t out_size);
int gdb_assign_expression(Gdb *g, const char *expression, const char *new_value,
                          char *actual, size_t actual_size);
int gdb_assign_register(Gdb *g, const char *name, const char *new_value,
                        char *actual, size_t actual_size);
int gdb_current_return_type(Gdb *g, char *type, size_t type_size,
                            bool *is_void);
int gdb_force_return(Gdb *g, const char *value);
int gdb_inspect_address(Gdb *g, const char *expression, GdbAddressInfo *info);
int gdb_expression_address(Gdb *g, const char *expression, char *address,
                           size_t address_size);
int gdb_delete_breakpoint(Gdb *g, int number);
int gdb_enable_breakpoint(Gdb *g, int number, bool enable);
int gdb_refresh(Gdb *g);
int gdb_refresh_breakpoints(Gdb *g);
int gdb_list_children(Gdb *g, const char *expression, GdbChild *children,
                      int max_children, int *child_count);

#endif
