// https://viewsourcecode.org/snaptoken/kilo/index.html

/*** includes ***/
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>

/*** data ***/

struct termios orig_termios; // global var for og term attr

/*** terminal ***/

// error handling
void die(const char *s) // prints error message and exits program
{
  perror(s);
  exit(1); // exit w/ status of 1 (aka failure w/ any non-zero valure)
}

void disableRawMode()
{
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
    atexit(disableRawMode); // returns -1 when encountering error
}

void enableRawMode()
{
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode); // called automatically when program exits

  struct termios raw = orig_termios; // make copy of orig_termios before making change

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

/*** init ***/

int main()
{
  enableRawMode(); // raw mode basically processes each keypress as it comes in

  while (1)
  {
    char c = '\0';
    // reads 1 byte from input c at a time
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
      die("read");
    if (iscntrl(c))                  // tests if the char is a control char (ascii 0-31 and ascii 127)
      printf("%d\r\n", c);           // control chars can't be printed on screen
    else                             // prints ascii value of char (%d) and char
      printf("%d ('%c')\r\n", c, c); // other ascii chars are all printable
    if (c == 'q')                    // input q to quit
      break;
  }
  return 0;
}