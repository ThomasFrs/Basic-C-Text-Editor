// https://viewsourcecode.org/snaptoken/kilo/index.html

/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

/*** defines ***/

#define SEX_VERSION "0.0.1"
#define SEX_TAB_STOP 8

#define CTRL_KEY(k) ((k)&0x1f)

enum editorKey
{
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

/*** data ***/

// erow = editor row
typedef struct erow // data type for storing row of text in editor
{
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

struct editorConfig
{
  int cx, cy; // to keep track of the cursor's position
  int rx;     // index into the render field
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios; // global var term width and height
};

struct editorConfig E;

/*** terminal ***/

// error handling
void die(const char *s) // prints error message and exits program
{
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1); // exit w/ status of 1 (aka failure w/ any non-zero value)
}

void disableRawMode()
{
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr"); // returns -1 when encountering error
}

void enableRawMode()
{
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode); // called automatically when program exits

  struct termios raw = E.orig_termios; // make copy of orig_termios before making change

  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // | disables ctrl+m | | | disables ctrl+s and ctrl+q
  raw.c_oflag &= ~(OPOST);                                  // turns off output processing (\n to \r\n)
  raw.c_cflag |= (CS8);
  // no characters are visible but still typed | disables canonical mode, data read 1 byte at a time
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // disables ctrl+v | disables ctrl+c and ctrl+z
  raw.c_cc[VMIN] = 0;                              // min nb ob B of input before read()
  raw.c_cc[VTIME] = 1;                             // max time to wait before read() returns (100 milliseconds)

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

int editorReadKey()
{
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
  {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b')
  {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[') // for the arrow keys
    {
      if (seq[1] >= '0' && seq[1] <= '9')
      {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';
        if (seq[2] == '~')
        {
          switch (seq[1])
          {
          case '1':
            return HOME_KEY;
          case '2':
            return END_KEY;
          case '3':
            return DEL_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      }
      else
      {
        switch (seq[1])
        {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    }

    else if (seq[0] == 'O')
    {
      switch (seq[1])
      {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }

    return '\x1b';
  }
  else
    return c;
}

int getCursorPosition(int *rows, int *cols)
{
  char buf[32];
  unsigned int i = 0;

  // escape sequence and the actual response
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;
  // first character is not interpreted as an escape sequence
  while (i < sizeof(buf) - 1)
  {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0';
  printf("\r\n&buf[1]: '%s'\r\n", &buf[1]);

  if (buf[0] != '\x1b' || buf[1] != '[') // make sure responded with esc sequence
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) // puts values into rows and cols var
    return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols)
{
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
  {
    // moves cursor to the bottom right and gets columns and rows
    // C (Cursor Forward) command moves cursor to the right
    // B (Cursor Down) command moves the cursor down
    // we use 999 to ensure it reaches to the bottom and the right
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  }
  else
  {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx)
{
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++)
  {
    if (row->chars[j] == '\t')
      rx += (SEX_TAB_STOP - 1) - (rx % SEX_TAB_STOP);
    rx++;
  }

  return rx;
}

void editorUpdateRow(erow *row)
{
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t')
      tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs * (SEX_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++)
  {
    if (row->chars[j] == '\t')
    {
      row->render[idx++] = ' ';
      while (idx % SEX_TAB_STOP != 0)
        row->render[idx++] = ' ';
    }
    else
      row->render[idx++] = row->chars[j];
  }

  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(char *s, size_t len)
{
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename)
{
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1)
  {
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorAppendRow(line, linelen);
  }
  free(line);
  fclose(fp);
}

/*** append buffer ***/
struct abuf // pointer to buffer in memory, and length
{
  char *b;
  int len;
};

// constructor for abuf type, constant representing empty buffer
#define ABUF_INIT \
  {               \
    NULL, 0       \
  }

// we need buffer to generate with write at the same time w/out loops
void abAppend(struct abuf *ab, const char *s, int len)
{
  // to form the new string
  // we need enough memory to append a string s to abuf
  // allocates block of memory w/ size of current string + size of the string we're appending
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len); // copy string s at end of buffer data
  // update pointer and length of the abuf to new values
  ab->b = new;
  ab->len += len;
}

// frees current block of memory for allocation of another block big enough
// basically deallocates dynamic memory used by abuf
void abFree(struct abuf *ab)
{
  free(ab->b);
}

/*** output ***/

void editorScroll()
{
  E.rx = 0;
  if (E.cy < E.numrows)
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

  if (E.cy < E.rowoff)
    E.rowoff = E.cy;
  if (E.cy >= E.rowoff + E.screenrows)
    E.rowoff = E.cy - E.screenrows + 1;
  if (E.rx < E.coloff)
    E.coloff = E.rx;
  if (E.rx >= E.coloff + E.screencols)
    E.coloff = E.rx - E.screencols + 1;
}

void editorDrawRows(struct abuf *ab)
{
  int y;
  for (y = 0; y < E.screenrows; y++)
  {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows)
    {
      if (E.numrows == 0 && y == E.screenrows / 3) // displays welcome message
      {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "Simplified Editor Extended -- version %s",
                                  SEX_VERSION);
        if (welcomelen > E.screencols)
          welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2; // space betwe
        if (padding)
        {
          abAppend(ab, "~", 1);
          padding--;
        }

        while (padding--)
          abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      }
      else
        abAppend(ab, "~", 1);
    }

    else
    {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    abAppend(ab, "\x1b[K", 3); // clears each line as we redraw them
    // only if not the last line of the file
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab)
{
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines", E.filename ? E.filename : "[No Name]", E.numrows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);

  if (len > E.screencols)
    len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols)
  {
    if (E.screencols - len == rlen)
    {
      abAppend(ab, rstatus, rlen);
      break;
    }
    else
    {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorSetStatusMessage(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

void editorDrawMessageBar(struct abuf *ab)
{
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols)
    msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen()
{
  editorScroll();

  struct abuf ab = ABUF_INIT; // new ab abuf initialization

  abAppend(&ab, "\x1b[?25l", 6); // hides the cursor before drawing
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1); // term uses 1-indexed values
  abAppend(&ab, buf, strlen(buf));                                                         // strlen is exact pos wanted for cursor

  abAppend(&ab, "\x1b[?25h", 6); // show cursor after drawing

  write(STDOUT_FILENO, ab.b, ab.len); // writes the buffer's content
  abFree(&ab);                        // frees the memory used by abuf
}

/*** input ***/

void editorMoveCursor(int key)
{
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key)
  {
  case ARROW_LEFT:
    if (E.cx != 0) // not to go past edges
      E.cx--;
    else if (E.cy > 0) // go to previous line if <- at beginning
    {
      E.cy--;
      E.cx = E.row[E.cy].size;
    }
    break;
  case ARROW_RIGHT:
    if (row && E.cx < row->size)
      E.cx++;
    else if (row && E.cx == row->size) // go to next line if -> at end
    {
      E.cy++;
      E.cx = 0;
    }
    break;
  case ARROW_UP:
    if (E.cy != 0)
      E.cy--;
    break;
  case ARROW_DOWN:
    if (E.cy < E.numrows)
      E.cy++;
    break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen)
    E.cx = rowlen;
}

void editorProcessKeypress() // waits for keypress then returns it
{
  int c = editorReadKey();

  switch (c)
  {
  case CTRL_KEY('q'):
    // clear the screen when quitting to avoid errors
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;

  case HOME_KEY:
    E.cx = 0;
    break;

  case END_KEY:
    if (E.cy < E.numrows)
      E.cx = E.row[E.cy].size;
    break;

  case PAGE_UP:
  case PAGE_DOWN:
  {
    if (c == PAGE_UP)
      E.cy = E.rowoff;
    else if (c == PAGE_DOWN)
    {
      E.cy = E.rowoff + E.screenrows - 1;
      if (E.cy > E.numrows)
        E.cy = E.numrows;
    }

    int times = E.screenrows;
    while (times--)
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
  }
  break;

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;
  }
}

/*** init ***/

void initEditor()
{
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0; // 1 or 0 for now cuz only 1 line of text
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
  E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
  enableRawMode(); // raw mode basically processes each keypress as it comes in
  initEditor();
  if (argc >= 2)
    editorOpen(argv[1]);

  editorSetStatusMessage("HELP: Ctrl-Q = quit");

  while (1)
  {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}