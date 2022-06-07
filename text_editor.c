// https://viewsourcecode.org/snaptoken/kilo/index.html

/*** includes ***/
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>

/*** defines ***/

#define CTRL_KEY(k) ((k)&0x1f)

/*** data ***/

struct editorConfig
{
  int screenrows;
  int screencols;
  struct termios orig_termios; // global var term width and height
};

struct editorConfig E;

/*** terminal ***/

// error handling
void die(const char *s) // prints error message and exits program
{
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

  tcgetattr(STDIN_FILENO, &raw);                            // read current arg into a struct
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

char editorReadKey()
{
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
  {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }
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

void editorDrawRows(struct abuf *ab)
{
  int y;
  for (y = 0; y < E.screenrows; y++)
  {
    abAppend(ab, "~", 1);

    abAppend(ab, "\x1b[K", 3); // clears each line as we redraw them
    // only if not the last line of the file
    if (y < E.screenrows - 1)
      abAppend(ab, "\r\n", 2);
  }
}

void editorRefreshScreen()
{
  struct abuf ab = ABUF_INIT; // new ab abuf initialization

  abAppend(&ab, "\x1b[?25l", 6); // hides the cursor before drawing
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  abAppend(&ab, "\x1b[H", 3);
  abAppend(&ab, "\x1b[?25l", 6); // show cursor after drawing

  write(STDOUT_FILENO, ab.b, ab.len); // writes the buffer's content
  abFree(&ab);                        // frees the memory used by abuf
}

/*** input ***/

void editorProcessKeypress() // waits for keypress then returns it
{
  char c = editorReadKey();

  switch (c)
  {
  case CTRL_KEY('q'):
    // clear the screen when quitting to avoid errors
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }
}

/*** init ***/

void initEditor()
{
  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main()
{
  enableRawMode(); // raw mode basically processes each keypress as it comes in
  initEditor();

  while (1)
  {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}