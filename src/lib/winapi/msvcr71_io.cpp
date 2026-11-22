// msvcr71.dll: streams (FILE in guest memory, _flag kept for the feof/ferror macros), low-level files with the
// runtime's text mode (CR-LF to LF, CTRL-Z ends the file), folders, the environment, formatted output and time
// (the time zone is UTC, as GetTimeZoneInformation reports).

#include "msvcr71_internal.h"
#include <winapi/support.h>
#include <lib/file.h>
#include <lib/heap.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <recomp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace win32 { namespace msvcr71
{

namespace
{

enum : x86::reg32
{
    IoRead = 0x1, IoWrite = 0x2, IoEof = 0x10, IoError = 0x20, IoReadWrite = 0x80,
    FileSlots = 20, FileBytes = 32
};

enum : int
{
    OpenWriteOnly = 1, OpenReadWrite = 2, OpenAppend = 0x8, OpenCreate = 0x100, OpenTruncate = 0x200,
    OpenExclusive = 0x400, OpenText = 0x4000, OpenBinary = 0x8000
};

struct Descriptor
{
    int     host = -1;      // -1: free; the console descriptors 0-2 have none
    bool    text = false;
    bool    eof = false;    // CTRL-Z seen in text mode
    int     console = -1;   // 0 stdin, 1 stdout, 2 stderr
};

struct Stream
{
    int     fd;
    int     pushback = -1;
    bool    owned;          // the FILE block was allocated on the guest heap
};

pf::LwMutex s_ioMutex("msvcr71_io");
std::vector<Descriptor> s_descriptors;
std::map<x86::reg32, Stream> s_streams;
std::string s_consoleLine[3];

void initDescriptors()
{
    if (!s_descriptors.empty())
        return;
    s_descriptors.resize(3);
    for (int i = 0; i < 3; ++i)
    {
        s_descriptors[size_t(i)].console = i;
        s_descriptors[size_t(i)].text = true;
    }
}

Descriptor* descriptor(int fd)
{
    initDescriptors();
    if (fd < 0 || size_t(fd) >= s_descriptors.size())
        return nullptr;
    Descriptor& d = s_descriptors[size_t(fd)];
    return d.console >= 0 || d.host >= 0 ? &d : nullptr;
}

int newDescriptor(int host, bool text)
{
    initDescriptors();
    size_t fd = 3;
    while (fd < s_descriptors.size() && (s_descriptors[fd].host >= 0 || s_descriptors[fd].console >= 0))
        ++fd;
    if (fd == s_descriptors.size())
        s_descriptors.emplace_back();
    s_descriptors[fd].host = host;
    s_descriptors[fd].text = text;
    s_descriptors[fd].eof = false;
    s_descriptors[fd].console = -1;
    return int(fd);
}

void consoleWrite(int which, const char* data, size_t length)
{
    std::string& line = s_consoleLine[which];
    for (size_t i = 0; i < length; ++i)
    {
        if (data[i] == '\n')
        {
            pf::logInfo("%s: %s", which == 2 ? "stderr" : "stdout", line.c_str());
            line.clear();
        }
        else if (data[i] != '\r')
        {
            line += data[i];
        }
    }
}

// Reads with the descriptor's mode; returns the bytes delivered, 0 at the end, -1 on an error.
int readDescriptor(Descriptor& d, char* out, x86::reg32 count)
{
    if (d.console >= 0)
        return 0;
    if (!d.text)
        return int(::read(d.host, out, count));
    if (d.eof || !count)
        return 0;
    std::vector<char> raw(count);
    const ssize_t got = ::read(d.host, raw.data(), count);
    if (got <= 0)
        return int(got);
    int n = 0;
    for (ssize_t i = 0; i < got; ++i)
    {
        const char c = raw[size_t(i)];
        if (c == 0x1a)
        {
            d.eof = true;
            break;
        }
        if (c != '\r')
        {
            out[n++] = c;
            continue;
        }
        if (i + 1 < got)
        {
            if (raw[size_t(i) + 1] == '\n')
            {
                out[n++] = '\n';
                ++i;
            }
            else
            {
                out[n++] = '\r';
            }
            continue;
        }
        char next;
        const ssize_t peeked = ::read(d.host, &next, 1);
        if (peeked == 1 && next == '\n')
        {
            out[n++] = '\n';
        }
        else
        {
            out[n++] = '\r';
            if (peeked == 1)
                ::lseek(d.host, -1, SEEK_CUR);
        }
    }
    return n;
}

// Writes with the descriptor's mode; returns the source bytes written, -1 on an error.
int writeDescriptor(Descriptor& d, const char* data, x86::reg32 count)
{
    if (d.console >= 0)
    {
        if (d.console > 0)
            consoleWrite(d.console, data, count);
        return int(count);
    }
    if (!d.text)
        return int(::write(d.host, data, count));
    std::string converted;
    converted.reserve(count + count / 8);
    for (x86::reg32 i = 0; i < count; ++i)
    {
        if (data[i] == '\n')
            converted += '\r';
        converted += data[i];
    }
    const ssize_t written = ::write(d.host, converted.data(), converted.size());
    if (written < 0)
        return -1;
    return written == ssize_t(converted.size()) ? int(count) : -1;
}

Stream* stream(x86::reg32 file)
{
    initDescriptors();
    auto it = s_streams.find(file);
    if (it != s_streams.end())
        return &it->second;
    const x86::reg32 iob = iobAddress();
    for (x86::reg32 i = 0; i < 3; ++i)
    {
        if (file == iob + i * FileBytes)
            return &(s_streams[file] = Stream{ int(i), -1, false });
    }
    return nullptr;
}

x86::reg32& fileFlag(x86::reg32 file)
{
    return *reinterpret_cast<x86::reg32*>(g_guestBase + file + 12);
}

int hostFlags(int flags)
{
    int host = (flags & 3) == OpenWriteOnly ? O_WRONLY : (flags & 3) == OpenReadWrite ? O_RDWR : O_RDONLY;
    if (flags & OpenAppend)
        host |= O_APPEND;
    if (flags & OpenCreate)
        host |= O_CREAT;
    if (flags & OpenTruncate)
        host |= O_TRUNC;
    if (flags & OpenExclusive)
        host |= O_EXCL;
    return host;
}

bool textMode(int flags)
{
    return !(flags & OpenBinary) && ((flags & OpenText) || fileMode() != OpenBinary);
}

std::string resolveFullPath(const char* path)
{
    std::string full;
    if (std::strlen(path) >= 2 && path[1] == ':')
    {
        full = path;
    }
    else
    {
        full = File::getCurrentDirectory();
        if (path[0] == '\\' || path[0] == '/')
            full = full.substr(0, 2);
        else if (full.back() != '\\')
            full += '\\';
        full += path;
    }
    for (char& c : full)
    {
        if (c == '/')
            c = '\\';
    }
    std::vector<std::string> parts;
    size_t start = 3;
    while (start <= full.size())
    {
        const size_t end = full.find('\\', start);
        const std::string part = full.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..")
        {
            if (!parts.empty())
                parts.pop_back();
        }
        else if (!part.empty() && part != ".")
        {
            parts.push_back(part);
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    std::string out = full.substr(0, 3);
    for (size_t i = 0; i < parts.size(); ++i)
        out += (i ? "\\" : "") + parts[i];
    if (!full.empty() && full.back() == '\\' && !parts.empty())
        out += '\\';
    return out;
}

// ---- Time ---------------------------------------------------------------------------------------------------------

x86::sreg64 nowSeconds()
{
    struct timeval now;
    gettimeofday(&now, nullptr);
    return x86::sreg64(now.tv_sec);
}

x86::sreg64 daysFromCivil(x86::sreg64 y, x86::sreg64 m, x86::sreg64 d)
{
    y -= m <= 2;
    const x86::sreg64 era = (y >= 0 ? y : y - 399) / 400;
    const x86::sreg64 yoe = y - era * 400;
    const x86::sreg64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const x86::sreg64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// struct tm (nine ints) of a time; false outside 1970-3000.
bool brokenDown(x86::sreg64 t, x86::reg32* tm)
{
    if (t < 0 || t > 32535215999ll)
        return false;
    x86::sreg64 days = t / 86400;
    const x86::sreg64 rest = t % 86400;
    tm[0] = x86::reg32(rest % 60);
    tm[1] = x86::reg32((rest / 60) % 60);
    tm[2] = x86::reg32(rest / 3600);
    tm[6] = x86::reg32((days + 4) % 7);
    const x86::sreg64 z = days + 719468;
    const x86::sreg64 era = z / 146097;
    const x86::sreg64 doe = z - era * 146097;
    const x86::sreg64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    x86::sreg64 y = yoe + era * 400;
    const x86::sreg64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const x86::sreg64 mp = (5 * doy + 2) / 153;
    const x86::sreg64 d = doy - (153 * mp + 2) / 5 + 1;
    const x86::sreg64 m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    tm[3] = x86::reg32(d);
    tm[4] = x86::reg32(m - 1);
    tm[5] = x86::reg32(y - 1900);
    tm[7] = x86::reg32(days - daysFromCivil(y, 1, 1));
    tm[8] = 0;
    return true;
}

x86::reg32 brokenDownResult(WinApplication* app, const x86::CPU& cpu, x86::sreg64 t)
{
    ThreadData& data = threadDataOf(app, cpu);
    if (!brokenDown(t, data.tm))
        return 0;
    return threadData(app, cpu) + x86::reg32(offsetof(ThreadData, tm));
}

// mktime: normalises the fields, writes them back and returns the time, -1 outside the range.
x86::sreg64 makeTime(x86::reg32 address, x86::sreg64 limit)
{
    x86::sreg32 tm[9];
    std::memcpy(tm, g_guestBase + address, sizeof(tm));
    x86::sreg64 month = tm[4];
    x86::sreg64 year = x86::sreg64(tm[5]) + 1900 + (month >= 0 ? month / 12 : (month - 11) / 12);
    month = ((month % 12) + 12) % 12;
    const x86::sreg64 t = (daysFromCivil(year, month + 1, 1) + tm[3] - 1) * 86400 + x86::sreg64(tm[2]) * 3600
                        + x86::sreg64(tm[1]) * 60 + tm[0];
    x86::reg32 normal[9];
    if (t > limit || !brokenDown(t, normal))
        return -1;
    std::memcpy(g_guestBase + address, normal, sizeof(normal));
    return t;
}

}

// ---- Streams -------------------------------------------------------------------------------------------------------

bool streamWrite(WinApplication* app, const x86::CPU& cpu, x86::reg32 file, const char* data, size_t length)
{
    pf::LwLockGuard guard(s_ioMutex);
    Stream* s = stream(file);
    Descriptor* d = s ? descriptor(s->fd) : nullptr;
    if (!d)
    {
        setErrno(app, cpu, ErrBadF);
        return false;
    }
    if (writeDescriptor(*d, data, x86::reg32(length)) < 0)
    {
        fileFlag(file) |= IoError;
        return false;
    }
    return true;
}

x86::reg32 fopen(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 path, x86::reg32 mode)
{
    const char* m = guestChars(mode);
    int flags;
    x86::reg32 streamFlag;
    switch (m[0])
    {
    case 'r': flags = 0; streamFlag = IoRead; break;
    case 'w': flags = OpenWriteOnly | OpenCreate | OpenTruncate; streamFlag = IoWrite; break;
    case 'a': flags = OpenWriteOnly | OpenCreate | OpenAppend; streamFlag = IoWrite; break;
    default:
        setErrno(app, cpu, ErrInval);
        return 0;
    }
    for (const char* p = m + 1; *p; ++p)
    {
        if (*p == '+')
        {
            flags = (flags & ~3) | OpenReadWrite;
            streamFlag = IoReadWrite;
        }
        else if (*p == 't')
        {
            flags |= OpenText;
        }
        else if (*p == 'b')
        {
            flags |= OpenBinary;
        }
    }
    const std::string host = File::hostPath(guestChars(path));
    const int fd = ::open(host.c_str(), hostFlags(flags), 0664);
    if (fd < 0)
    {
        setErrno(app, cpu, crtErrno(errno));
        pf::logDebug("fopen %s (%s): errno %d", guestChars(path), m, errno);
        return 0;
    }
    pf::LwLockGuard guard(s_ioMutex);
    const x86::reg32 iob = data_iob(app);
    x86::reg32 file = 0;
    bool owned = false;
    for (x86::reg32 i = 3; i < FileSlots && !file; ++i)
    {
        const x86::reg32 slot = iob + i * FileBytes;
        if (!s_streams.count(slot))
            file = slot;
    }
    if (!file)
    {
        file = heap::allocate(FileBytes, true);
        owned = true;
    }
    if (!file)
    {
        ::close(fd);
        setErrno(app, cpu, ErrMFile);
        return 0;
    }
    std::memset(g_guestBase + file, 0, FileBytes);
    const int guestFd = newDescriptor(fd, textMode(flags));
    fileFlag(file) = streamFlag;
    rc::st32(file + 16, x86::reg32(guestFd));
    s_streams[file] = Stream{ guestFd, -1, owned };
    return file;
}

int fclose(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 file)
{
    pf::LwLockGuard guard(s_ioMutex);
    Stream* s = stream(file);
    if (!s)
    {
        setErrno(app, cpu, ErrInval);
        return -1;
    }
    Descriptor* d = descriptor(s->fd);
    int result = 0;
    if (d && d->console < 0)
    {
        result = ::close(d->host) == 0 ? 0 : -1;
        d->host = -1;
    }
    const bool owned = s->owned;
    s_streams.erase(file);
    std::memset(g_guestBase + file, 0, FileBytes);
    if (owned)
        heap::release(file);
    return result;
}

UINT fread(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, UINT size, UINT count, x86::reg32 file)
{
    const x86::reg64 total = x86::reg64(size) * count;
    if (!total)
        return 0;
    pf::LwLockGuard guard(s_ioMutex);
    Stream* s = stream(file);
    Descriptor* d = s ? descriptor(s->fd) : nullptr;
    if (!d)
    {
        setErrno(app, cpu, ErrBadF);
        return 0;
    }
    char* out = guestChars(buffer);
    x86::reg64 got = 0;
    if (s->pushback >= 0)
    {
        out[got++] = char(s->pushback);
        s->pushback = -1;
    }
    while (got < total)
    {
        const int n = readDescriptor(*d, out + got, x86::reg32(total - got));
        if (n <= 0)
        {
            fileFlag(file) |= n < 0 ? IoError : IoEof;
            break;
        }
        got += x86::reg64(n);
    }
    return UINT(got / size);
}

UINT fwrite(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, UINT size, UINT count, x86::reg32 file)
{
    const x86::reg64 total = x86::reg64(size) * count;
    if (!total)
        return 0;
    return streamWrite(app, cpu, file, guestChars(buffer), size_t(total)) ? count : 0;
}

int fflush(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 file)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(file);
    return 0;
}

int fgetc(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 file)
{
    pf::LwLockGuard guard(s_ioMutex);
    Stream* s = stream(file);
    Descriptor* d = s ? descriptor(s->fd) : nullptr;
    if (!d)
    {
        setErrno(app, cpu, ErrBadF);
        return -1;
    }
    if (s->pushback >= 0)
    {
        const int c = s->pushback;
        s->pushback = -1;
        return c;
    }
    char c;
    const int n = readDescriptor(*d, &c, 1);
    if (n <= 0)
    {
        fileFlag(file) |= n < 0 ? IoError : IoEof;
        return -1;
    }
    return x86::reg8(c);
}

int fputc(WinApplication* app, x86::CPU& cpu, Cdecl, int c, x86::reg32 file)
{
    const char byte = char(c);
    return streamWrite(app, cpu, file, &byte, 1) ? (c & 0xff) : -1;
}

int ungetc(WinApplication* app, x86::CPU& cpu, Cdecl, int c, x86::reg32 file)
{
    RC_USE(app);
    RC_USE(cpu);
    pf::LwLockGuard guard(s_ioMutex);
    Stream* s = stream(file);
    if (c == -1 || !s || s->pushback >= 0)
        return -1;
    s->pushback = c & 0xff;
    fileFlag(file) &= ~IoEof;
    return c & 0xff;
}

// ---- Formatted text ------------------------------------------------------------------------------------------------

int printf(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    GuestArgs args(cpu.esp + 8);
    const std::string text = formatNarrow(app, rc::ld32(cpu.esp + 4), args);
    return streamWrite(app, cpu, data_iob(app) + FileBytes, text.data(), text.size()) ? int(text.size()) : -1;
}

int fprintf(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    GuestArgs args(cpu.esp + 12);
    const std::string text = formatNarrow(app, rc::ld32(cpu.esp + 8), args);
    return streamWrite(app, cpu, rc::ld32(cpu.esp + 4), text.data(), text.size()) ? int(text.size()) : -1;
}

int vfprintf(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 file, x86::reg32 format, x86::reg32 arguments)
{
    GuestArgs args(arguments);
    const std::string text = formatNarrow(app, format, args);
    return streamWrite(app, cpu, file, text.data(), text.size()) ? int(text.size()) : -1;
}

int sprintf(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    GuestArgs args(cpu.esp + 12);
    const std::string text = formatNarrow(app, rc::ld32(cpu.esp + 8), args);
    std::memcpy(guestChars(rc::ld32(cpu.esp + 4)), text.c_str(), text.size() + 1);
    return int(text.size());
}

int vsprintf(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, x86::reg32 format, x86::reg32 arguments)
{
    RC_USE(cpu);
    GuestArgs args(arguments);
    const std::string text = formatNarrow(app, format, args);
    std::memcpy(guestChars(buffer), text.c_str(), text.size() + 1);
    return int(text.size());
}

// The count is the buffer size: the terminator is written only when it fits, -1 when the text was cut.
int _vsnprintf(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, UINT count, x86::reg32 format,
               x86::reg32 arguments)
{
    RC_USE(cpu);
    GuestArgs args(arguments);
    const std::string text = formatNarrow(app, format, args);
    if (text.size() < count)
    {
        std::memcpy(guestChars(buffer), text.c_str(), text.size() + 1);
        return int(text.size());
    }
    std::memcpy(guestChars(buffer), text.data(), count);
    return text.size() == count ? int(count) : -1;
}

int _snprintf(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    return _vsnprintf(app, cpu, Cdecl{}, rc::ld32(cpu.esp + 4), rc::ld32(cpu.esp + 8), rc::ld32(cpu.esp + 12),
                      cpu.esp + 16);
}

int vswprintf_count(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, UINT count, x86::reg32 format,
                    x86::reg32 arguments)
{
    RC_USE(cpu);
    GuestArgs args(arguments);
    const std::u16string text = formatWide(app, format, args);
    const size_t n = text.size() < count ? text.size() + 1 : count;
    for (size_t i = 0; i < n; ++i)
        rc::st16(buffer + x86::reg32(2 * i), i < text.size() ? x86::reg16(text[i]) : 0);
    if (text.size() < count)
        return int(text.size());
    return text.size() == count ? int(count) : -1;
}

int sscanf(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    RC_USE(app);
    GuestArgs args(cpu.esp + 12);
    return scanNarrow(guestChars(rc::ld32(cpu.esp + 4)), guestChars(rc::ld32(cpu.esp + 8)), args);
}

// ---- Low-level files -----------------------------------------------------------------------------------------------

int _open(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 path, int flags, int permissions)
{
    RC_USE(permissions);
    const std::string host = File::hostPath(guestChars(path));
    const int fd = ::open(host.c_str(), hostFlags(flags), 0664);
    if (fd < 0)
    {
        setErrno(app, cpu, crtErrno(errno));
        return -1;
    }
    pf::LwLockGuard guard(s_ioMutex);
    return newDescriptor(fd, textMode(flags));
}

int _close(WinApplication* app, x86::CPU& cpu, Cdecl, int fd)
{
    pf::LwLockGuard guard(s_ioMutex);
    Descriptor* d = descriptor(fd);
    if (!d || d->console >= 0)
    {
        setErrno(app, cpu, ErrBadF);
        return -1;
    }
    const int result = ::close(d->host) == 0 ? 0 : -1;
    d->host = -1;
    return result;
}

int _read(WinApplication* app, x86::CPU& cpu, Cdecl, int fd, x86::reg32 buffer, UINT count)
{
    pf::LwLockGuard guard(s_ioMutex);
    Descriptor* d = descriptor(fd);
    if (!d)
    {
        setErrno(app, cpu, ErrBadF);
        return -1;
    }
    const int n = readDescriptor(*d, guestChars(buffer), count);
    if (n < 0)
        setErrno(app, cpu, crtErrno(errno));
    return n;
}

int _write(WinApplication* app, x86::CPU& cpu, Cdecl, int fd, x86::reg32 buffer, UINT count)
{
    pf::LwLockGuard guard(s_ioMutex);
    Descriptor* d = descriptor(fd);
    if (!d)
    {
        setErrno(app, cpu, ErrBadF);
        return -1;
    }
    const int n = writeDescriptor(*d, guestChars(buffer), count);
    if (n < 0)
        setErrno(app, cpu, crtErrno(errno));
    return n;
}

x86::sreg32 _lseek(WinApplication* app, x86::CPU& cpu, Cdecl, int fd, x86::sreg32 offset, int origin)
{
    pf::LwLockGuard guard(s_ioMutex);
    Descriptor* d = descriptor(fd);
    if (!d || d->console >= 0 || origin < 0 || origin > 2)
    {
        setErrno(app, cpu, d ? ErrInval : ErrBadF);
        return -1;
    }
    const off_t result = ::lseek(d->host, offset, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END);
    if (result < 0)
    {
        setErrno(app, cpu, ErrInval);
        return -1;
    }
    d->eof = false;
    return x86::sreg32(result);
}

x86::sreg32 _filelength(WinApplication* app, x86::CPU& cpu, Cdecl, int fd)
{
    pf::LwLockGuard guard(s_ioMutex);
    Descriptor* d = descriptor(fd);
    struct stat status;
    if (!d || d->console >= 0 || fstat(d->host, &status) != 0)
    {
        setErrno(app, cpu, ErrBadF);
        return -1;
    }
    return x86::sreg32(status.st_size);
}

// struct _finddata_t: attrib, time_create, time_access, time_write, size, name[260].
x86::sreg32 _findfirst(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 pattern, x86::reg32 data)
{
    FileEnumerator* enumerator = new FileEnumerator(guestChars(pattern));
    const x86::reg32 handle = app->allocateResource(enumerator);
    if (_findnext(app, cpu, Cdecl{}, x86::sreg32(handle), data) != 0)
    {
        app->freeResource(handle);
        return -1;
    }
    return x86::sreg32(handle);
}

int _findnext(WinApplication* app, x86::CPU& cpu, Cdecl, x86::sreg32 handle, x86::reg32 data)
{
    FileEnumerator* enumerator = dynamic_cast<FileEnumerator*>(app->getResource(x86::reg32(handle)));
    WIN32_FIND_DATAA found;
    if (!enumerator || !enumerator->findNext(&found))
    {
        setErrno(app, cpu, ErrNoEnt);
        return -1;
    }
    auto toTime = [](const FILETIME& ft) {
        const x86::reg64 ticks = (x86::reg64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return ticks ? x86::reg32((ticks - 116444736000000000ull) / 10000000ull) : 0xffffffffu;
    };
    rc::st32(data + 0, found.dwFileAttributes);
    rc::st32(data + 4, toTime(found.ftCreationTime));
    rc::st32(data + 8, toTime(found.ftLastAccessTime));
    rc::st32(data + 12, toTime(found.ftLastWriteTime));
    rc::st32(data + 16, found.nFileSizeLow);
    std::memcpy(guestChars(data + 20), found.cFileName, MAX_PATH);
    return 0;
}

int _findclose(WinApplication* app, x86::CPU& cpu, Cdecl, x86::sreg32 handle)
{
    if (!app->freeResource(x86::reg32(handle)))
    {
        setErrno(app, cpu, ErrNoEnt);
        return -1;
    }
    return 0;
}

int _mkdir(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 path)
{
    if (::mkdir(File::hostPath(guestChars(path)).c_str(), 0777) != 0)
    {
        setErrno(app, cpu, crtErrno(errno));
        return -1;
    }
    return 0;
}

int _rmdir(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 path)
{
    if (::rmdir(File::hostPath(guestChars(path)).c_str()) != 0)
    {
        setErrno(app, cpu, crtErrno(errno));
        return -1;
    }
    return 0;
}

int remove(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 path)
{
    if (::unlink(File::hostPath(guestChars(path)).c_str()) != 0)
    {
        setErrno(app, cpu, crtErrno(errno));
        return -1;
    }
    return 0;
}

x86::reg32 _getcwd(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, int size)
{
    const std::string cwd = File::getCurrentDirectory();
    if (!buffer)
    {
        const x86::reg32 length = x86::reg32(cwd.size() + 1);
        buffer = allocate(size > int(length) ? x86::reg32(size) : length);
        if (!buffer)
        {
            setErrno(app, cpu, ErrNoMem);
            return 0;
        }
    }
    else if (size <= 0 || cwd.size() + 1 > size_t(size))
    {
        setErrno(app, cpu, ErrRange);
        return 0;
    }
    std::memcpy(guestChars(buffer), cwd.c_str(), cwd.size() + 1);
    return buffer;
}

int _getdrive(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    RC_USE(app);
    RC_USE(cpu);
    return 3;
}

x86::reg32 _fullpath(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, x86::reg32 path, UINT size)
{
    const std::string full = resolveFullPath(path ? guestChars(path) : "");
    if (!buffer)
    {
        size = MAX_PATH;
        buffer = allocate(size);
        if (!buffer)
        {
            setErrno(app, cpu, ErrNoMem);
            return 0;
        }
    }
    if (full.size() + 1 > size)
    {
        setErrno(app, cpu, ErrRange);
        return 0;
    }
    std::memcpy(guestChars(buffer), full.c_str(), full.size() + 1);
    return buffer;
}

// struct _diskfree_t: total_clusters, avail_clusters, sectors_per_cluster, bytes_per_sector. Drive 0 is the
// current one; C (3) is the only drive.
UINT _getdiskfree(WinApplication* app, x86::CPU& cpu, Cdecl, UINT drive, x86::reg32 info)
{
    RC_USE(app);
    RC_USE(cpu);
    x86::reg64 total = 0, free = 0;
    if ((drive != 0 && drive != 3) || !File::diskSpace(&total, &free))
        return 15;  // ERROR_INVALID_DRIVE
    const x86::reg64 cluster = 4096;
    rc::st32(info + 0, x86::reg32(total / cluster > 0xffffffffu ? 0xffffffffu : total / cluster));
    rc::st32(info + 4, x86::reg32(free / cluster > 0xffffffffu ? 0xffffffffu : free / cluster));
    rc::st32(info + 8, 8);
    rc::st32(info + 12, 512);
    return 0;
}

// Names compare without case, as on Windows.
x86::reg32 getenv(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 name)
{
    RC_USE(cpu);
    const char* wanted = guestChars(name);
    const size_t length = std::strlen(wanted);
    for (x86::reg32 e = app->getEnv(); *guestChars(e); e += x86::reg32(std::strlen(guestChars(e)) + 1))
    {
        const char* entry = guestChars(e);
        if (strncasecmp(entry, wanted, length) == 0 && entry[length] == '=')
            return e + x86::reg32(length) + 1;
    }
    return 0;
}

// ---- Time ----------------------------------------------------------------------------------------------------------

x86::sreg32 time(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 out)
{
    RC_USE(app);
    RC_USE(cpu);
    const x86::sreg32 now = x86::sreg32(nowSeconds());
    if (out)
        rc::st32(out, x86::reg32(now));
    return now;
}

x86::sreg64 _time64(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 out)
{
    RC_USE(app);
    RC_USE(cpu);
    const x86::sreg64 now = nowSeconds();
    if (out)
        rc::st64(out, x86::reg64(now));
    return now;
}

// Milliseconds since the process started (CLOCKS_PER_SEC 1000).
x86::sreg32 clock(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    RC_USE(app);
    RC_USE(cpu);
    return x86::sreg32(pf::ticksMs());
}

// struct _timeb: time, millitm, timezone (minutes west of UTC), dstflag.
void _ftime(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 out)
{
    RC_USE(app);
    RC_USE(cpu);
    struct timeval now;
    gettimeofday(&now, nullptr);
    rc::st32(out + 0, x86::reg32(now.tv_sec));
    rc::st16(out + 4, x86::reg16(now.tv_usec / 1000));
    rc::st16(out + 6, 0);
    rc::st16(out + 8, 0);
}

x86::reg32 localtime(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 time)
{
    return brokenDownResult(app, cpu, x86::sreg32(rc::ld32(time)));
}

x86::reg32 gmtime(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 time)
{
    return brokenDownResult(app, cpu, x86::sreg32(rc::ld32(time)));
}

x86::sreg32 mktime(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 tm)
{
    RC_USE(app);
    RC_USE(cpu);
    return x86::sreg32(makeTime(tm, 0x7fffffff));
}

x86::reg32 _localtime64(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 time)
{
    return brokenDownResult(app, cpu, x86::sreg64(rc::ld64(time)));
}

x86::reg32 _gmtime64(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 time)
{
    return brokenDownResult(app, cpu, x86::sreg64(rc::ld64(time)));
}

x86::sreg64 _mktime64(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 tm)
{
    RC_USE(app);
    RC_USE(cpu);
    return makeTime(tm, 32535215999ll);
}

}}
