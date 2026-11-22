#include <winapi/ws2_32.h>
#include <winapi/kernel32.h>
#include <winapi/support.h>
#include <lib/heap.h>
#include <lib/teb.h>
#include <lib/winapp.h>
#include <cstdio>

namespace win32 { namespace ws2_32
{

static const int SOCKET_ERROR = -1;
static const SOCKET INVALID_SOCKET = 0xffffffff;
static const int WSASYSNOTREADY = 10091;
static const int WSANOTINITIALISED = 10093;

// A socket call without a started Winsock: SOCKET_ERROR with WSANOTINITIALISED, as Windows answers.
static int notInitialised(WinApplication* app, x86::CPU& cpu, const char* api)
{
    offline(app, cpu, api);
    setLastError(app, cpu, WSANOTINITIALISED);
    return SOCKET_ERROR;
}

static ULONG swap32(ULONG value)
{
    return __builtin_bswap32(value);
}

static WORD swap16(WORD value)
{
    return WORD((value >> 8) | (value << 8));
}

// The network subsystem is never ready, so Winsock is never started.
int WSAStartup(WinApplication* app, x86::CPU& cpu, WORD wVersionRequested, x86::reg32 lpWSAData)
{
    RC_USE(wVersionRequested);
    RC_USE(lpWSAData);
    offline(app, cpu, "WSAStartup");
    return WSASYSNOTREADY;
}

int WSACleanup(WinApplication* app, x86::CPU& cpu)
{
    return notInitialised(app, cpu, "WSACleanup");
}

// The calling thread's last error, as GetLastError.
int WSAGetLastError(WinApplication* app, x86::CPU& cpu)
{
    return int(app->getMemory<x86::reg32>(cpu.efs + teb::LastError));
}

int WSAIoctl(WinApplication* app, x86::CPU& cpu, SOCKET s, DWORD dwIoControlCode, x86::reg32 lpvInBuffer,
             DWORD cbInBuffer, x86::reg32 lpvOutBuffer, DWORD cbOutBuffer, x86::reg32 lpcbBytesReturned,
             x86::reg32 lpOverlapped, x86::reg32 lpCompletionRoutine)
{
    RC_USE(s);
    RC_USE(dwIoControlCode);
    RC_USE(lpvInBuffer);
    RC_USE(cbInBuffer);
    RC_USE(lpvOutBuffer);
    RC_USE(cbOutBuffer);
    RC_USE(lpcbBytesReturned);
    RC_USE(lpOverlapped);
    RC_USE(lpCompletionRoutine);
    return notInitialised(app, cpu, "WSAIoctl");
}

SOCKET socket(WinApplication* app, x86::CPU& cpu, int af, int type, int protocol)
{
    RC_USE(af);
    RC_USE(type);
    RC_USE(protocol);
    notInitialised(app, cpu, "socket");
    return INVALID_SOCKET;
}

int closesocket(WinApplication* app, x86::CPU& cpu, SOCKET s)
{
    RC_USE(s);
    return notInitialised(app, cpu, "closesocket");
}

int shutdown(WinApplication* app, x86::CPU& cpu, SOCKET s, int how)
{
    RC_USE(s);
    RC_USE(how);
    return notInitialised(app, cpu, "shutdown");
}

int bind(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, int namelen)
{
    RC_USE(s);
    RC_USE(name);
    RC_USE(namelen);
    return notInitialised(app, cpu, "bind");
}

int connect(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, int namelen)
{
    RC_USE(s);
    RC_USE(name);
    RC_USE(namelen);
    return notInitialised(app, cpu, "connect");
}

int listen(WinApplication* app, x86::CPU& cpu, SOCKET s, int backlog)
{
    RC_USE(s);
    RC_USE(backlog);
    return notInitialised(app, cpu, "listen");
}

SOCKET accept(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 addr, x86::reg32 addrlen)
{
    RC_USE(s);
    RC_USE(addr);
    RC_USE(addrlen);
    notInitialised(app, cpu, "accept");
    return INVALID_SOCKET;
}

int ioctlsocket(WinApplication* app, x86::CPU& cpu, SOCKET s, LONG cmd, x86::reg32 argp)
{
    RC_USE(s);
    RC_USE(cmd);
    RC_USE(argp);
    return notInitialised(app, cpu, "ioctlsocket");
}

int setsockopt(WinApplication* app, x86::CPU& cpu, SOCKET s, int level, int optname, x86::reg32 optval,
               int optlen)
{
    RC_USE(s);
    RC_USE(level);
    RC_USE(optname);
    RC_USE(optval);
    RC_USE(optlen);
    return notInitialised(app, cpu, "setsockopt");
}

int getsockopt(WinApplication* app, x86::CPU& cpu, SOCKET s, int level, int optname, x86::reg32 optval,
               x86::reg32 optlen)
{
    RC_USE(s);
    RC_USE(level);
    RC_USE(optname);
    RC_USE(optval);
    RC_USE(optlen);
    return notInitialised(app, cpu, "getsockopt");
}

int send(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags)
{
    RC_USE(s);
    RC_USE(buf);
    RC_USE(len);
    RC_USE(flags);
    return notInitialised(app, cpu, "send");
}

int sendto(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags, x86::reg32 to,
           int tolen)
{
    RC_USE(s);
    RC_USE(buf);
    RC_USE(len);
    RC_USE(flags);
    RC_USE(to);
    RC_USE(tolen);
    return notInitialised(app, cpu, "sendto");
}

int recv(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags)
{
    RC_USE(s);
    RC_USE(buf);
    RC_USE(len);
    RC_USE(flags);
    return notInitialised(app, cpu, "recv");
}

int recvfrom(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags, x86::reg32 from,
             x86::reg32 fromlen)
{
    RC_USE(s);
    RC_USE(buf);
    RC_USE(len);
    RC_USE(flags);
    RC_USE(from);
    RC_USE(fromlen);
    return notInitialised(app, cpu, "recvfrom");
}

int select(WinApplication* app, x86::CPU& cpu, int nfds, x86::reg32 readfds, x86::reg32 writefds,
           x86::reg32 exceptfds, x86::reg32 timeout)
{
    RC_USE(nfds);
    RC_USE(readfds);
    RC_USE(writefds);
    RC_USE(exceptfds);
    RC_USE(timeout);
    return notInitialised(app, cpu, "select");
}

int getpeername(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, x86::reg32 namelen)
{
    RC_USE(s);
    RC_USE(name);
    RC_USE(namelen);
    return notInitialised(app, cpu, "getpeername");
}

int getsockname(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, x86::reg32 namelen)
{
    RC_USE(s);
    RC_USE(name);
    RC_USE(namelen);
    return notInitialised(app, cpu, "getsockname");
}

// No host entry: NULL with WSANOTINITIALISED.
x86::reg32 gethostbyname(WinApplication* app, x86::CPU& cpu, LPCSTR name)
{
    RC_USE(name);
    notInitialised(app, cpu, "gethostbyname");
    return 0;
}

x86::reg32 gethostbyaddr(WinApplication* app, x86::CPU& cpu, x86::reg32 addr, int len, int type)
{
    RC_USE(addr);
    RC_USE(len);
    RC_USE(type);
    notInitialised(app, cpu, "gethostbyaddr");
    return 0;
}

int gethostname(WinApplication* app, x86::CPU& cpu, x86::reg32 name, int namelen)
{
    RC_USE(name);
    RC_USE(namelen);
    return notInitialised(app, cpu, "gethostname");
}

x86::reg32 getservbyname(WinApplication* app, x86::CPU& cpu, LPCSTR name, LPCSTR proto)
{
    RC_USE(name);
    RC_USE(proto);
    notInitialised(app, cpu, "getservbyname");
    return 0;
}

x86::reg32 getservbyport(WinApplication* app, x86::CPU& cpu, int port, LPCSTR proto)
{
    RC_USE(port);
    RC_USE(proto);
    notInitialised(app, cpu, "getservbyport");
    return 0;
}

// Dotted address parts a.b.c.d, a.b.c (c 16 bits), a.b (b 24 bits) or a (32 bits), each decimal, octal (0) or
// hexadecimal (0x); INADDR_NONE for anything else. The result is in network order.
ULONG inet_addr(WinApplication* app, x86::CPU& cpu, LPCSTR cp)
{
    RC_USE(app);
    RC_USE(cpu);
    static const ULONG InaddrNone = 0xffffffff;
    if (!cp)
        return InaddrNone;
    x86::reg64 parts[4];
    int count = 0;
    const char* p = cp;
    for (;;)
    {
        if (count == 4)
            return InaddrNone;
        unsigned base = 10;
        if (*p == '0')
        {
            base = 8;
            ++p;
            if (*p == 'x' || *p == 'X')
            {
                base = 16;
                ++p;
            }
        }
        x86::reg64 value = 0;
        bool digits = base == 8;
        for (;; ++p)
        {
            const char c = *p;
            unsigned d;
            if (c >= '0' && c <= '9')
                d = unsigned(c - '0');
            else if (base == 16 && c >= 'a' && c <= 'f')
                d = unsigned(c - 'a' + 10);
            else if (base == 16 && c >= 'A' && c <= 'F')
                d = unsigned(c - 'A' + 10);
            else
                break;
            if (d >= base)
                return InaddrNone;
            value = value * base + d;
            if (value > 0xffffffffull)
                return InaddrNone;
            digits = true;
        }
        if (!digits)
            return InaddrNone;
        parts[count++] = value;
        if (*p != '.')
            break;
        ++p;
    }
    if (*p && *p != ' ')
        return InaddrNone;
    x86::reg64 address;
    switch (count)
    {
    case 1: address = parts[0]; break;
    case 2:
        if (parts[0] > 0xff || parts[1] > 0xffffff)
            return InaddrNone;
        address = parts[0] << 24 | parts[1];
        break;
    case 3:
        if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xffff)
            return InaddrNone;
        address = parts[0] << 24 | parts[1] << 16 | parts[2];
        break;
    default:
        if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xff || parts[3] > 0xff)
            return InaddrNone;
        address = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
        break;
    }
    return swap32(ULONG(address));
}

// "a.b.c.d" in a buffer of Winsock's that the next call overwrites.
x86::reg32 inet_ntoa(WinApplication* app, x86::CPU& cpu, ULONG in)
{
    RC_USE(cpu);
    static x86::reg32 s_buffer = 0;
    if (!s_buffer)
        s_buffer = heap::allocate(16, true);
    if (!s_buffer)
        return 0;
    const x86::reg8* b = reinterpret_cast<const x86::reg8*>(&in);
    snprintf(&app->getMemory<char>(s_buffer), 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return s_buffer;
}

// The WSA event calls are the kernel event calls: a manual-reset event that starts non-signalled.
HANDLE WSACreateEvent(WinApplication* app, x86::CPU& cpu)
{
    return kernel32::CreateEventA(app, cpu, 0, 1, 0, 0);
}

BOOL WSACloseEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent)
{
    return kernel32::CloseHandle(app, cpu, hEvent);
}

BOOL WSASetEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent)
{
    return kernel32::SetEvent(app, cpu, hEvent);
}

BOOL WSAResetEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent)
{
    return kernel32::ResetEvent(app, cpu, hEvent);
}

DWORD WSAWaitForMultipleEvents(WinApplication* app, x86::CPU& cpu, DWORD cEvents, const HANDLE* lphEvents,
                               BOOL fWaitAll, DWORD dwTimeout, BOOL fAlertable)
{
    return kernel32::WaitForMultipleObjectsEx(app, cpu, cEvents, lphEvents, fWaitAll, dwTimeout, fAlertable);
}

int WSARecv(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 lpBuffers, DWORD dwBufferCount,
            x86::reg32 lpNumberOfBytesRecvd, x86::reg32 lpFlags, x86::reg32 lpOverlapped,
            x86::reg32 lpCompletionRoutine)
{
    RC_USE(s);
    RC_USE(lpBuffers);
    RC_USE(dwBufferCount);
    RC_USE(lpNumberOfBytesRecvd);
    RC_USE(lpFlags);
    RC_USE(lpOverlapped);
    RC_USE(lpCompletionRoutine);
    return notInitialised(app, cpu, "WSARecv");
}

int WSARecvFrom(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 lpBuffers, DWORD dwBufferCount,
                x86::reg32 lpNumberOfBytesRecvd, x86::reg32 lpFlags, x86::reg32 lpFrom, x86::reg32 lpFromlen,
                x86::reg32 lpOverlapped, x86::reg32 lpCompletionRoutine)
{
    RC_USE(s);
    RC_USE(lpBuffers);
    RC_USE(dwBufferCount);
    RC_USE(lpNumberOfBytesRecvd);
    RC_USE(lpFlags);
    RC_USE(lpFrom);
    RC_USE(lpFromlen);
    RC_USE(lpOverlapped);
    RC_USE(lpCompletionRoutine);
    return notInitialised(app, cpu, "WSARecvFrom");
}

BOOL WSAGetOverlappedResult(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 lpOverlapped,
                            x86::reg32 lpcbTransfer, BOOL fWait, x86::reg32 lpdwFlags)
{
    RC_USE(s);
    RC_USE(lpOverlapped);
    RC_USE(lpcbTransfer);
    RC_USE(fWait);
    RC_USE(lpdwFlags);
    notInitialised(app, cpu, "WSAGetOverlappedResult");
    return 0;
}

ULONG htonl(WinApplication* app, x86::CPU& cpu, ULONG hostlong)
{
    RC_USE(app);
    RC_USE(cpu);
    return swap32(hostlong);
}

ULONG ntohl(WinApplication* app, x86::CPU& cpu, ULONG netlong)
{
    RC_USE(app);
    RC_USE(cpu);
    return swap32(netlong);
}

WORD htons(WinApplication* app, x86::CPU& cpu, WORD hostshort)
{
    RC_USE(app);
    RC_USE(cpu);
    return swap16(hostshort);
}

WORD ntohs(WinApplication* app, x86::CPU& cpu, WORD netshort)
{
    RC_USE(app);
    RC_USE(cpu);
    return swap16(netshort);
}

}}
