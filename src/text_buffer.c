#include "text_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

TextBuffer* g_bufferList = NULL;

static void EnsureCapacity(TextBuffer* buffer, size_t requiredSize);
static void AddUndoEntry(TextBuffer* buffer, ActionType type, size_t position, const char* text, size_t length);
static void ClearRedoStack(TextBuffer* buffer);
static bool IsWordChar(char c);

TextBuffer* CreateTextBuffer(void) {
    TextBuffer* buffer = (TextBuffer*)malloc(sizeof(TextBuffer));
    if (!buffer) return NULL;

    buffer->capacity = INITIAL_CAPACITY;
    buffer->text = (char*)malloc(buffer->capacity);
    if (!buffer->text) {
        free(buffer);
        return NULL;
    }

    buffer->text[0] = '\0';
    buffer->length = 0;
    buffer->cursorPos = 0;
    buffer->selectionStart = 0;
    buffer->selectionEnd = 0;
    buffer->hasSelection = false;
    buffer->filePath[0] = '\0';
    buffer->isModified = false;
    buffer->scrollY = 0;
    buffer->scrollX = 0;
    buffer->undoStack = NULL;
    buffer->redoStack = NULL;
    buffer->refCount = 0;
    buffer->next = NULL;
    buffer->prev = NULL;

    // Add to global list
    buffer->next = g_bufferList;
    if (g_bufferList) {
        g_bufferList->prev = buffer;
    }
    g_bufferList = buffer;

    return buffer;
}

TextBuffer* FindBufferByPath(const char* filePath) {
    if (!filePath || !filePath[0]) return NULL;

    for (TextBuffer* buffer = g_bufferList; buffer != NULL; buffer = buffer->next) {
        if (buffer->filePath[0] && strcmp(buffer->filePath, filePath) == 0) {
            return buffer;
        }
    }
    return NULL;
}

void DestroyTextBuffer(TextBuffer* buffer) {
    if (!buffer) return;

    if (buffer->text) free(buffer->text);

    // Free undo stack
    UndoEntry* undo = buffer->undoStack;
    while (undo) {
        UndoEntry* next = undo->next;
        if (undo->text) free(undo->text);
        free(undo);
        undo = next;
    }

    // Free redo stack
    UndoEntry* redo = buffer->redoStack;
    while (redo) {
        UndoEntry* next = redo->next;
        if (redo->text) free(redo->text);
        free(redo);
        redo = next;
    }

    // Remove from global list
    if (buffer->prev) {
        buffer->prev->next = buffer->next;
    } else {
        g_bufferList = buffer->next;
    }
    if (buffer->next) {
        buffer->next->prev = buffer->prev;
    }

    free(buffer);
}

static void EnsureCapacity(TextBuffer* buffer, size_t requiredSize) {
    if (requiredSize <= buffer->capacity) return;

    size_t newCapacity = buffer->capacity * 2;
    while (newCapacity < requiredSize) {
        newCapacity *= 2;
    }

    char* newText = (char*)realloc(buffer->text, newCapacity);
    if (newText) {
        buffer->text = newText;
        buffer->capacity = newCapacity;
    }
}

void InsertChar(TextBuffer* buffer, char c) {
    if (!buffer) return;

    if (buffer->hasSelection) {
        DeleteSelection(buffer);
    }

    EnsureCapacity(buffer, buffer->length + 2);

    // Add undo entry
    char charStr[2] = {c, '\0'};
    AddUndoEntry(buffer, ACTION_INSERT, buffer->cursorPos, charStr, 1);

    // Move text after cursor
    memmove(buffer->text + buffer->cursorPos + 1,
            buffer->text + buffer->cursorPos,
            buffer->length - buffer->cursorPos + 1);

    buffer->text[buffer->cursorPos] = c;
    buffer->length++;
    buffer->cursorPos++;
    buffer->isModified = true;

    ClearRedoStack(buffer);
}

void InsertText(TextBuffer* buffer, const char* text, size_t length) {
    if (!buffer || !text || length == 0) return;

    if (buffer->hasSelection) {
        DeleteSelection(buffer);
    }

    EnsureCapacity(buffer, buffer->length + length + 1);

    // Add undo entry
    AddUndoEntry(buffer, ACTION_INSERT, buffer->cursorPos, text, length);

    // Move text after cursor
    memmove(buffer->text + buffer->cursorPos + length,
            buffer->text + buffer->cursorPos,
            buffer->length - buffer->cursorPos + 1);

    memcpy(buffer->text + buffer->cursorPos, text, length);
    buffer->length += length;
    buffer->cursorPos += length;
    buffer->isModified = true;

    ClearRedoStack(buffer);
}

void DeleteChar(TextBuffer* buffer) {
    if (!buffer || buffer->cursorPos == 0) return;

    if (buffer->hasSelection) {
        DeleteSelection(buffer);
        return;
    }

    // Add undo entry
    AddUndoEntry(buffer, ACTION_DELETE, buffer->cursorPos - 1, buffer->text + buffer->cursorPos - 1, 1);

    // Move text
    memmove(buffer->text + buffer->cursorPos - 1,
            buffer->text + buffer->cursorPos,
            buffer->length - buffer->cursorPos + 1);

    buffer->length--;
    buffer->cursorPos--;
    buffer->isModified = true;

    ClearRedoStack(buffer);
}

void DeleteSelection(TextBuffer* buffer) {
    if (!buffer || !buffer->hasSelection) return;

    size_t start = buffer->selectionStart < buffer->selectionEnd ? buffer->selectionStart : buffer->selectionEnd;
    size_t end = buffer->selectionStart < buffer->selectionEnd ? buffer->selectionEnd : buffer->selectionStart;
    size_t deleteLen = end - start;

    // Add undo entry
    AddUndoEntry(buffer, ACTION_DELETE, start, buffer->text + start, deleteLen);

    // Move text
    memmove(buffer->text + start,
            buffer->text + end,
            buffer->length - end + 1);

    buffer->length -= deleteLen;
    buffer->cursorPos = start;
    buffer->hasSelection = false;
    buffer->isModified = true;

    ClearRedoStack(buffer);
}

void MoveCursorLeft(TextBuffer* buffer, bool byWord) {
    if (!buffer || buffer->cursorPos == 0) return;

    if (byWord) {
        buffer->cursorPos--;
        while (buffer->cursorPos > 0 && !IsWordChar(buffer->text[buffer->cursorPos])) {
            buffer->cursorPos--;
        }
        while (buffer->cursorPos > 0 && IsWordChar(buffer->text[buffer->cursorPos - 1])) {
            buffer->cursorPos--;
        }
    } else {
        buffer->cursorPos--;
    }
}

void MoveCursorRight(TextBuffer* buffer, bool byWord) {
    if (!buffer || buffer->cursorPos >= buffer->length) return;

    if (byWord) {
        while (buffer->cursorPos < buffer->length && !IsWordChar(buffer->text[buffer->cursorPos])) {
            buffer->cursorPos++;
        }
        while (buffer->cursorPos < buffer->length && IsWordChar(buffer->text[buffer->cursorPos])) {
            buffer->cursorPos++;
        }
    } else {
        buffer->cursorPos++;
    }
}

void MoveCursorUp(TextBuffer* buffer) {
    if (!buffer) return;

    size_t currentLine = GetLineFromPos(buffer, buffer->cursorPos);
    if (currentLine == 0) return;

    size_t currentCol = GetColumnFromPos(buffer, buffer->cursorPos);
    buffer->cursorPos = GetPosFromLineColumn(buffer, currentLine - 1, currentCol);
}

void MoveCursorDown(TextBuffer* buffer) {
    if (!buffer) return;

    size_t currentLine = GetLineFromPos(buffer, buffer->cursorPos);
    size_t totalLines = GetLineCount(buffer);
    if (currentLine >= totalLines - 1) return;

    size_t currentCol = GetColumnFromPos(buffer, buffer->cursorPos);
    buffer->cursorPos = GetPosFromLineColumn(buffer, currentLine + 1, currentCol);
}

void MoveCursorHome(TextBuffer* buffer) {
    if (!buffer) return;

    // Find start of current line
    size_t pos = buffer->cursorPos;
    while (pos > 0 && buffer->text[pos - 1] != '\n') {
        pos--;
    }
    buffer->cursorPos = pos;
}

void MoveCursorEnd(TextBuffer* buffer) {
    if (!buffer) return;

    // Find end of current line
    size_t pos = buffer->cursorPos;
    while (pos < buffer->length && buffer->text[pos] != '\n') {
        pos++;
    }
    buffer->cursorPos = pos;
}

bool LoadFile(TextBuffer* buffer, const char* filePath) {
    if (!buffer || !filePath) return false;

    FILE* file = fopen(filePath, "rb");
    if (!file) return false;

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    EnsureCapacity(buffer, fileSize + 1);

    size_t bytesRead = fread(buffer->text, 1, fileSize, file);
    fclose(file);

    buffer->text[bytesRead] = '\0';
    buffer->length = bytesRead;
    buffer->cursorPos = 0;
    buffer->hasSelection = false;
    buffer->isModified = false;
    strncpy(buffer->filePath, filePath, MAX_PATH_LENGTH - 1);
    buffer->filePath[MAX_PATH_LENGTH - 1] = '\0';

    return true;
}

bool SaveFile(TextBuffer* buffer, const char* filePath) {
    if (!buffer) return false;

    const char* path = filePath ? filePath : buffer->filePath;
    if (!path || path[0] == '\0') return false;

    FILE* file = fopen(path, "wb");
    if (!file) return false;

    size_t bytesWritten = fwrite(buffer->text, 1, buffer->length, file);
    fclose(file);

    if (bytesWritten == buffer->length) {
        buffer->isModified = false;
        if (filePath) {
            strncpy(buffer->filePath, filePath, MAX_PATH_LENGTH - 1);
            buffer->filePath[MAX_PATH_LENGTH - 1] = '\0';
        }
        return true;
    }

    return false;
}

static void AddUndoEntry(TextBuffer* buffer, ActionType type, size_t position, const char* text, size_t length) {
    UndoEntry* entry = (UndoEntry*)malloc(sizeof(UndoEntry));
    if (!entry) return;

    entry->type = type;
    entry->position = position;
    entry->length = length;
    entry->cursorPos = buffer->cursorPos;
    entry->text = (char*)malloc(length + 1);
    if (entry->text) {
        memcpy(entry->text, text, length);
        entry->text[length] = '\0';
    }

    entry->next = buffer->undoStack;
    entry->prev = NULL;
    if (buffer->undoStack) {
        buffer->undoStack->prev = entry;
    }
    buffer->undoStack = entry;
}

static void ClearRedoStack(TextBuffer* buffer) {
    UndoEntry* entry = buffer->redoStack;
    while (entry) {
        UndoEntry* next = entry->next;
        if (entry->text) free(entry->text);
        free(entry);
        entry = next;
    }
    buffer->redoStack = NULL;
}

void Undo(TextBuffer* buffer) {
    if (!buffer || !buffer->undoStack) return;

    UndoEntry* entry = buffer->undoStack;
    buffer->undoStack = entry->next;
    if (buffer->undoStack) {
        buffer->undoStack->prev = NULL;
    }

    if (entry->type == ACTION_INSERT) {
        // Remove inserted text
        memmove(buffer->text + entry->position,
                buffer->text + entry->position + entry->length,
                buffer->length - entry->position - entry->length + 1);
        buffer->length -= entry->length;
    } else {
        // Re-insert deleted text
        EnsureCapacity(buffer, buffer->length + entry->length + 1);
        memmove(buffer->text + entry->position + entry->length,
                buffer->text + entry->position,
                buffer->length - entry->position + 1);
        memcpy(buffer->text + entry->position, entry->text, entry->length);
        buffer->length += entry->length;
    }

    buffer->cursorPos = entry->cursorPos;
    buffer->isModified = true;

    // Move to redo stack
    entry->next = buffer->redoStack;
    entry->prev = NULL;
    if (buffer->redoStack) {
        buffer->redoStack->prev = entry;
    }
    buffer->redoStack = entry;
}

void Redo(TextBuffer* buffer) {
    if (!buffer || !buffer->redoStack) return;

    UndoEntry* entry = buffer->redoStack;
    buffer->redoStack = entry->next;
    if (buffer->redoStack) {
        buffer->redoStack->prev = NULL;
    }

    if (entry->type == ACTION_INSERT) {
        // Re-insert text
        EnsureCapacity(buffer, buffer->length + entry->length + 1);
        memmove(buffer->text + entry->position + entry->length,
                buffer->text + entry->position,
                buffer->length - entry->position + 1);
        memcpy(buffer->text + entry->position, entry->text, entry->length);
        buffer->length += entry->length;
        buffer->cursorPos = entry->position + entry->length;
    } else {
        // Re-delete text
        memmove(buffer->text + entry->position,
                buffer->text + entry->position + entry->length,
                buffer->length - entry->position - entry->length + 1);
        buffer->length -= entry->length;
        buffer->cursorPos = entry->position;
    }

    buffer->isModified = true;

    // Move back to undo stack
    entry->next = buffer->undoStack;
    entry->prev = NULL;
    if (buffer->undoStack) {
        buffer->undoStack->prev = entry;
    }
    buffer->undoStack = entry;
}

size_t GetLineFromPos(TextBuffer* buffer, size_t pos) {
    if (!buffer) return 0;

    size_t line = 0;
    for (size_t i = 0; i < pos && i < buffer->length; i++) {
        if (buffer->text[i] == '\n') {
            line++;
        }
    }
    return line;
}

size_t GetColumnFromPos(TextBuffer* buffer, size_t pos) {
    if (!buffer) return 0;

    size_t col = 0;
    size_t i = pos;
    while (i > 0 && buffer->text[i - 1] != '\n') {
        i--;
        col++;
    }
    return col;
}

size_t GetPosFromLineColumn(TextBuffer* buffer, size_t line, size_t column) {
    if (!buffer) return 0;

    size_t currentLine = 0;
    size_t pos = 0;

    // Find start of target line
    while (pos < buffer->length && currentLine < line) {
        if (buffer->text[pos] == '\n') {
            currentLine++;
        }
        pos++;
    }

    // Move to column
    size_t col = 0;
    while (pos < buffer->length && col < column && buffer->text[pos] != '\n') {
        pos++;
        col++;
    }

    return pos;
}

int GetLineCount(TextBuffer* buffer) {
    if (!buffer) return 0;

    int count = 1;
    for (size_t i = 0; i < buffer->length; i++) {
        if (buffer->text[i] == '\n') {
            count++;
        }
    }
    return count;
}

static bool IsWordChar(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

bool SearchForward(TextBuffer* buffer, const char* pattern, size_t* foundPos) {
    if (!buffer || !pattern || !foundPos || !pattern[0]) return false;
    
    size_t patternLen = strlen(pattern);
    size_t searchStart = buffer->cursorPos;
    
    for (size_t i = searchStart; i <= buffer->length - patternLen; i++) {
        bool match = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (tolower((unsigned char)buffer->text[i + j]) != tolower((unsigned char)pattern[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            *foundPos = i;
            return true;
        }
    }
    
    // Wrap around from beginning
    for (size_t i = 0; i < searchStart; i++) {
        bool match = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (tolower((unsigned char)buffer->text[i + j]) != tolower((unsigned char)pattern[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            *foundPos = i;
            return true;
        }
    }
    
    return false;
}

bool SearchBackward(TextBuffer* buffer, const char* pattern, size_t* foundPos) {
    if (!buffer || !pattern || !foundPos || !pattern[0]) return false;
    
    size_t patternLen = strlen(pattern);
    if (patternLen > buffer->length) return false;
    
    size_t searchStart = (buffer->cursorPos > 0) ? buffer->cursorPos - 1 : 0;
    
    // Search backward from cursor
    for (size_t i = searchStart; i != (size_t)-1; i--) {
        if (i + patternLen > buffer->length) continue;
        
        bool match = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (tolower((unsigned char)buffer->text[i + j]) != tolower((unsigned char)pattern[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            *foundPos = i;
            return true;
        }
    }
    
    // Wrap around from end
    for (size_t i = buffer->length - patternLen; i > searchStart; i--) {
        bool match = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (tolower((unsigned char)buffer->text[i + j]) != tolower((unsigned char)pattern[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            *foundPos = i;
            return true;
        }
    }
    
    return false;
}
