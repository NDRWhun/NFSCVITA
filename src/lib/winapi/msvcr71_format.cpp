// printf and scanf of the Microsoft C runtime 7.1: its flags, sizes (h, l, I64, I32, I, L, w), %p as eight
// upper-case digits, %n, at most 17 significant digits in floating conversions (zeros after them), three-digit
// exponents and its spellings of infinities and NaNs (1.#INF, 1.#QNAN, 1.#SNAN, -1.#IND).

#include "msvcr71_internal.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace win32 { namespace msvcr71
{

namespace
{

enum : unsigned
{
    FlagLeft = 1, FlagSign = 2, FlagSpace = 4, FlagAlternate = 8, FlagZero = 16,
    FlagShort = 32, FlagLong = 64, FlagWide = 128, Flag64 = 256, FlagLongDouble = 512
};

// Decimal digits of a double as the runtime's _fltout2 gives them: value = 0.d1d2d3... * 10^decpt, 17 significant
// digits; special values give the digits "1#INF", "1#QNAN", "1#SNAN" or "1#IND" with decpt 1.
struct Digits
{
    bool        negative;
    bool        special;
    int         decpt;
    std::string digits;
};

Digits fltout(double value)
{
    Digits d;
    x86::reg64 bits;
    std::memcpy(&bits, &value, 8);
    d.negative = (bits >> 63) != 0;
    const x86::reg32 exponent = x86::reg32(bits >> 52) & 0x7ff;
    const x86::reg64 mantissa = bits & 0xfffffffffffffull;
    d.special = exponent == 0x7ff;
    if (d.special)
    {
        d.decpt = 1;
        if (!mantissa)
            d.digits = "1#INF";
        else if (d.negative && mantissa == 0x8000000000000ull)
            d.digits = "1#IND";
        else if (mantissa & 0x8000000000000ull)
            d.digits = "1#QNAN";
        else
            d.digits = "1#SNAN";
        return d;
    }
    if (exponent == 0 && mantissa == 0)
    {
        d.decpt = 0;
        d.digits = "0";
        return d;
    }
    char text[40];
    snprintf(text, sizeof(text), "%.16e", value < 0 ? -value : value);
    // d.dddddddddddddddde[+-]x
    d.digits.assign(1, text[0]);
    d.digits.append(text + 2, 16);
    d.decpt = int(std::strtol(text + 19, nullptr, 10)) + 1;
    return d;
}

// The first n digits rounded as _fptostr rounds them: half up on the digit string, digits past it read as zeros, a
// carry out of the first digit gives a leading 1 (n + 1 digits) and raises decpt. Nothing for n < 0.
std::string roundDigits(Digits& d, int n)
{
    std::string out = d.digits.substr(0, n > 0 ? size_t(n) : 0);
    while (int(out.size()) < n)
        out += '0';
    if (n >= 0 && size_t(n) < d.digits.size() && d.digits[n] >= '5')
    {
        int i = n - 1;
        while (i >= 0 && out[i] == '9')
            out[i--] = '0';
        if (i < 0)
        {
            out.insert(out.begin(), '1');
            ++d.decpt;
        }
        else
        {
            ++out[i];
        }
    }
    return out;
}

void appendExponent(std::string& out, int exponent, bool upper)
{
    out += upper ? 'E' : 'e';
    out += exponent < 0 ? '-' : '+';
    char text[16];
    snprintf(text, sizeof(text), "%03d", exponent < 0 ? -exponent : exponent);
    out += text;
}

// e style: d.ddd e+xxx with precision digits after the point.
std::string formatE(double value, int precision, unsigned flags, bool upper)
{
    Digits d = fltout(value);
    const bool zero = d.digits == "0";
    std::string digits = roundDigits(d, precision + 1);
    std::string out(1, digits[0]);
    if (precision > 0 || (flags & FlagAlternate))
        out += '.';
    out.append(digits, 1, size_t(precision));
    appendExponent(out, zero ? 0 : d.decpt - 1, upper);
    return out;
}

// f style: the integer digits, the point, zeros below the first digit and the digits up to precision.
std::string formatF(double value, int precision, unsigned flags)
{
    Digits d = fltout(value);
    const std::string digits = roundDigits(d, precision + d.decpt);
    std::string out = d.decpt > 0 ? digits.substr(0, size_t(d.decpt)) : std::string("0");
    if (precision > 0 || (flags & FlagAlternate))
        out += '.';
    if (precision > 0)
    {
        if (d.decpt < 0)
        {
            out.append(size_t(-d.decpt < precision ? -d.decpt : precision), '0');
            out += digits;
        }
        else
        {
            out.append(digits, size_t(d.decpt), std::string::npos);
        }
    }
    return out;
}

// g style: e style when the exponent is below -4 or at least the precision, f style otherwise; trailing zeros
// removed unless '#'.
std::string formatG(double value, int precision, unsigned flags, bool upper)
{
    if (precision == 0)
        precision = 1;
    Digits d = fltout(value);
    if (d.digits == "0")
        d.decpt = 1;
    roundDigits(d, precision);
    const int magnitude = d.decpt - 1;
    std::string out = magnitude < -4 || magnitude >= precision
                    ? formatE(value, precision - 1, flags, upper)
                    : formatF(value, precision - 1 - magnitude, flags);
    if (!(flags & FlagAlternate))
    {
        const size_t point = out.find('.');
        if (point != std::string::npos)
        {
            size_t exponent = out.find_first_of("eE", point);
            const std::string tail = exponent == std::string::npos ? std::string() : out.substr(exponent);
            std::string head = out.substr(0, exponent == std::string::npos ? out.size() : exponent);
            while (!head.empty() && head.back() == '0')
                head.pop_back();
            if (!head.empty() && head.back() == '.')
                head.pop_back();
            out = head + tail;
        }
    }
    return out;
}

std::string integerDigits(x86::reg64 value, unsigned base, bool upper)
{
    if (!value)
        return std::string();
    const char* set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    while (value)
    {
        out.insert(out.begin(), set[value % base]);
        value /= base;
    }
    return out;
}

template< typename Unit >
struct Output
{
    std::basic_string<Unit> text;

    void put(char c, int count = 1)
    {
        for (int i = 0; i < count; ++i)
            text += Unit(x86::reg8(c));
    }
    void putUnit(x86::reg32 unit)
    {
        text += Unit(unit);
    }
};

// Reads the format string as units.
std::u16string readFormat(x86::reg32 format, bool wide)
{
    std::u16string out;
    if (!format)
        return out;
    if (wide)
    {
        for (x86::reg32 a = format;; a += 2)
        {
            x86::reg16 unit;
            std::memcpy(&unit, g_guestBase + a, 2);
            if (!unit)
                break;
            out += char16_t(unit);
        }
    }
    else
    {
        for (const char* p = guestChars(format); *p; ++p)
            out += char16_t(x86::reg8(*p));
    }
    return out;
}

template< typename Unit >
std::basic_string<Unit> format(x86::reg32 formatAddress, GuestArgs& args, bool wideFunction)
{
    const std::u16string f = readFormat(formatAddress, wideFunction);
    Output<Unit> out;
    for (size_t i = 0; i < f.size();)
    {
        char16_t c = f[i++];
        if (c != '%')
        {
            out.putUnit(c);
            continue;
        }
        if (i < f.size() && f[i] == '%')
        {
            out.putUnit('%');
            ++i;
            continue;
        }
        unsigned flags = 0;
        for (; i < f.size(); ++i)
        {
            const char16_t g = f[i];
            if (g == '-')
                flags |= FlagLeft;
            else if (g == '+')
                flags |= FlagSign;
            else if (g == ' ')
                flags |= FlagSpace;
            else if (g == '#')
                flags |= FlagAlternate;
            else if (g == '0')
                flags |= FlagZero;
            else
                break;
        }
        int width = 0;
        if (i < f.size() && f[i] == '*')
        {
            width = x86::sreg32(args.next32());
            if (width < 0)
            {
                flags |= FlagLeft;
                width = -width;
            }
            ++i;
        }
        else
        {
            while (i < f.size() && f[i] >= '0' && f[i] <= '9')
                width = width * 10 + (f[i++] - '0');
        }
        int precision = -1;
        if (i < f.size() && f[i] == '.')
        {
            ++i;
            precision = 0;
            if (i < f.size() && f[i] == '*')
            {
                precision = x86::sreg32(args.next32());
                if (precision < 0)
                    precision = -1;
                ++i;
            }
            else
            {
                while (i < f.size() && f[i] >= '0' && f[i] <= '9')
                    precision = precision * 10 + (f[i++] - '0');
            }
        }
        for (bool more = true; more && i < f.size();)
        {
            switch (f[i])
            {
            case 'h': flags |= FlagShort; ++i; break;
            case 'l':
                ++i;
                if (i < f.size() && f[i] == 'l')
                {
                    flags |= Flag64;
                    ++i;
                }
                else
                {
                    flags |= FlagLong;
                }
                break;
            case 'L': flags |= FlagLongDouble; ++i; break;
            case 'w': flags |= FlagWide; ++i; break;
            case 'I':
                if (i + 2 < f.size() && f[i + 1] == '6' && f[i + 2] == '4')
                {
                    flags |= Flag64;
                    i += 3;
                }
                else if (i + 2 < f.size() && f[i + 1] == '3' && f[i + 2] == '2')
                {
                    i += 3;
                }
                else
                {
                    ++i;
                }
                break;
            default: more = false; break;
            }
        }
        if (i >= f.size())
            break;
        const char16_t type = f[i++];
        std::string body;           // narrow text of the conversion
        std::u16string wideBody;    // wide text (strings and characters in a wide output)
        bool useWide = false;
        std::string prefix;
        bool numeric = false;
        switch (type)
        {
        case 'c':
        case 'C':
        {
            const x86::reg32 v = args.next32();
            const bool wideArg = type == 'C' ? !(flags & FlagShort) && !wideFunction
                                             : (flags & (FlagLong | FlagWide)) || (wideFunction && !(flags & FlagShort));
            const x86::reg32 unit = wideArg ? (v & 0xffff) : (v & 0xff);
            if (sizeof(Unit) == 2)
            {
                wideBody.assign(1, char16_t(unit));
                useWide = true;
            }
            else if (unit < 256)
            {
                body.assign(1, char(unit));
            }
            break;
        }
        case 's':
        case 'S':
        {
            const x86::reg32 p = args.next32();
            const bool wideArg = type == 'S' ? !(flags & FlagShort) && !wideFunction
                                             : (flags & (FlagLong | FlagWide)) || (wideFunction && !(flags & FlagShort));
            std::u16string units;
            if (!p)
            {
                for (const char* q = "(null)"; *q && (precision < 0 || int(units.size()) < precision); ++q)
                    units += char16_t(*q);
            }
            else if (wideArg)
            {
                for (x86::reg32 a = p; precision < 0 || int(units.size()) < precision; a += 2)
                {
                    x86::reg16 unit;
                    std::memcpy(&unit, g_guestBase + a, 2);
                    if (!unit)
                        break;
                    units += char16_t(unit);
                }
            }
            else
            {
                for (const char* q = guestChars(p); *q && (precision < 0 || int(units.size()) < precision); ++q)
                    units += char16_t(x86::reg8(*q));
            }
            if (sizeof(Unit) == 2)
            {
                wideBody = units;
                useWide = true;
            }
            else
            {
                for (char16_t u : units)
                {
                    if (u >= 256)
                        break;
                    body += char(u);
                }
            }
            break;
        }
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p':
        {
            numeric = true;
            x86::reg64 value;
            bool negative = false;
            const bool isSigned = type == 'd' || type == 'i';
            if (flags & Flag64)
                value = args.next64();
            else
                value = args.next32();
            if (type == 'p')
            {
                precision = 8;
                value &= 0xffffffffu;
            }
            else if (!(flags & Flag64))
            {
                if (flags & FlagShort)
                    value = isSigned ? x86::reg64(x86::sreg64(x86::sreg16(value))) : (value & 0xffff);
                else
                    value = isSigned ? x86::reg64(x86::sreg64(x86::sreg32(value))) : (value & 0xffffffffu);
            }
            if (isSigned && x86::sreg64(value) < 0)
            {
                negative = true;
                value = x86::reg64(-x86::sreg64(value));
            }
            const unsigned base = type == 'o' ? 8 : (type == 'x' || type == 'X' || type == 'p') ? 16 : 10;
            body = integerDigits(value, base, type == 'X' || type == 'p');
            if (precision >= 0)
                flags &= ~FlagZero;
            else
                precision = 1;
            while (int(body.size()) < precision)
                body.insert(body.begin(), '0');
            if (type == 'o' && (flags & FlagAlternate) && (body.empty() || body[0] != '0'))
                body.insert(body.begin(), '0');
            if ((type == 'x' || type == 'X') && (flags & FlagAlternate) && value)
                prefix = type == 'x' ? "0x" : "0X";
            if (isSigned)
            {
                if (negative)
                    prefix = "-";
                else if (flags & FlagSign)
                    prefix = "+";
                else if (flags & FlagSpace)
                    prefix = " ";
            }
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G':
        {
            numeric = true;
            const double value = args.nextDouble();
            if (precision < 0)
                precision = 6;
            if (type == 'f')
                body = formatF(value, precision, flags);
            else if (type == 'e' || type == 'E')
                body = formatE(value, precision, flags, type == 'E');
            else
                body = formatG(value, precision, flags, type == 'G');
            if (std::signbit(value))
                prefix = "-";
            else if (flags & FlagSign)
                prefix = "+";
            else if (flags & FlagSpace)
                prefix = " ";
            break;
        }
        case 'n':
        {
            const x86::reg32 p = args.next32();
            const x86::reg32 count = x86::reg32(out.text.size());
            if (p)
            {
                if (flags & FlagShort)
                {
                    const x86::reg16 v = x86::reg16(count);
                    std::memcpy(g_guestBase + p, &v, 2);
                }
                else
                {
                    std::memcpy(g_guestBase + p, &count, 4);
                }
            }
            continue;
        }
        default:
            out.putUnit(type);
            continue;
        }
        const int length = int(prefix.size() + (useWide ? wideBody.size() : body.size()));
        const int pad = width > length ? width - length : 0;
        if (!(flags & FlagLeft) && !(numeric && (flags & FlagZero)))
            out.put(' ', pad);
        for (char p : prefix)
            out.put(p);
        if (!(flags & FlagLeft) && numeric && (flags & FlagZero))
            out.put('0', pad);
        if (useWide)
        {
            for (char16_t u : wideBody)
                out.putUnit(u);
        }
        else
        {
            for (char b : body)
                out.putUnit(x86::reg8(b));
        }
        if (flags & FlagLeft)
            out.put(' ', pad);
    }
    return out.text;
}

// ---- scanf ----------------------------------------------------------------------------------------------------------

bool isSpace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

int digitValue(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 99;
}

}

std::string formatNarrow(WinApplication* app, x86::reg32 formatAddress, GuestArgs& args)
{
    RC_USE(app);
    return format<char>(formatAddress, args, false);
}

std::u16string formatWide(WinApplication* app, x86::reg32 formatAddress, GuestArgs& args)
{
    RC_USE(app);
    return format<char16_t>(formatAddress, args, true);
}

int scanNarrow(const char* input, const char* fmt, GuestArgs& args)
{
    const char* in = input;
    int assigned = 0;
    bool anyInput = false;
    for (const char* f = fmt; *f;)
    {
        if (isSpace(x86::reg8(*f)))
        {
            while (isSpace(x86::reg8(*f)))
                ++f;
            while (isSpace(x86::reg8(*in)))
                ++in;
            continue;
        }
        if (*f != '%' || f[1] == '%')
        {
            if (*f == '%')
                ++f;
            if (!*in)
                return anyInput || assigned ? assigned : -1;
            if (*in != *f)
                return assigned;
            ++in;
            ++f;
            continue;
        }
        ++f;
        bool suppress = false;
        if (*f == '*')
        {
            suppress = true;
            ++f;
        }
        int width = 0;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        unsigned flags = 0;
        for (bool more = true; more;)
        {
            switch (*f)
            {
            case 'h': flags |= FlagShort; ++f; break;
            case 'l':
                ++f;
                if (*f == 'l')
                {
                    flags |= Flag64;
                    ++f;
                }
                else
                {
                    flags |= FlagLong;
                }
                break;
            case 'L': flags |= FlagLongDouble; ++f; break;
            case 'w': flags |= FlagWide; ++f; break;
            case 'I':
                if (f[1] == '6' && f[2] == '4')
                {
                    flags |= Flag64;
                    f += 3;
                }
                else if (f[1] == '3' && f[2] == '2')
                {
                    f += 3;
                }
                else
                {
                    ++f;
                }
                break;
            default: more = false; break;
            }
        }
        const char type = *f ? *f++ : 0;
        if (!type)
            break;
        if (type != 'c' && type != '[' && type != 'n')
        {
            while (isSpace(x86::reg8(*in)))
                ++in;
        }
        if (type == 'n')
        {
            if (!suppress)
            {
                const x86::reg32 p = args.next32();
                const x86::reg32 count = x86::reg32(in - input);
                if (flags & FlagShort)
                {
                    const x86::reg16 v = x86::reg16(count);
                    std::memcpy(g_guestBase + p, &v, 2);
                }
                else
                {
                    std::memcpy(g_guestBase + p, &count, 4);
                }
            }
            continue;
        }
        if (!*in)
            return anyInput || assigned ? assigned : -1;
        anyInput = true;
        const int limit = width > 0 ? width : 0x7fffffff;
        switch (type)
        {
        case 'c':
        {
            const int n = width > 0 ? width : 1;
            const x86::reg32 p = suppress ? 0 : args.next32();
            int k = 0;
            for (; k < n && in[k]; ++k)
            {
                if (p)
                    g_guestBase[p + k] = x86::reg8(in[k]);
            }
            if (k < n)
                return assigned;
            in += n;
            if (!suppress)
                ++assigned;
            break;
        }
        case 's':
        {
            const x86::reg32 p = suppress ? 0 : args.next32();
            int k = 0;
            while (k < limit && in[k] && !isSpace(x86::reg8(in[k])))
            {
                if (p)
                    g_guestBase[p + k] = x86::reg8(in[k]);
                ++k;
            }
            if (p)
                g_guestBase[p + k] = 0;
            in += k;
            if (!suppress)
                ++assigned;
            break;
        }
        case '[':
        {
            bool negate = false;
            if (*f == '^')
            {
                negate = true;
                ++f;
            }
            bool set[256] = {};
            const char* start = f;
            while (*f && (*f != ']' || f == start))
            {
                if (f[1] == '-' && f[2] && f[2] != ']')
                {
                    const x86::reg8 a = x86::reg8(f[0]), b = x86::reg8(f[2]);
                    for (int ch = a < b ? a : b; ch <= (a < b ? b : a); ++ch)
                        set[ch] = true;
                    f += 3;
                }
                else
                {
                    set[x86::reg8(*f)] = true;
                    ++f;
                }
            }
            if (*f == ']')
                ++f;
            const x86::reg32 p = suppress ? 0 : args.next32();
            int k = 0;
            while (k < limit && in[k] && set[x86::reg8(in[k])] != negate)
            {
                if (p)
                    g_guestBase[p + k] = x86::reg8(in[k]);
                ++k;
            }
            if (!k)
                return assigned;
            if (p)
                g_guestBase[p + k] = 0;
            in += k;
            if (!suppress)
                ++assigned;
            break;
        }
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p':
        {
            unsigned base = type == 'o' ? 8 : (type == 'x' || type == 'X' || type == 'p') ? 16
                          : type == 'i' ? 0 : 10;
            int k = 0;
            bool negative = false;
            if (k < limit && (in[k] == '-' || in[k] == '+'))
            {
                negative = in[k] == '-';
                ++k;
            }
            if (base == 0)
            {
                if (k < limit && in[k] == '0')
                {
                    base = 8;
                    if (k + 1 < limit && (in[k + 1] == 'x' || in[k + 1] == 'X'))
                    {
                        base = 16;
                        k += 2;
                    }
                }
                else
                {
                    base = 10;
                }
            }
            else if (base == 16 && k + 1 < limit && in[k] == '0' && (in[k + 1] == 'x' || in[k + 1] == 'X'))
            {
                k += 2;
            }
            x86::reg64 value = 0;
            int digits = 0;
            while (k < limit && digitValue(x86::reg8(in[k])) < int(base))
            {
                value = value * base + x86::reg64(digitValue(x86::reg8(in[k])));
                ++k;
                ++digits;
            }
            if (!digits && !(base == 8 && k > 0 && in[k - 1] == '0'))
                return assigned;
            if (negative)
                value = x86::reg64(-x86::sreg64(value));
            in += k;
            if (!suppress)
            {
                const x86::reg32 p = args.next32();
                if (flags & Flag64)
                    std::memcpy(g_guestBase + p, &value, 8);
                else if (flags & FlagShort)
                {
                    const x86::reg16 v = x86::reg16(value);
                    std::memcpy(g_guestBase + p, &v, 2);
                }
                else
                {
                    const x86::reg32 v = x86::reg32(value);
                    std::memcpy(g_guestBase + p, &v, 4);
                }
                ++assigned;
            }
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G':
        {
            char token[512];
            int k = 0;
            auto take = [&](bool ok) { if (ok && k < limit && k < int(sizeof(token)) - 1) { token[k] = in[k]; ++k; return true; } return false; };
            take(in[k] == '-' || in[k] == '+');
            int mantissa = 0;
            while (take(in[k] >= '0' && in[k] <= '9'))
                ++mantissa;
            if (take(in[k] == '.'))
            {
                while (take(in[k] >= '0' && in[k] <= '9'))
                    ++mantissa;
            }
            if (!mantissa)
                return assigned;
            const int beforeExponent = k;
            if (take(in[k] == 'e' || in[k] == 'E'))
            {
                take(in[k] == '-' || in[k] == '+');
                int exponentDigits = 0;
                while (take(in[k] >= '0' && in[k] <= '9'))
                    ++exponentDigits;
                if (!exponentDigits)
                    k = beforeExponent;
            }
            token[k] = 0;
            in += k;
            if (!suppress)
            {
                const double value = strtod(token, nullptr);
                const x86::reg32 p = args.next32();
                if (flags & (FlagLong | FlagLongDouble))
                {
                    std::memcpy(g_guestBase + p, &value, 8);
                }
                else
                {
                    const float v = float(value);
                    std::memcpy(g_guestBase + p, &v, 4);
                }
                ++assigned;
            }
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

}}
