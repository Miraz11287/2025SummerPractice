# Документация к текстовому редактору на C (Windows Console API)

## 1. Общее описание

Этот проект — минималистичный текстовый редактор, вдохновлённый [kilo](https://viewsourcecode.org/snaptoken/kilo/), но переписанный под Windows Console API.

Редактор:
- Работает в обычном `cmd` или PowerShell.
- Поддерживает редактирование текста, поиск, сохранение.
- Имеет подсветку синтаксиса для C/C++.
- Не использует POSIX-заголовки (`termios`, `ioctl`), всё через WinAPI.

## 2. Возможности

- Перемещение курсора по файлу.
- Редактирование текста.
- Сохранение изменений (`Ctrl-S`).
- Поиск текста в документе (`Ctrl-F`).
- Выход с предупреждением при несохранённых изменениях (`Ctrl-Q`).
- Подсветка синтаксиса для C.

## 3. Горячие клавиши

| Клавиша       | Действие                               |
|---------------|----------------------------------------|
| **Ctrl-S**    | Сохранить файл                         |
| **Ctrl-Q**    | Выйти из редактора                     |
| **Ctrl-F**    | Найти текст                            |
| Стрелки       | Перемещение курсора                    |
| Home / End    | Перейти в начало/конец строки          |
| PageUp/Down   | Прокрутка экрана                       |
| Enter         | Перенос строки                         |
| Backspace     | Удалить символ слева                   |
| Delete        | Удалить символ справа                  |

---

## 4. Структура кода

Редактор состоит из таких частей:

1. **Константы и структуры**  
   - Настройки редактора (размер табуляции, количество подтверждений для выхода).
   - Перечисления для кодов клавиш и типов подсветки.
   - Структуры `erow` (строка файла) и `editorConfig` (всё состояние редактора).

2. **Утилиты**  
   - `die()` — аварийный выход с очисткой экрана.
   - `editorSetStatusMessage()` — установка текста в статус-бар.

3. **Работа с консолью (WinAPI)**  
   - `enableRawMode()` — включение сырого режима, чтобы ввод шёл посимвольно.
   - `disableRawMode()` — восстановление настроек при выходе.
   - `editorReadKey()` — чтение клавиш с обработкой ESC-последовательностей.
   - `getWindowSize()` — размер консоли.

4. **Подсветка синтаксиса**  
   - `HLDB` — массив правил для разных типов файлов (здесь C/C++).
   - `editorUpdateSyntax()` — алгоритм разбора строки и установки цветов.

5. **Работа со строками**  
   - Вставка, удаление строк.
   - Обновление визуального представления (`render`) с учётом табуляций.

6. **Редактирование**  
   - `editorInsertChar()` — вставка символа.
   - `editorInsertNewline()` — перенос строки.
   - `editorDelChar()` — удаление символа или слияние строк.

7. **Файл I/O**  
   - `editorOpen()` — загрузка файла.
   - `editorSave()` — сохранение файла.

8. **Поиск**  
   - `editorPrompt()` — запрос ввода от пользователя.
   - `editorFindCallback()` — поиск совпадений.

9. **Вывод**  
   - `editorDrawRows()` — отрисовка содержимого файла.
   - `editorDrawStatusBar()` — статусная строка.
   - `editorRefreshScreen()` — обновление экрана.

10. **Обработка клавиш**  
    - `editorProcessKeypress()` — реакция на клавиши (перемещение, сохранение, поиск и т.д.).

11. **Главный цикл**  
    - Настройка консоли.
    - Основной цикл: перерисовка + обработка ввода.

---

## 5. Ключевые функции и логика

### 5.1. Инициализация

```c
static void initEditor(void) {
    E.cx = E.cy = E.rx = 0;
    E.rowoff = E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    E.screenrows -= 2;
}
```
- Обнуляем курсор, скролл, список строк.
- Получаем размер окна и оставляем 2 строки для статус-бара.

### 5.2. Включение сырого режима
``` c
static void enableRawMode(void) {
    E.hIn = GetStdHandle(STD_INPUT_HANDLE);
    E.hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    GetConsoleMode(E.hIn, &E.inOrigMode);
    GetConsoleMode(E.hOut, &E.outOrigMode);

    DWORD in = E.inOrigMode;
    in &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE);
    in |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(E.hIn, in);

    DWORD out = E.outOrigMode;
    out |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(E.hOut, out);

    atexit(disableRawMode);
}
```
- Отключаем эхо и построчный ввод.
- Включаем режим ANSI-последовательностей.
- Запоминаем исходные настройки, чтобы вернуть при выходе.

### 5.3. Чтение клавиш
``` c 
static int editorReadKey(void) {
    DWORD n = 0; char c;
    while (1) {
        ReadFile(E.hIn, &c, 1, &n, NULL);
        if (n == 1) break;
    }
    if (c == '\x1b') { /* обработка ESC-последовательностей */ }
    if (c == 8) return BACKSPACE;
    return (unsigned char)c;
}
```
- Читаем один байт.
- Если это ESC, читаем дополнительно, чтобы понять стрелки и спецклавиши.

### 5.4. Обновление подсветки
```
static void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);
    /* Логика проверки ключевых слов, комментариев, строк, чисел */
}
```
- Для каждой позиции строки выбирается цвет в зависимости от содержимого.

- Подсветка хранится отдельно от текста.

### 5.5. Вставка символа
```c 
static void editorInsertChar(int c) {
    if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}
```
- Если курсор в конце файла, создаётся новая пустая строка.
- Символ вставляется в текущую позицию.

### 5.6. Сохранение файла
```c 
static void editorSave(void) {
    int len;
    char *buf = editorRowsToString(&len);
    FILE *fp = fopen(E.filename, "wb");
    fwrite(buf, 1, len, fp);
    fclose(fp);
    free(buf);
    E.dirty = 0;
}
```
- Собирает все строки в единый буфер.
- Записывает его в файл.

### 5.7 Вывод на экран
```c 
static void editorRefreshScreen(void) {
    editorScroll();
    abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l\x1b[H", 10); // Скрыть курсор, в начало
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6); // Показать курсор
    fwrite(ab.b, 1, ab.len, stdout);
    abFree(&ab);
}
```
- Формирует весь экран в памяти.
- Использует ANSI-команды для управления курсором и цветами.
- Выводит весь буфер одним разом.

# 6. Пример запуска
Компиляция:
``` bash
gcc -std=c99 -Wall -Wextra -O2 -o kilo.exe textEditor.c
```
Запуск:
```bash
./kilo.exe             # пустой буфер
./kilo.exe main.c      # открыть файл main.c
```
# 7. Возможные улучшения
- Подсветка для других языков.
- Поддержка UTF-8.
- Множественная отмена/повтор (Undo/Redo).
- Автоматическое сохранение.
- Мини-карта файла сбоку.

