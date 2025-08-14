// kilo_win.c — минималистичный текстовый редактор под Windows Console API
// Без POSIX: никакого termios/ioctl. Работает в обычном cmd/PowerShell.
// Включает виртуальные терминальные последовательности (ANSI) в консоли Windows 10+
// Компиляция (MinGW-w64):
//   gcc -std=c99 -Wall -Wextra -O2 -o kilo.exe kilo_win.c
// Запуск:
//   kilo.exe [файл]
// Управление:
//   Стрелки/Home/End/PageUp/PageDown — перемещение
//   Ctrl-S — сохранить (спросит имя, если нет)
//   Ctrl-Q — выход (просит подтвердить при несохранённых)
//   Ctrl-F — поиск (ESC — выйти, стрелки — след./пред.)
//   Backspace/Delete/Enter/печать — редактирование

#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0A00 // Windows 10

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>

/* ============================ Константы ============================= */

#define KILO_VERSION "win-0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 2

#define CTRL_KEY(k) ((k) & 0x1f)

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

enum editorHighlight {
  HL_NORMAL = 0,
  HL_COMMENT,
  HL_MLCOMMENT,
  HL_KEYWORD1,
  HL_KEYWORD2,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/* ============================ Структуры ============================= */

struct editorSyntax {
  char *filetype;
  char **filematch;
  char **keywords;
  char *singleline_comment_start;
  char *multiline_comment_start;
  char *multiline_comment_end;
  int flags;
};

typedef struct erow {
  int idx;
  int size;
  int rsize;
  char *chars;
  char *render;
  unsigned char *hl;
  bool hl_open_comment;
} erow;

struct editorConfig {
  int cx, cy;          // курсор (chars)
  int rx;              // курсор (render)
  int rowoff;          // вертикальный скролл
  int coloff;          // горизонтальный скролл
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;
  char *filename;
  char statusmsg[160];
  time_t statusmsg_time;
  struct editorSyntax *syntax;
  // WinAPI
  HANDLE hIn, hOut;
  DWORD inOrigMode, outOrigMode;
};

static struct editorConfig E;

/* ============================ Утилиты =============================== */

static void ewrite(const void *s, size_t n) {
  fwrite(s, 1, n, stdout);
}

static void ewrites(const char *s) { ewrite(s, strlen(s)); }

static void die(const char *msg) {
  ewrites("\x1b[2J\x1b[H");
  fprintf(stderr, "%s\n", msg);
  ExitProcess(1);
}

static void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/* ============================ Терминал ============================== */

static void disableRawMode(void) {
  if (E.hIn) SetConsoleMode(E.hIn, E.inOrigMode);
  if (E.hOut) SetConsoleMode(E.hOut, E.outOrigMode);
}

static void enableRawMode(void) {
  E.hIn = GetStdHandle(STD_INPUT_HANDLE);
  E.hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!E.hIn || !E.hOut) die("GetStdHandle failed");

  if (!GetConsoleMode(E.hIn, &E.inOrigMode)) die("GetConsoleMode(in)");
  if (!GetConsoleMode(E.hOut, &E.outOrigMode)) die("GetConsoleMode(out)");

  DWORD in = E.inOrigMode;
  in &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE);
  in |= ENABLE_VIRTUAL_TERMINAL_INPUT; // стрелки как ESC [ A
  if (!SetConsoleMode(E.hIn, in)) die("SetConsoleMode(in)");

  DWORD out = E.outOrigMode;
  out |= ENABLE_VIRTUAL_TERMINAL_PROCESSING; // ANSI-escape
  out &= ~(DISABLE_NEWLINE_AUTO_RETURN);     // на всякий случай
  if (!SetConsoleMode(E.hOut, out)) die("SetConsoleMode(out)");

  atexit(disableRawMode);
}

static int editorReadKey(void) {
  DWORD n = 0; char c;
  while (1) {
    if (!ReadFile(E.hIn, &c, 1, &n, NULL)) die("ReadFile");
    if (n == 1) break;
  }

  if (c == '\x1b') {
    char seq[3]; DWORD m;
    if (!ReadFile(E.hIn, &seq[0], 1, &m, NULL) || m != 1) return '\x1b';
    if (!ReadFile(E.hIn, &seq[1], 1, &m, NULL) || m != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (!ReadFile(E.hIn, &seq[2], 1, &m, NULL) || m != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
    return '\x1b';
  }
  // Backspace в Windows часто приходит как 8
  if (c == 8) return BACKSPACE;
  return (unsigned char)c;
}

static int getWindowSize(int *rows, int *cols) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(E.hOut, &info)) return -1;
  *cols = info.srWindow.Right - info.srWindow.Left + 1;
  *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  return 0;
}

/* ========================= Синтакс-подсветка ======================== */

static bool is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];{}", c) != NULL;
}

static char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
static char *C_HL_keywords[] = {
  // управляющие
  "switch", "if", "while", "for", "break", "continue", "return", "else",
  "struct", "union", "typedef", "static", "enum", "class", "case", "default",
  // типы (KEYWORD2)
  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", "short|", "size_t|", "ssize_t|", "const|", "volatile|", NULL
};

static struct editorSyntax HLDB[] = {
  {
    .filetype = "c",
    .filematch = C_HL_extensions,
    .keywords = C_HL_keywords,
    .singleline_comment_start = "//",
    .multiline_comment_start = "/*",
    .multiline_comment_end = "*/",
    .flags = HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
  },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

static void editorUpdateSyntax(erow *row);

static int editorSyntaxToColor(int hl) {
  switch (hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 36; // cyan
    case HL_KEYWORD1: return 33;  // yellow
    case HL_KEYWORD2: return 32;  // green
    case HL_STRING: return 35;    // magenta
    case HL_NUMBER: return 31;    // red
    case HL_MATCH: return 34;     // blue
    default: return 39;           // default
  }
}

static void editorSelectSyntaxHighlight(void) {
  E.syntax = NULL;
  if (!E.filename) return;
  char *ext = strrchr(E.filename, '.');
  for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    struct editorSyntax *s = &HLDB[j];
    for (unsigned int i = 0; s->filematch[i]; i++) {
      int is_ext = (s->filematch[i][0] == '.');
      if ((is_ext && ext && _stricmp(ext, s->filematch[i]) == 0) ||
          (!is_ext && strstr(E.filename, s->filematch[i]))) {
        E.syntax = s;
        for (int r = 0; r < E.numrows; r++) editorUpdateSyntax(&E.row[r]);
        return;
      }
    }
  }
}

static void editorUpdateSyntax(erow *row) {
  row->hl = (unsigned char*)realloc(row->hl, row->rsize);
  memset(row->hl, HL_NORMAL, row->rsize);
  if (!E.syntax) return;

  char **keywords = E.syntax->keywords;
  char *scs = E.syntax->singleline_comment_start;
  char *mcs = E.syntax->multiline_comment_start;
  char *mce = E.syntax->multiline_comment_end;

  int scs_len = scs ? (int)strlen(scs) : 0;
  int mcs_len = mcs ? (int)strlen(mcs) : 0;
  int mce_len = mce ? (int)strlen(mce) : 0;

  int prev_sep = 1;
  int in_string = 0;
  int in_comment = row->hl_open_comment;

  for (int i = 0; i < row->rsize; i++) {
    char c = row->render[i];
    unsigned char prev_hl = (i > 0) ? row->hl[i-1] : HL_NORMAL;

    if (scs_len && !in_string && !in_comment) {
      if (!strncmp(&row->render[i], scs, scs_len)) {
        memset(&row->hl[i], HL_COMMENT, row->rsize - i);
        break;
      }
    }

    if (mcs_len && mce_len && !in_string) {
      if (in_comment) {
        row->hl[i] = HL_MLCOMMENT;
        if (!strncmp(&row->render[i], mce, mce_len)) {
          for (int j = 0; j < mce_len; j++) row->hl[i + j] = HL_MLCOMMENT;
          i += mce_len - 1;
          in_comment = 0; prev_sep = 1; continue;
        } else { continue; }
      } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
        for (int j = 0; j < mcs_len; j++) row->hl[i + j] = HL_MLCOMMENT;
        i += mcs_len - 1; in_comment = 1; continue;
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (in_string) {
        row->hl[i] = HL_STRING;
        if (c == '\\' && i + 1 < row->rsize) { row->hl[i+1] = HL_STRING; i += 2; continue; }
        if (c == in_string) in_string = 0;
        prev_sep = 1; continue;
      } else {
        if (c == '"' || c == '\'') { in_string = c; row->hl[i] = HL_STRING; continue; }
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if ((isdigit((unsigned char)c) && (prev_sep || prev_hl == HL_NUMBER)) ||
          (c == '.' && prev_hl == HL_NUMBER)) {
        row->hl[i] = HL_NUMBER; prev_sep = 0; continue;
      }
    }

    if (prev_sep) {
      for (int j = 0; keywords[j]; j++) {
        int klen = (int)strlen(keywords[j]);
        int kw2 = keywords[j][klen-1] == '|';
        if (kw2) klen--;
        if (!strncmp(&row->render[i], keywords[j], klen) && is_separator(row->render[i + klen])) {
          memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
          i += klen - 1; break;
        }
      }
    }

    prev_sep = is_separator(c);
  }

  int changed = (row->hl_open_comment != in_comment);
  row->hl_open_comment = in_comment;
  if (changed && row->idx + 1 < E.numrows) editorUpdateSyntax(&E.row[row->idx + 1]);
}

/* ============================ Строки ================================ */

static int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  for (int j = 0; j < cx; j++) {
    if (row->chars[j] == '\t') rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    rx++;
  }
  return rx;
}

static int editorRowRxToCx(erow *row, int rx) {
  int cur_rx = 0, cx;
  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t') cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
    cur_rx++;
    if (cur_rx > rx) return cx;
  }
  return cx;
}

static void editorUpdateRow(erow *row) {
  int tabs = 0;
  for (int j = 0; j < row->size; j++) if (row->chars[j] == '\t') tabs++;
  free(row->render);
  row->render = (char*)malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);
  int idx = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
  editorUpdateSyntax(row);
}

static void editorInsertRow(int at, const char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  E.row = (erow*)realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
  for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

  E.row[at].idx = at;
  E.row[at].size = (int)len;
  E.row[at].chars = (char*)malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;
  E.row[at].hl_open_comment = false;
  editorUpdateRow(&E.row[at]);
  E.numrows++;
  E.dirty++;
}

static void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

static void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
  E.numrows--;
  E.dirty++;
}

static void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = (char*)realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = (char)c;
  editorUpdateRow(row);
  E.dirty++;
}

static void editorRowAppendString(erow *row, const char *s, size_t len) {
  row->chars = (char*)realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += (int)len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

static void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/* ============================ Редактирование ======================== */

static void editorInsertChar(int c) {
  if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

static void editorInsertNewline(void) {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cy++; E.cx = 0;
}

static void editorDelChar(void) {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;
  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/* ============================ Файл I/O ============================== */

static char *editorRowsToString(int *buflen) {
  int totlen = 0;
  for (int j = 0; j < E.numrows; j++) totlen += E.row[j].size + 1;
  *buflen = totlen;
  char *buf = (char*)malloc(totlen);
  int p = 0;
  for (int j = 0; j < E.numrows; j++) {
    memcpy(&buf[p], E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    buf[p++] = '\n';
  }
  return buf;
}

static void editorOpen(const char *filename) {
  free(E.filename);
  E.filename = _strdup(filename);
  editorSelectSyntaxHighlight();

  FILE *fp = fopen(filename, "rb");
  if (!fp) return; // новый файл

  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz < 0) { fclose(fp); return; }
  char *data = (char*)malloc((size_t)sz + 1);
  if (!data) { fclose(fp); return; }
  size_t rd = fread(data, 1, (size_t)sz, fp);
  fclose(fp);
  data[rd] = '\0';

  // Разбиваем по "\n" (поддержим CRLF)
  char *start = data;
  for (size_t i = 0; i < rd; i++) {
    if (data[i] == '\n') {
      size_t len = &data[i] - start;
      if (len && start[len-1] == '\r') len--; // CRLF
      editorInsertRow(E.numrows, start, len);
      start = &data[i+1];
    }
  }
  if (start < data + rd) {
    size_t len = data + rd - start;
    if (len && start[len-1] == '\r') len--;
    editorInsertRow(E.numrows, start, len);
  }
  free(data);
  E.dirty = 0;
}

static void editorSave(void) {
  if (!E.filename) {
    // Мини prompt имени файла
    editorSetStatusMessage("Save as: (ESC cancel)");
    size_t cap = 0; char *buf = NULL; size_t len = 0;
    while (1) {
      int c = editorReadKey();
      if (c == '\x1b') { editorSetStatusMessage("Сохранение отменено"); free(buf); return; }
      if (c == '\r') break;
      if (!isprint(c) && c != ' ' && c != '_' && c != '-' && c != '.') continue;
      if (len + 1 >= cap) { cap = cap ? cap * 2 : 32; buf = (char*)realloc(buf, cap); }
      buf[len++] = (char)c; buf[len] = '\0';
      editorSetStatusMessage("Save as: %s", buf);
    }
    if (!buf || !buf[0]) { editorSetStatusMessage("Сохранение отменено"); free(buf); return; }
    E.filename = buf; // владение переходит в E
    editorSelectSyntaxHighlight();
  }

  int len;
  char *buf = editorRowsToString(&len);
  FILE *fp = fopen(E.filename, "wb");
  if (!fp) { free(buf); editorSetStatusMessage("Не удалось сохранить: %s", E.filename); return; }
  size_t wr = fwrite(buf, 1, (size_t)len, fp);
  fclose(fp);
  free(buf);
  if ((int)wr == len) { E.dirty = 0; editorSetStatusMessage("%d bytes written", len); }
  else { editorSetStatusMessage("Ошибка записи"); }
}

/* ============================== Поиск =============================== */

static char *editorPrompt(const char *prompt, void (*callback)(const char *, int));

static void editorFindCallback(const char *query, int key) {
  static int saved_hl_line = -1;
  static unsigned char *saved_hl = NULL;

  if (saved_hl) {
    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl); saved_hl = NULL;
  }

  if (key == '\r' || key == '\x1b' || key == CTRL_KEY('g')) { saved_hl_line = -1; return; }

  static int last_match = -1; static int direction = 1;
  if (key == ARROW_RIGHT || key == ARROW_DOWN) direction = 1;
  else if (key == ARROW_LEFT || key == ARROW_UP) direction = -1;
  else { last_match = -1; direction = 1; }

  if (last_match == -1) direction = 1;
  int current = last_match;

  for (int i = 0; i < E.numrows; i++) {
    current += direction;
    if (current == -1) current = E.numrows - 1;
    else if (current == E.numrows) current = 0;

    erow *row = &E.row[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      E.cy = current;
      E.cx = editorRowRxToCx(row, (int)(match - row->render));
      E.rowoff = E.numrows;

      saved_hl_line = current;
      saved_hl = (unsigned char*)malloc(row->rsize);
      memcpy(saved_hl, row->hl, row->rsize);
      memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
      break;
    }
  }
}

static char *editorPrompt(const char *prompt, void (*callback)(const char *, int)) {
  size_t bufsize = 128; char *buf = (char*)malloc(bufsize); size_t buflen = 0; buf[0] = '\0';
  while (1) {
    editorSetStatusMessage(prompt, buf);
    // перерисуем экран перед чтением
    // произойдет в основном цикле
    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) { if (buflen) buf[--buflen] = '\0'; }
    else if (c == '\x1b') { editorSetStatusMessage(""); if (callback) callback(buf, c); free(buf); return NULL; }
    else if (c == '\r') { if (buflen) { editorSetStatusMessage(""); if (callback) callback(buf, c); return buf; } }
    else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) { bufsize *= 2; buf = (char*)realloc(buf, bufsize); }
      buf[buflen++] = (char)c; buf[buflen] = '\0';
    }
    if (callback) callback(buf, c);
  }
}

/* ============================ Вывод ================================ */

typedef struct abuf { char *b; size_t len; } abuf;
#define ABUF_INIT {NULL, 0}

static void abAppend(abuf *ab, const char *s, size_t len) {
  char *newb = (char*)realloc(ab->b, ab->len + len);
  if (!newb) return; memcpy(&newb[ab->len], s, len); ab->b = newb; ab->len += len;
}
static void abFree(abuf *ab) { free(ab->b); }

static void editorScroll(void) {
  E.rx = 0; if (E.cy < E.numrows) E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  if (E.cy < E.rowoff) E.rowoff = E.cy;
  if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
  if (E.rx < E.coloff) E.coloff = E.rx;
  if (E.rx >= E.coloff + E.screencols) E.coloff = E.rx - E.screencols + 1;
}

static void editorDrawRows(abuf *ab) {
  for (int y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows/3) {
        char welcome[120];
        int wl = snprintf(welcome, sizeof(welcome), "Kilo (Windows) -- version %s", KILO_VERSION);
        if (wl > E.screencols) wl = E.screencols;
        int padding = (E.screencols - wl) / 2;
        if (padding) { abAppend(ab, "~", 1); padding--; }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, wl);
      } else abAppend(ab, "~", 1);
    } else {
      int len = E.row[filerow].rsize - E.coloff; if (len < 0) len = 0; if (len > E.screencols) len = E.screencols;
      char *c = &E.row[filerow].render[E.coloff];
      unsigned char *hl = &E.row[filerow].hl[E.coloff];
      int current_color = -1;
      for (int j = 0; j < len; j++) {
        if (iscntrl((unsigned char)c[j])) {
          char sym = (c[j] <= 26) ? '@' + c[j] : '?';
          abAppend(ab, "\x1b[7m", 4); abAppend(ab, &sym, 1); abAppend(ab, "\x1b[m", 3);
          if (current_color != -1) { char buf[16]; int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color); abAppend(ab, buf, (size_t)clen); }
        } else if (hl[j] == HL_NORMAL) {
          if (current_color != -1) { abAppend(ab, "\x1b[39m", 5); current_color = -1; }
          abAppend(ab, &c[j], 1);
        } else {
          int color = editorSyntaxToColor(hl[j]);
          if (color != current_color) { current_color = color; char buf[16]; int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color); abAppend(ab, buf, (size_t)clen); }
          abAppend(ab, &c[j], 1);
        }
      }
      abAppend(ab, "\x1b[39m", 5);
    }
    abAppend(ab, "\x1b[K\r\n", 5);
  }
}

static void editorDrawStatusBar(abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[120], rstatus[120];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                      E.syntax ? E.syntax->filetype : "no ft",
                      E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) { abAppend(ab, rstatus, rlen); break; }
    else { abAppend(ab, " ", 1); len++; }
  }
  abAppend(ab, "\x1b[m\r\n", 6);
}

static void editorDrawMessageBar(abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = (int)strlen(E.statusmsg); if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5) abAppend(ab, E.statusmsg, (size_t)msglen);
}

static void editorRefreshScreen(void) {
  editorScroll();
  abuf ab = ABUF_INIT;
  abAppend(&ab, "\x1b[?25l\x1b[H", 10);
  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));
  abAppend(&ab, "\x1b[?25h", 6);
  ewrite(ab.b, ab.len); fflush(stdout);
  abFree(&ab);
}

/* ========================= Обработка клавиш ========================= */

static void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) E.cx--; else if (E.cy > 0) { E.cy--; E.cx = E.row[E.cy].size; }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) E.cx++; else if (row && E.cx == row->size) { E.cy++; E.cx = 0; }
      break;
    case ARROW_UP: if (E.cy != 0) E.cy--; break;
    case ARROW_DOWN: if (E.cy < E.numrows) E.cy++; break;
  }
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0; if (E.cx > rowlen) E.cx = rowlen;
}

static void editorProcessKeypress(void) {
  static int quit_times = KILO_QUIT_TIMES;
  int c = editorReadKey();
  switch (c) {
    case '\r': editorInsertNewline(); break;
    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) { editorSetStatusMessage("Есть несохранённые изменения — Ctrl-Q ещё %d", quit_times); quit_times--; return; }
      ewrites("\x1b[2J\x1b[H"); exit(0);
    case CTRL_KEY('s'): editorSave(); break;
    case CTRL_KEY('f'): { char *q = editorPrompt("Поиск: %s (ESC отмена, стрелки — след./пред.)", editorFindCallback); if (q) free(q); } break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;
    case CTRL_KEY('l'):
    case '\x1b': break; // ignore
    case HOME_KEY: E.cx = 0; break;
    case END_KEY: if (E.cy < E.numrows) E.cx = E.row[E.cy].size; break;
    case PAGE_UP:
    case PAGE_DOWN: {
      if (c == PAGE_UP) E.cy = E.rowoff; else { E.cy = E.rowoff + E.screenrows - 1; if (E.cy > E.numrows) E.cy = E.numrows; }
      int times = E.screenrows; while (times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN); break;
    }
    case ARROW_UP: case ARROW_DOWN: case ARROW_LEFT: case ARROW_RIGHT:
      editorMoveCursor(c); break;
    default:
      if (!iscntrl(c) && c < 128) editorInsertChar(c);
      break;
  }
  quit_times = KILO_QUIT_TIMES;
}

/* ============================== Инициализация ======================= */

static void initEditor(void) {
  E.cx = E.cy = E.rx = 0; E.rowoff = E.coloff = 0; E.numrows = 0; E.row = NULL; E.dirty = 0; E.filename = NULL; E.statusmsg[0] = '\0'; E.statusmsg_time = 0; E.syntax = NULL;
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2;
}

int main(int argc, char **argv) {
  enableRawMode();
  initEditor();
  if (argc >= 2) editorOpen(argv[1]);
  editorSetStatusMessage("HELP: Ctrl-S=save | Ctrl-Q=quit | Ctrl-F=find");
  while (1) { editorRefreshScreen(); editorProcessKeypress(); }
}
