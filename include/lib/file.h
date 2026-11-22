#ifndef LIB_FILE_H_
#define LIB_FILE_H_

#include <lib/winapp.h>
#include <winapi/types.h>
#include <string>
#include <vector>


namespace win32
{

enum
{
    FILE_SHARE_READ                 = 0x00000001,
    FILE_SHARE_WRITE                = 0x00000002,
    FILE_SHARE_DELETE               = 0x00000004,

    CREATE_NEW                      = 1,
    CREATE_ALWAYS                   = 2,
    OPEN_EXISTING                   = 3,
    OPEN_ALWAYS                     = 4,
    TRUNCATE_EXISTING               = 5,

    FILE_READ_DATA                  = 0x00000001,
    FILE_WRITE_DATA                 = 0x00000002,
    FILE_APPEND_DATA                = 0x00000004,
    GENERIC_ALL                     = 0x10000000,
    GENERIC_WRITE                   = 0x40000000,
    GENERIC_READ                    = 0x80000000,


    FILE_ATTRIBUTE_READONLY         = 0x0001,
    FILE_ATTRIBUTE_HIDDEN           = 0x0002,
    FILE_ATTRIBUTE_SYSTEM           = 0x0004,
    FILE_ATTRIBUTE_ARCHIVE          = 0x0020,
    FILE_ATTRIBUTE_NORMAL           = 0x0080,
    FILE_ATTRIBUTE_TEMPORARY        = 0x0100,
    FILE_ATTRIBUTE_OFFLINE          = 0x1000,
    FILE_ATTRIBUTE_ENCRYPTED        = 0x4000,

    FILE_FLAG_OPEN_NO_RECALL        = 0x00100000,
    FILE_FLAG_OPEN_REPARSE_POINT    = 0x00200000,
    FILE_FLAG_SESSION_AWARE         = 0x00800000,
    FILE_FLAG_POSIX_SEMANTICS       = 0x01000000,
    FILE_FLAG_BACKUP_SEMANTICS      = 0x02000000,
    FILE_FLAG_DELETE_ON_CLOSE       = 0x04000000,
    FILE_FLAG_SEQUENTIAL_SCAN       = 0x08000000,
    FILE_FLAG_RANDOM_ACCESS         = 0x10000000,
    FILE_FLAG_NO_BUFFERING          = 0x20000000,
    FILE_FLAG_OVERLAPPED            = 0x40000000,
    FILE_FLAG_WRITE_THROUGH         = 0x80000000
};

class FileEnumerator : public GenericResource
{
public:
    FileEnumerator(const char* filename);
    ~FileEnumerator();

    bool findNext(WIN32_FIND_DATAA* data);

private:
    std::string                     m_pattern;
    std::vector<WIN32_FIND_DATAA>   m_files;
    int                             m_index;
};

// A read-only file keeps its descriptor only while it is among the most recently used ones (a bounded pool); it is
// reopened at its position on the next access. Files opened for writing keep their descriptor until closed.
class File : public GenericResource
{
public:
    // Edits data just read: offset is the data's position in the file. Any thread.
    typedef void (*ReadFilter)(x86::reg32 offset, x86::reg8* data, x86::reg32 length);
    // Reads in place of the descriptor's read: up to length bytes at offset into data, from the descriptor or
    // elsewhere; leaves the descriptor at offset plus the bytes given, which it returns (-1 on error). Any thread.
    typedef x86::reg32 (*ReadSource)(int file, x86::reg32 offset, x86::reg8* data, x86::reg32 length);

    File(int file, const char* name);
    File(const File& other);
    File(const char* path, x86::reg32 mode, x86::reg32 flags, x86::reg32 creationFlags);
    ~File();
    File& operator=(const File&) = delete;

    x86::reg32 seek(x86::reg32 mode, x86::sreg32 offset);
    x86::reg32 read(void* buffer, x86::reg32 max, x86::reg32* totalRead);
    x86::reg32 write(const void *buffer, x86::reg32 max, x86::reg32* totalWritten);
    x86::reg32 truncate();
    x86::reg32 size() const;

    static std::string getCurrentDirectory();
    // False if the directory does not exist.
    static bool setCurrentDirectory(const char* path);
    static void setDataDirectory(const char* path);
    static void setCdDirectory(const char* path);
    static x86::reg32 remove(const char* filename);
    // Size and free space of the volume holding the data folder, in bytes. False if unknown.
    static bool diskSpace(x86::reg64* total, x86::reg64* free);
    // Host path of a guest path, without a trailing separator.
    static std::string hostPath(const char* path);
    // Applies the filter to reads of files opened later whose host path ends with the suffix (lower case).
    // Before guest code runs.
    static void setReadFilter(const char* pathSuffix, ReadFilter filter);
    // Reads files opened later whose host path ends with the suffix (lower case) through the source. Before guest
    // code runs.
    static void setReadSource(const char* pathSuffix, ReadSource source);

    operator void*() { return m_valid ? this : nullptr; }
private:
    void initReadFilter();
    // Opens the descriptor (pool lock held), closing idle pooled ones when the pool or the system is full.
    bool openDescriptor(int flags);
    // Closes the descriptor and keeps the position (pool lock held).
    void closeDescriptor();
    void leavePool();
    void joinPoolNewest();
    // Closes the least recently used idle pooled descriptor; false if there is none (pool lock held).
    static bool evictOldest();
    // The descriptor, reopened if needed and kept until release(); -1 if the file cannot be opened.
    int acquire();
    void release();

    int m_file;             // -1 while closed
    std::string m_filename;
    ReadFilter m_readFilter;
    ReadSource m_readSource;
    int m_reopenFlags;      // open flags without creation or truncation
    long m_position;        // position while closed
    int m_busy;             // operations using the descriptor
    bool m_valid;
    bool m_pooled;          // read-only: closed when idle and reopened on demand
    File* m_older;          // pooled files with a descriptor, least recently used first
    File* m_newer;
};

}

#endif
