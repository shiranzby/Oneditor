// mini_vim.c - Vim风格极简编辑器（支持命令历史、滚动算法优化、:go 跳转行）
// 主要功能：基本Vim模式、撤销、命令行、历史、中文行号、UTF-8兼容
// 构建：gcc oneditor.c -o oneditor.exe
#include <stdio.h>         // 标准输入输出
#include <stdlib.h>        // 标准库
#include <string.h>        // 字符串处理
#include <windows.h>       // Windows API
#include <conio.h>         // 控制台输入
#include <wchar.h>         // 宽字符处理
#include <locale.h>        // 区域设置
#include <ctype.h>         // 字符处理

#define MAX_LINES 1000                // 最大文本行数
#define MAX_COLS  512                 // 每行最大字符数
#define MAX_ROWS 100                  // 屏幕最大显示行数
#define MAX_COLS_SCREEN 256           // 屏幕最大显示列数
#define UNDO_STACK 100                // 撤销栈深度
#define CMD_HISTORY_MAX 100           // 命令历史条数

// 编辑器模式（普通、插入）
typedef enum { MODE_NORMAL, MODE_INSERT } EditorMode;

// 撤销状态结构体：快照一份文本内容与光标
typedef struct {
    char lines[MAX_LINES][MAX_COLS];
    int line_count;
    int cx, cy;
} UndoState;

// 命令处理函数指针类型
typedef void (*CmdHandler)(int);
// 命令表结构体
typedef struct {
    int key;                 // 响应键
    CmdHandler handler;      // 处理函数
    const char* desc;        // 描述
} CmdEntry;

// ------------- 命令分发表预定义 -------------
#define NORM_CMD_NUM 18
// 命令处理函数声明
void norm_insert(int), norm_insert_head(int), norm_insert_end(int);
void norm_left(int), norm_right(int), norm_up(int), norm_down(int);
void norm_line_head(int), norm_line_end(int), norm_del_char(int);
void undo_handler(int), norm_cmdmode(int), norm_search_next(int), norm_search_prev(int);
void norm_combo_handler(int);
// 命令表
CmdEntry normal_cmds[NORM_CMD_NUM] = {
    {'i', norm_insert, "插入"},
    {'I', norm_insert_head, "行首插入"},
    {'A', norm_insert_end, "行尾插入"},
    {'h', norm_left, "左"},
    {'l', norm_right, "右"},
    {'j', norm_up, "上"},
    {'k', norm_down, "下"},
    {'0', norm_line_head, "行首"},
    {'9', norm_line_end, "行尾"},
    {'x', norm_del_char, "删字符"},
    {'u', undo_handler, "撤销"},
    {':', norm_cmdmode, "命令模式"},
    {'n', norm_search_next, "下一个"},
    {'N', norm_search_prev, "上一个"},
    {'d', norm_combo_handler, "删行"},
    {'g', norm_combo_handler, "gg"},
    {'G', norm_combo_handler, "GG"},
    {'o', norm_combo_handler, "oo"},
};

// ------------- 全局状态 -------------
// 命令历史
char cmd_history[CMD_HISTORY_MAX][256];
int cmd_history_count = 0;
int cmd_history_pos = -1;

// 编辑器主缓冲/状态
char lines[MAX_LINES][MAX_COLS]; // 编辑区
int line_count = 1, cx = 0, cy = 0; // 当前行数，光标
int insert_mode = 0;                // 是否插入模式
char filename[256] = "";            // 当前文件名
char last_pat[128] = "";            // 最近搜索内容
int last_found = -1, show_lineno = 0; // 最近查找行，是否显示行号
int scroll = 0, hscroll = 0;          // 滚动行/列
char screenbuf[MAX_ROWS][MAX_COLS_SCREEN]; // 屏幕缓冲区
UndoState undo_stack[UNDO_STACK]; // 撤销栈
int undo_top = 0, undo_cur = 0;   // 撤销指针
EditorMode mode = MODE_NORMAL;    // 当前编辑器模式

// 帮助信息
const char *normal_help =
"可用命令：\n"
"i：插入模式  :：命令模式\n"
"h：左  j：上  k：下  l：右  0：行首  9：行尾\n"
"gg/GG：首/末行  u：撤销  x：删字符  dd：删行\n"
"oo：下方插入新行\n";

const char *insert_help =
"可用命令：\n"
"输入文本，支持退格、回车换行\n"
"ESC：返回正常模式\n";

const char *cmd_help =
"可用命令：\n"
":w 保存 :w 文件名 另存\n"
":q 退出 :q!强制退出\n"
":wq 保存并退出\n"
":r 文件名 打开文件\n"
":set nu 显行号 :set nonu 隐藏行号\n"
":go 行号 跳转到指定行\n"
":!命令 外部命令 :f 内容 搜索 n/N 查找\n";

// 设置控制台为UTF-8模式
void set_console_utf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setlocale(LC_ALL, "");
}

// 设置控制台为原始输入模式
void set_console_raw() {
    HANDLE h=GetStdHandle(STD_INPUT_HANDLE);
    DWORD m;
    GetConsoleMode(h,&m);
    m&=~(ENABLE_ECHO_INPUT|ENABLE_LINE_INPUT);
    SetConsoleMode(h,m);
}

// 设置控制台为普通输入模式
void set_console_normal() {
    HANDLE h=GetStdHandle(STD_INPUT_HANDLE);
    DWORD m;
    GetConsoleMode(h,&m);
    m|=(ENABLE_ECHO_INPUT|ENABLE_LINE_INPUT);
    SetConsoleMode(h,m);
}

// 控制台输出utf8字符串
void print_utf8(const char *utf8str) {
    if (!utf8str) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8str, -1, NULL, 0);
    if (wlen <= 0) return;
    wchar_t *wbuf = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wbuf) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8str, -1, wbuf, wlen);
    DWORD written = 0;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), wbuf, wcslen(wbuf), &written, NULL);
    free(wbuf);
}

// 获取utf8字符长度
int utf8_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    else if ((c & 0xE0) == 0xC0) return 2;
    else if ((c & 0xF0) == 0xE0) return 3;
    else if ((c & 0xF8) == 0xF0) return 4;
    else return 1;
}
// 判断字符宽度：英文1，部分中文2
int char_width(const char *s, int pos) {
    unsigned char c = (unsigned char)s[pos];
    if (c < 0x80) return 1;
    else if ((c & 0xE0) == 0xC0) return 1;
    else if ((c & 0xF0) == 0xE0) return 2;
    else if ((c & 0xF8) == 0xF0) return 2;
    else return 1;
}
// 计算字符串的可见宽度（列数）
int str_vis_width(const char *s) {
    int w=0;
    for(int i=0;s[i];) {
        w+=char_width(s,i);
        i+=utf8_len((unsigned char)s[i]);
    }
    return w;
}
// 可见宽度转实际字节位置（用于插入/删除等）
int vis2real(const char *s, int vis) {
    int width=0,i=0;
    while(s[i]&&width<vis) {
        int w=char_width(s,i),clen=utf8_len((unsigned char)s[i]);
        if(width+w>vis) break;
        width+=w;
        i+=clen;
    }
    return i;
}
// 删除指定可见宽度位置的字符
void delvis(char *s, int vis) {
    int pos=vis2real(s,vis);
    if(!s[pos]) return;
    int del=utf8_len((unsigned char)s[pos]);
    memmove(&s[pos],&s[pos+del],strlen(&s[pos+del])+1);
}
// 在指定可见宽度位置插入字符
void insvis(char *s, int vis, const char *ins, int inslen) {
    int pos=vis2real(s,vis), slen=strlen(s);
    if(slen+inslen>=MAX_COLS-1) return;
    memmove(&s[pos+inslen],&s[pos],slen-pos+1);
    memcpy(&s[pos],ins,inslen);
}
// 去除字符串前后空白
void trim(char *s) {
    char *p=s;
    while(*p==' '||*p=='\t') p++;
    if(p!=s) memmove(s,p,strlen(p)+1);
    p=s+strlen(s)-1;
    while(p>=s&&(*p==' '||*p=='\t')) *p--=0;
}
// 不区分大小写查找子串
char *strcasestr2(const char *h, const char *n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        const char *hh=h,*nn=n;
        while(*hh&&*nn&&tolower((unsigned char)*hh)==tolower((unsigned char)*nn)) hh++,nn++;
        if(!*nn) return (char*)h;
    }
    return NULL;
}

// utf8转gbk（windows下中文路径支持）
int utf8_to_gbk(const char *utf8, char *gbk, int gbk_size) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return 0;
    wchar_t *wbuf = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wbuf) return 0;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wlen);
    int glen = WideCharToMultiByte(936, 0, wbuf, -1, gbk, gbk_size, NULL, NULL);
    free(wbuf);
    return glen;
}
// 支持utf8文件名的fopen
FILE *fopen_utf8(const char *fname, const char *mode) {
    char gbk_fname[512];
    utf8_to_gbk(fname, gbk_fname, sizeof(gbk_fname));
    return fopen(gbk_fname, mode);
}
// 保存文件
void file_save(const char *fname) {
    FILE *fp = fopen_utf8(fname, "w");
    if (!fp) { char msg[512]; snprintf(msg,sizeof(msg),"无法打开文件: %s\n",fname); print_utf8(msg); return; }
    for (int i=0;i<line_count;i++) fprintf(fp,"%s\n",lines[i]);
    fclose(fp);
    strncpy(filename, fname, 255); filename[255]=0;
    char msg[512]; snprintf(msg,sizeof(msg),"已保存到 %s\n",fname); print_utf8(msg);
}
// 加载文件
void file_load(const char *fname) {
    FILE *fp = fopen_utf8(fname, "r");
    if (!fp) { char msg[512]; snprintf(msg,sizeof(msg),"无法打开文件: %s\n",fname); print_utf8(msg); return; }
    line_count=0;
    while(fgets(lines[line_count],MAX_COLS,fp)&&line_count<MAX_LINES) {
        size_t len=strlen(lines[line_count]);
        if(len&&lines[line_count][len-1]=='\n') lines[line_count][len-1]=0;
        line_count++;
    }
    fclose(fp);
    strncpy(filename, fname, 255); filename[255]=0;
    char msg[512]; snprintf(msg,sizeof(msg),"已打开文件: %s\n",fname); print_utf8(msg);
}

// 撤销保存
void undo_save() {
    UndoState *u = &undo_stack[undo_top];
    for (int i=0;i<line_count;i++) strcpy(u->lines[i],lines[i]);
    u->line_count=line_count; u->cx=cx; u->cy=cy;
    undo_top=(undo_top+1)%UNDO_STACK;
    if(undo_top==undo_cur) undo_cur=(undo_cur+1)%UNDO_STACK;
}
// 撤销恢复
void undo_restore() {
    if(undo_top==undo_cur) return;
    undo_top=(undo_top-1+UNDO_STACK)%UNDO_STACK;
    UndoState *u=&undo_stack[undo_top];
    for(int i=0;i<u->line_count;i++) strcpy(lines[i],u->lines[i]);
    line_count=u->line_count; cx=u->cx; cy=u->cy;
}

// 调整纵向滚动
void adjust_scroll(int help_lines) {
    HANDLE hOut=GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut,&csbi);
    int win_rows=csbi.srWindow.Bottom-csbi.srWindow.Top+1;
    int text_rows=win_rows-help_lines-1;
    if(text_rows<1) text_rows=1;
    int max_scroll = (line_count > text_rows) ? line_count - text_rows : 0;
    if(cy<scroll) scroll=cy;
    else if(cy>=scroll+text_rows) scroll=cy-text_rows+1;
    if(scroll<0) scroll=0;
    if(scroll>max_scroll) scroll=max_scroll;
}
// 调整横向滚动
void adjust_hscroll(int win_cols) {
    int left_margin=show_lineno?5:0, text_cols=win_cols-left_margin;
    if(text_cols<1) text_cols=1;
    if(cx<hscroll) hscroll=cx;
    else if(cx>=hscroll+text_cols) hscroll=cx-text_cols+1;
    if(hscroll<0) hscroll=0;
}

// 清空屏幕缓冲
void clear_screen_buf(int rows, int cols) {
    for(int i=0;i<rows;i++) { memset(screenbuf[i],' ',cols); if(cols>0) screenbuf[i][cols-1]=0; }
}

// 刷新屏幕缓冲到控制台
void flush_screen_buf(int rows, int cols) {
    HANDLE hOut=GetStdHandle(STD_OUTPUT_HANDLE); COORD pos={0,0}; DWORD written;
    for(int i=0;i<rows;i++) { pos.X=0; pos.Y=i; WriteConsoleOutputCharacterA(hOut,screenbuf[i],cols-1,pos,&written); }
}

// 统计字符串行数
int count_lines(const char *s) { int n=1; for(;*s;s++) if(*s=='\n') n++; return n; }

// 显示底部帮助
void show_bottom_help(const char *help, int win_rows, int win_cols) {
    int help_lines=count_lines(help)+2, start_line=win_rows-help_lines, line=start_line;
    if(insert_mode) {
        snprintf(screenbuf[line],win_cols,"%-*s",win_cols-1,"插入模式"); line++;
        const char *p=insert_help;
        while(*p) { int len=0; while(p[len]&&p[len]!='\n') len++;
            snprintf(screenbuf[line],win_cols,"%.*s%*s",len,p,win_cols-1-len,""); line++;
            if(p[len]=='\n') p+=len+1; else break;
        }
    } else {
        snprintf(screenbuf[line],win_cols,"%-*s",win_cols-1,"正常模式"); line++;
        const char *p=normal_help;
        while(*p) { int len=0; while(p[len]&&p[len]!='\n') len++;
            snprintf(screenbuf[line],win_cols,"%.*s%*s",len,p,win_cols-1-len,""); line++;
            if(p[len]=='\n') p+=len+1; else break;
        }
    }
    snprintf(screenbuf[win_rows-1],win_cols,": %-*s",win_cols-3,"");
}

// 绘制界面
void draw() {
    HANDLE hOut=GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut,&csbi);
    //获取行数和列数
    int win_rows=csbi.srWindow.Bottom-csbi.srWindow.Top+1;
    int win_cols=csbi.srWindow.Right-csbi.srWindow.Left+1;
    if(win_rows>MAX_ROWS) win_rows=MAX_ROWS;
    if(win_cols>MAX_COLS_SCREEN) win_cols=MAX_COLS_SCREEN;
    //根据模式获取帮助信息
    const char *help=insert_mode?insert_help:normal_help;
    int help_lines=count_lines(help)+2, text_rows=win_rows-help_lines-1;
    if(text_rows<1) text_rows=1;
    adjust_scroll(help_lines); adjust_hscroll(win_cols);
    clear_screen_buf(win_rows,win_cols);
    //循环清空并填充缓冲区
    for(int i=0;i<text_rows;i++) {
        int idx=scroll+i, col=0;
        if(idx>=line_count) { memset(screenbuf[i],' ',win_cols-1); screenbuf[i][win_cols-1]=0; continue; }
        if(show_lineno) { snprintf(screenbuf[i],win_cols,"%4d ",idx+1); col=5; }
        int realj=vis2real(lines[idx],hscroll);
        while(lines[idx][realj]&&col<win_cols-1) {
            int clen=utf8_len((unsigned char)lines[idx][realj]);
            int cwidth=char_width(lines[idx],realj);
            if(col+cwidth>win_cols-1) break;
            for(int k=0;k<clen&&col<win_cols-1;k++) screenbuf[i][col++]=lines[idx][realj+k];
            realj+=clen;
        }
        if(col<win_cols-1) screenbuf[i][col++]=' ';
        while(col<win_cols-1) screenbuf[i][col++]=' ';
        screenbuf[i][win_cols-1]=0;
    }
    int blank_line=text_rows;
    if(blank_line<win_rows-1) { memset(screenbuf[blank_line],' ',win_cols-1); screenbuf[blank_line][win_cols-1]=0; }
    show_bottom_help(help,win_rows,win_cols);
    flush_screen_buf(win_rows,win_cols);
    int display_x=0, realpos=vis2real(lines[cy],cx), realstart=vis2real(lines[cy],hscroll);
    for(int i=realstart;i<realpos;) { display_x+=char_width(lines[cy],i); i+=utf8_len((unsigned char)lines[cy][i]); }
    COORD pos; pos.X=(show_lineno?5:0)+display_x; pos.Y=cy-scroll;
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE),pos);
}

// 向后查找
int search_pat(const char *pattern, int start) {
    for(int i=start;i<line_count;i++) if(strcasestr2(lines[i],pattern)) return i;
    for(int i=0;i<start;i++) if(strcasestr2(lines[i],pattern)) return i;
    return -1;
}
// 向前查找
int search_pat_rev(const char *pattern, int start) {
    for(int i=start;i>=0;i--) if(strcasestr2(lines[i],pattern)) return i;
    for(int i=line_count-1;i>start;i--) if(strcasestr2(lines[i],pattern)) return i;
    return -1;
}

// 光标左移
int move_cx_left(const char *s, int cx) {
    if (cx <= 0) return 0;
    int prev_cx = 0, i = 0, last_cx = 0;
    while (s[i]) {
        int w = char_width(s, i);
        int clen = utf8_len((unsigned char)s[i]);
        if (prev_cx + w >= cx) break;
        prev_cx += w;
        i += clen;
        last_cx = prev_cx;
    }
    return last_cx;
}

// 光标右移
int move_cx_right(const char *s, int cx) {
    int i = 0, cur_cx = 0;
    while (s[i]) {
        int w = char_width(s, i);
        int clen = utf8_len((unsigned char)s[i]);
        if (cur_cx == cx) return cur_cx + w;
        cur_cx += w;
        i += clen;
    }
    return cx;
}

// 模式切换
void set_mode(EditorMode m) {
    mode=m; insert_mode=(m==MODE_INSERT);
    HANDLE hOut=GetStdHandle(STD_INPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut,&csbi);
    int win_cols=csbi.srWindow.Right-csbi.srWindow.Left+1;
    adjust_scroll(count_lines(normal_help)+2); adjust_hscroll(win_cols); draw();
}

// 撤销命令
void undo_handler(int key) { undo_restore(); set_mode(mode); }

// 保存命令
void save_curfile_handler(int key) {
    if(filename[0]=='\0') {
        char input[16]={0}; set_console_normal();
        print_utf8("当前未打开文件，默认文件名为test.txt\n:w test.txt    可另存为test.txt文件\n是否确认继续保存为test.txt？(y/n)\n请输入y或n后回车: ");
        fgets(input,sizeof(input),stdin); set_console_raw();
        int i=0; while(input[i]==' '||input[i]=='\t') i++;
        if(input[i]=='y'||input[i]=='Y') file_save("test.txt");
    } else file_save(filename);
}

// 进入插入模式
void norm_insert(int key) { set_mode(MODE_INSERT); }
// 行首插入
void norm_insert_head(int key) { cx=0; set_mode(MODE_INSERT); }
// 行尾插入
void norm_insert_end(int key) { cx=str_vis_width(lines[cy]); set_mode(MODE_INSERT); }
// 光标左
void norm_left(int key) { if(cx>0) cx=move_cx_left(lines[cy],cx); }
// 光标右
void norm_right(int key) { if(cx<str_vis_width(lines[cy])) cx=move_cx_right(lines[cy],cx); }
// 光标上
void norm_up(int key) { if(cy>0) cy--; if(cx>str_vis_width(lines[cy])) cx=str_vis_width(lines[cy]); }
// 光标下
void norm_down(int key) { if(cy<line_count-1) cy++; if(cx>str_vis_width(lines[cy])) cx=str_vis_width(lines[cy]); }
// 行首
void norm_line_head(int key) { cx=0; }
// 行尾
void norm_line_end(int key) { cx=str_vis_width(lines[cy]); }
// 删除字符
void norm_del_char(int key) { int vislen=str_vis_width(lines[cy]); if(cx<vislen) delvis(lines[cy],cx); }
void norm_cmdmode(int key);

// 插入新行
void norm_insert_newline(int key) {
    if(line_count<MAX_LINES-1) {
        for(int i=line_count;i>cy+1;i--) strcpy(lines[i],lines[i-1]);
        strcpy(lines[cy+1],""); cy++; cx=0; line_count++; insert_mode=1;
    }
}
// 删除当前行
void norm_del_line(int key) {
    if(line_count>1) {
        for(int i=cy;i<line_count-1;i++) strcpy(lines[i],lines[i+1]);
        line_count--; if(cy>=line_count) cy=line_count-1; if(cx>str_vis_width(lines[cy])) cx=str_vis_width(lines[cy]);
    } else {
        strcpy(lines[0],""); cy=0; cx=0; line_count=1;
    }
}
// 查找下一个
void norm_search_next(int key) {
    if(last_pat[0]) {
        int found=search_pat(last_pat,cy+1);
        if(found!=-1) { cy=found; last_found=found; if(cx>str_vis_width(lines[cy])) cx=str_vis_width(lines[cy]); }
    }
}
// 查找上一个
void norm_search_prev(int key) {
    if(last_pat[0]) {
        int found=search_pat_rev(last_pat,cy-1);
        if(found!=-1) { cy=found; last_found=found; if(cx>str_vis_width(lines[cy])) cx=str_vis_width(lines[cy]); }
    }
}
// 空操作
void norm_noop(int key) {}
// 组合键处理（gg, GG, oo, dd）
void norm_combo_handler(int key) {
    static int gcount = 0, ocount = 0, dcount=0;
    if(key == 'g') { gcount++; if(gcount==2) { cy=0; if(cx>str_vis_width(lines[cy])) cx=str_vis_width(lines[cy]); gcount=0; } return; }
    if(key == 'G') { gcount++; if(gcount==2) { cy=line_count-1; if(cx>str_vis_width(lines[cy])) cx=str_vis_width(lines[cy]); gcount=0; } return; }
    if(key == 'o') { ocount++; if(ocount==2) { norm_insert_newline(key); ocount=0; } return; }
    if(key == 'd') { dcount++; if(dcount==2) { norm_del_line(key); dcount=0; } return; }
    gcount=0; ocount=0; dcount=0;
}

// 命令模式入口
void norm_cmdmode(int key) {
    set_console_normal();
    HANDLE hOut=GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut,&csbi);
    int win_cols=csbi.srWindow.Right-csbi.srWindow.Left+1, win_rows=csbi.srWindow.Bottom-csbi.srWindow.Top+1;
    int normal_help_lines=count_lines(normal_help)+2, command_help_lines=count_lines(cmd_help)+2;
    int max_help_lines=normal_help_lines>command_help_lines?normal_help_lines:command_help_lines;
    int start_line=win_rows-max_help_lines, line=win_rows-command_help_lines;
    for(int i=start_line;i<win_rows;i++) { memset(screenbuf[i],' ',win_cols-1); screenbuf[i][win_cols-1]=0; }
    snprintf(screenbuf[line],win_cols,"%-*s",win_cols-1,"命令模式"); line++;
    const char *p=cmd_help;
    while(*p) { int len=0; while(p[len]&&p[len]!='\n') len++;
        snprintf(screenbuf[line],win_cols,"%.*s%*s",len,p,win_cols-1-len,""); line++;
        if(p[len]=='\n') p+=len+1; else break;
    }
    snprintf(screenbuf[win_rows-1],win_cols,":%-*s",win_cols-2,"");
    flush_screen_buf(win_rows,win_cols);

    char cmd[256]="";
    wchar_t wbuf[MAX_COLS]={0}; int wlen=0;
    int hist_pos = cmd_history_count;
    while(1) {
        int ch=_getwch();
        if(ch==13||ch==10) break;
        else if(ch==8||ch==127) { if(wlen>0) wlen--; }
        else if(ch==27) { set_console_raw(); draw(); return; }
        else if(ch==0||ch==224) {
            int arrow=_getwch();
            if(arrow==72) {
                if(hist_pos > 0) hist_pos--;
                if(hist_pos < cmd_history_count) {
                    wlen = MultiByteToWideChar(CP_UTF8,0,cmd_history[hist_pos],-1,wbuf,MAX_COLS-1); if(wlen>0) wlen--;
                }
            } else if(arrow==80) {
                if(hist_pos < cmd_history_count-1) hist_pos++;
                else hist_pos = cmd_history_count;
                if(hist_pos < cmd_history_count) {
                    wlen = MultiByteToWideChar(CP_UTF8,0,cmd_history[hist_pos],-1,wbuf,MAX_COLS-1); if(wlen>0) wlen--;
                } else wlen=0;
            }
        } else if(wlen<MAX_COLS-1) wbuf[wlen++]=ch;
        int utf8len=WideCharToMultiByte(CP_UTF8,0,wbuf,wlen,cmd,sizeof(cmd)-1,NULL,NULL);
        cmd[utf8len]=0;
        snprintf(screenbuf[win_rows-1],win_cols,":%-*s",win_cols-2,cmd);
        flush_screen_buf(win_rows,win_cols);
        COORD pos; pos.X=1+str_vis_width(cmd); pos.Y=win_rows-1;
        SetConsoleCursorPosition(hOut,pos);
    }
    int utflen=WideCharToMultiByte(CP_UTF8,0,wbuf,wlen,cmd,sizeof(cmd)-1,NULL,NULL);
    cmd[utflen]=0;
    trim(cmd);
    if(cmd[0]) {
        if(cmd_history_count==0 || strcmp(cmd, cmd_history[cmd_history_count-1])!=0) {
            if(cmd_history_count<CMD_HISTORY_MAX) strcpy(cmd_history[cmd_history_count++], cmd);
            else {
                for(int i=1;i<CMD_HISTORY_MAX;i++) strcpy(cmd_history[i-1], cmd_history[i]);
                strcpy(cmd_history[CMD_HISTORY_MAX-1], cmd);
            }
        }
    }
    set_console_raw();
    // :go 跳转
    if(strncmp(cmd,"go ",3)==0) {
        int lineno = atoi(cmd+3);
        if(lineno >= 1 && lineno <= line_count) {
            cy = lineno - 1;
            if(cx > str_vis_width(lines[cy])) cx = str_vis_width(lines[cy]);
        } else {
            print_utf8("行号超出范围，按任意键返回\n");
            _getch();
        }
    }
    // 其它命令处理
    else if(strncmp(cmd,"f ",2)==0) {
        char *pattern=(char*)(cmd+2); trim(pattern);
        strncpy(last_pat,pattern,127); last_pat[127]=0;
        int found=search_pat(last_pat,cy+1);
        if(found!=-1) { cy=found; last_found=found; if(cx>str_vis_width(lines[cy])) cx=str_vis_width(lines[cy]); }
        else { print_utf8("未找到匹配内容！\n"); _getch(); last_pat[0]=0; }
    } else if(strncmp(cmd,"wq",2)==0) {
        if(cmd[2]==' '&&cmd[3]) { file_save(cmd+3); exit(0); }
        else { if(filename[0]=='\0') { char input[16]={0}; set_console_normal(); print_utf8("当前未打开文件，默认文件名为test.txt\n:w test.txt    可另存为test.txt文件\n是否确认继续保存为test.txt？(y/n)\n请输入y或n后回车: "); fgets(input,sizeof(input),stdin); set_console_raw(); int i=0; while(input[i]==' '||input[i]=='\t') i++; if(input[i]=='y'||input[i]=='Y') { file_save("test.txt"); exit(0); } else { print_utf8("已取消保存。按任意键返回\n"); _getch(); draw(); return; }} else { file_save(filename); exit(0); } }
    } else if(strncmp(cmd,"w ",2)==0) file_save(cmd+2);
    else if(strcmp(cmd,"w")==0) { if(filename[0]=='\0') { char input[16]={0}; set_console_normal(); print_utf8("当前未打开文件，默认文件名为test.txt\n:w test.txt    可另存为test.txt文件\n是否确认继续保存为test.txt？(y/n)\n请输入y或n后回车: "); fgets(input,sizeof(input),stdin); set_console_raw(); int i=0; while(input[i]==' '||input[i]=='\t') i++; if(input[i]=='y'||input[i]=='Y') file_save("test.txt"); else { print_utf8("已取消保存。按任意键返回\n"); _getch(); } } else file_save(filename); }
    else if(strcmp(cmd,"q")==0||strcmp(cmd,"q!")==0) exit(0);
    else if(strncmp(cmd,"r ",2)==0) file_load(cmd+2);
    else if(strcmp(cmd,"set nu")==0) { show_lineno=1; print_utf8("已开启显示行号，按任意键返回\n"); _getch(); }
    else if(strcmp(cmd,"set nonu")==0) { show_lineno=0; print_utf8("已关闭显示行号，按任意键返回\n"); _getch(); }
    else if(cmd[0]=='!') { system(cmd+1); print_utf8("外部命令已执行，按任意键返回\n"); _getch(); }
    else if(cmd[0]) { char msg[128]; snprintf(msg,sizeof(msg),"未识别命令: %s 按任意键返回\n",cmd); print_utf8(msg); _getch(); }
    draw();
}

// 正常模式命令分发
void norm_dispatch(int key) {
    for(int i=0;i<NORM_CMD_NUM;i++) {
        if(normal_cmds[i].key==key) {
            normal_cmds[i].handler(key);
            return;
        }
    }
    norm_combo_handler(key);
}

// 插入模式命令分发
/**
 * 
 *
 * 根据用户按下的键值（key），在插入模式下执行相应的编辑操作，包括：
 * - Esc（27）：切换到普通模式。
 * - 回车（13 或 10）：在当前行下方插入新行，并将光标移动到新行起始。
 * - 退格（8 或 127）：删除光标前的字符，或在行首时合并上一行。
 * - 方向键（0 或 224 后跟箭头码）：移动光标位置（左右移动字符，或上下移动行）。
 * - 其他字符：将输入的字符（支持 UTF-8 和代理对）插入到当前光标位置。
 *
 * 主要流程：
 * 1. 获取控制台窗口信息，计算窗口宽度。
 * 2. 根据不同按键类型，调用相应的编辑操作函数（如 undo_save、insvis、delvis 等）。
 * 3. 维护行内容（lines）、光标位置（cx, cy）、行数（line_count）等全局变量。
 *
 *
 * 
 *
 *
 * @param key 用户按下的键值（支持 ASCII、控制键和 Unicode 字符）。
 */
void insert_dispatch(int key) {
    HANDLE hOut=GetStdHandle(STD_INPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut,&csbi);
    int win_cols=csbi.srWindow.Right-csbi.srWindow.Left+1;
    if(key==27) { set_mode(MODE_NORMAL); return; }
    else if(key==13||key==10) {
        if(line_count<MAX_LINES-1) {
            undo_save();
            for(int i=line_count;i>cy+1;i--) strcpy(lines[i],lines[i-1]);
            int realpos=vis2real(lines[cy],cx);
            strcpy(lines[cy+1],lines[cy]+realpos);
            lines[cy][realpos]=0; cy++; cx=0; line_count++;
        }
    } else if(key==8||key==127) {
        if(cx>0) { undo_save(); delvis(lines[cy],cx-1); cx--; }
        else if(cy>0) {
            undo_save();
            int prevlen=strlen(lines[cy-1]);
            if(prevlen+strlen(lines[cy])<MAX_COLS-1) {
                strcat(lines[cy-1],lines[cy]);
                for(int i=cy;i<line_count-1;i++) strcpy(lines[i],lines[i+1]);
                line_count--; cy--; cx=str_vis_width(lines[cy]);
            }
        }
    } else if(key==0||key==224) {
        int arrow=_getwch();
        if(arrow==75&&cx>0) cx=move_cx_left(lines[cy],cx);
        else if(arrow==77&&cx<str_vis_width(lines[cy])) cx=move_cx_right(lines[cy],cx);
        else if(arrow==72&&cy>0) { cy--; if(cx>str_vis_width(lines[cy])) cx=str_vis_width(lines[cy]); }
        else if(arrow==80&&cy<line_count-1) { cy++; if(cx>str_vis_width(lines[cy])) cx=str_vis_width(lines[cy]); }
    } else {
        //转换为宽字符
        wchar_t wch=key;
        if(wch>=0xD800&&wch<=0xDBFF) {
            //高代理，三位一个
            wchar_t wch2=_getwch(); wchar_t wstr[3]={wch,wch2,0}; char utf8[8]={0};
            int utflen=WideCharToMultiByte(CP_UTF8,0,wstr,2,utf8,sizeof(utf8)-1,NULL,NULL);
            if(utflen>0&&strlen(lines[cy])+utflen<MAX_COLS-1) { undo_save(); insvis(lines[cy],cx,utf8,utflen); cx+=char_width(utf8,0); }
        } else {
            //非高代理，直接转换UTF8流，并使用insvis插入
            wchar_t wstr[2]={wch,0}; char utf8[8]={0};
            int utflen=WideCharToMultiByte(CP_UTF8,0,wstr,1,utf8,sizeof(utf8)-1,NULL,NULL);
            if(utflen>0&&strlen(lines[cy])+utflen<MAX_COLS-1) { undo_save(); insvis(lines[cy],cx,utf8,utflen); cx+=char_width(utf8,0); }
        }
    }
}

// 主程序入口
int main(int argc, char *argv[]) {
    set_console_utf8(); set_console_raw();
    if(argc>1) { strncpy(filename,argv[1],255); file_load(filename); } else { strcpy(lines[0],""); filename[0]=0; }
    adjust_scroll(count_lines(normal_help)+2); draw();
    while(1) {
        int key=0;
        if(insert_mode) {
            key=_getwch();
            insert_dispatch(key);
            adjust_hscroll(MAX_COLS_SCREEN);
            adjust_scroll(count_lines(insert_help)+2); draw();
            continue;
        }
        key=_getch();
        if(key==0||key==224) {
            int arrow=_getwch();
            if(arrow==75) norm_left(key);
            else if(arrow==77) norm_right(key);
            else if(arrow==72) norm_up(key);
            else if(arrow==80) norm_down(key);
        } else {
            norm_dispatch(key);
        }
        adjust_hscroll(MAX_COLS_SCREEN);
        adjust_scroll(count_lines(normal_help)+2); draw();
    }
    return 0;
}