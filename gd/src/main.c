#include "gdb.h"

#include <ctype.h>
#include <dirent.h>
#include <locale.h>
#include <ncurses.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char **lines; int count; char path[GD_PATH_MAX]; time_t mtime; bool debug_lines; } Source;
typedef enum { VIEW_MAIN, VIEW_HELP, VIEW_BREAKS, VIEW_OUTPUT, VIEW_ADDRESS, VIEW_REGISTER, VIEW_FUNCTIONS, VIEW_OPEN_MENU, VIEW_OPEN_FILES, VIEW_EDIT_VALUE, VIEW_FORCE_RETURN, VIEW_CATCH_MENU, VIEW_CATCH_SEARCH } View;
typedef enum { MODE_VIM, MODE_GDB } InputMode;
typedef enum { PANE_SOURCE, PANE_VARIABLES, PANE_REGISTERS, PANE_STACK } PaneFocus;
typedef enum { CODE_SOURCE, CODE_ASSEMBLY } CodeView;
typedef enum { FUNCTION_BREAKPOINT, FUNCTION_OPEN } FunctionAction;
typedef enum { EDIT_VARIABLE, EDIT_REGISTER } EditKind;
enum { COLOR_BREAK=1, COLOR_COND, COLOR_WATCH, COLOR_ERROR, COLOR_INFO, COLOR_MUTED };

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
} FunctionDialog;

#define UI_MAX_SOURCE_FILES 512
typedef struct {
    char path[GD_PATH_MAX];
    char display[GD_PATH_MAX];
    bool debug_lines;
} SourceFileCandidate;

typedef struct {
    char input[512];
    SourceFileCandidate all[UI_MAX_SOURCE_FILES];
    int all_count;
    int matches[UI_MAX_SOURCE_FILES];
    int count;
    int selected;
    int top;
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

static void source_free(Source *s) { for (int i=0;i<s->count;i++) free(s->lines[i]); free(s->lines); memset(s,0,sizeof(*s)); }
static int source_load(Source *s, const char *path) {
    struct stat st;
    if (!path || !*path) return -1;
    if (stat(path,&st)) return -1;
    if (!strcmp(s->path,path) && s->mtime==st.st_mtime) return 0;
    FILE *fp=fopen(path,"r"); if(!fp)return -1; source_free(s); snprintf(s->path,sizeof(s->path),"%s",path); s->mtime=st.st_mtime;
    char *line=NULL; size_t cap=0; ssize_t n;
    while((n=getline(&line,&cap,fp))>=0){ if(n&&line[n-1]=='\n')line[--n]='\0'; char **v=realloc(s->lines,(size_t)(s->count+1)*sizeof(*v)); if(!v)break; s->lines=v;s->lines[s->count++]=strdup(line); }
    free(line);fclose(fp);return 0;
}
static const char *path_name(const char *path){const char*p=strrchr(path,'/');return p?p+1:path;}
static void path_directory(const char*path,char*out,size_t size){snprintf(out,size,"%s",path?path:"");char*p=strrchr(out,'/');if(p){if(p==out)p[1]='\0';else *p='\0';}else snprintf(out,size,".");}
static void project_root_init(Gdb*g,char*out,size_t size){char exe[GD_PATH_MAX]="",src[GD_PATH_MAX]="",exe_dir[GD_PATH_MAX],src_dir[GD_PATH_MAX];if(!realpath(g->executable,exe))snprintf(exe,sizeof(exe),"%s",g->executable);path_directory(exe,exe_dir,sizeof(exe_dir));if(!g->fullname[0]||!realpath(g->fullname,src)){snprintf(out,size,"%s",exe_dir);return;}path_directory(src,src_dir,sizeof(src_dir));size_t last=0,i=0;while(exe_dir[i]&&src_dir[i]&&exe_dir[i]==src_dir[i]){if(exe_dir[i]=='/')last=i;i++;}if(!exe_dir[i]&&!src_dir[i])snprintf(out,size,"%s",src_dir);else if(last>0){size_t n=last;if(n>=size)n=size-1;memcpy(out,src_dir,n);out[n]='\0';}else snprintf(out,size,"%s",exe_dir);}
static bool project_path(const char*path,const char*root){if(!path||!*path)return false;if(path[0]!='/')return true;size_t n=strlen(root);return n>0&&!strncmp(path,root,n)&&(path[n]=='/'||path[n]=='\0');}
static bool source_extension(const char*path){const char*dot=strrchr(path,'.');if(!dot)return false;const char*extensions[]={".c",".h",".cc",".hh",".cpp",".hpp",".cxx",".hxx",NULL};for(int i=0;extensions[i];i++)if(!strcasecmp(dot,extensions[i]))return true;return false;}
static bool same_source_path(const char*a,const char*b){if(!a||!b||!*a||!*b)return false;if(!strcmp(a,b))return true;char ra[GD_PATH_MAX],rb[GD_PATH_MAX];return realpath(a,ra)&&realpath(b,rb)&&!strcmp(ra,rb);}
static bool debug_source_path(GdbSourceFile*files,int count,const char*path){for(int i=0;i<count;i++)if(same_source_path(path,files[i].fullname[0]?files[i].fullname:files[i].file))return true;return false;}
static int source_candidate_compare(const void*left,const void*right){const SourceFileCandidate*a=left,*b=right;return strcmp(a->display,b->display);}
static void scan_source_tree(const char*root,const char*dir,GdbSourceFile*debug_files,int debug_count,FileDialog*d,int depth){if(depth>16||d->all_count>=UI_MAX_SOURCE_FILES)return;DIR*dp=opendir(dir);if(!dp)return;struct dirent*entry;while((entry=readdir(dp))&&d->all_count<UI_MAX_SOURCE_FILES){if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")||!strcmp(entry->d_name,".git"))continue;char path[GD_PATH_MAX];if(snprintf(path,sizeof(path),"%s/%s",dir,entry->d_name)>=(int)sizeof(path))continue;struct stat st;if(lstat(path,&st)||S_ISLNK(st.st_mode))continue;if(S_ISDIR(st.st_mode)){scan_source_tree(root,path,debug_files,debug_count,d,depth+1);continue;}if(!S_ISREG(st.st_mode)||!source_extension(path))continue;bool duplicate=false;for(int i=0;i<d->all_count;i++)if(same_source_path(d->all[i].path,path)){duplicate=true;break;}if(duplicate)continue;SourceFileCandidate*c=&d->all[d->all_count++];memset(c,0,sizeof(*c));snprintf(c->path,sizeof(c->path),"%s",path);size_t root_len=strlen(root);snprintf(c->display,sizeof(c->display),"%s",!strncmp(path,root,root_len)&&path[root_len]=='/'?path+root_len+1:path);c->debug_lines=debug_source_path(debug_files,debug_count,path);}closedir(dp);}
static void filter_file_dialog(FileDialog*d){d->count=0;d->selected=0;d->top=0;for(int i=0;i<d->all_count;i++)if(!d->input[0]||strstr(d->all[i].display,d->input)||strstr(path_name(d->all[i].path),d->input))d->matches[d->count++]=i;}
static void load_file_dialog(Gdb*g,const char*project_root,FileDialog*d){memset(d,0,sizeof(*d));GdbSourceFile debug_files[UI_MAX_SOURCE_FILES];int debug_count=0;if(gdb_list_source_files(g,debug_files,UI_MAX_SOURCE_FILES,&debug_count))debug_count=0;scan_source_tree(project_root,project_root,debug_files,debug_count,d,0);qsort(d->all,(size_t)d->all_count,sizeof(d->all[0]),source_candidate_compare);filter_file_dialog(d);}
static void history_update(SourceHistory*h,Source*s,int cursor,int column,int top,int hscroll){if(h->index<0||h->index>=h->count||!s->path[0])return;SourceLocation*loc=&h->items[h->index];if(loc->code_view!=CODE_SOURCE||strcmp(loc->path,s->path))return;loc->cursor=cursor;loc->column=column;loc->top=top;loc->hscroll=hscroll;loc->debug_lines=s->debug_lines;}
static void history_update_assembly(SourceHistory*h,int selected,int top){if(h->index<0||h->index>=h->count)return;SourceLocation*loc=&h->items[h->index];if(loc->code_view!=CODE_ASSEMBLY)return;loc->asm_selected=selected;loc->asm_top=top;}
static void history_push(SourceHistory*h,const char*path,int cursor,int column,int top,int hscroll,bool debug_lines){if(!path||!*path)return;if(h->index>=0&&h->index<h->count){SourceLocation*current=&h->items[h->index];if(current->code_view==CODE_SOURCE&&!strcmp(current->path,path)&&current->cursor==cursor){current->column=column;current->top=top;current->hscroll=hscroll;current->debug_lines=debug_lines;return;}}h->count=h->index+1;if(h->count>=UI_HISTORY_MAX){memmove(&h->items[0],&h->items[1],(UI_HISTORY_MAX-1)*sizeof(h->items[0]));h->count=UI_HISTORY_MAX-1;h->index--;}SourceLocation*loc=&h->items[h->count++];memset(loc,0,sizeof(*loc));loc->code_view=CODE_SOURCE;snprintf(loc->path,sizeof(loc->path),"%s",path);loc->cursor=cursor;loc->column=column;loc->top=top;loc->hscroll=hscroll;loc->debug_lines=debug_lines;h->index=h->count-1;}
static void history_push_assembly(SourceHistory*h,const char*address,const char*function,int selected,int top){if(!address||!*address)return;if(h->index>=0&&h->index<h->count){SourceLocation*current=&h->items[h->index];if(current->code_view==CODE_ASSEMBLY&&!strcmp(current->address,address)){current->asm_selected=selected;current->asm_top=top;return;}}h->count=h->index+1;if(h->count>=UI_HISTORY_MAX){memmove(&h->items[0],&h->items[1],(UI_HISTORY_MAX-1)*sizeof(h->items[0]));h->count=UI_HISTORY_MAX-1;h->index--;}SourceLocation*loc=&h->items[h->count++];memset(loc,0,sizeof(*loc));loc->code_view=CODE_ASSEMBLY;snprintf(loc->address,sizeof(loc->address),"%s",address);snprintf(loc->function,sizeof(loc->function),"%s",function?function:"");loc->asm_selected=selected;loc->asm_top=top;h->index=h->count-1;}
static bool source_navigate(Source*s,SourceHistory*h,const char*path,int line,int*cursor,int*column,int*top,int*hscroll,bool debug_lines,bool record){if(!path||!*path||line<1)return false;history_update(h,s,*cursor,*column,*top,*hscroll);if(source_load(s,path))return false;s->debug_lines=debug_lines;*cursor=line-1;if(*cursor<0)*cursor=0;if(*cursor>=s->count)*cursor=s->count?s->count-1:0;*column=0;*hscroll=0;*top=*cursor;if(record)history_push(h,s->path,*cursor,*column,*top,*hscroll,debug_lines);return true;}
static bool history_move(SourceHistory*h,Source*s,Gdb*g,int direction,CodeView*code_view,int*cursor,int*column,int*top,int*hscroll,int*asm_selected,int*asm_top){int target=h->index+direction;if(target<0||target>=h->count)return false;SourceLocation*loc=&h->items[target];if(loc->code_view==CODE_SOURCE){if(source_load(s,loc->path))return false;s->debug_lines=loc->debug_lines;*cursor=loc->cursor;*column=loc->column;*top=loc->top;*hscroll=loc->hscroll;*code_view=CODE_SOURCE;}else{if(gdb_disassemble_at(g,loc->address))return false;*asm_selected=loc->asm_selected;*asm_top=loc->asm_top;*code_view=CODE_ASSEMBLY;}h->index=target;return true;}
static const char *state_name(GdbState s){switch(s){case GDB_NOT_STARTED:return"NOT STARTED";case GDB_RUNNING:return"RUNNING";case GDB_STOPPED:return"STOPPED";case GDB_EXITED:return"EXITED";default:return"ERROR";}}
static void clipped(int y,int x,int width,const char *fmt,...){char b[8192];va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof(b),fmt,ap);va_end(ap);if(width>0)mvaddnstr(y,x,b,width);}
static int breakpoint_mark(Gdb *g,const char *file,int line){for(int i=0;i<g->break_count;i++){GdbBreakpoint*b=&g->breaks[i];if(!b->watchpoint&&!b->catchpoint&&b->line==line&&(!strcmp(b->file,file)||strstr(file,b->file))){if(!b->enabled)return 3;return b->condition[0]?2:1;}}return 0;}
static GdbBreakpoint* breakpoint_at(Gdb*g,const char*file,int line){for(int i=0;i<g->break_count;i++){GdbBreakpoint*b=&g->breaks[i];if(!b->watchpoint&&!b->catchpoint&&b->line==line&&(!strcmp(b->file,file)||strstr(file,b->file)))return b;}return NULL;}
static GdbBreakpoint* breakpoint_at_address(Gdb*g,const char*address){if(!address||!address[0])return NULL;unsigned long long value=strtoull(address,NULL,16);for(int i=0;i<g->break_count;i++){GdbBreakpoint*b=&g->breaks[i];if(!b->watchpoint&&!b->catchpoint&&b->address[0]&&strtoull(b->address,NULL,16)==value)return b;}return NULL;}
static int watchpoint_for_expression(Gdb*g,const char*expression){for(int i=0;i<g->break_count;i++){GdbBreakpoint*b=&g->breaks[i];if(b->watchpoint&&b->condition[0]&&!strcmp(b->condition,expression))return b->number;}return 0;}
static int break_color(const GdbBreakpoint*b){if(!b->enabled)return COLOR_MUTED;if(b->watchpoint)return COLOR_WATCH;if(b->catchpoint)return COLOR_COND;return b->condition[0]?COLOR_COND:COLOR_BREAK;}
static GdbBreakpoint* breakpoint_by_number(Gdb*g,int number){for(int i=0;i<g->break_count;i++)if(g->breaks[i].number==number)return &g->breaks[i];return NULL;}
static int newest_break_number(Gdb *g){int n=0;for(int i=0;i<g->break_count;i++)if(g->breaks[i].number>n)n=g->breaks[i].number;return n;}
static int new_break_line(Gdb *g,const char *file,int previous_max){for(int i=0;i<g->break_count;i++){GdbBreakpoint*b=&g->breaks[i];if(!b->watchpoint&&!b->catchpoint&&b->number>previous_max&&b->line>0&&(!strcmp(b->file,file)||strstr(file,b->file)))return b->line;}return 0;}
static const char*focus_name(PaneFocus focus){return focus==PANE_SOURCE?"Code":focus==PANE_VARIABLES?"Variables":focus==PANE_REGISTERS?"Registers":"Stack";}
static PaneFocus cycle_focus(PaneFocus focus,CodeView code_view,int direction){PaneFocus panes[3]={PANE_SOURCE,code_view==CODE_ASSEMBLY?PANE_REGISTERS:PANE_VARIABLES,PANE_STACK};int current=0;for(int i=0;i<3;i++)if(panes[i]==focus)current=i;current=(current+direction+3)%3;return panes[current];}
static bool word_char(int c);
static int source_search(Source*s,int cursor,const char*needle,int direction){if(!s->count||!needle||!*needle)return-1;for(int offset=1;offset<=s->count;offset++){int line=(cursor+direction*offset)%s->count;if(line<0)line+=s->count;if(strstr(s->lines[line],needle))return line;}return-1;}
static int text_column(const char*line,const char*needle,int direction){if(direction>0){const char*p=strstr(line,needle);return p?(int)(p-line):0;}const char*p=line,*last=NULL;while((p=strstr(p,needle))){last=p;p++;}return last?(int)(last-line):0;}
static bool text_search_position(Source*s,int*line,int*column,const char*needle,int direction,bool whole_word){if(!s->count||!needle||!*needle)return false;int nlen=(int)strlen(needle);for(int off=0;off<=s->count;off++){int ln=(*line+direction*off)%s->count;if(ln<0)ln+=s->count;const char*t=s->lines[ln];int len=(int)strlen(t),found=-1;if(direction>0){int start=off==0?*column+1:0;if(start<0)start=0;for(int i=start;i+nlen<=len;i++)if(!strncmp(t+i,needle,(size_t)nlen)&&(!whole_word||((i==0||!word_char(t[i-1]))&&(i+nlen==len||!word_char(t[i+nlen]))))){found=i;break;}}else{int limit=off==0?*column-1:len-nlen;if(limit>len-nlen)limit=len-nlen;for(int i=limit;i>=0;i--)if(!strncmp(t+i,needle,(size_t)nlen)&&(!whole_word||((i==0||!word_char(t[i-1]))&&(i+nlen==len||!word_char(t[i+nlen]))))){found=i;break;}}if(found>=0){*line=ln;*column=found;return true;}}return false;}
static bool word_char(int c){return isalnum((unsigned char)c)||c=='_';}
static void clamp_column(Source*s,int line,int*column){if(!s->count){*column=0;return;}int len=(int)strlen(s->lines[line]);if(*column<0)*column=0;if(*column>=len)*column=len?len-1:0;}
static bool word_under_cursor(Source*s,int line,int column,char*out,size_t size,int*start_out){if(!s->count||line<0||line>=s->count)return false;const char*t=s->lines[line];int len=(int)strlen(t);if(!len)return false;if(column>=len)column=len-1;if(!word_char(t[column])){while(column<len&&!word_char(t[column]))column++;if(column>=len)return false;}int start=column,end=column+1;while(start>0&&word_char(t[start-1]))start--;while(end<len&&word_char(t[end]))end++;size_t n=(size_t)(end-start);if(n>=size)n=size-1;memcpy(out,t+start,n);out[n]='\0';if(start_out)*start_out=start;return true;}
static int word_occurrence(const char*line,const char*word,int from,int direction){int len=(int)strlen(line),wlen=(int)strlen(word);if(!wlen)return-1;if(direction>0){for(int i=from<0?0:from;i+wlen<=len;i++)if((i==0||!word_char(line[i-1]))&&!strncmp(line+i,word,(size_t)wlen)&&(i+wlen==len||!word_char(line[i+wlen])))return i;}else{if(from>len-wlen)from=len-wlen;for(int i=from;i>=0;i--)if((i==0||!word_char(line[i-1]))&&!strncmp(line+i,word,(size_t)wlen)&&(i+wlen==len||!word_char(line[i+wlen])))return i;}return-1;}
static bool search_word(Source*s,int*line,int*column,const char*word,int direction){if(!s->count)return false;for(int off=0;off<s->count;off++){int ln=(*line+direction*off)%s->count;if(ln<0)ln+=s->count;int from;if(off==0)from=*column+direction;else from=direction>0?0:(int)strlen(s->lines[ln]);int col=word_occurrence(s->lines[ln],word,from,direction);if(col>=0){*line=ln;*column=col;return true;}}return false;}
static void word_motion(Source*s,int*line,int*column,int direction,bool to_end){if(!s->count)return;int ln=*line,col=*column;for(int pass=0;pass<s->count+1;pass++){const char*t=s->lines[ln];int len=(int)strlen(t);if(direction>0){if(to_end){if(col<len&&word_char(t[col]))while(col+1<len&&word_char(t[col+1]))col++;else{while(col<len&&!word_char(t[col]))col++;if(col<len)while(col+1<len&&word_char(t[col+1]))col++;}}else{if(col<len&&word_char(t[col]))while(col<len&&word_char(t[col]))col++;while(col<len&&!word_char(t[col]))col++;}if(col<len){*line=ln;*column=col;return;}ln++;col=0;if(ln>=s->count)ln=0;}else{col--;while(col>=0&&!word_char(t[col]))col--;while(col>0&&word_char(t[col-1]))col--;if(col>=0){*line=ln;*column=col;return;}ln--;if(ln<0)ln=s->count-1;col=(int)strlen(s->lines[ln]);}}}
static bool goto_declaration(Source*s,int*line,int*column,bool global){char word[256];if(!word_under_cursor(s,*line,*column,word,sizeof(word),NULL))return false;int fallback_line=-1,fallback_col=0;int start=global?0:*line-1,end=global?s->count:*line,step=global?1:-1;for(int ln=start;global?ln<end:ln>=0;ln+=step){int col=word_occurrence(s->lines[ln],word,global?0:(int)strlen(s->lines[ln]),global?1:-1);if(col<0)continue;if(fallback_line<0){fallback_line=ln;fallback_col=col;}const char*t=s->lines[ln];bool declaration=false;const char*types[]={"int","char","long","short","float","double","void","struct","enum","union","size_t","bool","const","static","unsigned","signed","volatile",NULL};for(int i=0;types[i];i++)if(strstr(t,types[i])&&strstr(t,types[i])<t+col){declaration=true;break;}if(declaration){*line=ln;*column=col;return true;}}if(fallback_line>=0){*line=fallback_line;*column=fallback_col;return true;}return false;}
static bool match_bracket(Source*s,int*line,int*column){if(!s->count)return false;int ln=*line,col=*column;const char*opens="([{",*closes=")]}",*t=s->lines[ln];int len=(int)strlen(t),kind=-1,direction=1;for(int i=col;i<len;i++){const char*p=strchr(opens,t[i]);const char*q=strchr(closes,t[i]);if(p){kind=(int)(p-opens);col=i;direction=1;break;}if(q){kind=(int)(q-closes);col=i;direction=-1;break;}}if(kind<0)return false;char open=opens[kind],close=closes[kind];int depth=0;for(int guard=0;guard<100000;guard++){col+=direction;while(ln>=0&&ln<s->count&&(col<0||col>=(int)strlen(s->lines[ln]))){if(direction>0){ln++;col=0;}else{ln--;if(ln>=0)col=(int)strlen(s->lines[ln])-1;}}if(ln<0||ln>=s->count)break;char c=s->lines[ln][col];if(c==(direction>0?open:close))depth++;else if(c==(direction>0?close:open)){if(depth==0){*line=ln;*column=col;return true;}depth--;}}return false;}
static void paragraph_motion(Source*s,int*line,int direction){if(!s->count)return;int ln=*line+direction;while(ln>0&&ln<s->count&&s->lines[ln][0]=='\0')ln+=direction;while(ln>0&&ln<s->count&&s->lines[ln][0]!='\0')ln+=direction;if(ln<0)ln=0;if(ln>=s->count)ln=s->count-1;*line=ln;}
static int expansion_index(VarExpansion*items,int count,const char*expression){for(int i=0;i<count;i++)if(!strcmp(items[i].expression,expression))return i;return-1;}
static void add_var_header(VarRow*rows,int*row_count,const char*label){if(*row_count>=UI_MAX_VAR_ROWS)return;VarRow*r=&rows[(*row_count)++];memset(r,0,sizeof(*r));r->header=true;snprintf(r->label,sizeof(r->label),"%s",label);}
static void add_var_tree(VarRow*rows,int*row_count,VarExpansion*expansions,int expansion_count,const char*expression,const char*label,const char*value,const char*type,int depth,bool expandable){if(*row_count>=UI_MAX_VAR_ROWS||depth>8)return;VarRow*r=&rows[(*row_count)++];memset(r,0,sizeof(*r));snprintf(r->expression,sizeof(r->expression),"%s",expression);snprintf(r->label,sizeof(r->label),"%s",label);snprintf(r->value,sizeof(r->value),"%s",value);snprintf(r->type,sizeof(r->type),"%s",type?type:"");r->depth=depth;r->expandable=expandable;int idx=expansion_index(expansions,expansion_count,expression);r->expanded=idx>=0;if(idx>=0)for(int i=0;i<expansions[idx].child_count;i++){GdbChild*c=&expansions[idx].children[i];add_var_tree(rows,row_count,expansions,expansion_count,c->expression,c->name,c->value,c->type,depth+1,c->numchild>0);}}
static int build_var_rows(Gdb*g,VarExpansion*expansions,int expansion_count,VarRow*rows){int count=0;if(g->arg_count){add_var_header(rows,&count,"Args");for(int i=0;i<g->arg_count;i++)add_var_tree(rows,&count,expansions,expansion_count,g->args[i].name,g->args[i].name,g->args[i].value,g->args[i].type,0,true);}if(g->local_count){add_var_header(rows,&count,"Locals");for(int i=0;i<g->local_count;i++)add_var_tree(rows,&count,expansions,expansion_count,g->locals[i].name,g->locals[i].name,g->locals[i].value,g->locals[i].type,0,true);}return count;}
static int selectable_var(VarRow*rows,int count,int start,int direction){for(int i=start;i>=0&&i<count;i+=direction)if(!rows[i].header)return i;return-1;}
static void collapse_expansion(VarExpansion*items,int*count,int index){if(index<0||index>=*count)return;memmove(&items[index],&items[index+1],(size_t)(*count-index-1)*sizeof(*items));(*count)--;}
static void toggle_expansion(Gdb*g,VarExpansion*items,int*count,const char*expression){int idx=expansion_index(items,*count,expression);if(idx>=0){collapse_expansion(items,count,idx);snprintf(g->message,sizeof(g->message),"Collapsed %.900s",expression);return;}if(*count>=UI_MAX_EXPANSIONS){snprintf(g->message,sizeof(g->message),"Too many expanded variables");return;}VarExpansion*e=&items[*count];memset(e,0,sizeof(*e));snprintf(e->expression,sizeof(e->expression),"%s",expression);if(!gdb_list_children(g,expression,e->children,UI_MAX_CHILDREN,&e->child_count)&&e->child_count>0)(*count)++;}
static void refresh_expansions(Gdb*g,VarExpansion*items,int*count){for(int i=0;i<*count;){int child_count=0;if(gdb_list_children(g,items[i].expression,items[i].children,UI_MAX_CHILDREN,&child_count)||child_count<=0){collapse_expansion(items,count,i);continue;}items[i].child_count=child_count;i++;}}
static int build_address_panel(Gdb*g,VarRow*rows,int row_count,int selected,AddressPanel*panel){memset(panel,0,sizeof(*panel));if(selected<0||selected>=row_count||rows[selected].header)return-1;if(gdb_inspect_address(g,rows[selected].expression,&panel->info))return-1;if(panel->info.pointer&&panel->info.address_available&&!panel->info.is_null&&!panel->info.optimized_out){for(int i=0;i<row_count&&panel->match_count<UI_MAX_ADDRESS_MATCHES;i++){if(rows[i].header||strstr(rows[i].value,"<optimized out>"))continue;char address[128];if(gdb_expression_address(g,rows[i].expression,address,sizeof(address))||strcmp(address,panel->info.address))continue;bool duplicate=false;for(int j=0;j<panel->match_count;j++)if(!strcmp(panel->matches[j],rows[i].expression)){duplicate=true;break;}if(!duplicate)snprintf(panel->matches[panel->match_count++],sizeof(panel->matches[0]),"%.500s",rows[i].expression);}}snprintf(g->message,sizeof(g->message),"Address details: %.700s%s",panel->info.expression,panel->info.pointer?" (pointer)":"");return 0;}
static void layout_heights(int rows,int*source_h,int*variables_h,int*stack_h){int available=rows-7;*source_h=available*55/100;if(*source_h<4)*source_h=4;int rest=available-*source_h;*variables_h=rest/2;if(*variables_h<3)*variables_h=3;*stack_h=rest-*variables_h;if(*stack_h<2)*stack_h=2;while(3+*source_h+1+*variables_h+1+*stack_h>rows-2){if(*source_h>4)(*source_h)--;else if(*variables_h>3)(*variables_h)--;else if(*stack_h>2)(*stack_h)--;else break;}}
static void line_rule(int y,const char *title){int cols=getmaxx(stdscr);attron(COLOR_PAIR(COLOR_MUTED)|A_DIM);mvhline(y,0,ACS_HLINE,cols);attroff(COLOR_PAIR(COLOR_MUTED)|A_DIM);if(title){char b[128];snprintf(b,sizeof(b)," %s ",title);attron(A_BOLD|COLOR_PAIR(COLOR_INFO));mvaddstr(y,2,b);attroff(A_BOLD|COLOR_PAIR(COLOR_INFO));}}
static void draw_stop_summary(Gdb*g,int cols){int breaks=0,conditional=0,catches=0,watches=0,disabled=0;for(int i=0;i<g->break_count;i++){GdbBreakpoint*b=&g->breaks[i];if(!b->enabled)disabled++;if(b->watchpoint)watches++;else if(b->catchpoint)catches++;else{breaks++;if(b->condition[0])conditional++;}}move(1,0);clrtoeol();attron(A_BOLD);addstr(" STOPS ");attroff(A_BOLD);attron(COLOR_PAIR(COLOR_BREAK));printw("[B] %d",breaks);attroff(COLOR_PAIR(COLOR_BREAK));if(conditional)printw(" (cond %d)",conditional);attron(COLOR_PAIR(COLOR_COND));printw("  [C] %d",catches);attroff(COLOR_PAIR(COLOR_COND));attron(COLOR_PAIR(COLOR_WATCH));printw("  [W] %d",watches);attroff(COLOR_PAIR(COLOR_WATCH));if(disabled){attron(COLOR_PAIR(COLOR_MUTED)|A_DIM);printw("  off %d",disabled);attroff(COLOR_PAIR(COLOR_MUTED)|A_DIM);}if(g->stop_breakpoint){GdbBreakpoint*b=breakpoint_by_number(g,g->stop_breakpoint);int x=getcurx(stdscr);if(x<cols-12){addstr("  | last: ");if(b){attron(COLOR_PAIR(break_color(b))|A_BOLD);printw("%c#%d",b->watchpoint?'W':b->catchpoint?'C':'B',b->number);attroff(COLOR_PAIR(break_color(b))|A_BOLD);if(b->watchpoint&&b->condition[0])printw(" %s",b->condition);}else printw("#%d",g->stop_breakpoint);if(g->watch_old[0]||g->watch_new[0])printw(" %s -> %s",g->watch_old[0]?g->watch_old:"?",g->watch_new[0]?g->watch_new:"?");}}if(g->catch_event[0]&&getcurx(stdscr)<cols-18){attron(COLOR_PAIR(COLOR_COND)|A_BOLD);printw("  | %s%s%s",g->catch_event,g->catch_target[0]?":":"",g->catch_target);if(g->catch_phase[0])printw(" %s",g->catch_phase);attroff(COLOR_PAIR(COLOR_COND)|A_BOLD);}}

static void draw_source_code(Gdb*g,Source*s,int cursor,int column,int top,int hscroll,InputMode mode,PaneFocus focus,const char*search_text,bool search_whole_word,const char*project_root,int history_index,int history_count,int height,int cols){bool at_exec=g->source_available&&s->debug_lines&&cursor+1==g->line&&same_source_path(s->path,g->fullname);char title[384];snprintf(title,sizeof(title),"Source [%s]%s%s  [%s] %.210s:%d  history %d/%d",at_exec?"EXEC":"VIEW",focus==PANE_SOURCE?" *":"",s->debug_lines?"":" [REFERENCE ONLY]",project_path(s->path,project_root)?"project":"external",s->path[0]?path_name(s->path):"-",cursor+1,history_count?history_index+1:0,history_count);line_rule(2,title);if(!s->count){attron(COLOR_PAIR(COLOR_MUTED)|A_DIM);clipped(4,2,cols-4,"Source file is not available. Press d for Disassembly.");attroff(COLOR_PAIR(COLOR_MUTED)|A_DIM);return;}for(int r=0;r<height;r++){int ln=top+r+1;if(ln>s->count)break;bool current=g->source_available&&ln==g->line&&same_source_path(s->path,g->fullname);GdbBreakpoint*bp=breakpoint_at(g,s->path,ln);int y=3+r,len=(int)strlen(s->lines[ln-1]),start=hscroll<len?hscroll:len;char marker[16]="";if(bp)snprintf(marker,sizeof(marker),"[%c#%d]",bp->enabled?'B':'d',bp->number);if(current){attron(COLOR_PAIR(COLOR_WATCH)|A_BOLD);mvaddstr(y,0,"=>");attroff(COLOR_PAIR(COLOR_WATCH)|A_BOLD);}if(bp){int color=break_color(bp);attron(COLOR_PAIR(color)|(bp->enabled?A_BOLD:A_DIM));mvaddnstr(y,2,marker,7);attroff(COLOR_PAIR(color)|(bp->enabled?A_BOLD:A_DIM));}mvaddch(y,9,ln-1==cursor?'>':' ');clipped(y,11,7,"%5d  ",ln);if(hscroll>0)mvaddch(y,17,'<');if(current)attron(A_BOLD);clipped(y,18,cols-18,"%s",s->lines[ln-1]+start);if(current)attroff(A_BOLD);if(mode==MODE_VIM&&search_text&&search_text[0]){const char*p=s->lines[ln-1];int slen=(int)strlen(search_text);while((p=strstr(p,search_text))){int pos=(int)(p-s->lines[ln-1]);bool boundary=!search_whole_word||((pos==0||!word_char(s->lines[ln-1][pos-1]))&&(pos+slen==len||!word_char(s->lines[ln-1][pos+slen])));if(boundary){int visible_start=pos<hscroll?hscroll:pos,visible_end=pos+slen;if(visible_end>hscroll&&visible_start-hscroll<cols-18){int cell=18+visible_start-hscroll,count=visible_end-visible_start;if(cell+count>cols)count=cols-cell;if(count>0)mvchgat(y,cell,count,A_REVERSE|A_BOLD,COLOR_COND,NULL);}}p+=slen;}}if(focus==PANE_SOURCE&&ln-1==cursor){int cell=18+column-hscroll;if(cell>=18&&cell<cols)mvchgat(y,cell,1,A_REVERSE|A_BOLD,COLOR_INFO,NULL);}}}

static void draw_disassembly(Gdb*g,int selected,int top,PaneFocus focus,int height,int cols){char title[384];const char*function=g->disassembly_function[0]?g->disassembly_function:g->function[0]?g->function:"<unknown>";snprintf(title,sizeof(title),"Disassembly [%s]%s  Function: %.180s  RIP: %.120s",g->current_instruction>=0?"EXEC":"VIEW",focus==PANE_SOURCE?" *":"",function,g->pc[0]?g->pc:"<unavailable>");line_rule(2,title);if(!g->instruction_count){attron(COLOR_PAIR(COLOR_MUTED)|A_DIM);clipped(4,2,cols-4,"Disassembly unavailable until the program is stopped.");attroff(COLOR_PAIR(COLOR_MUTED)|A_DIM);return;}for(int r=0;r<height;r++){int idx=top+r;if(idx>=g->instruction_count)break;GdbInstruction*ins=&g->instructions[idx];GdbBreakpoint*bp=breakpoint_at_address(g,ins->address);int y=3+r;bool selected_row=focus==PANE_SOURCE&&idx==selected;if(selected_row)attron(A_REVERSE);if(ins->current){attron(COLOR_PAIR(COLOR_WATCH)|A_BOLD);mvaddstr(y,0,"=>");attroff(COLOR_PAIR(COLOR_WATCH)|A_BOLD);}if(bp){attron(COLOR_PAIR(break_color(bp))|A_BOLD);clipped(y,2,7,"[B#%d]",bp->number);attroff(COLOR_PAIR(break_color(bp))|A_BOLD);}else clipped(y,2,7," ");clipped(y,9,19,"%-18s",ins->address);if(ins->call){attron(COLOR_PAIR(COLOR_COND)|A_BOLD);clipped(y,28,cols-29,"CALL -> %s  |  %s",ins->call_target[0]?ins->call_target:"<indirect>",ins->instruction);attroff(COLOR_PAIR(COLOR_COND)|A_BOLD);}else{if(ins->current)attron(A_BOLD);clipped(y,28,cols-29,"%s",ins->instruction);if(ins->current)attroff(A_BOLD);}if(selected_row)attroff(A_REVERSE);}}

static void draw_variables(Gdb*g,VarRow*var_rows,int var_count,int var_top,int var_selected,PaneFocus focus,int y,int height,int cols){line_rule(y-1,focus==PANE_VARIABLES?"Variables *":"Variables");if(!g->source_available){attron(COLOR_PAIR(COLOR_MUTED)|A_DIM);clipped(y+1,2,cols-4,"Debug information unavailable.");clipped(y+2,2,cols-4,"Use Registers / Disassembly.");attroff(COLOR_PAIR(COLOR_MUTED)|A_DIM);return;}for(int r=0;r<height;r++){int idx=var_top+r;if(idx>=var_count)break;VarRow*v=&var_rows[idx];if(v->header){attron(A_BOLD|COLOR_PAIR(COLOR_INFO));clipped(y+r,2,cols-3,"%s",v->label);attroff(A_BOLD|COLOR_PAIR(COLOR_INFO));continue;}bool selected=focus==PANE_VARIABLES&&idx==var_selected;if(selected)attron(A_REVERSE);int x=3+v->depth*2;char marker=v->expanded?'-':v->expandable?'+':' ';mvaddch(y+r,x++,marker);mvaddch(y+r,x++,' ');int wid=watchpoint_for_expression(g,v->expression);if(wid){attron(COLOR_PAIR(COLOR_WATCH)|A_BOLD);char tag[24];snprintf(tag,sizeof(tag),"[W#%d] ",wid);mvaddstr(y+r,x,tag);x+=(int)strlen(tag);attroff(COLOR_PAIR(COLOR_WATCH)|A_BOLD);}clipped(y+r,x,cols-x-1,"%s = %s%s%s",v->label,v->value,v->type[0]?"  ":"",v->type);if(selected)attroff(A_REVERSE);}if(var_count==0){attron(COLOR_PAIR(COLOR_MUTED)|A_DIM);clipped(y,2,cols-3,"(variables unavailable; stop the program first)");attroff(COLOR_PAIR(COLOR_MUTED)|A_DIM);}}

static int syscall_argument(const char*name){const char*names[]={"rdi","rsi","rdx","r10","r8","r9"};for(int i=0;i<6;i++)if(!strcmp(name,names[i]))return i+1;return 0;}
static void draw_registers(Gdb*g,int selected,int top,PaneFocus focus,int y,int height,int cols,bool call_selected){bool syscall=!strcmp(g->catch_event,"syscall");char title[200];snprintf(title,sizeof(title),"Registers%s%s%s%s%s",focus==PANE_REGISTERS?" *":"",syscall?"  [syscall ":"",syscall?(g->catch_phase[0]?g->catch_phase:"stop"):"",syscall?"]":"",!syscall&&call_selected?"  [call arguments]":"");line_rule(y-1,title);if(!g->register_count){attron(COLOR_PAIR(COLOR_MUTED)|A_DIM);clipped(y+1,2,cols-4,"Registers unavailable until the program is stopped.");attroff(COLOR_PAIR(COLOR_MUTED)|A_DIM);return;}if(!g->amd64_sysv){attron(COLOR_PAIR(COLOR_COND));clipped(y,2,cols-4,"Argument register mapping unavailable for this architecture.");attroff(COLOR_PAIR(COLOR_COND));y++;height--;}for(int row=0;row<height;row++){int idx=top+row;if(idx>=g->register_count)break;GdbRegister*r=&g->registers[idx];int arg=syscall?syscall_argument(r->name):r->argument;bool sysno=syscall&&!strcmp(r->name,"orig_rax");bool special=syscall&&(!strcmp(r->name,"rax")||sysno||arg);bool selected_row=focus==PANE_REGISTERS&&idx==selected;if(selected_row)attron(A_REVERSE);if(special||arg)attron(COLOR_PAIR(COLOR_INFO)|A_BOLD);else if(r->changed)attron(COLOR_PAIR(COLOR_WATCH)|A_BOLD);if(sysno)clipped(y+row,2,cols-4,"sysno / %-7s = %s%s%s",r->name,r->changed?r->previous:r->value,r->changed?" -> ":"",r->changed?r->value:"");else if(syscall&&!strcmp(r->name,"rax")&&!strcmp(g->catch_phase,"Return"))clipped(y+row,2,cols-4,"return / rax = %s%s%s",r->changed?r->previous:r->value,r->changed?" -> ":"",r->changed?r->value:"");else if(arg)clipped(y+row,2,cols-4,"arg%d / %-7s = %s%s%s",arg,r->name,r->changed?r->previous:r->value,r->changed?" -> ":"",r->changed?r->value:"");else clipped(y+row,2,cols-4,"%-12s = %s%s%s",r->name,r->changed?r->previous:r->value,r->changed?" -> ":"",r->changed?r->value:"");if(special||arg)attroff(COLOR_PAIR(COLOR_INFO)|A_BOLD);else if(r->changed)attroff(COLOR_PAIR(COLOR_WATCH)|A_BOLD);if(selected_row)attroff(A_REVERSE);}}

static void draw_stack(Gdb*g,int selected,PaneFocus focus,int y,int height,int cols,const char*project_root){line_rule(y-1,focus==PANE_STACK?"Stack *  [P] project  [X] external":"Stack  [P] project  [X] external");for(int i=0;i<g->frame_count&&i<height;i++){GdbFrame*f=&g->frames[i];bool selected_row=focus==PANE_STACK&&i==selected;bool active=f->level==g->selected_frame;bool has_source=f->file[0]&&f->line>0;bool in_project=has_source&&project_path(f->file,project_root);if(selected_row)attron(A_REVERSE);if(active)attron(A_BOLD);attron(COLOR_PAIR(has_source?(in_project?COLOR_INFO:COLOR_COND):COLOR_MUTED));clipped(y+i,1,cols-2,"%c #%d %s",active?'>':' ',f->level,has_source?(in_project?"[P]":"[X]"):"[A]");attroff(COLOR_PAIR(has_source?(in_project?COLOR_INFO:COLOR_COND):COLOR_MUTED));const char*func=f->func[0]&&strcmp(f->func,"??")?f->func:NULL;if(has_source)clipped(y+i,10,cols-11,"%s()  %s:%d",func?func:"?",f->file,f->line);else clipped(y+i,10,cols-11,"%s%s%s",func?func:"",func?"()  ":"",f->address[0]?f->address:"<address unavailable>");if(active)attroff(A_BOLD);if(selected_row)attroff(A_REVERSE);}}

static void draw_main(Gdb*g,Source*s,int cursor,int column,int top,int hscroll,InputMode mode,CodeView code_view,PaneFocus focus,VarRow*var_rows,int var_count,int var_top,int var_selected,int reg_selected,int reg_top,int asm_selected,int asm_top,int stack_selected,const char*search_text,bool search_whole_word,const char*project_root,int history_index,int history_count){int rows,cols,code_h,middle_h,stack_h;getmaxyx(stdscr,rows,cols);layout_heights(rows,&code_h,&middle_h,&stack_h);if(code_view==CODE_ASSEMBLY)while(middle_h<6&&stack_h>2){middle_h++;stack_h--;}erase();attron(A_BOLD|COLOR_PAIR(COLOR_INFO));clipped(0,1,cols-2,"gd [%s] [%s] [%s]",mode==MODE_VIM?"VIM":"GDB",code_view==CODE_ASSEMBLY?"ASM":"SRC",focus_name(focus));attroff(A_BOLD|COLOR_PAIR(COLOR_INFO));clipped(0,27,cols-28,"| %-11s | Exec frame #%d %s%s%s",state_name(g->state),g->selected_frame,g->function[0]?g->function:"-",g->source_available?":":" @ ",g->source_available?(char[32]){0}:g->pc);if(g->source_available){char where[64];snprintf(where,sizeof(where),"%d",g->line);clipped(0,cols>20?cols-20:0,19,"Exec line %s",where);}draw_stop_summary(g,cols);if(code_view==CODE_ASSEMBLY)draw_disassembly(g,asm_selected,asm_top,focus,code_h,cols);else draw_source_code(g,s,cursor,column,top,hscroll,mode,focus,search_text,search_whole_word,project_root,history_index,history_count,code_h,cols);int y=3+code_h+1;bool call_selected=asm_selected>=0&&asm_selected<g->instruction_count&&g->instructions[asm_selected].call;if(code_view==CODE_ASSEMBLY)draw_registers(g,reg_selected,reg_top,focus,y,middle_h,cols,call_selected);else draw_variables(g,var_rows,var_count,var_top,var_selected,focus,y,middle_h,cols);y+=middle_h+1;draw_stack(g,stack_selected,focus,y,stack_h,cols,project_root);line_rule(rows-2,NULL);int status_color=!strncmp(g->message,"ERROR",5)?COLOR_ERROR:strstr(g->message,"Watchpoint")||strstr(g->message,"watchpoint")?COLOR_WATCH:strstr(g->message,"Catch")||strstr(g->message,"Caught")?COLOR_COND:COLOR_INFO;attron(COLOR_PAIR(status_color));clipped(rows-1,1,cols-2,"%s",g->message);attroff(COLOR_PAIR(status_color));}
static WINDOW*create_popup(const char*title,int wanted_height,int wanted_width){int rows,cols,height,width,x,y;getmaxyx(stdscr,rows,cols);height=wanted_height<rows-2?wanted_height:rows-2;width=wanted_width<cols-2?wanted_width:cols-2;if(height<5)height=rows;if(width<20)width=cols;y=(rows-height)/2;x=(cols-width)/2;WINDOW*window=newwin(height,width,y,x);if(!window)return NULL;keypad(window,TRUE);box(window,0,0);if(width>(int)strlen(title)+4)mvwprintw(window,0,(width-(int)strlen(title)-2)/2," %s ",title);return window;}
static void draw_help_popup(void){static const char*lines[]={"Execution","  r / c       Run / Continue","  n / s       Next / Step into","  f           Finish current function and return to caller","              caller has source -> Source, otherwise Assembly","  i / I       Step instruction / Next instruction","  R           Force return (confirmation required)","","View / History","  Tab         Switch pane    Shift-Tab: reverse","  d / e       Source/Assembly / Return to execution position","  o           Open Function / File","  [ / Ctrl-o  View history back    ]: forward","              history only changes the view; f executes","","Stop conditions","  b / B / F   Breakpoint / Conditional / Function","  C / w / L   Catchpoint / Watchpoint / Stop list","","Panes","  Variables   Enter expand, p value, a address, E edit","  Registers   p details, E edit","  Stack       Enter selects frame and opens its location","","Modes / Other","  F2          GDB / read-only VIM mode","  VIM         h/j/k/l, /?, n/N, gd/gD, *#, %","  O           Program output","  ?           Help    q: Quit",NULL};int count=0;while(lines[count])count++;WINDOW*window=create_popup("Help",count+2,76);if(!window)return;int height,width;getmaxyx(window,height,width);for(int i=0;i<count&&i+1<height-1;i++){if(lines[i][0]&&!isspace((unsigned char)lines[i][0]))wattron(window,A_BOLD);mvwaddnstr(window,i+1,2,lines[i],width-4);wattroff(window,A_BOLD);}wnoutrefresh(window);delwin(window);}
static void draw_output(Gdb*g){erase();attron(A_BOLD);mvaddstr(0,2,"Program Output");attroff(A_BOLD);int rows,cols;getmaxyx(stdscr,rows,cols);int y=2;const char*p=g->output;while(*p&&y<rows-2){const char*e=strchr(p,'\n');int n=e?(int)(e-p):(int)strlen(p);mvaddnstr(y++,1,p,n<cols-2?n:cols-2);if(!e)break;p=e+1;}mvaddstr(rows-1,2,"Esc / o: Back");refresh();}
static void draw_address(const AddressPanel*p){int rows,cols;getmaxyx(stdscr,rows,cols);erase();int width=cols-4;if(width>76)width=76;if(width<36)width=cols;int height=p->info.pointer?10+p->match_count:7;if(p->info.pointer&&!p->match_count)height=11;if(height>rows-2)height=rows-2;if(height<7)height=7;int x=(cols-width)/2,y=(rows-height)/2;if(x<0)x=0;if(y<0)y=0;attron(COLOR_PAIR(COLOR_INFO)|A_BOLD);mvaddch(y,x,ACS_ULCORNER);mvhline(y,x+1,ACS_HLINE,width-2);mvaddch(y,x+width-1,ACS_URCORNER);for(int r=1;r<height-1;r++){mvaddch(y+r,x,ACS_VLINE);mvaddch(y+r,x+width-1,ACS_VLINE);}mvaddch(y+height-1,x,ACS_LLCORNER);mvhline(y+height-1,x+1,ACS_HLINE,width-2);mvaddch(y+height-1,x+width-1,ACS_LRCORNER);const char*title=p->info.pointer?" Pointer Details ":" Address ";clipped(y,x+(width-(int)strlen(title))/2,(int)strlen(title),"%s",title);attroff(COLOR_PAIR(COLOR_INFO)|A_BOLD);int row=y+2;clipped(row++,x+2,width-4,"Expression : %s",p->info.expression);clipped(row++,x+2,width-4,"Type       : %s",p->info.type);if(p->info.pointer){clipped(row++,x+2,width-4,"Address    : %s",p->info.address);clipped(row++,x+2,width-4,"Points to  : %s",p->info.points_to);row++;attron(A_BOLD|COLOR_PAIR(COLOR_WATCH));clipped(row++,x+2,width-4,p->match_count==1?"Matched variable:":"Matched variables:");attroff(A_BOLD|COLOR_PAIR(COLOR_WATCH));if(!p->match_count){clipped(row++,x+4,width-6,"not found");}else for(int i=0;i<p->match_count&&row<y+height-1;i++)clipped(row++,x+4,width-6,"%s",p->matches[i]);}else{clipped(row++,x+2,width-4,"Value      : %s",p->info.value);clipped(row++,x+2,width-4,"Address    : %s",p->info.address);}clipped(rows-1,2,cols-4,"Esc / a / Enter: Back");refresh();}
static void build_register_panel(const GdbRegister*r,RegisterPanel*p){memset(p,0,sizeof(*p));snprintf(p->name,sizeof(p->name),"%s",r->name);snprintf(p->value,sizeof(p->value),"%s",r->value);char*end=NULL;p->numeric=strtoull(r->value,&end,0);p->numeric_available=end&&end!=r->value&&(*end=='\0'||isspace((unsigned char)*end));}
static void draw_register_panel(const RegisterPanel*p){int rows,cols;getmaxyx(stdscr,rows,cols);erase();int width=60;if(width>cols-2)width=cols-2;int height=9,x=(cols-width)/2,y=(rows-height)/2;if(x<0)x=0;if(y<0)y=0;attron(COLOR_PAIR(COLOR_INFO)|A_BOLD);mvaddch(y,x,ACS_ULCORNER);mvhline(y,x+1,ACS_HLINE,width-2);mvaddch(y,x+width-1,ACS_URCORNER);for(int r=1;r<height-1;r++){mvaddch(y+r,x,ACS_VLINE);mvaddch(y+r,x+width-1,ACS_VLINE);}mvaddch(y+height-1,x,ACS_LLCORNER);mvhline(y+height-1,x+1,ACS_HLINE,width-2);mvaddch(y+height-1,x+width-1,ACS_LRCORNER);attroff(COLOR_PAIR(COLOR_INFO)|A_BOLD);attron(A_BOLD);clipped(y+2,x+2,width-4,"Register: %s",p->name);attroff(A_BOLD);clipped(y+3,x+2,width-4,"Value   : %s",p->value);if(p->numeric_available){clipped(y+4,x+2,width-4,"Decimal : %llu",p->numeric);clipped(y+5,x+2,width-4,"Hex     : 0x%llx",p->numeric);}else clipped(y+4,x+2,width-4,"Numeric value unavailable");clipped(rows-1,2,cols-4,"Esc / p / Enter: Back");refresh();}
static void refresh_function_dialog(Gdb*g,FunctionDialog*d){if(gdb_list_functions(g,d->input,d->candidates,GD_MAX_FUNCTIONS,&d->count)){d->count=0;}d->selected=0;d->top=0;}
static void draw_function_dialog(Gdb*g,FunctionDialog*d){
    int rows,cols;getmaxyx(stdscr,rows,cols);erase();
    int width=cols-4;if(width>100)width=100;if(width<48)width=cols;
    int height=rows-4;if(height>20)height=20;if(height<10)height=rows;
    int x=(cols-width)/2,y=(rows-height)/2;if(x<0)x=0;if(y<0)y=0;
    attron(COLOR_PAIR(COLOR_INFO)|A_BOLD);
    mvaddch(y,x,ACS_ULCORNER);mvhline(y,x+1,ACS_HLINE,width-2);mvaddch(y,x+width-1,ACS_URCORNER);
    for(int r=1;r<height-1;r++){mvaddch(y+r,x,ACS_VLINE);mvaddch(y+r,x+width-1,ACS_VLINE);}
    mvaddch(y+height-1,x,ACS_LLCORNER);mvhline(y+height-1,x+1,ACS_HLINE,width-2);mvaddch(y+height-1,x+width-1,ACS_LRCORNER);
    const char*title=d->action==FUNCTION_OPEN?" Open Function ":" Function Breakpoint ";clipped(y,x+(width-(int)strlen(title))/2,(int)strlen(title),"%s",title);
    attroff(COLOR_PAIR(COLOR_INFO)|A_BOLD);
    clipped(y+2,x+2,width-4,"Function: %s_",d->input);
    attron(A_BOLD);clipped(y+4,x+2,width-4,"Candidates");attroff(A_BOLD);
    int visible=height-9;
    if(d->selected<d->top)d->top=d->selected;
    if(d->selected>=d->top+visible)d->top=d->selected-visible+1;
    if(!d->count){
        attron(COLOR_PAIR(COLOR_MUTED)|A_DIM);
        clipped(y+6,x+3,width-6,d->input[0]?"No matching function symbols.":"No function symbols available.");
        if(!d->input[0])clipped(y+7,x+3,width-6,"Use address breakpoint in Assembly Mode.");
        attroff(COLOR_PAIR(COLOR_MUTED)|A_DIM);
    }
    int module_width=width>=80?16:12;
    int name_width=width-module_width-27;
    if(name_width<12)name_width=12;
    for(int row=0;row<visible;row++){
        int idx=d->top+row;if(idx>=d->count)break;
        GdbFunction*f=&d->candidates[idx];bool selected=idx==d->selected;
        if(selected)attron(A_REVERSE);
        if(d->action==FUNCTION_OPEN){char location[GD_PATH_MAX+32];if(f->source[0]&&f->line>0)snprintf(location,sizeof(location),"%s:%d",f->source,f->line);else snprintf(location,sizeof(location),"<source unknown> %s",f->address[0]?f->address:"");clipped(y+5+row,x+2,width-4,"%c %-*.*s  %s",selected?'>':' ',name_width,name_width,f->name,location);}
        else clipped(y+5+row,x+2,width-4,"%c %-*.*s  %-*.*s  %.18s",selected?'>':' ',name_width,name_width,f->name,module_width,module_width,f->module[0]?f->module:"<unknown>",f->address[0]?f->address:"<unavailable>");
        if(selected)attroff(A_REVERSE);
    }
    attron(COLOR_PAIR(COLOR_MUTED));
    clipped(y+height-3,x+2,width-4,"j/k or arrows: Select   Enter: %s   Esc: Cancel",d->action==FUNCTION_OPEN?"Open":"Set");
    attroff(COLOR_PAIR(COLOR_MUTED));
    int color=!strncmp(g->message,"ERROR",5)||strstr(g->message,"not found")?COLOR_ERROR:COLOR_INFO;
    attron(COLOR_PAIR(color));clipped(y+height-2,x+2,width-4,"%s",g->message);attroff(COLOR_PAIR(color));
    refresh();
}
static void draw_open_menu(int selected){int rows,cols;getmaxyx(stdscr,rows,cols);erase();int width=52,height=10,x=(cols-width)/2,y=(rows-height)/2;if(x<0)x=0;if(y<0)y=0;attron(COLOR_PAIR(COLOR_INFO)|A_BOLD);mvaddch(y,x,ACS_ULCORNER);mvhline(y,x+1,ACS_HLINE,width-2);mvaddch(y,x+width-1,ACS_URCORNER);for(int r=1;r<height-1;r++){mvaddch(y+r,x,ACS_VLINE);mvaddch(y+r,x+width-1,ACS_VLINE);}mvaddch(y+height-1,x,ACS_LLCORNER);mvhline(y+height-1,x+1,ACS_HLINE,width-2);mvaddch(y+height-1,x+width-1,ACS_LRCORNER);const char*title=" Open Source ";clipped(y,x+(width-(int)strlen(title))/2,(int)strlen(title),"%s",title);attroff(COLOR_PAIR(COLOR_INFO)|A_BOLD);const char*items[]={"Function","File"};for(int i=0;i<2;i++){if(i==selected)attron(A_REVERSE);clipped(y+3+i*2,x+4,width-8,"%c %s",i==selected?'>':' ',items[i]);if(i==selected)attroff(A_REVERSE);}attron(COLOR_PAIR(COLOR_MUTED));clipped(y+height-2,x+2,width-4,"j/k: Select   Enter: Open   Esc: Cancel");attroff(COLOR_PAIR(COLOR_MUTED));refresh();}
static void draw_file_dialog(Gdb*g,FileDialog*d){int rows,cols;getmaxyx(stdscr,rows,cols);erase();int width=cols-4;if(width>110)width=110;if(width<48)width=cols;int height=rows-4;if(height>22)height=22;if(height<10)height=rows;int x=(cols-width)/2,y=(rows-height)/2;if(x<0)x=0;if(y<0)y=0;attron(COLOR_PAIR(COLOR_INFO)|A_BOLD);mvaddch(y,x,ACS_ULCORNER);mvhline(y,x+1,ACS_HLINE,width-2);mvaddch(y,x+width-1,ACS_URCORNER);for(int r=1;r<height-1;r++){mvaddch(y+r,x,ACS_VLINE);mvaddch(y+r,x+width-1,ACS_VLINE);}mvaddch(y+height-1,x,ACS_LLCORNER);mvhline(y+height-1,x+1,ACS_HLINE,width-2);mvaddch(y+height-1,x+width-1,ACS_LRCORNER);const char*title=" Open File ";clipped(y,x+(width-(int)strlen(title))/2,(int)strlen(title),"%s",title);attroff(COLOR_PAIR(COLOR_INFO)|A_BOLD);clipped(y+2,x+2,width-4,"File: %s_",d->input);attron(A_BOLD);clipped(y+4,x+2,width-4,"Sources  (debug line info / reference only)");attroff(A_BOLD);int visible=height-9;if(d->selected<d->top)d->top=d->selected;if(d->selected>=d->top+visible)d->top=d->selected-visible+1;if(!d->count){attron(COLOR_PAIR(COLOR_MUTED)|A_DIM);clipped(y+6,x+3,width-6,"No matching project source files.");attroff(COLOR_PAIR(COLOR_MUTED)|A_DIM);}for(int row=0;row<visible;row++){int pos=d->top+row;if(pos>=d->count)break;SourceFileCandidate*f=&d->all[d->matches[pos]];bool selected=pos==d->selected;if(selected)attron(A_REVERSE);clipped(y+5+row,x+2,width-4,"%c [%s] %s",selected?'>':' ',f->debug_lines?"DEBUG":"REFERENCE",f->display);if(selected)attroff(A_REVERSE);}attron(COLOR_PAIR(COLOR_MUTED));clipped(y+height-3,x+2,width-4,"Type to filter   j/k: Select   Enter: Open   Esc: Cancel");attroff(COLOR_PAIR(COLOR_MUTED));attron(COLOR_PAIR(COLOR_INFO));clipped(y+height-2,x+2,width-4,"%s",g->message);attroff(COLOR_PAIR(COLOR_INFO));refresh();}
static void draw_edit_dialog(EditDialog*d){int rows,cols;getmaxyx(stdscr,rows,cols);erase();int width=cols-4;if(width>76)width=76;if(width<46)width=cols;int height=15,x=(cols-width)/2,y=(rows-height)/2;if(y<0)y=0;if(x<0)x=0;attron(COLOR_PAIR(COLOR_INFO)|A_BOLD);mvaddch(y,x,ACS_ULCORNER);mvhline(y,x+1,ACS_HLINE,width-2);mvaddch(y,x+width-1,ACS_URCORNER);for(int r=1;r<height-1;r++){mvaddch(y+r,x,ACS_VLINE);mvaddch(y+r,x+width-1,ACS_VLINE);}mvaddch(y+height-1,x,ACS_LLCORNER);mvhline(y+height-1,x+1,ACS_HLINE,width-2);mvaddch(y+height-1,x+width-1,ACS_LRCORNER);const char*title=d->kind==EDIT_REGISTER?" Edit Register ":" Edit Variable ";clipped(y,x+(width-(int)strlen(title))/2,(int)strlen(title),"%s",title);attroff(COLOR_PAIR(COLOR_INFO)|A_BOLD);clipped(y+2,x+2,width-4,d->kind==EDIT_REGISTER?"Register   : %s":"Expression : %s",d->label);if(d->type[0])clipped(y+3,x+2,width-4,"Type       : %s",d->type);clipped(y+4,x+2,width-4,"Current    : %s",d->current);clipped(y+6,x+2,width-4,"New value  : %s_",d->input);if(d->confirm){attron(COLOR_PAIR(COLOR_COND)|A_BOLD);clipped(y+8,x+2,width-4,"Modify: %s",d->expression);clipped(y+9,x+2,width-4,"%s -> %s",d->current,d->input);clipped(y+11,x+2,width-4,"Apply? [y/N]");attroff(COLOR_PAIR(COLOR_COND)|A_BOLD);}else{attron(COLOR_PAIR(COLOR_MUTED));clipped(y+11,x+2,width-4,"Enter: Review   Esc: Cancel");attroff(COLOR_PAIR(COLOR_MUTED));}clipped(y+13,x+2,width-4,"Changes affect only the debugged process.");refresh();}
static void draw_force_return_dialog(ForceReturnDialog*d){int rows,cols;getmaxyx(stdscr,rows,cols);erase();int width=cols-4;if(width>78)width=78;if(width<48)width=cols;int height=16,x=(cols-width)/2,y=(rows-height)/2;if(y<0)y=0;if(x<0)x=0;attron(COLOR_PAIR(COLOR_INFO)|A_BOLD);mvaddch(y,x,ACS_ULCORNER);mvhline(y,x+1,ACS_HLINE,width-2);mvaddch(y,x+width-1,ACS_URCORNER);for(int r=1;r<height-1;r++){mvaddch(y+r,x,ACS_VLINE);mvaddch(y+r,x+width-1,ACS_VLINE);}mvaddch(y+height-1,x,ACS_LLCORNER);mvhline(y+height-1,x+1,ACS_HLINE,width-2);mvaddch(y+height-1,x+width-1,ACS_LRCORNER);const char*title=" Force Return ";clipped(y,x+(width-(int)strlen(title))/2,(int)strlen(title),"%s",title);attroff(COLOR_PAIR(COLOR_INFO)|A_BOLD);clipped(y+2,x+2,width-4,"Function    : %s()",d->function[0]?d->function:"<unknown>");clipped(y+3,x+2,width-4,"Return type : %s",d->type_known?d->return_type:"<unknown>");if(!d->is_void)clipped(y+5,x+2,width-4,"Return value: %s_",d->input);else clipped(y+5,x+2,width-4,"Return value: <void>");if(d->confirm){attron(COLOR_PAIR(COLOR_ERROR)|A_BOLD);clipped(y+8,x+2,width-4,"Current function will not execute remaining instructions.");clipped(y+10,x+2,width-4,"Force return%s%s? [y/N]",d->is_void?"":" value ",d->is_void?"":d->input);attroff(COLOR_PAIR(COLOR_ERROR)|A_BOLD);}else{attron(COLOR_PAIR(COLOR_MUTED));clipped(y+10,x+2,width-4,"Enter: Review   Esc: Cancel");attroff(COLOR_PAIR(COLOR_MUTED));}clipped(y+13,x+2,width-4,"Control flow of the debugged process will change.");refresh();}
static void refresh_catch_dialog(Gdb*g,CatchDialog*d){if(gdb_list_catch_candidates(g,d->event,d->input,d->candidates,GD_MAX_CATCH_CANDIDATES,&d->count))d->count=0;d->selected=0;d->top=0;}
static void draw_catch_menu(int selected){int rows,cols;getmaxyx(stdscr,rows,cols);erase();int width=58,height=18,x=(cols-width)/2,y=(rows-height)/2;if(width>cols)width=cols;if(y<0)y=0;if(x<0)x=0;attron(COLOR_PAIR(COLOR_COND)|A_BOLD);mvaddch(y,x,ACS_ULCORNER);mvhline(y,x+1,ACS_HLINE,width-2);mvaddch(y,x+width-1,ACS_URCORNER);for(int r=1;r<height-1;r++){mvaddch(y+r,x,ACS_VLINE);mvaddch(y+r,x+width-1,ACS_VLINE);}mvaddch(y+height-1,x,ACS_LLCORNER);mvhline(y+height-1,x+1,ACS_HLINE,width-2);mvaddch(y+height-1,x+width-1,ACS_LRCORNER);const char*title=" Catch Event ";clipped(y,x+(width-(int)strlen(title))/2,(int)strlen(title),"%s",title);attroff(COLOR_PAIR(COLOR_COND)|A_BOLD);const char*items[]={"Syscall","Signal","Fork","Vfork","Exec","Shared library load"};for(int i=0;i<6;i++){if(i==selected)attron(A_REVERSE);clipped(y+2+i*2,x+4,width-8,"%c %s",i==selected?'>':' ',items[i]);if(i==selected)attroff(A_REVERSE);}attron(COLOR_PAIR(COLOR_MUTED));clipped(y+height-2,x+2,width-4,"j/k: Select   Enter: Configure   Esc: Cancel");attroff(COLOR_PAIR(COLOR_MUTED));refresh();}
static void draw_catch_search(Gdb*g,CatchDialog*d){int rows,cols;getmaxyx(stdscr,rows,cols);erase();int width=cols-4;if(width>78)width=78;if(width<44)width=cols;int height=rows-4;if(height>20)height=20;if(height<10)height=rows;int x=(cols-width)/2,y=(rows-height)/2;if(x<0)x=0;if(y<0)y=0;attron(COLOR_PAIR(COLOR_COND)|A_BOLD);mvaddch(y,x,ACS_ULCORNER);mvhline(y,x+1,ACS_HLINE,width-2);mvaddch(y,x+width-1,ACS_URCORNER);for(int r=1;r<height-1;r++){mvaddch(y+r,x,ACS_VLINE);mvaddch(y+r,x+width-1,ACS_VLINE);}mvaddch(y+height-1,x,ACS_LLCORNER);mvhline(y+height-1,x+1,ACS_HLINE,width-2);mvaddch(y+height-1,x+width-1,ACS_LRCORNER);char title[64];snprintf(title,sizeof(title)," Catch %s ",d->event);clipped(y,x+(width-(int)strlen(title))/2,(int)strlen(title),"%s",title);attroff(COLOR_PAIR(COLOR_COND)|A_BOLD);clipped(y+2,x+2,width-4,"%s: %s_",!strcmp(d->event,"syscall")?"Syscall":"Signal",d->input);attron(A_BOLD);clipped(y+4,x+2,width-4,"GDB candidates");attroff(A_BOLD);int visible=height-9;if(d->selected<d->top)d->top=d->selected;if(d->selected>=d->top+visible)d->top=d->selected-visible+1;if(!d->count){attron(COLOR_PAIR(COLOR_MUTED)|A_DIM);clipped(y+6,x+3,width-6,"No matching %s names from GDB.",d->event);attroff(COLOR_PAIR(COLOR_MUTED)|A_DIM);}for(int row=0;row<visible;row++){int idx=d->top+row;if(idx>=d->count)break;if(idx==d->selected)attron(A_REVERSE);clipped(y+5+row,x+2,width-4,"%c %s",idx==d->selected?'>':' ',d->candidates[idx]);if(idx==d->selected)attroff(A_REVERSE);}attron(COLOR_PAIR(COLOR_MUTED));clipped(y+height-3,x+2,width-4,"Type to filter   j/k: Select   Enter: Set   Esc: Cancel");attroff(COLOR_PAIR(COLOR_MUTED));attron(COLOR_PAIR(!strncmp(g->message,"ERROR",5)?COLOR_ERROR:COLOR_INFO));clipped(y+height-2,x+2,width-4,"%s",g->message);attroff(COLOR_PAIR(!strncmp(g->message,"ERROR",5)?COLOR_ERROR:COLOR_INFO));refresh();}
static void draw_breaks(Gdb *g, int selected, const char *project_root) {
    int rows, cols;
    erase();
    getmaxyx(stdscr, rows, cols);
    attron(A_BOLD | COLOR_PAIR(COLOR_INFO));
    mvaddstr(0, 2, "Stop conditions");
    attroff(A_BOLD | COLOR_PAIR(COLOR_INFO));
    attron(A_BOLD);
    mvaddstr(2, 1, "ID       State  Kind         Location / Expression");
    attroff(A_BOLD);
    line_rule(3, NULL);
    if (!g->break_count) {
        attron(COLOR_PAIR(COLOR_MUTED) | A_DIM);
        mvaddstr(5, 2, "No breakpoints, catchpoints, or watchpoints");
        attroff(COLOR_PAIR(COLOR_MUTED) | A_DIM);
    }
    for (int i = 0; i < g->break_count && 4 + i < rows - 3; i++) {
        GdbBreakpoint *b = &g->breaks[i];
        bool selected_row = i == selected;
        int color = break_color(b);
        char kind = b->watchpoint ? 'W' : b->catchpoint ? 'C' : 'B';
        char id[24], detail[4096];
        snprintf(id, sizeof(id), "%c#%d", kind, b->number);
        if (b->catchpoint) {
            snprintf(detail, sizeof(detail), "%s%s%s%s",
                     b->catch_type[0] ? b->catch_type : "event",
                     b->catch_target[0] ? "  " : "",
                     b->catch_target,
                     b->number == g->stop_breakpoint ? "  [last stop]" : "");
        } else if (b->watchpoint) {
            bool last = b->number == g->stop_breakpoint &&
                        (g->watch_old[0] || g->watch_new[0]);
            snprintf(detail, sizeof(detail), "%s%s%s%s%s%s",
                     b->condition[0] ? b->condition : "<expression unavailable>",
                     last ? "  [" : "", last ? (g->watch_old[0] ? g->watch_old : "?") : "",
                     last ? " -> " : "", last ? (g->watch_new[0] ? g->watch_new : "?") : "",
                     last ? "]" : "");
        } else if (b->function_breakpoint) {
            snprintf(detail, sizeof(detail), "%s%s%s%s%s",
                     b->function[0] ? b->function : b->original_location,
                     b->pending ? "  [pending]" : "",
                     b->address[0] && !b->pending ? "  @ " : "",
                     b->address[0] && !b->pending ? b->address : "",
                     b->file[0] ? "  [source available]" : "");
        } else if (b->file[0] && b->line > 0) {
            snprintf(detail, sizeof(detail), "[%c] %s:%d%s%s",
                     project_path(b->file, project_root) ? 'P' : 'X',
                     b->file[0] ? b->file : "?", b->line,
                     b->condition[0] ? "  if " : "", b->condition);
        } else {
            snprintf(detail, sizeof(detail), "[A] *%s%s%s",
                     b->address[0] ? b->address : "<address unavailable>",
                     b->condition[0] ? "  if " : "", b->condition);
        }
        if (selected_row) attron(A_REVERSE);
        if (!b->enabled) attron(A_DIM);
        attron(COLOR_PAIR(color) | A_BOLD);
        clipped(4 + i, 1, 8, "%-7s", id);
        attroff(COLOR_PAIR(color) | A_BOLD);
        clipped(4 + i, 10, 7, "%-6s", b->enabled ? "on" : "off");
        clipped(4 + i, 17, 13, "%-12s", b->catchpoint ? b->catch_type :
                b->watchpoint ? "watch" :
                b->function_breakpoint ? "function" :
                (!b->file[0] || b->line < 1) ? "address" :
                b->condition[0] ? "conditional" : "break");
        clipped(4 + i, 30, cols - 31, "%s", detail);
        if (!b->enabled) attroff(A_DIM);
        if (selected_row) attroff(A_REVERSE);
    }
    line_rule(rows - 2, NULL);
    attron(COLOR_PAIR(COLOR_MUTED));
    clipped(rows - 1, 1, cols - 2,
            "j/k Select  d Delete  e Toggle  Enter Jump  Esc/L Back");
    attroff(COLOR_PAIR(COLOR_MUTED));
    refresh();
}
static bool prompt_text(const char*label,char*out,size_t size){int rows,cols;getmaxyx(stdscr,rows,cols);timeout(-1);echo();curs_set(1);move(rows-1,0);clrtoeol();clipped(rows-1,0,cols-1,"%s",label);int x=(int)strlen(label);move(rows-1,x);int rc=getnstr(out,(int)size-1);noecho();curs_set(0);timeout(80);return rc!=ERR&&out[0];}
static bool confirm_quit(void){char a[8]="";return prompt_text("Debuggee is active. Quit? [y/N] ",a,sizeof(a))&&(a[0]=='y'||a[0]=='Y');}
int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"Usage: gd [--args] PROGRAM [ARG...]\n");return 2;}
    int first=!strcmp(argv[1],"--args")?2:1;
    if(first>=argc){fprintf(stderr,"gd: missing program\n");return 2;}
    Gdb g;
    if(gdb_start(&g,argv[first],argc>first+1?&argv[first+1]:NULL)){fprintf(stderr,"gd: failed to start GDB: %s\n",g.message);return 1;}
    setlocale(LC_ALL,"");initscr();
    if(has_colors()){start_color();use_default_colors();init_pair(COLOR_BREAK,COLOR_CYAN,-1);init_pair(COLOR_COND,COLOR_YELLOW,-1);init_pair(COLOR_WATCH,COLOR_GREEN,-1);init_pair(COLOR_ERROR,COLOR_RED,-1);init_pair(COLOR_INFO,COLOR_CYAN,-1);init_pair(COLOR_MUTED,COLOR_WHITE,-1);}
    cbreak();noecho();keypad(stdscr,TRUE);curs_set(0);timeout(80);
    Source src={0};SourceHistory history={.index=-1};char project_root[GD_PATH_MAX]="";
    project_root_init(&g,project_root,sizeof(project_root));
    View view=VIEW_MAIN;InputMode mode=MODE_GDB;CodeView code_view=g.source_available?CODE_SOURCE:CODE_ASSEMBLY;PaneFocus focus=PANE_SOURCE;
    int cursor=0,source_col=0,top=0,hscroll=0,bsel=0,var_selected=0,var_top=0,reg_selected=0,reg_top=0,asm_selected=0,asm_top=0,stack_selected=0,vim_count=0,search_direction=1,expansion_count=0;
    static VarExpansion expansions[UI_MAX_EXPANSIONS];static VarRow var_rows[UI_MAX_VAR_ROWS];static AddressPanel address_panel;static RegisterPanel register_panel;static FunctionDialog function_dialog;static FileDialog file_dialog;static EditDialog edit_dialog;static ForceReturnDialog force_return_dialog;static CatchDialog catch_dialog;
    int open_selected=0,catch_selected=0;char last_search[1024]="";bool search_active=false,search_whole_word=false,done=false;
    while(!done){
        GdbState state_before=g.state;int line_before=g.line;char file_before[GD_PATH_MAX],pc_before[128];
        snprintf(file_before,sizeof(file_before),"%s",g.fullname);snprintf(pc_before,sizeof(pc_before),"%s",g.pc);
        g.changed=false;gdb_poll(&g,0);
        bool stopped_now=g.state==GDB_STOPPED&&(state_before!=GDB_STOPPED||g.line!=line_before||strcmp(g.fullname,file_before)||strcmp(g.pc,pc_before));
        if(stopped_now){
            expansion_count=0;var_selected=0;var_top=0;stack_selected=0;
            if(!strcmp(g.catch_event,"exec")){source_free(&src);memset(&history,0,sizeof(history));history.index=-1;project_root_init(&g,project_root,sizeof(project_root));}
            if(g.source_available){code_view=CODE_SOURCE;if(focus==PANE_REGISTERS)focus=PANE_VARIABLES;}
            else{code_view=CODE_ASSEMBLY;if(focus==PANE_VARIABLES)focus=PANE_REGISTERS;}
            asm_selected=g.current_instruction>=0?g.current_instruction:0;
        }
        if(!src.count&&g.source_available){if(!source_navigate(&src,&history,g.fullname,g.line,&cursor,&source_col,&top,&hscroll,true,true)){code_view=CODE_ASSEMBLY;if(focus==PANE_VARIABLES)focus=PANE_REGISTERS;snprintf(g.message,sizeof(g.message),"Source file unavailable; using Assembly: %.850s",g.fullname);}}
        else if(stopped_now&&g.source_available){if(!same_source_path(src.path,g.fullname)){if(!source_navigate(&src,&history,g.fullname,g.line,&cursor,&source_col,&top,&hscroll,true,true)){code_view=CODE_ASSEMBLY;if(focus==PANE_VARIABLES)focus=PANE_REGISTERS;snprintf(g.message,sizeof(g.message),"Source file unavailable; using Assembly: %.850s",g.fullname);}}else{src.debug_lines=true;cursor=g.line-1;source_col=0;hscroll=0;top=cursor;}}
        clamp_column(&src,cursor,&source_col);
        int rows,cols,source_h,variables_h,stack_h;getmaxyx(stdscr,rows,cols);layout_heights(rows,&source_h,&variables_h,&stack_h);
        if(code_view==CODE_ASSEMBLY)while(variables_h<6&&stack_h>2){variables_h++;stack_h--;}
        int text_width=cols-18;if(cursor<top)top=cursor;if(cursor>=top+source_h)top=cursor-source_h+1;if(top<0)top=0;if(source_col<hscroll)hscroll=source_col;if(text_width>0&&source_col>=hscroll+text_width)hscroll=source_col-text_width+1;if(hscroll<0)hscroll=0;
        if(code_view==CODE_SOURCE)history_update(&history,&src,cursor,source_col,top,hscroll);else history_update_assembly(&history,asm_selected,asm_top);
        int var_count=build_var_rows(&g,expansions,expansion_count,var_rows);
        if(!var_count)var_selected=0;else if(var_selected>=var_count||var_rows[var_selected].header){int start=var_selected<var_count?var_selected:0;int selectable=selectable_var(var_rows,var_count,start,1);if(selectable<0)selectable=selectable_var(var_rows,var_count,start-1,-1);var_selected=selectable>=0?selectable:0;}
        if(var_selected<var_top)var_top=var_selected;
        if(var_selected>=var_top+variables_h)var_top=var_selected-variables_h+1;
        if(reg_selected>=g.register_count)reg_selected=g.register_count?g.register_count-1:0;
        if(reg_selected<reg_top)reg_top=reg_selected;
        if(reg_selected>=reg_top+variables_h)reg_top=reg_selected-variables_h+1;
        if(asm_selected>=g.instruction_count)asm_selected=g.instruction_count?g.instruction_count-1:0;
        if(asm_selected<asm_top)asm_top=asm_selected;
        if(asm_selected>=asm_top+source_h)asm_top=asm_selected-source_h+1;
        if(stopped_now&&g.current_instruction>=0){asm_selected=g.current_instruction;asm_top=asm_selected-source_h/2;if(asm_top<0)asm_top=0;}
        if(stopped_now){if(code_view==CODE_SOURCE&&g.source_available&&src.count&&same_source_path(src.path,g.fullname))history_push(&history,src.path,cursor,source_col,top,hscroll,true);else if(g.pc[0])history_push_assembly(&history,g.pc,g.function,asm_selected,asm_top);}
        if(stack_selected>=g.frame_count)stack_selected=g.frame_count?g.frame_count-1:0;
        if(view==VIEW_MAIN){draw_main(&g,&src,cursor,source_col,top,hscroll,mode,code_view,focus,var_rows,var_count,var_top,var_selected,reg_selected,reg_top,asm_selected,asm_top,stack_selected,last_search,search_whole_word,project_root,history.index,history.count);refresh();}else if(view==VIEW_HELP){draw_main(&g,&src,cursor,source_col,top,hscroll,mode,code_view,focus,var_rows,var_count,var_top,var_selected,reg_selected,reg_top,asm_selected,asm_top,stack_selected,last_search,search_whole_word,project_root,history.index,history.count);wnoutrefresh(stdscr);draw_help_popup();doupdate();}else if(view==VIEW_OUTPUT)draw_output(&g);else if(view==VIEW_ADDRESS)draw_address(&address_panel);else if(view==VIEW_REGISTER)draw_register_panel(&register_panel);else if(view==VIEW_FUNCTIONS)draw_function_dialog(&g,&function_dialog);else if(view==VIEW_OPEN_MENU)draw_open_menu(open_selected);else if(view==VIEW_OPEN_FILES)draw_file_dialog(&g,&file_dialog);else if(view==VIEW_EDIT_VALUE)draw_edit_dialog(&edit_dialog);else if(view==VIEW_FORCE_RETURN)draw_force_return_dialog(&force_return_dialog);else if(view==VIEW_CATCH_MENU)draw_catch_menu(catch_selected);else if(view==VIEW_CATCH_SEARCH)draw_catch_search(&g,&catch_dialog);else draw_breaks(&g,bsel,project_root);
        int ch=getch();
        if(ch==ERR)continue;
        if(view==VIEW_EDIT_VALUE){
            if(edit_dialog.confirm){
                if(ch=='y'||ch=='Y'){char actual[GD_TEXT_MAX]="",before[GD_TEXT_MAX],target[512];snprintf(before,sizeof(before),"%s",edit_dialog.current);snprintf(target,sizeof(target),"%s",edit_dialog.expression);int rc=edit_dialog.kind==EDIT_REGISTER?gdb_assign_register(&g,edit_dialog.label,edit_dialog.input,actual,sizeof(actual)):gdb_assign_expression(&g,edit_dialog.expression,edit_dialog.input,actual,sizeof(actual));view=VIEW_MAIN;if(!rc){if(edit_dialog.kind==EDIT_VARIABLE)refresh_expansions(&g,expansions,&expansion_count);snprintf(g.message,sizeof(g.message),"Modified: %.300s  %.300s -> %.300s",target,before,actual);}}
                else{view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Value modification cancelled");}
            }else if(ch==27){view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Value modification cancelled");}
            else if(ch==KEY_BACKSPACE||ch==127||ch==8){size_t len=strlen(edit_dialog.input);if(len)edit_dialog.input[len-1]='\0';}
            else if(ch=='\n'||ch==KEY_ENTER){if(edit_dialog.input[0])edit_dialog.confirm=true;else snprintf(g.message,sizeof(g.message),"New value is required");}
            else if(ch>=32&&ch<127){size_t len=strlen(edit_dialog.input);if(len+1<sizeof(edit_dialog.input)){edit_dialog.input[len]=(char)ch;edit_dialog.input[len+1]='\0';}}
            continue;
        }
        if(view==VIEW_FORCE_RETURN){
            if(force_return_dialog.confirm){
                if(ch=='y'||ch=='Y'){char function[256],value[GD_TEXT_MAX];snprintf(function,sizeof(function),"%s",force_return_dialog.function);snprintf(value,sizeof(value),"%s",force_return_dialog.input);int rc=gdb_force_return(&g,force_return_dialog.is_void?NULL:value);bool raw_result=!rc&&strstr(g.message,"raw System V")!=NULL;view=VIEW_MAIN;if(!rc){expansion_count=0;var_selected=0;var_top=0;stack_selected=0;asm_selected=g.current_instruction>=0?g.current_instruction:0;asm_top=0;focus=PANE_SOURCE;if(g.source_available&&source_navigate(&src,&history,g.fullname,g.line,&cursor,&source_col,&top,&hscroll,true,true))code_view=CODE_SOURCE;else{code_view=CODE_ASSEMBLY;if(g.pc[0])history_push_assembly(&history,g.pc,g.function,asm_selected,asm_top);}snprintf(g.message,sizeof(g.message),"Forced return: %.250s()%s%.650s%s",function,value[0]?" -> ":"",value,raw_result?" [raw rax]":"");}}
                else{view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Force return cancelled");}
            }else if(ch==27){view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Force return cancelled");}
            else if(ch==KEY_BACKSPACE||ch==127||ch==8){size_t len=strlen(force_return_dialog.input);if(len)force_return_dialog.input[len-1]='\0';}
            else if(ch=='\n'||ch==KEY_ENTER){if(!force_return_dialog.is_void&&force_return_dialog.type_known&&!force_return_dialog.input[0])snprintf(g.message,sizeof(g.message),"Return value is required for %s",force_return_dialog.return_type);else force_return_dialog.confirm=true;}
            else if(ch>=32&&ch<127&&!force_return_dialog.is_void){size_t len=strlen(force_return_dialog.input);if(len+1<sizeof(force_return_dialog.input)){force_return_dialog.input[len]=(char)ch;force_return_dialog.input[len+1]='\0';}}
            continue;
        }
        if(view==VIEW_CATCH_MENU){
            const char*events[]={"syscall","signal","fork","vfork","exec","load"};
            if(ch==27){view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Catchpoint cancelled");}
            else if((ch=='j'||ch==KEY_DOWN)&&catch_selected<5)catch_selected++;
            else if((ch=='k'||ch==KEY_UP)&&catch_selected>0)catch_selected--;
            else if(ch=='\n'||ch==KEY_ENTER){const char*event=events[catch_selected];if(catch_selected<2){memset(&catch_dialog,0,sizeof(catch_dialog));snprintf(catch_dialog.event,sizeof(catch_dialog.event),"%s",event);refresh_catch_dialog(&g,&catch_dialog);view=VIEW_CATCH_SEARCH;}else{if(!gdb_set_catchpoint(&g,event,NULL))view=VIEW_MAIN;}}
            continue;
        }
        if(view==VIEW_CATCH_SEARCH){
            if(ch==27){view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Catchpoint cancelled");}
            else if((ch=='j'||ch==KEY_DOWN)&&catch_dialog.selected+1<catch_dialog.count)catch_dialog.selected++;
            else if((ch=='k'||ch==KEY_UP)&&catch_dialog.selected>0)catch_dialog.selected--;
            else if(ch==KEY_BACKSPACE||ch==127||ch==8){size_t len=strlen(catch_dialog.input);if(len)catch_dialog.input[len-1]='\0';refresh_catch_dialog(&g,&catch_dialog);}
            else if(ch=='\n'||ch==KEY_ENTER){char target[128]="";if(catch_dialog.count)snprintf(target,sizeof(target),"%s",catch_dialog.candidates[catch_dialog.selected]);else snprintf(target,sizeof(target),"%s",catch_dialog.input);if(!target[0])snprintf(g.message,sizeof(g.message),"Select or enter a %s name",catch_dialog.event);else if(!gdb_set_catchpoint(&g,catch_dialog.event,target))view=VIEW_MAIN;}
            else if(ch>=32&&ch<127){size_t len=strlen(catch_dialog.input);if(len+1<sizeof(catch_dialog.input)){catch_dialog.input[len]=(char)ch;catch_dialog.input[len+1]='\0';refresh_catch_dialog(&g,&catch_dialog);}}
            continue;
        }
        if(view==VIEW_OPEN_MENU){
            if(ch==27){view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Open cancelled");}
            else if((ch=='j'||ch==KEY_DOWN||ch=='k'||ch==KEY_UP))open_selected=1-open_selected;
            else if(ch=='\n'||ch==KEY_ENTER){if(open_selected==0){memset(&function_dialog,0,sizeof(function_dialog));function_dialog.action=FUNCTION_OPEN;refresh_function_dialog(&g,&function_dialog);view=VIEW_FUNCTIONS;}else{load_file_dialog(&g,project_root,&file_dialog);view=VIEW_OPEN_FILES;}}
            continue;
        }
        if(view==VIEW_OPEN_FILES){
            if(ch==27){view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Open file cancelled");}
            else if((ch=='j'||ch==KEY_DOWN)&&file_dialog.selected+1<file_dialog.count)file_dialog.selected++;
            else if((ch=='k'||ch==KEY_UP)&&file_dialog.selected>0)file_dialog.selected--;
            else if(ch==KEY_BACKSPACE||ch==127||ch==8){size_t len=strlen(file_dialog.input);if(len)file_dialog.input[len-1]='\0';filter_file_dialog(&file_dialog);}
            else if((ch=='\n'||ch==KEY_ENTER)&&file_dialog.count){SourceFileCandidate*f=&file_dialog.all[file_dialog.matches[file_dialog.selected]];if(source_navigate(&src,&history,f->path,1,&cursor,&source_col,&top,&hscroll,f->debug_lines,true)){focus=PANE_SOURCE;code_view=CODE_SOURCE;view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Opened %s%s",f->display,f->debug_lines?"":" [REFERENCE ONLY]");}else snprintf(g.message,sizeof(g.message),"ERROR: source unavailable: %.900s",f->path);}
            else if(ch>=32&&ch<127){size_t len=strlen(file_dialog.input);if(len+1<sizeof(file_dialog.input)){file_dialog.input[len]=(char)ch;file_dialog.input[len+1]='\0';filter_file_dialog(&file_dialog);}}
            continue;
        }
        if(view==VIEW_FUNCTIONS){
            if(ch==27){view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),function_dialog.action==FUNCTION_OPEN?"Open function cancelled":"Function breakpoint cancelled");}
            else if((ch=='j'||ch==KEY_DOWN)&&function_dialog.selected+1<function_dialog.count)function_dialog.selected++;
            else if((ch=='k'||ch==KEY_UP)&&function_dialog.selected>0)function_dialog.selected--;
            else if(ch==KEY_BACKSPACE||ch==127||ch==8){size_t len=strlen(function_dialog.input);if(len)function_dialog.input[len-1]='\0';refresh_function_dialog(&g,&function_dialog);}
            else if(ch=='\n'||ch==KEY_ENTER){char target[512]="";int chosen=-1;for(int i=0;i<function_dialog.count;i++)if(function_dialog.input[0]&&!strcmp(function_dialog.input,function_dialog.candidates[i].name)){chosen=i;snprintf(target,sizeof(target),"%s",function_dialog.input);break;}if(chosen<0&&function_dialog.count){chosen=function_dialog.selected;snprintf(target,sizeof(target),"%s",function_dialog.candidates[chosen].name);}if(!target[0]&&function_dialog.input[0])snprintf(target,sizeof(target),"%s",function_dialog.input);if(!target[0])snprintf(g.message,sizeof(g.message),"No function selected");else if(function_dialog.action==FUNCTION_BREAKPOINT){if(gdb_set_function_breakpoint(&g,target))snprintf(g.message,sizeof(g.message),"Function not found: %.900s",target);else view=VIEW_MAIN;}else if(chosen<0)snprintf(g.message,sizeof(g.message),"Function not found: %.900s",target);else{GdbFunction*f=&function_dialog.candidates[chosen];if(f->source[0]&&f->line>0&&source_navigate(&src,&history,f->source,f->line,&cursor,&source_col,&top,&hscroll,true,true)){focus=PANE_SOURCE;code_view=CODE_SOURCE;view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Opened function %s at %s:%d",f->name,path_name(f->source),f->line);}else if(f->address[0]&&!gdb_disassemble_at(&g,f->address)){focus=PANE_SOURCE;code_view=CODE_ASSEMBLY;asm_selected=g.current_instruction>=0?g.current_instruction:0;asm_top=0;history_push_assembly(&history,f->address,f->name,asm_selected,asm_top);view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Opened function %s in Disassembly",f->name);}else snprintf(g.message,sizeof(g.message),"Source and disassembly unavailable: %.800s",f->name);}}
            else if(ch>=32&&ch<127){size_t len=strlen(function_dialog.input);if(len+1<sizeof(function_dialog.input)){function_dialog.input[len]=(char)ch;function_dialog.input[len+1]='\0';refresh_function_dialog(&g,&function_dialog);}}
            continue;
        }
        if(view==VIEW_HELP){if(ch==27||ch=='?'||ch=='h'||ch=='q')view=VIEW_MAIN;continue;}
        if(view!=VIEW_MAIN){if(ch==27||(view==VIEW_OUTPUT&&ch=='O')||(view==VIEW_BREAKS&&ch=='L')||(view==VIEW_ADDRESS&&(ch=='a'||ch=='\n'||ch==KEY_ENTER))||(view==VIEW_REGISTER&&(ch=='p'||ch=='\n'||ch==KEY_ENTER))){view=VIEW_MAIN;continue;}if(view==VIEW_BREAKS&&g.break_count){if(ch=='j'||ch==KEY_DOWN){if(bsel<g.break_count-1)bsel++;}else if(ch=='k'||ch==KEY_UP){if(bsel>0)bsel--;}else if(ch=='d'){gdb_delete_breakpoint(&g,g.breaks[bsel].number);if(bsel>=g.break_count)bsel=g.break_count-1;if(bsel<0)bsel=0;}else if(ch=='e')gdb_enable_breakpoint(&g,g.breaks[bsel].number,!g.breaks[bsel].enabled);else if(ch=='\n'||ch==KEY_ENTER){GdbBreakpoint*b=&g.breaks[bsel];if(b->catchpoint)snprintf(g.message,sizeof(g.message),"Catchpoint C#%d stops on %s%s%s",b->number,b->catch_type,b->catch_target[0]?" ":"",b->catch_target);else if(b->watchpoint)snprintf(g.message,sizeof(g.message),"Watchpoints have no browse location");else if(b->file[0]&&b->line>0&&source_navigate(&src,&history,b->file,b->line,&cursor,&source_col,&top,&hscroll,true,true)){focus=PANE_SOURCE;code_view=CODE_SOURCE;view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Breakpoint B#%d: %s:%d",b->number,path_name(b->file),b->line);}else if(b->function_breakpoint&&!b->pending&&!gdb_disassemble_at(&g,b->address)){focus=PANE_SOURCE;code_view=CODE_ASSEMBLY;asm_selected=0;asm_top=0;history_push_assembly(&history,b->address,b->function,asm_selected,asm_top);view=VIEW_MAIN;snprintf(g.message,sizeof(g.message),"Function %s @ %s",b->function[0]?b->function:b->original_location,b->address);}else snprintf(g.message,sizeof(g.message),b->pending?"Function breakpoint is pending":"This stop condition has no browse location");}}continue;}
        if(ch=='\t'){focus=cycle_focus(focus,code_view,1);snprintf(g.message,sizeof(g.message),"Focus: %s",focus_name(focus));continue;}
        if(ch==KEY_BTAB){focus=cycle_focus(focus,code_view,-1);snprintf(g.message,sizeof(g.message),"Focus: %s",focus_name(focus));continue;}
        if(ch==KEY_F(2)){mode=mode==MODE_VIM?MODE_GDB:MODE_VIM;search_active=false;vim_count=0;snprintf(g.message,sizeof(g.message),"%s mode",mode==MODE_VIM?"VIM navigation":"GDB control");continue;}
        if(ch=='?'&&mode==MODE_GDB){view=VIEW_HELP;continue;}
        if(ch=='C'){catch_selected=0;view=VIEW_CATCH_MENU;continue;}
        if(ch=='F'){memset(&function_dialog,0,sizeof(function_dialog));function_dialog.action=FUNCTION_BREAKPOINT;refresh_function_dialog(&g,&function_dialog);view=VIEW_FUNCTIONS;continue;}
        if(ch=='R'){if(g.state!=GDB_STOPPED)snprintf(g.message,sizeof(g.message),"Stop the program before forcing a return");else{memset(&force_return_dialog,0,sizeof(force_return_dialog));snprintf(force_return_dialog.function,sizeof(force_return_dialog.function),"%s",g.function[0]?g.function:"<unknown>");force_return_dialog.type_known=!gdb_current_return_type(&g,force_return_dialog.return_type,sizeof(force_return_dialog.return_type),&force_return_dialog.is_void);snprintf(g.message,sizeof(g.message),"Force return from %s()",force_return_dialog.function);view=VIEW_FORCE_RETURN;}continue;}
        if(ch=='o'){open_selected=0;view=VIEW_OPEN_MENU;continue;}
        if(mode==MODE_GDB&&ch=='e'){if(g.source_available&&g.fullname[0]&&source_navigate(&src,&history,g.fullname,g.line,&cursor,&source_col,&top,&hscroll,true,true)){code_view=CODE_SOURCE;focus=PANE_SOURCE;snprintf(g.message,sizeof(g.message),"Execution position: %s:%d",path_name(g.fullname),g.line);}else if(g.pc[0]&&!gdb_disassemble_at(&g,g.pc)){code_view=CODE_ASSEMBLY;focus=PANE_SOURCE;asm_selected=g.current_instruction>=0?g.current_instruction:0;asm_top=asm_selected>source_h/2?asm_selected-source_h/2:0;history_push_assembly(&history,g.pc,g.function,asm_selected,asm_top);snprintf(g.message,sizeof(g.message),"Execution position: %s @ %s",g.function[0]?g.function:"<unknown>",g.pc);}else snprintf(g.message,sizeof(g.message),"Execution position is unavailable");continue;}
        if(ch=='d'){if(code_view==CODE_SOURCE){if(g.pc[0]&&!gdb_disassemble_at(&g,g.pc)){code_view=CODE_ASSEMBLY;asm_selected=g.current_instruction>=0?g.current_instruction:0;history_push_assembly(&history,g.pc,g.function,asm_selected,asm_top);}if(focus==PANE_VARIABLES)focus=PANE_REGISTERS;snprintf(g.message,sizeof(g.message),"Assembly view");}else if(src.count){code_view=CODE_SOURCE;if(focus==PANE_REGISTERS)focus=PANE_VARIABLES;history_push(&history,src.path,cursor,source_col,top,hscroll,src.debug_lines);snprintf(g.message,sizeof(g.message),src.debug_lines?"Source view":"Source view (REFERENCE ONLY)");}else snprintf(g.message,sizeof(g.message),"No source file is available; remaining in Assembly view");continue;}
        if(code_view==CODE_ASSEMBLY&&(ch=='i'||ch=='I')){if(g.state!=GDB_STOPPED)snprintf(g.message,sizeof(g.message),"Stop the program before instruction stepping");else if(ch=='i')gdb_step_instruction(&g);else gdb_next_instruction(&g);continue;}
        if(ch=='['||ch==']'||ch==15){int direction=ch==']'?1:-1;if(history_move(&history,&src,&g,direction,&code_view,&cursor,&source_col,&top,&hscroll,&asm_selected,&asm_top)){focus=PANE_SOURCE;if(code_view==CODE_SOURCE)snprintf(g.message,sizeof(g.message),"View history %d/%d: %s:%d",history.index+1,history.count,path_name(src.path),cursor+1);else snprintf(g.message,sizeof(g.message),"View history %d/%d: Disassembly %s",history.index+1,history.count,g.disassembly_function[0]?g.disassembly_function:g.pc);}else snprintf(g.message,sizeof(g.message),direction<0?"No older view location":"No newer view location");continue;}
        bool stopped=g.state==GDB_STOPPED;char input[1024];
        if(focus==PANE_VARIABLES){
            if(ch=='j'||ch==KEY_DOWN){int next=selectable_var(var_rows,var_count,var_selected+1,1);if(next>=0)var_selected=next;}
            else if(ch=='k'||ch==KEY_UP){int next=selectable_var(var_rows,var_count,var_selected-1,-1);if(next>=0)var_selected=next;}
            else if((ch=='\n'||ch==KEY_ENTER)&&var_count){if(stopped)toggle_expansion(&g,expansions,&expansion_count,var_rows[var_selected].expression);else snprintf(g.message,sizeof(g.message),"Stop the program before expanding variables");}
            else if(ch=='p'&&var_count){if(stopped){char value[GD_TEXT_MAX];gdb_print(&g,var_rows[var_selected].expression,value,sizeof(value));}else snprintf(g.message,sizeof(g.message),"Stop the program before evaluating variables");}
            else if(ch=='a'&&var_count){if(stopped){if(!build_address_panel(&g,var_rows,var_count,var_selected,&address_panel))view=VIEW_ADDRESS;}else snprintf(g.message,sizeof(g.message),"Stop the program before inspecting addresses");}
            else if(ch=='E'&&var_count){if(!stopped)snprintf(g.message,sizeof(g.message),"Stop the program before modifying a value");else if(var_rows[var_selected].header)snprintf(g.message,sizeof(g.message),"Select a variable to modify");else if(strstr(var_rows[var_selected].value,"<optimized out>"))snprintf(g.message,sizeof(g.message),"Cannot modify variable: value is optimized out.");else{memset(&edit_dialog,0,sizeof(edit_dialog));edit_dialog.kind=EDIT_VARIABLE;snprintf(edit_dialog.expression,sizeof(edit_dialog.expression),"%s",var_rows[var_selected].expression);snprintf(edit_dialog.label,sizeof(edit_dialog.label),"%s",var_rows[var_selected].expression);snprintf(edit_dialog.type,sizeof(edit_dialog.type),"%s",var_rows[var_selected].type);snprintf(edit_dialog.current,sizeof(edit_dialog.current),"%s",var_rows[var_selected].value);view=VIEW_EDIT_VALUE;}}
            else if(ch=='w'&&var_count){if(stopped)gdb_watch(&g,var_rows[var_selected].expression);else snprintf(g.message,sizeof(g.message),"Stop the program before setting a watchpoint");}
            else if(ch=='B'&&var_count){if(!src.path[0])snprintf(g.message,sizeof(g.message),"No source location for conditional breakpoint");else if(!src.debug_lines)snprintf(g.message,sizeof(g.message),"Source line breakpoint unavailable: no debug line information.");else{input[0]='\0';char label[700];snprintf(label,sizeof(label),"Condition for %s (e.g. == 5): ",var_rows[var_selected].expression);if(prompt_text(label,input,sizeof(input))){char condition[1536];if(strchr("=!<>&|",input[0]))snprintf(condition,sizeof(condition),"(%.*s) %.*s",500,var_rows[var_selected].expression,900,input);else snprintf(condition,sizeof(condition),"%s",input);int oldmax=newest_break_number(&g),requested=cursor+1;if(!gdb_set_cond_breakpoint(&g,src.path,requested,condition)){int actual=new_break_line(&g,src.path,oldmax);if(actual>0)cursor=actual-1;}}}}
            else if(ch=='q'){if(g.state==GDB_RUNNING||g.state==GDB_STOPPED){if(confirm_quit())done=true;}else done=true;}
            else snprintf(g.message,sizeof(g.message),"Variables: Enter expand, p value, a address, w watch, B conditional breakpoint");
            continue;
        }
        if(focus==PANE_REGISTERS){
            if((ch=='j'||ch==KEY_DOWN)&&reg_selected+1<g.register_count)reg_selected++;
            else if((ch=='k'||ch==KEY_UP)&&reg_selected>0)reg_selected--;
            else if(ch=='p'&&g.register_count){build_register_panel(&g.registers[reg_selected],&register_panel);view=VIEW_REGISTER;snprintf(g.message,sizeof(g.message),"Register details: %s",g.registers[reg_selected].name);}
            else if(ch=='E'&&g.register_count){if(!stopped)snprintf(g.message,sizeof(g.message),"Stop the program before modifying a register");else{memset(&edit_dialog,0,sizeof(edit_dialog));edit_dialog.kind=EDIT_REGISTER;snprintf(edit_dialog.label,sizeof(edit_dialog.label),"%s",g.registers[reg_selected].name);snprintf(edit_dialog.expression,sizeof(edit_dialog.expression),"$%s",g.registers[reg_selected].name);snprintf(edit_dialog.current,sizeof(edit_dialog.current),"%s",g.registers[reg_selected].value);view=VIEW_EDIT_VALUE;}}
            else if(ch=='a')snprintf(g.message,sizeof(g.message),"Register memory view is planned after the Assembly MVP");
            else if(ch=='q'){if(g.state==GDB_RUNNING||g.state==GDB_STOPPED){if(confirm_quit())done=true;}else done=true;}
            else snprintf(g.message,sizeof(g.message),"Registers: j/k select, p details, Tab pane");
            continue;
        }
        if(focus==PANE_STACK){
            if((ch=='j'||ch==KEY_DOWN)&&stack_selected+1<g.frame_count)stack_selected++;
            else if((ch=='k'||ch==KEY_UP)&&stack_selected>0)stack_selected--;
            else if((ch=='\n'||ch==KEY_ENTER)&&g.frame_count){GdbFrame frame=g.frames[stack_selected];if(!stopped)snprintf(g.message,sizeof(g.message),"Stop the program before selecting a stack frame");else if(!gdb_select_frame(&g,frame.level)){expansion_count=0;var_selected=0;var_top=0;asm_selected=g.current_instruction>=0?g.current_instruction:0;focus=PANE_SOURCE;if(g.source_available){if(source_navigate(&src,&history,g.fullname,g.line,&cursor,&source_col,&top,&hscroll,true,true)){code_view=CODE_SOURCE;snprintf(g.message,sizeof(g.message),"Frame #%d: %s:%d [%s]",frame.level,path_name(g.fullname),g.line,project_path(g.fullname,project_root)?"project":"external");}else snprintf(g.message,sizeof(g.message),"ERROR: source unavailable: %.900s",g.fullname);}else{code_view=CODE_ASSEMBLY;history_push_assembly(&history,g.pc,g.function,asm_selected,asm_top);snprintf(g.message,sizeof(g.message),"Frame #%d: %s @ %s",frame.level,g.function[0]?g.function:"<unknown>",g.pc[0]?g.pc:"<address unavailable>");}}}
            else if(ch=='q'){if(g.state==GDB_RUNNING||g.state==GDB_STOPPED){if(confirm_quit())done=true;}else done=true;}
            else snprintf(g.message,sizeof(g.message),"Stack: j/k select, Enter selects frame and opens its source");
            continue;
        }
        if(code_view==CODE_ASSEMBLY){
            if((ch=='j'||ch==KEY_DOWN)&&asm_selected+1<g.instruction_count)asm_selected++;
            else if((ch=='k'||ch==KEY_UP)&&asm_selected>0)asm_selected--;
            else if(ch==KEY_NPAGE){asm_selected+=source_h>1?source_h-1:1;if(asm_selected>=g.instruction_count)asm_selected=g.instruction_count?g.instruction_count-1:0;}
            else if(ch==KEY_PPAGE){asm_selected-=source_h>1?source_h-1:1;if(asm_selected<0)asm_selected=0;}
            else if(ch=='b'&&g.instruction_count){if(stopped)gdb_toggle_address_breakpoint(&g,g.instructions[asm_selected].address);else snprintf(g.message,sizeof(g.message),"Stop the program before setting an address breakpoint");}
            else if(ch=='B')snprintf(g.message,sizeof(g.message),"Conditional breakpoints require a source location; use F for a function");
            else if((ch=='\n'||ch==KEY_ENTER)&&g.instruction_count){GdbInstruction*ins=&g.instructions[asm_selected];if(ins->call&&ins->call_target[0]){char target[256];snprintf(target,sizeof(target),"%s",ins->call_target);if(!gdb_disassemble_at(&g,target)){asm_selected=0;asm_top=0;history_push_assembly(&history,target,target,asm_selected,asm_top);snprintf(g.message,sizeof(g.message),"Opened call target %s",target);}else snprintf(g.message,sizeof(g.message),"Call target unavailable: %s",target);}else if(ins->call)snprintf(g.message,sizeof(g.message),"Indirect call target is unavailable");else snprintf(g.message,sizeof(g.message),"Selected instruction: %s",ins->address);}
            else if(ch=='r')gdb_run(&g);
            else if(ch=='c'&&stopped)gdb_continue(&g);
            else if(ch=='f'&&stopped)gdb_finish(&g);
            else if(ch=='L'){gdb_refresh_breakpoints(&g);view=VIEW_BREAKS;}
            else if(ch=='O')view=VIEW_OUTPUT;
            else if(ch=='h')view=VIEW_HELP;
            else if(ch=='q'){if(g.state==GDB_RUNNING||g.state==GDB_STOPPED){if(confirm_quit())done=true;}else done=true;}
            else snprintf(g.message,sizeof(g.message),"ASM: j/k browse, i/I step, b address breakpoint, B function breakpoint");
            continue;
        }
        if(mode==MODE_VIM){
            if(isdigit(ch)&&(ch!='0'||vim_count)){vim_count=vim_count*10+(ch-'0');if(vim_count>9999)vim_count=9999;snprintf(g.message,sizeof(g.message),"count: %d",vim_count);continue;}
            int count=vim_count?vim_count:1;vim_count=0;
            if(ch=='h'||ch==KEY_LEFT){source_col-=count;clamp_column(&src,cursor,&source_col);}
            else if(ch=='l'||ch==KEY_RIGHT){source_col+=count;clamp_column(&src,cursor,&source_col);}
            else if(ch=='j'||ch==KEY_DOWN){cursor+=count;if(cursor>=src.count)cursor=src.count?src.count-1:0;clamp_column(&src,cursor,&source_col);}
            else if(ch=='k'||ch==KEY_UP){cursor-=count;if(cursor<0)cursor=0;clamp_column(&src,cursor,&source_col);}
            else if(ch=='w'){for(int i=0;i<count;i++)word_motion(&src,&cursor,&source_col,1,false);}
            else if(ch=='b'){for(int i=0;i<count;i++)word_motion(&src,&cursor,&source_col,-1,false);}
            else if(ch=='e'){for(int i=0;i<count;i++)word_motion(&src,&cursor,&source_col,1,true);}
            else if(ch=='0')source_col=0;
            else if(ch=='^'){source_col=0;if(src.count)while(src.lines[cursor][source_col]&&isspace((unsigned char)src.lines[cursor][source_col]))source_col++;clamp_column(&src,cursor,&source_col);}
            else if(ch=='$'){source_col=src.count?(int)strlen(src.lines[cursor])-1:0;clamp_column(&src,cursor,&source_col);}
            else if(ch=='G'){cursor=src.count?src.count-1:0;source_col=0;}
            else if(ch=='H'){cursor=top;clamp_column(&src,cursor,&source_col);}
            else if(ch=='M'){cursor=top+source_h/2;if(cursor>=src.count)cursor=src.count-1;clamp_column(&src,cursor,&source_col);}
            else if(ch=='L'){cursor=top+source_h-1;if(cursor>=src.count)cursor=src.count-1;clamp_column(&src,cursor,&source_col);}
            else if(ch==KEY_NPAGE||ch==6){cursor+=count*(source_h>1?source_h-1:1);if(cursor>=src.count)cursor=src.count-1;clamp_column(&src,cursor,&source_col);}
            else if(ch==KEY_PPAGE||ch==2){cursor-=count*(source_h>1?source_h-1:1);if(cursor<0)cursor=0;clamp_column(&src,cursor,&source_col);}
            else if(ch==4){cursor+=count*(source_h/2>0?source_h/2:1);if(cursor>=src.count)cursor=src.count-1;clamp_column(&src,cursor,&source_col);}
            else if(ch==21){cursor-=count*(source_h/2>0?source_h/2:1);if(cursor<0)cursor=0;clamp_column(&src,cursor,&source_col);}
            else if(ch=='{'||ch=='}'){for(int i=0;i<count;i++)paragraph_motion(&src,&cursor,ch=='}'?1:-1);source_col=0;}
            else if(ch=='%'){if(!match_bracket(&src,&cursor,&source_col))snprintf(g.message,sizeof(g.message),"No matching bracket");}
            else if(ch=='g'){
                timeout(-1);int next=getch();timeout(80);
                if(next=='g'){cursor=0;source_col=0;}
                else if(next=='d'||next=='D'){char word[256]="";word_under_cursor(&src,cursor,source_col,word,sizeof(word),NULL);if(word[0]){snprintf(last_search,sizeof(last_search),"%s",word);search_direction=1;search_whole_word=true;}if(goto_declaration(&src,&cursor,&source_col,next=='D'))snprintf(g.message,sizeof(g.message),"%s -> %s at line %d; n/N repeat",next=='D'?"gD":"gd",word,cursor+1);else snprintf(g.message,sizeof(g.message),"Declaration not found: %s",word[0]?word:"<no identifier>");}
                else snprintf(g.message,sizeof(g.message),"Unknown g command");
            }
            else if(ch=='z'){
                timeout(-1);int next=getch();timeout(80);
                if(next=='z')top=cursor-source_h/2;else if(next=='t')top=cursor;else if(next=='b')top=cursor-source_h+1;else snprintf(g.message,sizeof(g.message),"Unknown z command");if(top<0)top=0;
            }
            else if(ch=='/'||ch=='?'){input[0]=0;if(prompt_text(ch=='/'?"Search forward: ":"Search backward: ",input,sizeof(input))){snprintf(last_search,sizeof(last_search),"%s",input);search_direction=ch=='/'?1:-1;search_whole_word=false;int found_line=cursor,found_col=source_col;if(text_search_position(&src,&found_line,&found_col,last_search,search_direction,search_whole_word)){cursor=found_line;source_col=found_col;snprintf(g.message,sizeof(g.message),"Search '%.*s': line %d",700,last_search,cursor+1);}else snprintf(g.message,sizeof(g.message),"Search text not found: %.*s",700,last_search);}}
            else if(ch=='n'||ch=='N'){if(last_search[0]){int direction=ch=='n'?search_direction:-search_direction,found_line=cursor,found_col=source_col;if(text_search_position(&src,&found_line,&found_col,last_search,direction,search_whole_word)){cursor=found_line;source_col=found_col;snprintf(g.message,sizeof(g.message),"Search '%.*s': line %d",700,last_search,cursor+1);}else snprintf(g.message,sizeof(g.message),"Search text not found: %.*s",700,last_search);}else snprintf(g.message,sizeof(g.message),"No previous search");}
            else if(ch=='*'||ch=='#'){char word[256];if(word_under_cursor(&src,cursor,source_col,word,sizeof(word),NULL)){snprintf(last_search,sizeof(last_search),"%s",word);search_direction=ch=='*'?1:-1;search_whole_word=true;if(search_word(&src,&cursor,&source_col,last_search,search_direction))snprintf(g.message,sizeof(g.message),"Search word '%s': line %d",last_search,cursor+1);else snprintf(g.message,sizeof(g.message),"Word not found: %s",last_search);}}
            else if(ch=='q'){if(g.state==GDB_RUNNING||g.state==GDB_STOPPED){if(confirm_quit())done=true;}else done=true;}
            else snprintf(g.message,sizeof(g.message),"VIM mode: Tab switches to GDB controls");
            continue;
        }
        if(search_active&&(ch=='n'||ch=='N')){int found=source_search(&src,cursor,last_search,ch=='n'?1:-1);if(found>=0){cursor=found;source_col=text_column(src.lines[cursor],last_search,ch=='n'?1:-1);snprintf(g.message,sizeof(g.message),"Search '%.*s': line %d  [n next / N previous / Esc end]",500,last_search,found+1);}else snprintf(g.message,sizeof(g.message),"Search text not found: %.*s  [Esc end]",700,last_search);continue;}if(search_active&&ch==27){search_active=false;snprintf(g.message,sizeof(g.message),"Search ended; n is debugger next");continue;}if(search_active&&ch!='/'&&ch!='j'&&ch!='k'&&ch!=KEY_UP&&ch!=KEY_DOWN&&ch!=KEY_NPAGE&&ch!=KEY_PPAGE&&ch!=4&&ch!=21&&ch!='g'&&ch!='G')search_active=false;if(ch=='j'||ch==KEY_DOWN){if(cursor+1<src.count)cursor++;}else if(ch=='k'||ch==KEY_UP){if(cursor>0)cursor--;}else if(ch==KEY_NPAGE){if(src.count){cursor+=source_h>1?source_h-1:1;if(cursor>=src.count)cursor=src.count-1;}}else if(ch==KEY_PPAGE){if(src.count){cursor-=source_h>1?source_h-1:1;if(cursor<0)cursor=0;}}else if(ch==4){if(src.count){cursor+=source_h/2>0?source_h/2:1;if(cursor>=src.count)cursor=src.count-1;}}else if(ch==21){if(src.count){cursor-=source_h/2>0?source_h/2:1;if(cursor<0)cursor=0;}}else if(ch=='g')cursor=0;else if(ch=='G')cursor=src.count?src.count-1:0;else if(ch=='/'){input[0]=0;if(prompt_text("Search: ",input,sizeof(input))){snprintf(last_search,sizeof(last_search),"%s",input);search_active=true;search_whole_word=false;int found=source_search(&src,cursor,last_search,1);if(found>=0){cursor=found;source_col=text_column(src.lines[cursor],last_search,1);snprintf(g.message,sizeof(g.message),"Search '%.*s': line %d  [n next / N previous / Esc end]",500,last_search,found+1);}else snprintf(g.message,sizeof(g.message),"Search text not found: %.*s  [n/N retry / Esc end]",650,last_search);}}else if(ch=='r')gdb_run(&g);else if(ch=='c'&&stopped)gdb_continue(&g);else if(ch=='n'&&stopped)gdb_next(&g);else if(ch=='s'&&stopped)gdb_step(&g);else if(ch=='f'&&stopped)gdb_finish(&g);else if(ch=='b'&&src.path[0]){if(!src.debug_lines)snprintf(g.message,sizeof(g.message),"Source line breakpoint unavailable: no debug line information.");else{int marked=breakpoint_mark(&g,src.path,cursor+1),oldmax=newest_break_number(&g),requested=cursor+1;if(!gdb_toggle_breakpoint(&g,src.path,requested)&&!marked){int actual=new_break_line(&g,src.path,oldmax);if(actual>0){cursor=actual-1;if(actual!=requested)snprintf(g.message,sizeof(g.message),"Breakpoint moved to executable line %d",actual);}}}}else if(ch=='B'&&src.path[0]){if(!src.debug_lines)snprintf(g.message,sizeof(g.message),"Source line breakpoint unavailable: no debug line information.");else{input[0]=0;if(prompt_text("Condition: ",input,sizeof(input))){int oldmax=newest_break_number(&g),requested=cursor+1;if(!gdb_set_cond_breakpoint(&g,src.path,requested,input)){int actual=new_break_line(&g,src.path,oldmax);if(actual>0){cursor=actual-1;if(actual!=requested)snprintf(g.message,sizeof(g.message),"Conditional breakpoint moved to executable line %d",actual);}}}}}else if(ch=='w'&&stopped){input[0]=0;if(prompt_text("Watch variable/expression: ",input,sizeof(input)))gdb_watch(&g,input);}else if(ch=='p'&&stopped){input[0]=0;if(prompt_text("Print expression: ",input,sizeof(input))){char value[GD_TEXT_MAX];gdb_print(&g,input,value,sizeof(value));}}else if(ch=='L'){gdb_refresh_breakpoints(&g);view=VIEW_BREAKS;}else if(ch=='O')view=VIEW_OUTPUT;else if(ch=='h')view=VIEW_HELP;else if(ch==18&&src.path[0]){src.mtime=0;source_load(&src,src.path);snprintf(g.message,sizeof(g.message),"Source reloaded");}else if(ch=='q'){if(g.state==GDB_RUNNING||g.state==GDB_STOPPED){if(confirm_quit())done=true;}else done=true;}}
    endwin();source_free(&src);gdb_shutdown(&g);return 0;}
