#include "libs/engine/misc/Logger.h"
#include "libs/engine/misc/Stopwatch.h"
#include "panel.h"
#include "raylib.h"
#include "text_buffer.h"

#include <ctype.h>
// #include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vterm.h>

#ifdef _WIN32
#    include <direct.h>
#    include <io.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    define PATH_SEP '\\'
#    define getcwd   _getcwd
#    define chdir    _chdir
#else
#    include <dirent.h>
#    include <sys/stat.h>
#    include <unistd.h>
#    define PATH_SEP '/'
#endif

#define WINDOW_WIDTH  1200
#define WINDOW_HEIGHT 800
#define FONT_SIZE     14
#define FONT_SPACING  1.0f
// #define LINE_HEIGHT     FONT_SIZE
#define LEFT_MARGIN     60
#define TOP_MARGIN      10
#define STATUS_HEIGHT   25
#define MAX_PATH        4096
#define KEY_REPEAT_TIME 200  // milliseconds

enum EditorMode
{
    MODE_NORMAL = 0,
    MODE_FILE_SEARCH,
    MODE_GREP_SEARCH,
    MODE_DIRECTORY_SEARCH,
    MODE_TERMINAL,
};

typedef struct
{
    // Window settings
    unsigned int windowWidth;
    unsigned int windowHeight;
    unsigned int fontSize;
    float        fontSpacing;
    unsigned int lineHeight;

    TextBuffer* buffers[10];
    int         bufferCount;
    int         activeBufferIndex;
    Font        font;
    // int          bufferCapacity;

    // Current directory
    char currentDir[512];

    // Editor mode
    unsigned short mode;
    unsigned short previousMode;

    // File browser
    // bool   showFileBrowser;
    bool   browserIsDirectoryMode;  // true for directory selection, false for file selection
    char   browserFilter[256];
    int    browserFilterCursor;
    char** browserItems;
    int    browserItemCount;
    int    browserCapacity;
    int    browserSelected;
    int    browserScrollOffset;

    // Grep browser
    // bool   showGrepBrowser;
    char   grepPattern[256];
    int    grepPatternCursor;
    char** grepResults;
    int    grepResultCount;
    int    grepResultCapacity;
    int    grepSelected;
    int    grepScrollOffset;

    // Directory search
    // bool   showDirectoryDialog;
    char   directoryPattern[256];
    int    directoryPatternCursor;
    char** directoryResults;
    int    directoryResultCount;
    int    directoryResultCapacity;
    int    directorySelected;
    int    directoryScrollOffset;

    // Mouse
    bool   mouseDown;
    size_t mouseDragStart;
    float  charWidth;

    Stopwatch    keyRepeatTimer;
    unsigned int lastKeyPressed;
    bool         waitForNoInput;
} EditorState;

static EditorState editor = { 0 };

// File browser helpers
static void ScanFilesRecursive(const char* basePath, bool dirsOnly)
{
    struct _finddata_t fileinfo;
    intptr_t           handle;
    char               searchPath[1024];
    char               currentPath[1024];

    snprintf(searchPath, sizeof(searchPath), "%s\\*", basePath);
    handle = _findfirst(searchPath, &fileinfo);
    if (handle == -1)
        return;

    do
    {
        if (strcmp(fileinfo.name, ".") == 0 || strcmp(fileinfo.name, "..") == 0)
            continue;
        if (fileinfo.name[0] == '.')
            continue;  // Skip hidden

        snprintf(currentPath, sizeof(currentPath), "%s\\%s", basePath, fileinfo.name);

        bool isDir = (fileinfo.attrib & _A_SUBDIR) != 0;

        if (isDir)
        {
            // Add directory
            if (editor.browserItemCount >= editor.browserCapacity)
            {
                editor.browserCapacity *= 2;
                editor.browserItems = (char**)realloc(editor.browserItems, editor.browserCapacity * sizeof(char*));
            }
            editor.browserItems[editor.browserItemCount++] = _strdup(currentPath);

            // Recurse into subdirectory (limit depth)
            if (strlen(currentPath) - strlen(basePath) < 200)
            {
                ScanFilesRecursive(currentPath, dirsOnly);
            }
        }
        else if (!dirsOnly)
        {
            // Add file
            if (editor.browserItemCount >= editor.browserCapacity)
            {
                editor.browserCapacity *= 2;
                editor.browserItems = (char**)realloc(editor.browserItems, editor.browserCapacity * sizeof(char*));
            }
            editor.browserItems[editor.browserItemCount++] = _strdup(currentPath);
        }
    } while (_findnext(handle, &fileinfo) == 0);

    _findclose(handle);

    // reverse items
    for (int i = 0; i < editor.browserItemCount / 2; i++)
    {
        char* temp                                           = editor.browserItems[i];
        editor.browserItems[i]                               = editor.browserItems[editor.browserItemCount - i - 1];
        editor.browserItems[editor.browserItemCount - i - 1] = temp;
    }
}

static void InitFileBrowser(bool dirsOnly)
{
    // Free previous items
    for (int i = 0; i < editor.browserItemCount; i++)
    {
        free(editor.browserItems[i]);
    }
    if (editor.browserItems)
    {
        free(editor.browserItems);
    }

    editor.browserCapacity        = 1000;
    editor.browserItems           = (char**)malloc(editor.browserCapacity * sizeof(char*));
    editor.browserItemCount       = 0;
    editor.browserSelected        = 0;
    editor.browserScrollOffset    = 0;
    editor.browserFilter[0]       = '\0';
    editor.browserFilterCursor    = 0;
    editor.browserIsDirectoryMode = dirsOnly;

    // Scan files from current directory
    ScanFilesRecursive(editor.currentDir, dirsOnly);
}

static void ExecuteRipgrep(const char* pattern)
{
    // Free previous results
    for (int i = 0; i < editor.grepResultCount; i++)
    {
        free(editor.grepResults[i]);
    }
    if (editor.grepResults)
    {
        free(editor.grepResults);
    }

    editor.grepResultCapacity = 1000;
    editor.grepResults        = (char**)malloc(editor.grepResultCapacity * sizeof(char*));
    editor.grepResultCount    = 0;
    editor.grepSelected       = 0;
    editor.grepScrollOffset   = 0;

    if (pattern[0] == '\0')
        return;

    // Build ripgrep command
    char command[2048];
    snprintf(command, sizeof(command), "rg --line-number --column --no-heading --color=never \"%s\" 2>nul", pattern);

    FILE* pipe = _popen(command, "r");
    if (!pipe)
        return;

    char line[2048];
    while (fgets(line, sizeof(line), pipe) && editor.grepResultCount < editor.grepResultCapacity)
    {
        // Remove trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (len > 1 && line[len - 2] == '\r')
            line[len - 2] = '\0';

        if (line[0] != '\0')
        {
            editor.grepResults[editor.grepResultCount++] = _strdup(line);
        }
    }

    _pclose(pipe);
    // reverse results to show top matches first
    for (int i = 0; i < editor.grepResultCount / 2; i++)
    {
        char* temp                                         = editor.grepResults[i];
        editor.grepResults[i]                              = editor.grepResults[editor.grepResultCount - i - 1];
        editor.grepResults[editor.grepResultCount - i - 1] = temp;
    }
}

static void InitEditor(void);
static void UpdateEditor(void);
static void ChangeMode(unsigned short newMode);
static void DrawEditor(void);
static void HandleInput(void);
static void DrawTextBuffer(TextBuffer* buffer, int x, int y, int width, int height);
static void DrawSelection(TextBuffer* buffer, int x, int y, int width, int height);
static void DrawLineNumbers(TextBuffer* buffer, int x, int y, int width, int height);
static void DrawStatusBar(TextBuffer* buffer, int x, int y, int width, int height);
static void DrawWindows(void);
static void DrawFileBrowser(void);
static void DrawGrepBrowser(void);
static void DrawTerminal(void);
static void AddBuffer(TextBuffer* buffer);
static void CloseBuffer(int index);
static void NewBuffer(void);
static void SaveCurrentBuffer(void);
static void UpdateFont(void);

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Notepad--");
    SetTargetFPS(60);
    SetExitKey(0);  // Disable ESC to close
    Logger_Init();

    InitEditor();

    while (!WindowShouldClose())
    {
        UpdateEditor();

        BeginDrawing();
        ClearBackground((Color){ 30, 30, 30, 255 });
        DrawEditor();
        EndDrawing();
    }

    // Cleanup
    for (int i = 0; i < editor.bufferCount; i++)
    {
        if (editor.buffers[i])
        {
            DestroyTextBuffer(editor.buffers[i]);
        }
    }
    // free(editor.buffers);

    for (int i = 0; i < editor.browserItemCount; i++)
    {
        free(editor.browserItems[i]);
    }
    if (editor.browserItems)
    {
        free(editor.browserItems);
    }

    for (int i = 0; i < editor.grepResultCount; i++)
    {
        free(editor.grepResults[i]);
    }
    if (editor.grepResults)
    {
        free(editor.grepResults);
    }

    UnloadFont(editor.font);
    CloseWindow();

    return 0;
}

static void InitEditor(void)
{
    // editor.font = LoadFontEx("C:/Windows/Fonts/consola.ttf", FONT_SIZE, NULL, 0);
    // if (editor.font.texture.id == 0)
    // {
    //     editor.font = GetFontDefault();
    // }
    editor.fontSize    = FONT_SIZE;
    editor.fontSpacing = FONT_SPACING;
    UpdateFont();

    // editor.bufferCapacity    = 10;
    // editor.buffers           = (TextBuffer**)malloc(sizeof(TextBuffer*) * editor.bufferCapacity);
    editor.bufferCount       = 0;
    editor.activeBufferIndex = -1;

    // Initialize current directory
    if (getcwd(editor.currentDir, sizeof(editor.currentDir)) == NULL)
    {
        strcpy(editor.currentDir, ".");
    }

    editor.mode = MODE_NORMAL;

    editor.browserItems     = NULL;
    editor.browserItemCount = 0;
    editor.browserCapacity  = 0;

    editor.grepPattern[0]     = '\0';
    editor.grepPatternCursor  = 0;
    editor.grepResults        = NULL;
    editor.grepResultCount    = 0;
    editor.grepResultCapacity = 0;

    editor.directoryPattern[0]     = '\0';
    editor.directoryPatternCursor  = 0;
    editor.directoryResults        = NULL;
    editor.directoryResultCount    = 0;
    editor.directoryResultCapacity = 0;

    editor.mouseDown      = false;
    editor.mouseDragStart = 0;
    editor.charWidth      = 10.0f;

    // Create initial empty buffer
    NewBuffer();
}

static void UpdateEditor(void)
{
    HandleInput();
    editor.windowWidth  = GetScreenWidth();
    editor.windowHeight = GetScreenHeight();
}

static void ChangeMode(unsigned short newMode)
{
    editor.previousMode = editor.mode;
    editor.mode         = newMode;
    while (GetCharPressed() > 0)
        ;  // Clear input buffer
    Stopwatch_Stop(&editor.keyRepeatTimer);
    editor.waitForNoInput = true;
}

static void UpdateFont(void)
{
    if (editor.font.texture.id != 0)
    {
        UnloadFont(editor.font);
    }
    editor.font = LoadFontEx("C:/Windows/Fonts/consola.ttf", editor.fontSize, NULL, 0);
    if (editor.font.texture.id == 0)
    {
        editor.font = GetFontDefault();
    }
    editor.lineHeight = editor.font.baseSize;
}

static void DrawEditor(void)
{
    // Draw windows
    DrawWindows();

    // Draw file browser if open (on top of everything)
    if (editor.mode == MODE_FILE_SEARCH || editor.mode == MODE_DIRECTORY_SEARCH)
    {
        DrawFileBrowser();
    }

    // Draw grep browser if open (on top of everything)
    if (editor.mode == MODE_GREP_SEARCH)
    {
        DrawGrepBrowser();
    }
}

static void DrawFileBrowser(void)
{
    // Draw panel
    Rectangle rect = DrawPanel(0, 0, editor.windowWidth, editor.windowHeight);

    DrawRectangle(rect.x, rect.y, rect.width, rect.height, (Color){ 35, 35, 35, 255 });
    DrawRectangleLines(rect.x, rect.y, rect.width, rect.height, (Color){ 100, 100, 100, 255 });


    // Title and filter
    const char* title = editor.browserIsDirectoryMode ? "Change Directory" : "Open File";
    DrawTextEx(editor.font, title, (Vector2){ rect.x + 10, rect.y + 10 }, editor.fontSize, editor.fontSpacing, WHITE);

    DrawTextEx(editor.font, "Filter:", (Vector2){ rect.x + 10, rect.y + 35 }, editor.fontSize, editor.fontSpacing,
               GRAY);

    float textWidth = MeasureTextEx(editor.font, "Filter:", editor.fontSize, editor.fontSpacing).x;

    DrawTextEx(editor.font, editor.browserFilter, (Vector2){ rect.x + 10 + textWidth, rect.y + 35 }, editor.fontSize,
               editor.fontSpacing, WHITE);

    // Current directory
    char dirLabel[600];
    snprintf(dirLabel, sizeof(dirLabel), "Dir: %s", editor.currentDir);

    DrawTextEx(editor.font, dirLabel, (Vector2){ rect.x + 10, rect.y + 60 }, editor.fontSize, editor.fontSpacing,
               (Color){ 120, 120, 120, 255 });

    // Filter and display matching items
    int visibleItems = rect.height / editor.fontSize - 11;
    int itemHeight   = editor.fontSize;
    int listY        = rect.y + 90;
    int displayCount = 0;

    // Apply filter and render
    for (int i = 0; i < editor.browserItemCount && displayCount < visibleItems + editor.browserScrollOffset; i++)
    {
        const char* item = editor.browserItems[i];

        // Filter by substring match (case insensitive)
        if (editor.browserFilter[0] != '\0')
        {
            bool        matches  = false;
            const char* filename = strrchr(item, '\\');
            if (filename)
                filename++;
            else
                filename = item;

            // Simple substring search
            char lowerItem[1024], lowerFilter[256];
            strncpy(lowerItem, filename, sizeof(lowerItem) - 1);
            strncpy(lowerFilter, editor.browserFilter, sizeof(lowerFilter) - 1);

            for (char* p = lowerItem; *p; p++)
                *p = tolower(*p);
            for (char* p = lowerFilter; *p; p++)
                *p = tolower(*p);

            if (strstr(lowerItem, lowerFilter))
            {
                matches = true;
            }

            if (!matches)
                continue;
        }

        if (displayCount >= editor.browserScrollOffset)
        {
            int idx = displayCount - editor.browserScrollOffset;

            // Highlight selected
            Color bgColor =
                (displayCount == editor.browserSelected) ? (Color){ 60, 120, 200, 255 } : (Color){ 45, 45, 45, 0 };
            if (bgColor.a > 0)
            {
                DrawRectangle(rect.x + 10, listY + idx * itemHeight, rect.width - 20, itemHeight - 2, bgColor);
            }

            // Display relative path
            const char* displayPath = item;
            if (strncmp(item, editor.currentDir, strlen(editor.currentDir)) == 0)
            {
                displayPath = item + strlen(editor.currentDir);
                if (displayPath[0] == '\\')
                    displayPath++;
            }
            while (MeasureTextEx(editor.font, displayPath, editor.fontSize, editor.fontSpacing).x > rect.width - 40)
            {
                displayPath++;
            }

            DrawTextEx(editor.font, displayPath, (Vector2){ rect.x + 15, listY + idx * itemHeight + 2 },
                       editor.fontSize, editor.fontSpacing, WHITE);
        }

        displayCount++;
    }

    // Instructions
    char infoText[256];
    snprintf(infoText, sizeof(infoText), "Found: %d | ESC: cancel | Enter: select", displayCount);
    DrawTextEx(editor.font, infoText, (Vector2){ rect.x + 10, rect.y + rect.height - 25 }, editor.fontSize,
               editor.fontSpacing, GRAY);
}

static void DrawGrepBrowser(void)
{
    // Draw panel
    Rectangle rect = DrawPanel(0, 0, editor.windowWidth, editor.windowHeight);

    DrawRectangle(rect.x, rect.y, rect.width, rect.height, (Color){ 35, 35, 35, 255 });
    DrawRectangleLines(rect.x, rect.y, rect.width, rect.height, (Color){ 100, 100, 100, 255 });

    // Title and pattern input
    DrawTextEx(editor.font, "Ripgrep Search", (Vector2){ rect.x + 10, rect.y + 10 }, editor.fontSize,
               editor.fontSpacing, WHITE);

    DrawTextEx(editor.font, "Pattern:", (Vector2){ rect.x + 10, rect.y + 35 }, editor.fontSize, editor.fontSpacing,
               GRAY);

    float textWidth = MeasureTextEx(editor.font, "Pattern:", editor.fontSize, editor.fontSpacing).x;

    DrawTextEx(editor.font, editor.grepPattern, (Vector2){ rect.x + 10 + textWidth, rect.y + 35 }, editor.fontSize,
               editor.fontSpacing, WHITE);

    // Blinking cursor for pattern input
    // if ((int)(GetTime() * 2) % 2 == 0)
    // {
    //     int cursorX = rect.x + 100 + MeasureTextEx(editor.font, editor.grepPattern, 16, 1).x;
    //     DrawRectangle(cursorX, rect.y + 35, 2, 16, WHITE);
    // }

    // Current directory
    char dirLabel[600];
    snprintf(dirLabel, sizeof(dirLabel), "Dir: %s", editor.currentDir);
    DrawTextEx(editor.font, dirLabel, (Vector2){ rect.x + 10, rect.y + 60 }, editor.fontSize, editor.fontSpacing,
               (Color){ 120, 120, 120, 255 });

    // Results list
    int visibleItems = rect.height / editor.fontSize - 11;
    int itemHeight   = editor.fontSize;
    int listY        = rect.y + 90;

    for (int i = editor.grepScrollOffset; i < editor.grepResultCount && i < editor.grepScrollOffset + visibleItems; i++)
    {
        int idx = i - editor.grepScrollOffset;

        // Highlight selected
        Color bgColor = (i == editor.grepSelected) ? (Color){ 60, 120, 200, 255 } : (Color){ 45, 45, 45, 0 };
        if (bgColor.a > 0)
        {
            DrawRectangle(rect.x + 10, listY + idx * itemHeight, rect.width - 20, itemHeight - 2, bgColor);
        }

        // Parse and display result (file:line:col:text)
        const char* result = editor.grepResults[i];
        while (MeasureTextEx(editor.font, result, editor.fontSize, editor.fontSpacing).x > rect.width - 40)
        {
            result++;
        }

        // Find first colon (end of filename)
        const char* firstColon = strchr(result, ':');
        if (firstColon)
        {
            // Display filename in cyan
            int  filenameLen = firstColon - result;
            char filename[512];
            strncpy(filename, result, filenameLen);
            filename[filenameLen] = '\0';

            DrawTextEx(editor.font, filename, (Vector2){ rect.x + 15, listY + idx * itemHeight + 2 }, editor.fontSize,
                       editor.fontSpacing, (Color){ 100, 200, 255, 255 });


            // Display rest in gray
            int filenameWidth = MeasureTextEx(editor.font, filename, editor.fontSize, editor.fontSpacing).x;
            DrawTextEx(editor.font, firstColon, (Vector2){ rect.x + 15 + filenameWidth, listY + idx * itemHeight + 2 },
                       editor.fontSize, editor.fontSpacing, (Color){ 180, 180, 180, 255 });
        }
        else
        {

            while (MeasureTextEx(editor.font, result, editor.fontSize, editor.fontSpacing).x > rect.width - 40)
            {
                result++;
            }
            DrawTextEx(editor.font, result, (Vector2){ rect.x + 15, listY + idx * itemHeight + 2 }, editor.fontSize,
                       editor.fontSpacing, WHITE);
        }
    }

    // Instructions
    char infoText[256];
    snprintf(infoText, sizeof(infoText), "Found: %d | ESC: cancel | Enter: open", editor.grepResultCount);
    DrawTextEx(editor.font, infoText, (Vector2){ rect.x + 10, rect.y + rect.height - 25 }, editor.fontSize,
               editor.fontSpacing, GRAY);
}

static void DrawTerminal(void)
{
}

static void HandleInput(void)
{
    TextBuffer* buffer = (editor.activeBufferIndex >= 0 && editor.activeBufferIndex < editor.bufferCount) ?
                             editor.buffers[editor.activeBufferIndex] :
                             NULL;

    if (editor.waitForNoInput)
    {
        if (!IsKeyDown(editor.lastKeyPressed))
        {
            editor.waitForNoInput = false;
        }
        return;
    }

    // Grep browser input
    if (editor.mode == MODE_GREP_SEARCH)
    {
        // Type pattern and execute on every keystroke
        int  key            = GetCharPressed();
        bool patternChanged = false;
        while (key > 0)
        {
            if (key >= 32 && key < 127 && editor.grepPatternCursor < 255)
            {
                editor.grepPattern[editor.grepPatternCursor++] = (char)key;
                editor.grepPattern[editor.grepPatternCursor]   = '\0';
                patternChanged                                 = true;
            }
            key = GetCharPressed();
        }

        if (IsKeyDown(KEY_BACKSPACE) && editor.grepPatternCursor > 0)
        {
            editor.lastKeyPressed                          = KEY_BACKSPACE;
            editor.grepPattern[--editor.grepPatternCursor] = '\0';
            patternChanged                                 = true;
        }

        // Execute search on pattern change
        if (patternChanged && editor.grepPattern[0] != '\0')
        {
            ExecuteRipgrep(editor.grepPattern);
        }
        else if (patternChanged && editor.grepPattern[0] == '\0')
        {
            // Clear results if pattern is empty
            for (int i = 0; i < editor.grepResultCount; i++)
            {
                free(editor.grepResults[i]);
            }
            editor.grepResultCount  = 0;
            editor.grepSelected     = 0;
            editor.grepScrollOffset = 0;
        }

        // Navigate results with IsKeyDown for continuous movement
        if ((IsKeyDown(KEY_DOWN)) && editor.grepResultCount > 0)
        {
            editor.lastKeyPressed = KEY_DOWN;
            editor.grepSelected++;
            if (editor.grepSelected >= editor.grepResultCount)
                editor.grepSelected = editor.grepResultCount - 1;
            if (editor.grepSelected >= editor.grepScrollOffset + 24)
                editor.grepScrollOffset++;
        }
        if ((IsKeyDown(KEY_UP)) && editor.grepResultCount > 0)
        {
            editor.lastKeyPressed = KEY_UP;
            editor.grepSelected--;
            if (editor.grepSelected < 0)
                editor.grepSelected = 0;
            if (editor.grepSelected < editor.grepScrollOffset)
                editor.grepScrollOffset--;
        }

        if (IsKeyPressed(KEY_ENTER) && editor.grepResultCount > 0)
        {
            editor.lastKeyPressed = KEY_ENTER;
            // Parse result: filename:line:column:text
            const char* result        = editor.grepResults[editor.grepSelected];
            char        filepath[512] = { 0 };
            int         line = 1, col = 1;

            const char* firstColon = strchr(result, ':');
            if (firstColon)
            {
                strncpy(filepath, result, firstColon - result);
                filepath[firstColon - result] = '\0';

                const char* secondColon = strchr(firstColon + 1, ':');
                if (secondColon)
                {
                    line = atoi(firstColon + 1);
                    col  = atoi(secondColon + 1);
                }
            }

            if (filepath[0] != '\0')
            {
                // Make absolute path
                char absolutePath[1024];
                if (filepath[0] != '/' && filepath[1] != ':')
                {
                    snprintf(absolutePath, sizeof(absolutePath), "%s\\%s", editor.currentDir, filepath);
                }
                else
                {
                    strncpy(absolutePath, filepath, sizeof(absolutePath) - 1);
                }

                // Open file
                TextBuffer* existing = FindBufferByPath(absolutePath);
                if (existing)
                {
                    for (int j = 0; j < editor.bufferCount; j++)
                    {
                        if (editor.buffers[j] == existing)
                        {
                            editor.activeBufferIndex = j;
                            buffer                   = existing;
                            break;
                        }
                    }
                }
                else
                {
                    TextBuffer* newBuffer = CreateTextBuffer();
                    if (LoadFile(newBuffer, absolutePath))
                    {
                        AddBuffer(newBuffer);
                        buffer = newBuffer;
                    }
                    else
                    {
                        DestroyTextBuffer(newBuffer);
                    }
                }

                // Jump to line and column
                if (buffer)
                {
                    size_t pos = GetPosFromLineColumn(buffer, line - 1, col - 1);
                    if (pos < buffer->length)
                    {
                        buffer->cursorPos = pos;
                    }
                }
                ChangeMode(MODE_NORMAL);
            }
        }

        if (IsKeyPressed(KEY_ESCAPE))
        {
            editor.lastKeyPressed = KEY_ESCAPE;
            ChangeMode(MODE_NORMAL);
        }
        return;
    }

    // File browser input
    if (editor.mode == MODE_FILE_SEARCH)
    {
        // Type to filter
        int key = GetCharPressed();
        while (key > 0)
        {
            if (key >= 32 && key < 127 && editor.browserFilterCursor < 255)
            {
                editor.browserFilter[editor.browserFilterCursor++] = (char)key;
                editor.browserFilter[editor.browserFilterCursor]   = '\0';
                editor.browserSelected                             = 0;
                editor.browserScrollOffset                         = 0;
            }
            key = GetCharPressed();
        }

        if (IsKeyPressed(KEY_BACKSPACE) && editor.browserFilterCursor > 0)
        {
            editor.lastKeyPressed                              = KEY_BACKSPACE;
            editor.browserFilter[--editor.browserFilterCursor] = '\0';
            editor.browserSelected                             = 0;
            editor.browserScrollOffset                         = 0;
        }

        // Count filtered items
        int filteredCount = 0;
        for (int i = 0; i < editor.browserItemCount; i++)
        {
            if (editor.browserFilter[0] != '\0')
            {
                const char* filename = strrchr(editor.browserItems[i], '\\');
                if (filename)
                    filename++;
                else
                    filename = editor.browserItems[i];

                char lowerItem[1024], lowerFilter[256];
                strncpy(lowerItem, filename, sizeof(lowerItem) - 1);
                strncpy(lowerFilter, editor.browserFilter, sizeof(lowerFilter) - 1);
                for (char* p = lowerItem; *p; p++)
                    *p = tolower(*p);
                for (char* p = lowerFilter; *p; p++)
                    *p = tolower(*p);

                if (!strstr(lowerItem, lowerFilter))
                    continue;
            }
            filteredCount++;
        }

        if (IsKeyPressed(KEY_DOWN))
        {
            editor.lastKeyPressed = KEY_DOWN;
            editor.browserSelected++;
            if (editor.browserSelected >= filteredCount)
                editor.browserSelected = filteredCount - 1;
            if (editor.browserSelected >= editor.browserScrollOffset + 22)
                editor.browserScrollOffset++;
        }
        if (IsKeyPressed(KEY_UP))
        {
            editor.lastKeyPressed = KEY_UP;
            editor.browserSelected--;
            if (editor.browserSelected < 0)
                editor.browserSelected = 0;
            if (editor.browserSelected < editor.browserScrollOffset)
                editor.browserScrollOffset--;
        }

        if (IsKeyPressed(KEY_ENTER) && filteredCount > 0)
        {
            editor.lastKeyPressed = KEY_ENTER;
            // Find the selected filtered item
            int currentIdx = 0;
            for (int i = 0; i < editor.browserItemCount; i++)
            {
                if (editor.browserFilter[0] != '\0')
                {
                    const char* filename = strrchr(editor.browserItems[i], '\\');
                    if (filename)
                        filename++;
                    else
                        filename = editor.browserItems[i];

                    char lowerItem[1024], lowerFilter[256];
                    strncpy(lowerItem, filename, sizeof(lowerItem) - 1);
                    strncpy(lowerFilter, editor.browserFilter, sizeof(lowerFilter) - 1);
                    for (char* p = lowerItem; *p; p++)
                        *p = tolower(*p);
                    for (char* p = lowerFilter; *p; p++)
                        *p = tolower(*p);

                    if (!strstr(lowerItem, lowerFilter))
                        continue;
                }

                if (currentIdx == editor.browserSelected)
                {
                    char* selectedPath = editor.browserItems[i];

                    if (editor.browserIsDirectoryMode)
                    {
                        // Change directory
                        if (chdir(selectedPath) == 0)
                        {
                            if (getcwd(editor.currentDir, sizeof(editor.currentDir)) == NULL)
                            {
                                strcpy(editor.currentDir, selectedPath);
                            }
                        }
                    }
                    else
                    {
                        // Open file
                        TextBuffer* existing = FindBufferByPath(selectedPath);
                        if (existing)
                        {
                            for (int j = 0; j < editor.bufferCount; j++)
                            {
                                if (editor.buffers[j] == existing)
                                {
                                    editor.activeBufferIndex = j;
                                    break;
                                }
                            }
                        }
                        else
                        {
                            TextBuffer* newBuffer = CreateTextBuffer();
                            if (LoadFile(newBuffer, selectedPath))
                            {
                                AddBuffer(newBuffer);
                            }
                            else
                            {
                                DestroyTextBuffer(newBuffer);
                            }
                        }
                    }
                    ChangeMode(MODE_NORMAL);
                    break;
                }
                currentIdx++;
            }
        }

        if (IsKeyPressed(KEY_ESCAPE))
        {
            ChangeMode(MODE_NORMAL);
        }
        return;
    }

    // directory dialog input (continued in next part...)
    if (editor.mode == MODE_DIRECTORY_SEARCH)
    {
        int key = GetCharPressed();
        while (key > 0)
        {
            if (key >= 32 && key < 127 && editor.directoryPatternCursor < 255)
            {
                editor.directoryPattern[editor.directoryPatternCursor++] = (char)key;
                editor.directoryPattern[editor.directoryPatternCursor]   = '\0';
            }
            key = GetCharPressed();
        }

        if (IsKeyPressed(KEY_BACKSPACE) && editor.directoryPatternCursor > 0)
        {
            editor.lastKeyPressed                                    = KEY_BACKSPACE;
            editor.directoryPattern[--editor.directoryPatternCursor] = '\0';
        }

        if (IsKeyPressed(KEY_ENTER) && editor.directoryPattern[0] != '\0')
        {
            editor.lastKeyPressed = KEY_ENTER;
            size_t foundPos;
            if (SearchForward(buffer, editor.directoryPattern, &foundPos))
            {
                buffer->cursorPos      = foundPos;
                buffer->hasSelection   = true;
                buffer->selectionStart = foundPos;
                buffer->selectionEnd   = foundPos + strlen(editor.directoryPattern);
            }
            ChangeMode(MODE_NORMAL);
        }

        if (IsKeyPressed(KEY_ESCAPE))
        {
            editor.lastKeyPressed = KEY_ESCAPE;
            ChangeMode(MODE_NORMAL);
        }
        return;
    }

    if (!buffer)
    {
        return;
    }

    bool ctrlPressed  = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shiftPressed = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    // Mouse wheel
    float wheel = GetMouseWheelMove();
    if (wheel != 0)
    {
        buffer->scrollY -= (int)(wheel * 3);
        if (buffer->scrollY < 0)
            buffer->scrollY = 0;
    }

    // Ctrl+O - Open file with integrated browser
    if (ctrlPressed && IsKeyPressed(KEY_O))
    {
        InitFileBrowser(false);  // false = files
        ChangeMode(MODE_FILE_SEARCH);
        return;
    }

    // Ctrl+D - Change directory with integrated browser
    if (ctrlPressed && IsKeyPressed(KEY_D))
    {
        InitFileBrowser(true);  // true = directories only
        ChangeMode(MODE_DIRECTORY_SEARCH);
        return;
    }

    // Ctrl+G - Ripgrep search
    if (ctrlPressed && IsKeyPressed(KEY_G))
    {
        ChangeMode(MODE_GREP_SEARCH);
        editor.grepPattern[0]    = '\0';
        editor.grepPatternCursor = 0;
        return;
    }

    // Ctrl+S - Save
    if (ctrlPressed && IsKeyPressed(KEY_S))
    {
        SaveCurrentBuffer();
        return;
    }

    // Ctrl+N - New
    if (ctrlPressed && IsKeyPressed(KEY_N))
    {
        NewBuffer();
        return;
    }

    // Ctrl+W - Close buffer
    if (ctrlPressed && IsKeyPressed(KEY_Q))
    {
        if (editor.bufferCount > 1)
        {
            CloseBuffer(editor.activeBufferIndex);
        }
        return;
    }

    // Ctrl+C/X/V - Copy/Cut/Paste
    if (ctrlPressed && IsKeyPressed(KEY_C))
    {
        if (buffer->hasSelection)
        {
            size_t start =
                buffer->selectionStart < buffer->selectionEnd ? buffer->selectionStart : buffer->selectionEnd;
            size_t end = buffer->selectionStart < buffer->selectionEnd ? buffer->selectionEnd : buffer->selectionStart;
            char*  clipText = (char*)malloc(end - start + 1);
            if (clipText)
            {
                memcpy(clipText, buffer->text + start, end - start);
                clipText[end - start] = '\0';
                SetClipboardText(clipText);
                free(clipText);
            }
        }
        return;
    }

    if (ctrlPressed && IsKeyPressed(KEY_X))
    {
        if (buffer->hasSelection)
        {
            size_t start =
                buffer->selectionStart < buffer->selectionEnd ? buffer->selectionStart : buffer->selectionEnd;
            size_t end = buffer->selectionStart < buffer->selectionEnd ? buffer->selectionEnd : buffer->selectionStart;
            char*  clipText = (char*)malloc(end - start + 1);
            if (clipText)
            {
                memcpy(clipText, buffer->text + start, end - start);
                clipText[end - start] = '\0';
                SetClipboardText(clipText);
                free(clipText);
            }
            DeleteSelection(buffer);
        }
        return;
    }

    if (ctrlPressed && IsKeyPressed(KEY_V))
    {
        const char* clipText = GetClipboardText();
        if (clipText && clipText[0])
        {
            InsertText(buffer, clipText, strlen(clipText));
        }
        return;
    }

    // Ctrl+Z/Y - Undo/Redo
    if (ctrlPressed && IsKeyPressed(KEY_Z))
    {
        Undo(buffer);
        return;
    }
    if (ctrlPressed && IsKeyPressed(KEY_Y))
    {
        Redo(buffer);
        return;
    }

    // Arrow keys with continuous movement support
    if (IsKeyDown(KEY_LEFT))
    {
        if (editor.lastKeyPressed == KEY_LEFT && !Stopwatch_IsZero(&editor.keyRepeatTimer))
        {
            return;
        }
        if (!buffer->hasSelection && shiftPressed)
        {
            buffer->hasSelection   = true;
            buffer->selectionStart = buffer->cursorPos;
        }
        // LOG_INF("Moving cursor left");
        MoveCursorLeft(buffer, ctrlPressed);
        if (shiftPressed && buffer->hasSelection)
        {
            buffer->selectionEnd = buffer->cursorPos;
        }
        else
        {
            buffer->hasSelection = false;
        }
        if (IsKeyPressed(KEY_LEFT))
        {
            editor.lastKeyPressed = KEY_LEFT;
            Stopwatch_Start(&editor.keyRepeatTimer, KEY_REPEAT_TIME);
        }
    }
    if (IsKeyDown(KEY_RIGHT))
    {
        // LOG_INF("KEY_RIGHT is down %d %d", editor.lastKeyPressed == KEY_RIGHT,
        //         Stopwatch_IsZero(&editor.keyRepeatTimer));
        if (editor.lastKeyPressed == KEY_RIGHT && !Stopwatch_IsZero(&editor.keyRepeatTimer))
        {
            return;
        }
        if (!buffer->hasSelection && shiftPressed)
        {
            buffer->hasSelection   = true;
            buffer->selectionStart = buffer->cursorPos;
        }
        // LOG_INF("Moving cursor right");
        MoveCursorRight(buffer, ctrlPressed);
        if (shiftPressed && buffer->hasSelection)
        {
            buffer->selectionEnd = buffer->cursorPos;
        }
        else
        {
            buffer->hasSelection = false;
        }
        if (IsKeyPressed(KEY_RIGHT))
        {
            editor.lastKeyPressed = KEY_RIGHT;
            Stopwatch_Start(&editor.keyRepeatTimer, KEY_REPEAT_TIME);
        }
    }
    if (IsKeyDown(KEY_UP))
    {
        if (editor.lastKeyPressed == KEY_UP && !Stopwatch_IsZero(&editor.keyRepeatTimer))
        {
            return;
        }
        if (!buffer->hasSelection && shiftPressed)
        {
            buffer->hasSelection   = true;
            buffer->selectionStart = buffer->cursorPos;
        }
        MoveCursorUp(buffer);
        if (shiftPressed && buffer->hasSelection)
        {
            buffer->selectionEnd = buffer->cursorPos;
        }
        else
        {
            buffer->hasSelection = false;
        }
        if (IsKeyPressed(KEY_UP))
        {
            editor.lastKeyPressed = KEY_UP;
            Stopwatch_Start(&editor.keyRepeatTimer, KEY_REPEAT_TIME);
        }
    }
    if (IsKeyDown(KEY_DOWN))
    {
        if (editor.lastKeyPressed == KEY_DOWN && !Stopwatch_IsZero(&editor.keyRepeatTimer))
        {
            return;
        }
        if (!buffer->hasSelection && shiftPressed)
        {
            buffer->hasSelection   = true;
            buffer->selectionStart = buffer->cursorPos;
        }
        MoveCursorDown(buffer);
        if (shiftPressed && buffer->hasSelection)
        {
            buffer->selectionEnd = buffer->cursorPos;
        }
        else
        {
            buffer->hasSelection = false;
        }
        if (IsKeyPressed(KEY_DOWN))
        {
            editor.lastKeyPressed = KEY_DOWN;
            Stopwatch_Start(&editor.keyRepeatTimer, KEY_REPEAT_TIME);
        }
    }
    if (IsKeyPressed(KEY_HOME))
    {
        if (ctrlPressed)
        {
            // Ctrl+Home - start of document
            buffer->cursorPos = 0;
            if (!shiftPressed)
            {
                buffer->hasSelection = false;
            }
            else
            {
                buffer->selectionEnd = buffer->cursorPos;
            }
        }
        else
        {
            MoveCursorHome(buffer);
            if (shiftPressed)
            {
                buffer->selectionEnd = buffer->cursorPos;
            }
        }
    }
    if (IsKeyPressed(KEY_END))
    {
        if (ctrlPressed)
        {
            // Ctrl+End - end of document
            buffer->cursorPos = buffer->length;
            if (!shiftPressed)
            {
                buffer->hasSelection = false;
            }
            else
            {
                buffer->selectionEnd = buffer->cursorPos;
            }
        }
        else
        {
            MoveCursorEnd(buffer);
            if (shiftPressed)
            {
                buffer->selectionEnd = buffer->cursorPos;
            }
        }
    }

    // Backspace/Delete
    if (IsKeyDown(KEY_BACKSPACE))
    {
        if (editor.lastKeyPressed == KEY_BACKSPACE && !Stopwatch_IsZero(&editor.keyRepeatTimer))
        {
            return;
        }
        DeleteChar(buffer);
        if (IsKeyPressed(KEY_BACKSPACE))
        {
            editor.lastKeyPressed = KEY_BACKSPACE;
            Stopwatch_Start(&editor.keyRepeatTimer, KEY_REPEAT_TIME);
        }
    }
    if (IsKeyDown(KEY_DELETE))
    {
        if (editor.lastKeyPressed == KEY_DELETE && !Stopwatch_IsZero(&editor.keyRepeatTimer))
        {
            return;
        }
        if (buffer->hasSelection)
        {
            DeleteSelection(buffer);
        }
        else if (buffer->cursorPos < buffer->length)
        {
            // buffer->cursorPos++;
            DeleteChar(buffer);
        }
        if (IsKeyPressed(KEY_DELETE))
        {
            editor.lastKeyPressed = KEY_DELETE;
            Stopwatch_Start(&editor.keyRepeatTimer, KEY_REPEAT_TIME);
        }
    }

    // Enter/Tab
    if (IsKeyDown(KEY_ENTER))
    {
        if (editor.lastKeyPressed == KEY_ENTER && !Stopwatch_IsZero(&editor.keyRepeatTimer))
        {
            return;
        }
        InsertChar(buffer, '\n');
        if (IsKeyPressed(KEY_ENTER))
        {
            editor.lastKeyPressed = KEY_ENTER;
            Stopwatch_Start(&editor.keyRepeatTimer, KEY_REPEAT_TIME);
        }
    }
    if (IsKeyDown(KEY_TAB))
    {
        if (editor.lastKeyPressed == KEY_TAB && !Stopwatch_IsZero(&editor.keyRepeatTimer))
        {
            return;
        }
        InsertText(buffer, "    ", 4);
        if (IsKeyPressed(KEY_TAB))
        {
            editor.lastKeyPressed = KEY_TAB;
            Stopwatch_Start(&editor.keyRepeatTimer, KEY_REPEAT_TIME);
        }
    }

    if (IsKeyPressed(KEY_EQUAL))
    {
        if (ctrlPressed)
        {
            editor.fontSize += 2;
            if (editor.fontSize > 48)
            {
                editor.fontSize = 48;
            }
            UpdateFont();
        }
    }

    if (IsKeyPressed(KEY_MINUS))
    {
        if (ctrlPressed)
        {
            editor.fontSize -= 2;
            if (editor.fontSize < 10)
            {
                editor.fontSize = 10;
            }
        }
        UpdateFont();
    }

    // Regular characters
    int key = GetCharPressed();
    while (key > 0)
    {
        if (key >= 32 && key < 127)
        {
            InsertChar(buffer, (char)key);
        }
        key = GetCharPressed();
    }

    // Auto-scroll
    size_t line         = GetLineFromPos(buffer, buffer->cursorPos);
    int    visibleLines = (editor.windowHeight - STATUS_HEIGHT) / editor.lineHeight;
    if ((int)line < buffer->scrollY)
    {
        buffer->scrollY = line;
    }
    else if ((int)line >= buffer->scrollY + visibleLines)
    {
        buffer->scrollY = line - visibleLines + 1;
    }
}

static void DrawTextBuffer(TextBuffer* buffer, int x, int y, int width, int height)
{
    if (!buffer || !buffer->text)
    {
        return;
    }

    int          drawY = y + TOP_MARGIN - (buffer->scrollY * editor.lineHeight);
    size_t       pos   = 0;
    char         lineBuffer[1024];
    int          lineBufferLen = 0;
    bool         cursorLine            = false;
    bool         selectionStartLine    = false;
    bool         selectionEndLine      = false;
    unsigned int cursorLinePos         = 0;
    unsigned int selectionLineStartPos = 0;
    unsigned int selectionLineEndPos   = 0;

    unsigned int selectionStart, selectionEnd;
    cursorLinePos = 0;
    selectionStart = 0;
    selectionEnd = 0;

    lineBuffer[0] = '\0';
    if(buffer->hasSelection)
    {
        selectionStart = (buffer->selectionStart < buffer->selectionEnd) ? buffer->selectionStart : buffer->selectionEnd;
        selectionEnd   = (buffer->selectionStart > buffer->selectionEnd) ? buffer->selectionStart : buffer->selectionEnd;
    }

    while (pos <= buffer->length)
    {
        char c = (pos < buffer->length) ? buffer->text[pos] : '\0';
        if (c == '\r')
        {
            pos++;
            continue;  // skip carriage returns
        }
        if (pos == buffer->cursorPos)
        {
            if (lineBufferLen > 0)
            {
                cursorLinePos = MeasureTextEx(editor.font, lineBuffer, editor.fontSize, editor.fontSpacing).x;
            }
            else
            {
                cursorLinePos = 0;
            }
            cursorLine = true;
        }
        if (pos == selectionStart && buffer->hasSelection)
        {
            selectionLineStartPos = MeasureTextEx(editor.font, lineBuffer, editor.fontSize, editor.fontSpacing).x;
            selectionStartLine    = true;
        }
        if (pos == selectionEnd && buffer->hasSelection)
        {
            selectionLineEndPos = MeasureTextEx(editor.font, lineBuffer, editor.fontSize, editor.fontSpacing).x;
            selectionEndLine  = true;
        }
        if (c == '\n' || c == '\0')
        {
            lineBuffer[lineBufferLen] = '\0';
            if (drawY >= y && drawY < y + height)
            {
                // LOG_INF("%s", lineBuffer);
                DrawTextEx(editor.font, lineBuffer, (Vector2){ LEFT_MARGIN - buffer->scrollX, drawY }, editor.fontSize,
                           editor.fontSpacing, (Color){ 220, 220, 220, 255 });
                if (cursorLine)
                {
                    DrawRectangle(LEFT_MARGIN - buffer->scrollX + cursorLinePos, drawY, 2, editor.lineHeight - 2,
                                  (Color){ 200, 200, 200, 255 });
                    cursorLine = false;
                }
                if (selectionStartLine || selectionEndLine)
                {
                    unsigned int selStartX = selectionStartLine ? selectionLineStartPos : 0;
                    unsigned int selEndX =
                        selectionEndLine ?
                            selectionLineEndPos :
                            MeasureTextEx(editor.font, lineBuffer, editor.fontSize, editor.fontSpacing).x;
                    DrawRectangle(LEFT_MARGIN - buffer->scrollX + selStartX, drawY, selEndX - selStartX,
                                  editor.lineHeight, (Color){ 0, 120, 215, 100 });
                    selectionStartLine = false;
                    selectionEndLine   = false;
                }
                else if (pos > selectionStart && pos < selectionEnd && buffer->hasSelection)
                {
                    unsigned int selStartX = 0;
                    unsigned int selEndX =
                        MeasureTextEx(editor.font, lineBuffer, editor.fontSize, editor.fontSpacing).x;
                    DrawRectangle(LEFT_MARGIN - buffer->scrollX + selStartX, drawY, selEndX - selStartX, editor.lineHeight,
                                  (Color){ 0, 120, 215, 100 });
                }
            }

            drawY += editor.lineHeight;
            lineBufferLen = 0;

            if (drawY > y + height)
            {
                break;
            }
        }
        else
        {
            if (lineBufferLen < 1023)
            {
                lineBuffer[lineBufferLen++] = c;
                lineBuffer[lineBufferLen]   = '\0';
            }
        }

        if (c == '\0')
        {
            break;
        }
        pos++;
    }
}

static void DrawSelection(TextBuffer* buffer, int x, int y, int width, int height)
{
    return;
    if (!buffer || !buffer->hasSelection)
        return;

    size_t start = buffer->selectionStart < buffer->selectionEnd ? buffer->selectionStart : buffer->selectionEnd;
    size_t end   = buffer->selectionStart < buffer->selectionEnd ? buffer->selectionEnd : buffer->selectionStart;

    size_t startLine = GetLineFromPos(buffer, start);
    size_t endLine   = GetLineFromPos(buffer, end);

    for (size_t line = startLine; line <= endLine; line++)
    {
        if ((int)line < buffer->scrollY)
            continue;

        size_t lineStart = GetPosFromLineColumn(buffer, line, 0);
        size_t lineEnd   = lineStart;
        while (lineEnd < buffer->length && buffer->text[lineEnd] != '\n')
        {
            lineEnd++;
        }

        size_t selStart = (line == startLine) ? start : lineStart;
        size_t selEnd   = (line == endLine) ? end : lineEnd;

        if (selStart < lineEnd && selEnd > lineStart)
        {
            size_t colStart = GetColumnFromPos(buffer, selStart);
            size_t colEnd   = GetColumnFromPos(buffer, selEnd);

            int x1   = LEFT_MARGIN + colStart * 10 - buffer->scrollX;
            int x2   = LEFT_MARGIN + colEnd * 10 - buffer->scrollX;
            int selY = y + TOP_MARGIN + (line - buffer->scrollY) * editor.lineHeight;

            DrawRectangle(x1, selY, x2 - x1, editor.lineHeight, (Color){ 0, 120, 215, 100 });
        }
    }
}

static void DrawLineNumbers(TextBuffer* buffer, int x, int y, int width, int height)
{
    if (!buffer)
        return;

    DrawRectangle(x, y, LEFT_MARGIN - 5, height, (Color){ 40, 40, 40, 255 });

    int lineCount = GetLineCount(buffer);
    int drawY     = y + TOP_MARGIN - (buffer->scrollY * editor.lineHeight);

    for (int i = 0; i < lineCount && drawY < y + height; i++)
    {
        if (drawY + editor.lineHeight >= y)
        {
            char lineNumStr[10];
            snprintf(lineNumStr, sizeof(lineNumStr), "%d", i + 1);
            DrawTextEx(editor.font, lineNumStr, (Vector2){ 10, drawY }, editor.fontSize, 1,
                       (Color){ 150, 150, 150, 255 });
        }
        drawY += editor.lineHeight;
    }
}

static void DrawWindows(void)
{
    // Draw active buffer
    if (editor.activeBufferIndex >= 0 && editor.activeBufferIndex < editor.bufferCount)
    {
        TextBuffer* buffer = editor.buffers[editor.activeBufferIndex];
        if (buffer)
        {
            int viewX = 0;
            int viewY = 0;
            int viewW = editor.windowWidth;
            int viewH = editor.windowHeight;

            DrawLineNumbers(buffer, viewX, viewY, viewW, viewH);
            DrawSelection(buffer, viewX, viewY, viewW, viewH);
            DrawTextBuffer(buffer, viewX, viewY, viewW, viewH);
            DrawStatusBar(buffer, viewX, viewY, viewW, viewH);
        }
    }
}

static void AddBuffer(TextBuffer* buffer)
{
    if (editor.bufferCount > 10)
    {
        return;  // Limit max buffers to 10 for simplicity
        // editor.bufferCapacity *= 2;
        // editor.buffers = (TextBuffer**)realloc(editor.buffers, sizeof(TextBuffer*) * editor.bufferCapacity);
    }

    editor.buffers[editor.bufferCount++] = buffer;
    editor.activeBufferIndex             = editor.bufferCount - 1;
    buffer->refCount++;
}

static void CloseBuffer(int index)
{
    if (index < 0 || index >= editor.bufferCount)
        return;

    TextBuffer* buffer = editor.buffers[index];
    if (buffer)
    {
        buffer->refCount--;
        if (buffer->refCount <= 0)
        {
            DestroyTextBuffer(buffer);
        }
    }

    for (int i = index; i < editor.bufferCount - 1; i++)
    {
        editor.buffers[i] = editor.buffers[i + 1];
    }
    editor.bufferCount--;

    if (editor.activeBufferIndex >= editor.bufferCount)
    {
        editor.activeBufferIndex = editor.bufferCount - 1;
    }

    if (editor.bufferCount == 0)
    {
        NewBuffer();
    }
}

static void SaveCurrentBuffer(void)
{
    if (editor.activeBufferIndex < 0 || editor.activeBufferIndex >= editor.bufferCount)
        return;

    TextBuffer* buffer = editor.buffers[editor.activeBufferIndex];
    if (!buffer)
        return;

    if (buffer->filePath[0] == '\0')
    {
        // Would need save as dialog - for now just skip
        return;
    }
    else
    {
        SaveFile(buffer, NULL);
    }
}

static void NewBuffer(void)
{
    TextBuffer* buffer = CreateTextBuffer();
    if (buffer)
    {
        AddBuffer(buffer);
    }
}

static void DrawStatusBar(TextBuffer* buffer, int x, int y, int width, int height)
{
    int statusPosY = height - STATUS_HEIGHT;
    DrawRectangle(x, y + statusPosY, width, STATUS_HEIGHT, (Color){ 40, 40, 40, 255 });

    if (editor.activeBufferIndex >= 0 && editor.activeBufferIndex < editor.bufferCount)
    {
        TextBuffer* buffer = editor.buffers[editor.activeBufferIndex];
        if (buffer)
        {
            size_t line  = GetLineFromPos(buffer, buffer->cursorPos) + 1;
            size_t col   = GetColumnFromPos(buffer, buffer->cursorPos) + 1;
            int    lines = GetLineCount(buffer);

            char statusText[256];
            snprintf(statusText, sizeof(statusText), " Line %zu, Col %zu | %d lines | %s | Dir: %s", line, col, lines,
                     buffer->filePath[0] ? buffer->filePath : "Untitled", editor.currentDir);

            char* statusTextPtr = statusText;

            DrawTextEx(editor.font, statusTextPtr, (Vector2){ 10, statusPosY + 4 }, 14, 1, WHITE);
        }
    }
}
