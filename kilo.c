#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

//defines
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define QUIT_TIMES 3

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

//data
typedef struct erow{
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

struct editorConfig {
    int cx,cy;
    int rx;
    int screenrows;
    int screencols;
    int numrows;
    
    int rowoff;
    int coloff;
    time_t statusmsg_time;
    char statusmsg[80];
    int dirty;
    erow *row;
    char *filename;
    struct termios orig_termios;
};

struct editorConfig E;
/*prototypes*/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

//terminal
void die(const char *s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H",3);
    perror(s);
    exit(1);
}

void disableRaw(){
    if(tcsetattr(STDIN_FILENO,TCSAFLUSH,&E.orig_termios)==-1) die("tcsetattr");
}

void enableRaw(){
    if(tcgetattr(STDIN_FILENO,&E.orig_termios)==-1) die("tcgetattr");
    atexit(disableRaw);

    struct termios raw;
    raw=E.orig_termios;
    raw.c_iflag &= ~(BRKINT |ICRNL |INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN| ISIG );
    raw.c_cc[VMIN]=0;
    raw.c_cc[VTIME]=1;

    if(tcsetattr(STDIN_FILENO,TCSAFLUSH,&raw)==-1)
        die("tcsetattr");
}

// Function to restore blocking mode for a file descriptor
void setBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}

int readKey(){
    setBlocking(STDIN_FILENO);
    int nread;
    char c;
    while((nread = read(STDIN_FILENO,&c,1))==-1){
        if(nread==-1 && errno !=EAGAIN && errno != EWOULDBLOCK) die("read");
    }

    if(c=='\x1b'){
        char seq[3];

        if(read(STDIN_FILENO, &seq[0],1)!=1) return '\x1b';
        if(read(STDIN_FILENO, &seq[1],1)!=1) return '\x1b';
        if(seq[0]=='['){
            if(seq[1]>='0' && seq[1]<='9'){
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if(seq[2]=='~'){
                    if(seq[1]=='1') return HOME_KEY;
                    if(seq[1]=='3') return DEL_KEY;
                    if(seq[1]=='4') return END_KEY;
                    if(seq[1]=='5') return PAGE_UP;
                    if(seq[1]=='6') return PAGE_DOWN;
                    if(seq[1]=='7') return HOME_KEY;
                    if(seq[1]=='8') return END_KEY;
                }
            }  
            else{
                if(seq[1]=='A') return ARROW_UP;
                if(seq[1]=='B') return ARROW_DOWN;
                if(seq[1]=='C') return ARROW_RIGHT;
                if(seq[1]=='D') return ARROW_LEFT;
                if(seq[1]=='H') return HOME_KEY;
                if(seq[1]=='F') return END_KEY;
            }
        }
        else if(seq[0] == 'O') {
            if(seq[1]=='H') return HOME_KEY;
            if(seq[1]=='F')return END_KEY;
      }
        return '\x1b';
    }
    else return c;
}

int getCursorPosition(int *rows, int *cols){
    char buf[32];
    unsigned int i=0;

    if(write(STDOUT_FILENO, "\x1b[6n]",4)!=4) return -1;

    while(i<sizeof(buf)-1){
        if(read(STDIN_FILENO,&buf[i],1)==-1) break;
        if(buf[i] == 'R') break;
        i++;
    }
    buf[i]='\0';

    if(buf[0] !='\x1b' || buf[1]!='[') return -1;
    if (sscanf(&buf[2],"%d;%d",rows,cols)!=2) return -1;
    
    return 0;
}

int getWindowSize(int *rows,int *cols){
    struct winsize ws;
    
    if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==-1 || ws.ws_col==0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 2) return -1;

        return getCursorPosition(rows,cols);
    }    
    else{
        *cols=ws.ws_col;
        *rows=ws.ws_row;
        return 0;
    }
}

/**row operations***/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    rx++;
  }
  return rx;
}


void editorUpdateRow(erow *row) {
  int tabs = 0;
  free(row->render);
  row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;
  free(row->render);
  row->render = malloc(row->size + tabs*7 + 1);
  int idx = 0;
  
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
    }
    else row->render[idx++] = row->chars[j];
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}


void editorAppendRow(int at,char *row, ssize_t len){
    if (at < 0 || at > E.numrows) return;

    E.row=realloc(E.row,sizeof(erow)*(E.numrows+1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size=len;
    E.row[at].chars=malloc(len+1);
    memcpy(E.row[at].chars,row,len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}


void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at){
    if (at < 0 || at > row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->chars=realloc(row->chars,row->size-1);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

void editorFreeRow(erow *row){
    erow *row1=&E.row[E.cy];
    int len=row->size+row1->size+1;
    row->chars=realloc(row->chars,len);
    memmove(&row->chars[row->size], &row1->chars[0], row1->size);
    E.cx=row->size;
    row->size+=row1->size;
    row->chars[row->size] = '\0';
    E.row=realloc(E.row,sizeof(erow)*(E.numrows-1));
    editorUpdateRow(row);
    E.numrows--;
    E.cy--;
    E.dirty++;
}

/*editor operations*/

void editorDelChar(){
    if (E.cx > 0) {
    editorRowDelChar(&E.row[E.cy],E.cx-1);
    E.cx--;
    }
    if(E.cx==0 && (E.cy!=0 && E.cy!=E.numrows)){
        editorFreeRow(&E.row[E.cy-1]);
    }
}

void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorAppendRow(E.numrows,"", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewLine(){
    if(E.cx==0) editorAppendRow(E.cy,"",0);
    else{
        erow *row=&E.row[E.cy];
        editorAppendRow(E.cy+1,&row->chars[E.cx],row->size-E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx=0;
}

/*** file i/o ***/

char *editorRowsToString(int *buflen){
    int totlen=0;
    for(int j=0;j<E.numrows;j++){
        totlen+=E.row[j].size+1;
    }
    *buflen=totlen;

    char *buf=malloc(totlen);
    char *p=buf;
    for(int j=0;j<E.numrows;j++){
        memcpy(p,E.row[j].chars,E.row[j].size);
        p+=E.row[j].size;
        *p='\n';
        p++;
    }
    return buf; 
}

void editorOpen(char *filename){
    free(E.filename);
    E.filename=strdup(filename);

    FILE *fp =fopen(filename,"r");
    if(!fp) die("fopen");

    char *line=NULL;
    size_t linecap=0;
    ssize_t linelen;

    while((linelen=getline(&line,&linecap,fp))!=-1){
        while(linelen>0 && (line[linelen-1]=='\n' || line[linelen-1]=='\r')){
            linelen--;
        }
        editorAppendRow(E.numrows,line,linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave(){
    if (E.filename == NULL){
        E.filename = editorPrompt("Save as: %s(ESC to cancel)");
    }
    
    if(E.filename==NULL) {
        editorSetStatusMessage("save aborted");
        return;
    }
    int len;
    char *buf=editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT,0644);
    if(fd!=-1){
        if(ftruncate(fd,len)!=-1){ 
            if(write(fd,buf,len)==len){
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return; 
            }   
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

void editorFind(){
    char *query=editorPrompt("Search %s(ESC To Cancel)");
    if(query==NULL) return;
    for(int j=0;)
}

//append buffer
struct abuf{
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab,const char *s, int len){
    char *new=realloc(ab->b, ab->len+len);

    if(new==NULL) return;
    memcpy(&new[ab->len],s,len);
    ab->b=new;
    ab->len+=len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

//output
void editorDrawRows(struct abuf *ab){
    int y;
    for(y=0;y<E.screenrows;y++) {
        int filerow=y+E.rowoff;
        if(filerow>=E.numrows){
            if(E.numrows == 0 && y==E.screenrows/3){
                char welcome[80];
                int welcomelen=sprintf(welcome,"Kilo editor -- version %s", KILO_VERSION);

                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;

                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else abAppend(ab,"~",1);            
        }
        else{
            int len=E.row[filerow].rsize- E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab,&E.row[filerow].render[E.coloff],len);
        }
        abAppend(ab,"\x1b[K", 3);
        abAppend(ab,"\r\n",2);
    }    
}

void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab,"\x1b[7m]",4);
    char status[80];
    char rstatus[60];
    
    int len=snprintf(status,sizeof(status),"%.20s-%d lines %s",E.filename?E.filename:"[NO NAME]",E.numrows,E.dirty?"(modified)":"");
    if(len>E.screencols) len=E.screencols;
    abAppend(ab,status,len);

    int rlen=snprintf(rstatus,sizeof(rstatus),"%d %d",E.cy+1,E.numrows);

    while(len<E.screencols) {
        if(E.screencols-len==rlen){
            abAppend(ab,rstatus,rlen);
            break;
        }
        else abAppend(ab," ",1);
        len++;
    }    
    abAppend(ab,"\x1b[m",3);
    abAppend(ab,"\r\n",2);
}

void editorDrawMessageBar(struct abuf *ab){
    abAppend(ab,"\x1b[K",3);
    int msglen=strlen(E.statusmsg);
    if(msglen>E.screencols) msglen=E.screencols;

    if (msglen && time(NULL) - E.statusmsg_time < 5) abAppend(ab,E.statusmsg,msglen);
}

void editorSetStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap,fmt);

    vsnprintf(E.statusmsg,sizeof(E.statusmsg),fmt,ap);
    va_end(ap);
    E.statusmsg_time=time(NULL);
}

void editorScroll(){
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if(E.cy<E.rowoff) E.rowoff=E.cy;

    if(E.cy>=E.screenrows+  E.rowoff) E.rowoff=E.cy-E.screenrows+1;

    if (E.cx < E.coloff) {
        E.coloff = E.cx;
    }
    if (E.cx >= E.coloff + E.screencols) {
        E.coloff = E.cx - E.screencols + 1;
    }
}

void editorRefreshScreen() {
  editorScroll();
  struct abuf ab=ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);  
  abAppend(&ab,"\x1b[H",3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);
  
  char buf[32];
  snprintf(buf,sizeof(buf),"\x1b[%d;%dH",E.cy - E.rowoff+ 1, E.rx -E.coloff+ 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

//input

char *editorPrompt(char *prompt){
    size_t bufsize=128;
    char *buf;
    buf=malloc(128);
    size_t buflen=0;
    buf[0]='\0';

    while(1){
        editorSetStatusMessage(prompt,buf);
        editorRefreshScreen();
        int c=readKey();

        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        }
        else if(c=='\x1b'){
            free(buf);
            return NULL;
        }
        else if(c=='\r'){
            if(c!=0){
                editorSetStatusMessage("");
                return buf;
            }
        }
        else if(c!=iscntrl(c) && c<128){
            if(buflen==bufsize-1){
                bufsize*=2;
                buf=realloc(buf,bufsize);
            }
            buf[buflen++]=c;
            buf[buflen]='\0';
        }                
    }
}

void editorMoveCursor(int key){
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    if(key==ARROW_LEFT){
        if(E.cx==0 && E.cy>0){
            E.cy--;
            row=&E.row[E.cy];
            E.cx=row->size;
        }
        else E.cx--;
    }
    if(key==ARROW_RIGHT && row && E.cx <= row->size) {
        if(E.cx==row->size){
            if(E.cy !=E.numrows) E.cy++;
            E.cx=0;
        }
        else E.cx++;
    }
    if(key==ARROW_UP && E.cy != 0) E.cy--;
    if(key==ARROW_DOWN && E.cy < E.numrows) E.cy++;
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void processKeypress(){
    static int quit_times=QUIT_TIMES;
    int c=readKey();
    
    if(c==CTRL_KEY('q')){
        if(quit_times>0 && E.dirty!=0){
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
          " Press Ctrl-Q %d more times to quit.", quit_times);
            quit_times--;
        return;
        }
        else{
            write(STDOUT_FILENO, "\x1b[2J",4);
            write(STDOUT_FILENO, "\x1b[H",3);
            exit(0);
        }
    }

    else if(c=='\r') editorInsertNewLine();

    else if(c==CTRL_KEY('s')) editorSave();

    else if(c==CTRL_KEY('l') || c=='\x1b' || c==BACKSPACE || c==CTRL_KEY('h') ||c==DEL_KEY) {
        if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
        editorDelChar();
    }
    else if(c==HOME_KEY){
        E.cx=0;
    }
    else if(c==END_KEY) E.cx=E.screencols-1;

    else if(c==PAGE_UP){
        E.cy=E.rowoff;
    }

    else if(c==PAGE_DOWN){
        E.cy=E.screenrows+E.rowoff-1;
        if (E.cy > E.numrows) E.cy = E.numrows;
    }
    else if(c==ARROW_UP|| c==ARROW_DOWN || c==ARROW_LEFT|| c==ARROW_RIGHT) editorMoveCursor(c);

    else editorInsertChar(c);
    quit_times = QUIT_TIMES;
}

//init
void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.row=NULL;
    E.numrows=0;
    E.rowoff=0;
    E.coloff=0;
    E.filename=NULL;
    E.statusmsg[0]='\0';
    E.statusmsg_time=0;
    E.dirty=0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc,char *argv[]){
    enableRaw();
    initEditor();
    if(argc>=2) editorOpen(argv[1]);

    editorSetStatusMessage("HELP: Ctrl-S = save |Ctrl-Q = quit");

    while(1){
        editorRefreshScreen();
        processKeypress();
    }
    return 0;
}