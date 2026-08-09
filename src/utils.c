/**********************************************************************************************
*
*   utils - Platform-independent utilities: trace log, memory and file loading/saving
*
*   This module implements the raylib "misc"/"files management" primitives that every
*   other raylib-on-vulkan module builds upon:
*     - TraceLog() + log level/callback configuration
*     - MemAlloc()/MemRealloc()/MemFree()
*     - LoadFileData()/SaveFileData()/LoadFileText()/SaveFileText() with user callbacks
*     - ExportDataAsCode()
*
*   NOTE: The file system query/manipulation helpers (FileExists, GetDirectoryPath, ...)
*   live in rcore.c, next to the rest of the core module.
*
*   License: zlib/libpng (same as raylib)
*
**********************************************************************************************/

#include "raylib.h"

#include "config.h"

#include <stdlib.h>                 // Required for: malloc(), realloc(), free()
#include <stdio.h>                  // Required for: vprintf(), fopen(), fseek(), fread(), fwrite(), fclose()
#include <stdarg.h>                 // Required for: va_list, va_start(), va_end()
#include <string.h>                 // Required for: strcpy(), strcat()

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------
#ifndef MAX_TRACELOG_MSG_LENGTH
    #define MAX_TRACELOG_MSG_LENGTH     256         // Max length of one trace-log message
#endif

// Allocators, following raylib's overridable pattern
#ifndef RL_MALLOC
    #define RL_MALLOC(sz)       malloc(sz)
#endif
#ifndef RL_CALLOC
    #define RL_CALLOC(n,sz)     calloc(n,sz)
#endif
#ifndef RL_REALLOC
    #define RL_REALLOC(ptr,sz)  realloc(ptr,sz)
#endif
#ifndef RL_FREE
    #define RL_FREE(ptr)        free(ptr)
#endif

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
static int logTypeLevel = LOG_INFO;                 // Minimum log type level

static TraceLogCallback traceLog = NULL;            // TraceLog callback function pointer
static LoadFileDataCallback loadFileData = NULL;    // LoadFileData callback function pointer
static SaveFileDataCallback saveFileData = NULL;    // SaveFileText callback function pointer
static LoadFileTextCallback loadFileText = NULL;    // LoadFileText callback function pointer
static SaveFileTextCallback saveFileText = NULL;    // SaveFileText callback function pointer

//----------------------------------------------------------------------------------
// Module Functions Definition - Utilities
//----------------------------------------------------------------------------------

// Set the current threshold (minimum) log level
void SetTraceLogLevel(int logLevel)
{
    logTypeLevel = logLevel;
}

// Set custom trace log
void SetTraceLogCallback(TraceLogCallback callback)
{
    traceLog = callback;
}

// Show trace log messages (LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR, LOG_FATAL)
void TraceLog(int logLevel, const char *text, ...)
{
#if SUPPORT_TRACELOG
    // Message has level below current threshold, don't emit it
    if (logLevel < logTypeLevel) return;

    va_list args;
    va_start(args, text);

    if (traceLog != NULL)
    {
        traceLog(logLevel, text, args);
        va_end(args);

        // NOTE: A fatal error still aborts, whatever the callback decides to print
        if (logLevel == LOG_FATAL) exit(EXIT_FAILURE);
        return;
    }

    char buffer[MAX_TRACELOG_MSG_LENGTH] = { 0 };

    switch (logLevel)
    {
        case LOG_TRACE: strcpy(buffer, "TRACE: "); break;
        case LOG_DEBUG: strcpy(buffer, "DEBUG: "); break;
        case LOG_INFO: strcpy(buffer, "INFO: "); break;
        case LOG_WARNING: strcpy(buffer, "WARNING: "); break;
        case LOG_ERROR: strcpy(buffer, "ERROR: "); break;
        case LOG_FATAL: strcpy(buffer, "FATAL: "); break;
        default: break;
    }

    unsigned int textSize = (unsigned int)strlen(text);
    memcpy(buffer + strlen(buffer), text, (textSize < (MAX_TRACELOG_MSG_LENGTH - 12))? textSize : (MAX_TRACELOG_MSG_LENGTH - 12));
    strcat(buffer, "\n");
    vprintf(buffer, args);
    fflush(stdout);

    va_end(args);

    if (logLevel == LOG_FATAL) exit(EXIT_FAILURE);  // If fatal logging, exit program
#else
    (void)logLevel;
    (void)text;
#endif
}

// Internal memory allocator
void *MemAlloc(unsigned int size)
{
    void *ptr = RL_MALLOC(size);
    return ptr;
}

// Internal memory reallocator
void *MemRealloc(void *ptr, unsigned int size)
{
    void *ret = RL_REALLOC(ptr, size);
    return ret;
}

// Internal memory free
void MemFree(void *ptr)
{
    RL_FREE(ptr);
}

// Set custom file data loader
void SetLoadFileDataCallback(LoadFileDataCallback callback) { loadFileData = callback; }

// Set custom file data saver
void SetSaveFileDataCallback(SaveFileDataCallback callback) { saveFileData = callback; }

// Set custom file text loader
void SetLoadFileTextCallback(LoadFileTextCallback callback) { loadFileText = callback; }

// Set custom file text saver
void SetSaveFileTextCallback(SaveFileTextCallback callback) { saveFileText = callback; }

// Load data from file into a buffer
unsigned char *LoadFileData(const char *fileName, int *dataSize)
{
    unsigned char *data = NULL;
    if (dataSize != NULL) *dataSize = 0;

    if (fileName == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: File name provided is not valid");
        return NULL;
    }

    if (loadFileData != NULL) return loadFileData(fileName, dataSize);

    FILE *file = fopen(fileName, "rb");
    if (file == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to open file", fileName);
        return NULL;
    }

    // WARNING: On binary streams SEEK_END could not be found,
    // using fseek() and ftell() could not work in some (rare) cases
    fseek(file, 0, SEEK_END);
    long size = ftell(file);        // Get file size
    fseek(file, 0, SEEK_SET);       // Reset file pointer

    if (size <= 0)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to read file", fileName);
        fclose(file);
        return NULL;
    }

    data = (unsigned char *)RL_MALLOC(size*sizeof(unsigned char));
    if (data == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to allocated memory for file reading", fileName);
        fclose(file);
        return NULL;
    }

    // NOTE: fread() returns number of read elements instead of bytes, so we read [1 byte, size elements]
    size_t count = fread(data, sizeof(unsigned char), size, file);

    if (count == 0) TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to read file", fileName);
    else if (count != (size_t)size) TRACELOG(LOG_WARNING, "FILEIO: [%s] File partially loaded (%i bytes out of %i)", fileName, (int)count, (int)size);
    else TRACELOG(LOG_INFO, "FILEIO: [%s] File loaded successfully", fileName);

    if (dataSize != NULL) *dataSize = (int)count;

    fclose(file);

    return data;
}

// Unload file data allocated by LoadFileData()
void UnloadFileData(unsigned char *data)
{
    RL_FREE(data);
}

// Save data to file from buffer
bool SaveFileData(const char *fileName, const void *data, int dataSize)
{
    if (fileName == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: File name provided is not valid");
        return false;
    }

    if (saveFileData != NULL) return saveFileData(fileName, (void *)data, dataSize);

    if ((data == NULL) || (dataSize <= 0))
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] No data provided to write", fileName);
        return false;
    }

    FILE *file = fopen(fileName, "wb");
    if (file == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to open file", fileName);
        return false;
    }

    size_t count = fwrite(data, sizeof(unsigned char), dataSize, file);

    if (count == 0) TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to write file", fileName);
    else if (count != (size_t)dataSize) TRACELOG(LOG_WARNING, "FILEIO: [%s] File partially written", fileName);
    else TRACELOG(LOG_INFO, "FILEIO: [%s] File saved successfully", fileName);

    int result = fclose(file);
    if (result != 0) return false;

    return true;
}

// Export data to code (.h), returns true on success
bool ExportDataAsCode(const unsigned char *data, int dataSize, const char *fileName)
{
    // NOTE: Text data buffer size is estimated considering raw data size in bytes
    // and requiring 6 char bytes for every byte: "0xFF, "
    const int bytesPerLine = 20;
    const int headerSize = 512;

    if ((data == NULL) || (dataSize <= 0) || (fileName == NULL)) return false;

    int txtDataCapacity = headerSize + dataSize*6 + (dataSize/bytesPerLine + 1)*2 + 64;
    char *txtData = (char *)RL_CALLOC(txtDataCapacity, sizeof(char));
    if (txtData == NULL) return false;

    int byteCount = 0;
    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "////////////////////////////////////////////////////////////////////////////////////////\n");
    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "//                                                                                    //\n");
    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "// DataAsCode exporter v1.0 - Raw data exported as an array of bytes                  //\n");
    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "//                                                                                    //\n");
    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "// more info and bugs-report:  github.com/raysan5/raylib                              //\n");
    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "// feedback and support:       ray[at]raylib.com                                      //\n");
    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "//                                                                                    //\n");
    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "////////////////////////////////////////////////////////////////////////////////////////\n\n");

    // Get file name from path and convert variable name to uppercase
    char varFileName[256] = { 0 };
    strncpy(varFileName, GetFileNameWithoutExt(fileName), sizeof(varFileName) - 1);
    for (int i = 0; (i < 256) && (varFileName[i] != '\0'); i++)
    {
        if ((varFileName[i] >= 'a') && (varFileName[i] <= 'z')) varFileName[i] = varFileName[i] - 32;
        else if (!(((varFileName[i] >= 'A') && (varFileName[i] <= 'Z')) ||
                   ((varFileName[i] >= '0') && (varFileName[i] <= '9')))) varFileName[i] = '_';
    }

    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "#define %s_DATA_SIZE %i\n\n", varFileName, dataSize);
    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "static unsigned char %s_DATA[%s_DATA_SIZE] = { ", varFileName, varFileName);
    for (int i = 0; i < (dataSize - 1); i++)
    {
        byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount,
                              ((i%bytesPerLine == 0)? "0x%x,\n" : "0x%x, "), data[i]);
    }
    byteCount += snprintf(txtData + byteCount, txtDataCapacity - byteCount, "0x%x };\n", data[dataSize - 1]);

    // NOTE: Text data length exported is determined by '\0' (NULL) character
    bool success = SaveFileText(fileName, txtData);

    RL_FREE(txtData);

    if (success) TRACELOG(LOG_INFO, "FILEIO: [%s] Data as code exported successfully", fileName);
    else TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to export data as code", fileName);

    return success;
}

// Load text data from file, returns a '\0' terminated string
// NOTE: text chars array should be freed manually
char *LoadFileText(const char *fileName)
{
    char *text = NULL;

    if (fileName == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: File name provided is not valid");
        return NULL;
    }

    if (loadFileText != NULL) return loadFileText(fileName);

    FILE *file = fopen(fileName, "rt");
    if (file == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to open text file", fileName);
        return NULL;
    }

    // WARNING: When reading a file as 'text' file,
    // text mode causes carriage return-linefeed translation...
    // ...but using fseek() should return correct byte-offset
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size <= 0)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to read text file", fileName);
        fclose(file);
        return NULL;
    }

    text = (char *)RL_MALLOC((size + 1)*sizeof(char));
    if (text == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to allocated memory for file reading", fileName);
        fclose(file);
        return NULL;
    }

    // NOTE: fread() returns number of read elements instead of bytes, so we read [1 byte, size elements]
    size_t count = fread(text, sizeof(char), size, file);

    // WARNING: \r\n is converted to \n on reading, so,
    // read bytes count gets reduced by the number of lines
    if (count < (size_t)size) text = (char *)RL_REALLOC(text, count + 1);

    // Zero-terminate the string
    text[count] = '\0';

    TRACELOG(LOG_INFO, "FILEIO: [%s] Text file loaded successfully", fileName);

    fclose(file);

    return text;
}

// Unload file text data allocated by LoadFileText()
void UnloadFileText(char *text)
{
    RL_FREE(text);
}

// Save text data to file (write), string must be '\0' terminated
bool SaveFileText(const char *fileName, const char *text)
{
    if (fileName == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: File name provided is not valid");
        return false;
    }

    if (saveFileText != NULL) return saveFileText(fileName, (char *)text);

    if (text == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] No text data provided to write", fileName);
        return false;
    }

    FILE *file = fopen(fileName, "wt");
    if (file == NULL)
    {
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to open text file", fileName);
        return false;
    }

    int count = fprintf(file, "%s", text);

    if (count < 0) TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to write text file", fileName);
    else TRACELOG(LOG_INFO, "FILEIO: [%s] Text file saved successfully", fileName);

    int result = fclose(file);
    if (result != 0) return false;

    return true;
}
