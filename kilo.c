/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define ENTER_KEY(k) ((k) == 0xa) || ((k) == 0xd)
#define ST_NORMAL 0
#define ST_INSERT 1
#define ST_VISUAL 2
#define CB_LEN 10
#define DEBUG 0

enum editorKey {
  DEL_KEY = 1000,
};

/*** data ***/

const char *ST_DISPLAY[] = { "NORMAL", "INSERT", "VISUAL" };

typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  unsigned int state;
  char cb[CB_LEN];
  unsigned int cb_i;
  int cx, cy;
  int coloff;
  int rowoff;
  int prevColoff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  struct termios orig_termios;
};
struct editorConfig E;

int clearControlBuffer() {
  E.cb_i = 0;
  E.cb[0] = '\0';
  return 0;
}

/*** terminal ***/

void clearScreen() {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
}

void die(const char *s) {
  for (int i = 0; i < E.numrows; i++) free(E.row[i].chars);
  clearScreen();
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);
  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }
  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    if (seq[1] >= '0' && seq[1] <= '9') {
      if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
      if (seq[2] == '~') {
        switch (seq[1]) {
          case '3': return DEL_KEY;
        }
      }
    }
    return '\x1b';
  }
  return c;
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
  unsigned int i = 0;
  for (; i < sizeof(buf) && read(STDIN_FILENO, &buf[i], 1) == 1 && buf[i] != 'R'; i++);
  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");
  char *line = NULL;
  ssize_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
}

/*** append buffer ***/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

void editorScroll() {
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  } else if (E.cy >= E.rowoff + E.screenrows - 2) {
    E.rowoff = E.cy - E.screenrows + 3;
  }
  if (E.cx < E.coloff) {
    E.coloff = E.cx;
  } else if (E.cx >= E.coloff + E.screencols) {
    E.coloff = E.cx - E.screencols + 1;
  }
}

void editorDrawControl(struct abuf *ab) {
  abAppend(ab, "-- ", 3);
  abAppend(ab, ST_DISPLAY[E.state], strlen(ST_DISPLAY[E.state]));
  abAppend(ab, " --\x1b[K\r\n", 8);
  if (E.cb[0] == ':') abAppend(ab, E.cb, E.cb_i);
  abAppend(ab, "\x1b[K", 3);
}

void editorDrawRows(struct abuf *ab) {
  for (int i = 0; i < E.screenrows - 2; i++) {
    int filerow = i + E.rowoff;
    if (filerow < E.numrows) {
      int len = E.row[filerow].size - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, &E.row[filerow].chars[E.coloff], len);
    } else if (E.numrows == 0 && i == E.screenrows / 3) {
      char welcome[80];
      int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
      if (welcomelen > E.screencols) welcomelen = E.screencols;
      int padding = (E.screencols - welcomelen) / 2;
      if (padding) {
        abAppend(ab, "~", 1);
        padding--;
      }
      while (padding--) abAppend(ab, " ", 1);
      abAppend(ab, welcome, welcomelen);
    } else  {
      abAppend(ab, "~", 1);
    }
    abAppend(ab, "\x1b[K\r\n", 5);
  }
  editorDrawControl(ab);
}

void editorRefreshScreen() {
  editorScroll();
  struct abuf ab = ABUF_INIT;
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);
  editorDrawRows(&ab);
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.cx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));
  abAppend(&ab, "\x1b[?25h", 6);
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

void adjustPrevCol(erow *prevRow) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (prevRow->size < rowlen && prevRow->size < E.prevColoff) {
    if (rowlen > E.prevColoff) E.cx = E.prevColoff;
    else E.cx = rowlen - 1;
  } else if (E.cx >= rowlen) {
    E.cx = rowlen - 1;
  }
}

int editorProcessNormalKeypress(int c) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  // Add to control buffer
  if (E.cb_i < CB_LEN - 1) {
    E.cb[E.cb_i++] = c;
    E.cb[E.cb_i] = '\0';
  }
  switch (E.cb[0]) {
    case 'g': {
      switch (E.cb[1]) {
        case 'g':
          E.cy = 0;
          adjustPrevCol(row);
          return clearControlBuffer();
      }
      break;
    }
    case 'G':
      E.cy = E.numrows - 1;
      adjustPrevCol(row);
      return clearControlBuffer();
    case 'h':
      if (E.cx > 0) {
        E.cx--;
        E.prevColoff = E.cx;
      }
      return clearControlBuffer();
    case 'i':
      E.state = ST_INSERT;
      return clearControlBuffer();
    case 'j':
      if (E.cy < E.numrows - 1) E.cy++;
      adjustPrevCol(row);
      return clearControlBuffer();
    case 'k':
      if (E.cy > 0) E.cy--;
      adjustPrevCol(row);
      return clearControlBuffer();
    case 'l':
      if (row && E.cx < row->size) {
        E.cx++;
        E.prevColoff = E.cx;
      }
      return clearControlBuffer();
  }
  switch (E.cb[E.cb_i - 1]) { 
    case ':':
      E.cb_i = 1;
      E.cb[0] = ':';
      E.cb[1] = '\0';
      return 0;
    case '\n':
    case '\r':
      if (E.cb[0] == ':') {
        if (E.cb[1] == 'q') {
          clearScreen();
          exit(0);
        }
      }
      return clearControlBuffer();
  }
  if (E.cb_i == CB_LEN - 1) return clearControlBuffer();
  return 1;
}

void editorProcessKeypress() {
  int c = editorReadKey();
  switch (E.state) {
    case ST_NORMAL:
      editorProcessNormalKeypress(c);
      break;
  }
  if (c == '\x1b' || c == CTRL_KEY('C') || c == CTRL_KEY('[')) E.state = ST_NORMAL;
  /*if (CTRL_KEY('q')) {
    clearScreen();
    exit(0);
  }*/
}

/*** debug ***/

void debugPrintKey(int c) {
  printf("%d ('%c')\r\n", c, c);
  if (c == 'q') exit(0);
  editorReadKey();
}
  
/*** init ***/

void initEditor() {
  enableRawMode();
  E.state = ST_NORMAL;
  clearControlBuffer();
  E.cx = 0;
  E.cy = 0;
  E.coloff = 0;
  E.rowoff = 0;
  E.prevColoff = 0;
  E.numrows = 0;
  E.row = NULL;
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }
  while (1) {
    if (DEBUG) {
      int c = editorReadKey();
      debugPrintKey(c);
    } else {
      editorRefreshScreen();
      editorProcessKeypress();
    }
  }
  return 0;
}
