#ifndef TEXT_BUFFER_H
#define TEXT_BUFFER_H

#include <stddef.h>
#include <stdbool.h>

#define MAX_PATH_LENGTH 1024
#define INITIAL_CAPACITY 1024

// Undo/Redo action types
typedef enum {
    ACTION_INSERT,
    ACTION_DELETE
} ActionType;

// Undo/Redo history entry
typedef struct UndoEntry {
    ActionType type;
    size_t position;
    char* text;
    size_t length;
    size_t cursorPos;
    struct UndoEntry* next;
    struct UndoEntry* prev;
} UndoEntry;

// Text buffer structure
typedef struct TextBuffer {
    char* text;
    size_t length;
    size_t capacity;
    size_t cursorPos;
    size_t selectionStart;
    size_t selectionEnd;
    bool hasSelection;
    char filePath[MAX_PATH_LENGTH];
    bool isModified;
    int scrollY;
    int scrollX;
    UndoEntry* undoStack;
    UndoEntry* redoStack;
    int refCount;
    struct TextBuffer* next;
    struct TextBuffer* prev;
} TextBuffer;

// Buffer management
extern TextBuffer* g_bufferList;

TextBuffer* CreateTextBuffer(void);
TextBuffer* FindBufferByPath(const char* filePath);
void DestroyTextBuffer(TextBuffer* buffer);

// Text operations
void InsertChar(TextBuffer* buffer, char c);
void InsertText(TextBuffer* buffer, const char* text, size_t length);
void DeleteChar(TextBuffer* buffer);
void DeleteSelection(TextBuffer* buffer);

// Cursor movement
void MoveCursorLeft(TextBuffer* buffer, bool byWord);
void MoveCursorRight(TextBuffer* buffer, bool byWord);
void MoveCursorUp(TextBuffer* buffer);
void MoveCursorDown(TextBuffer* buffer);
void MoveCursorHome(TextBuffer* buffer);
void MoveCursorEnd(TextBuffer* buffer);

// File operations
bool LoadFile(TextBuffer* buffer, const char* filePath);
bool SaveFile(TextBuffer* buffer, const char* filePath);

// Undo/Redo
void Undo(TextBuffer* buffer);
void Redo(TextBuffer* buffer);

// Search
bool SearchForward(TextBuffer* buffer, const char* pattern, size_t* foundPos);
bool SearchBackward(TextBuffer* buffer, const char* pattern, size_t* foundPos);

// Utility functions
size_t GetLineFromPos(TextBuffer* buffer, size_t pos);
size_t GetColumnFromPos(TextBuffer* buffer, size_t pos);
size_t GetPosFromLineColumn(TextBuffer* buffer, size_t line, size_t column);
int GetLineCount(TextBuffer* buffer);

#endif // TEXT_BUFFER_H
