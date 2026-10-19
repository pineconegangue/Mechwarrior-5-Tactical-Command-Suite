// PaperdollExtract.cpp - see PaperdollExtract.h. A line-for-line port of tools_paperdoll_extract.py
// (extract + canon); the comments there explain every rule (why alpha, why polarity, why the file and
// not the folder decides, why an earlier pak wins). Checked against the Python output pixel for pixel.
#include "PaperdollExtract.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <unordered_map>

#include "miniz.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#include "PaperdollCache.h"
#endif

namespace fs = std::filesystem;

namespace pde
{
namespace
{
constexpr uint32_t kPakMagic = 0x5A6F12E1;

std::string Lower(std::string s)
{
    for (auto &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}
bool StartsWith(const std::string &s, const std::string &p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool EndsWith(const std::string &s, const std::string &p)
{
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
template <typename T> T Rd(const std::vector<uint8_t> &b, size_t o)
{
    T v{};
    if (o + sizeof(T) <= b.size())
        std::memcpy(&v, b.data() + o, sizeof(T));
    return v;
}

// ------------------------------------------------------------------ the pak file ----
struct Pak
{
    FILE    *F = nullptr;
    uint64_t Size = 0;
    ~Pak()
    {
        if (F)
            fclose(F);
    }
    bool Open(const fs::path &p)
    {
#ifdef _WIN32
        F = _wfopen(p.wstring().c_str(), L"rb");
#else
        F = fopen(p.string().c_str(), "rb");
#endif
        if (!F)
            return false;
        std::error_code ec;
        Size = (uint64_t)fs::file_size(p, ec);
        return !ec;
    }
    bool Read(uint64_t off, size_t n, std::vector<uint8_t> &out)
    {
        // bug sweep 2026-10-17: a size or offset read from the pak is checked against the file before anything is
        // allocated - a damaged or old-format pak made resize() throw, and that ended GlobalSettings.
        if ((uint64_t)n > Size || off > Size - (uint64_t)n)
            return false;
        out.resize(n);
#ifdef _WIN32
        if (_fseeki64(F, (int64_t)off, SEEK_SET) != 0)
            return false;
#else
        if (fseeko(F, (off_t)off, SEEK_SET) != 0)
            return false;
#endif
        return n == 0 || fread(out.data(), 1, n, F) == n;
    }
};

// FString: int32 length (+ = ANSI incl. NUL, - = UTF-16 chars incl. NUL)
std::string RdStr(const std::vector<uint8_t> &b, size_t &o)
{
    const int32_t n = Rd<int32_t>(b, o);
    o += 4;
    if (n == 0)
        return {};
    const size_t left = o < b.size() ? b.size() - o : 0; // bug sweep 2026-10-17: lengths checked against the buffer
    if (n == INT32_MIN || (n > 0 ? (size_t)n > left : (size_t)-n > left / 2))
    {
        o = b.size();
        return {};
    }
    if (n > 0)
    {
        std::string s(b.begin() + o, b.begin() + std::min(b.size(), o + (size_t)n - 1));
        o += (size_t)n;
        return s;
    }
    const size_t m = (size_t)-n;
    std::string s;
    for (size_t i = 0; i + 1 < m; ++i)
    {
        const uint16_t c = Rd<uint16_t>(b, o + i * 2);
        s += c < 0x80 ? (char)c : '?'; // asset paths are ASCII
    }
    o += m * 2;
    return s;
}

struct Index
{
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, int32_t>>>> Tree; // dir -> files, in order
    std::vector<uint8_t> Entries;
};

std::string LoadIndex(Pak &pak, Index &ix)
{
    std::vector<uint8_t> tail;
    const uint64_t tailOff = pak.Size > 4096 ? pak.Size - 4096 : 0;
    if (!pak.Read(tailOff, (size_t)(pak.Size - tailOff), tail))
        return "cannot read the footer";
    int64_t io = -1, is = 0;
    uint8_t enc = 0;
    for (int i = (int)tail.size() - 4; i >= 0; --i)
        if (Rd<uint32_t>(tail, (size_t)i) == kPakMagic)
        {
            const int32_t ver = Rd<int32_t>(tail, (size_t)i + 4); // bug sweep 2026-10-17: only the 4.26 layout is read
            if (ver < 10)
                return "pak version " + std::to_string(ver) + " (older than 10) - not supported";
            io = Rd<int64_t>(tail, (size_t)i + 8);
            is = Rd<int64_t>(tail, (size_t)i + 16);
            enc = i >= 1 ? tail[(size_t)i - 1] : 0;
            break;
        }
    if (io < 0)
        return "pak footer magic not found";
    if (enc)
        return "index is encrypted - not supported";
    std::vector<uint8_t> idx;
    if (is <= 0 || !pak.Read((uint64_t)io, (size_t)is, idx))
        return "cannot read the index";
    size_t o = 0;
    RdStr(idx, o);  // mount point
    o += 4 + 8;     // NumEntries, PathHashSeed
    if (Rd<int32_t>(idx, o))
        o += 4 + 36;
    else
        o += 4;
    if (!Rd<int32_t>(idx, o))
        return "no full directory index";
    o += 4;
    const int64_t fdo = Rd<int64_t>(idx, o), fds = Rd<int64_t>(idx, o + 8);
    o += 16 + 20;
    const int32_t encSize = Rd<int32_t>(idx, o);
    o += 4;
    if (encSize < 0 || o + (size_t)encSize > idx.size())
        return "bad encoded entries size";
    ix.Entries.assign(idx.begin() + o, idx.begin() + o + encSize);
    std::vector<uint8_t> fd;
    if (fds <= 0 || !pak.Read((uint64_t)fdo, (size_t)fds, fd))
        return "cannot read the directory index";
    size_t p = 0;
    const int32_t ndir = Rd<int32_t>(fd, p);
    p += 4;
    for (int32_t d = 0; d < ndir && p < fd.size(); ++d)
    {
        std::string dir = RdStr(fd, p);
        const int32_t nf = Rd<int32_t>(fd, p);
        p += 4;
        std::vector<std::pair<std::string, int32_t>> files;
        for (int32_t k = 0; k < nf && p < fd.size(); ++k)
        {
            std::string fn = RdStr(fd, p);
            files.push_back({fn, Rd<int32_t>(fd, p)});
            p += 4;
        }
        ix.Tree.push_back({std::move(dir), std::move(files)});
    }
    return {};
}

struct Entry
{
    uint64_t Offset = 0, USize = 0, Size = 0;
    uint32_t Cm = 0, Encrypted = 0, NBlk = 0;
};

Entry DecodeEntry(const std::vector<uint8_t> &b, size_t off)
{
    Entry e;
    const uint32_t v = Rd<uint32_t>(b, off);
    off += 4;
    const bool o32 = (v >> 31) & 1, u32 = (v >> 30) & 1, s32 = (v >> 29) & 1;
    e.Cm = (v >> 23) & 0x3f;
    e.Encrypted = (v >> 22) & 1;
    e.NBlk = (v >> 6) & 0xffff;
    if (o32) { e.Offset = Rd<uint32_t>(b, off); off += 4; } else { e.Offset = Rd<uint64_t>(b, off); off += 8; }
    if (u32) { e.USize = Rd<uint32_t>(b, off); off += 4; } else { e.USize = Rd<uint64_t>(b, off); off += 8; }
    if (e.Cm == 0)
        e.Size = e.USize;
    else if (s32) { e.Size = Rd<uint32_t>(b, off); off += 4; } else { e.Size = Rd<uint64_t>(b, off); off += 8; }
    return e;
}

// The FPakEntry header repeated before the payload, then the payload; zlib-inflated when compressed.
// Several compression blocks are consecutive zlib streams: all are inflated, in order.
std::string Payload(Pak &pak, const Entry &e, std::vector<uint8_t> &out)
{
    if (e.Encrypted)
        return "encrypted entry";
    // bug sweep 2026-10-17: Offset, Size, USize, CompressionMethod (4), Hash (20), [blocks], Flags (1), BlockSize (4).
    // The method was only counted for compressed entries, so an UNCOMPRESSED entry was read 4 bytes early.
    uint64_t hdr = 8 + 8 + 8 + 4 + 20 + 1 + 4;
    if (e.Cm != 0)
        hdr += 4 + (uint64_t)(e.NBlk ? e.NBlk : 1) * 16; // the block table: count + {start, end} per block
    if (e.Size > (256ull << 20) || e.USize > (256ull << 20))
        return "entry too large (damaged index?)";
    std::vector<uint8_t> raw;
    if (!pak.Read(e.Offset + hdr, (size_t)e.Size, raw))
        return "read failed";
    if (e.Cm == 0)
    {
        raw.resize((size_t)std::min<uint64_t>(raw.size(), e.USize));
        out.swap(raw);
        return {};
    }
    out.assign((size_t)e.USize, 0);
    size_t inPos = 0, outPos = 0;
    while (inPos < raw.size() && outPos < out.size())
    {
        mz_stream s{};
        if (mz_inflateInit(&s) != MZ_OK)
            return "inflate init failed";
        s.next_in = raw.data() + inPos;
        s.avail_in = (unsigned int)(raw.size() - inPos);
        s.next_out = out.data() + outPos;
        s.avail_out = (unsigned int)(out.size() - outPos);
        const int r = mz_inflate(&s, MZ_FINISH);
        const size_t used = raw.size() - inPos - s.avail_in, made = out.size() - outPos - s.avail_out;
        mz_inflateEnd(&s);
        if (r != MZ_STREAM_END && r != MZ_OK && r != MZ_BUF_ERROR)
            return outPos ? std::string() : "inflate failed";
        if (!used && !made)
            break;
        inPos += used;
        outPos += made;
        if (r != MZ_STREAM_END)
            break;
    }
    out.resize(outPos);
    return outPos ? std::string() : "inflate produced nothing";
}

// ------------------------------------------------------------- texture parse ----
int BlockBytes(const std::string &f)
{
    if (f == "PF_DXT1") return 8;
    if (f == "PF_DXT5" || f == "PF_BC5" || f == "PF_BC7" || f == "PF_DXT3") return 16;
    return 0;
}
int64_t MipBytes(const std::string &f, int w, int h)
{
    if (int bb = BlockBytes(f))
        return (int64_t)std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * bb;
    if (f == "PF_B8G8R8A8" || f == "PF_R8G8B8A8") return (int64_t)w * h * 4;
    if (f == "PF_G8") return (int64_t)w * h;
    return -1;
}

std::string ParseTexture(const std::vector<uint8_t> &uasset, const std::vector<uint8_t> &uexp,
                         const std::vector<uint8_t> *ubulk, std::string &fmt, int &w, int &h, std::vector<uint8_t> &mip)
{
    static const uint8_t pf[3] = {'P', 'F', '_'};
    auto it = std::search(uexp.begin(), uexp.end(), pf, pf + 3);
    if (it == uexp.end())
        return "no pixel format string";
    const size_t i = (size_t)(it - uexp.begin());
    size_t j = i;
    while (j < uexp.size() && uexp[j])
        ++j;
    fmt.assign(uexp.begin() + i, uexp.begin() + j);
    if (i < 16)
        return "no dimensions";
    w = Rd<int32_t>(uexp, i - 16);
    h = Rd<int32_t>(uexp, i - 12);
    if (!(1 <= w && w <= 8192 && 1 <= h && h <= 8192))
        return "implausible dimensions " + std::to_string(w) + "x" + std::to_string(h);
    const size_t o = j + 1 + 12;
    const int32_t sod = Rd<int32_t>(uexp, o + 8);
    const int64_t off64 = Rd<int64_t>(uexp, o + 12);
    // 2026-10-19: this offset is RIGHT here (checked byte-for-byte against the game's own data) - the 4-byte
    // misalignment was only in tools_paperdoll_extract.py (its uasset/offset arithmetic lands 4 bytes early). Do not add +4.
    const int64_t start = off64 - (int64_t)uasset.size();
    const int64_t need = MipBytes(fmt, w, h);
    if (need >= 0 && sod != need)
        return "mip size " + std::to_string(sod) + " != expected " + std::to_string(need) + " for " + fmt;
    if (sod < 0)
        return "negative mip size";
    if (start >= 0 && start + sod <= (int64_t)uexp.size())
    {
        mip.assign(uexp.begin() + start, uexp.begin() + start + sod);
        return {};
    }
    if (ubulk && !ubulk->empty())
        for (int64_t cand : {off64, (int64_t)0, (int64_t)ubulk->size() - sod})
            if (cand >= 0 && cand + sod <= (int64_t)ubulk->size())
            {
                mip.assign(ubulk->begin() + cand, ubulk->begin() + cand + sod);
                return {};
            }
    return "payload outside uexp and no usable ubulk";
}

// ------------------------------------------------------------------ decoders ----
struct Rgb { int r, g, b; };
void BcColours(int c0, int c1, bool opaque, Rgb p[4])
{
    auto rgb = [](int c) { return Rgb{((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31}; };
    p[0] = rgb(c0);
    p[1] = rgb(c1);
    if (c0 > c1 || opaque)
    {
        p[2] = {(2 * p[0].r + p[1].r) / 3, (2 * p[0].g + p[1].g) / 3, (2 * p[0].b + p[1].b) / 3};
        p[3] = {(p[0].r + 2 * p[1].r) / 3, (p[0].g + 2 * p[1].g) / 3, (p[0].b + 2 * p[1].b) / 3};
    }
    else
    {
        p[2] = {(p[0].r + p[1].r) / 2, (p[0].g + p[1].g) / 2, (p[0].b + p[1].b) / 2};
        p[3] = {0, 0, 0};
    }
}

std::string DecodeLuma(const std::string &fmt, const std::vector<uint8_t> &d, int w, int h, std::vector<uint8_t> &out)
{
    out.assign((size_t)w * h, 0);
    if (fmt == "PF_DXT5")
    {
        // 2026-10-19 PAPERDOLL FIX: THE SHAPE IS IN ALPHA. The colour endpoints are flat white;
        // the anti-aliased line work (armour) and fill (internals) live in the BC4-style alpha
        // block (8 bytes: a0, a1, sixteen 3-bit indices). Luminance of the colour was the old
        // read - it carried no shape at all. Luminance stays the fallback for constant alpha.
        std::vector<uint8_t> alp((size_t)w * h, 0);
        int amin = 255, amax = 0;
        size_t bi = 0;
        for (int by = 0; by < h; by += 4)
            for (int bx = 0; bx < w; bx += 4)
            {
                if (bi + 16 > d.size())
                    return "short pixel data";
                const int a0 = d[bi], a1 = d[bi + 1];
                uint64_t bits = 0;
                for (int k = 0; k < 6; ++k)
                    bits |= (uint64_t)d[bi + 2 + k] << (8 * k);
                int pal[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
                if (a0 > a1)
                    for (int k = 1; k <= 6; ++k)
                        pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
                else
                {
                    for (int k = 1; k <= 4; ++k)
                        pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
                    pal[6] = 0;
                    pal[7] = 255;
                }
                bi += 16;
                for (int k = 0; k < 16; ++k)
                {
                    const int px = bx + (k & 3), py = by + (k >> 2);
                    if (px >= w || py >= h)
                        continue;
                    const int v = pal[(bits >> (3 * k)) & 7];
                    alp[(size_t)py * w + px] = (uint8_t)v;
                    amin = std::min(amin, v);
                    amax = std::max(amax, v);
                }
            }
        if (amax - amin > 32)
        {
            out.swap(alp);
            return {};
        }
        // constant alpha: fall through to the colour read below
    }
    if (fmt == "PF_DXT1" || fmt == "PF_DXT5" || fmt == "PF_DXT3")
    {
        const int step = BlockBytes(fmt), coff = fmt != "PF_DXT1" ? 8 : 0;
        size_t bi = 0;
        for (int by = 0; by < h; by += 4)
            for (int bx = 0; bx < w; bx += 4)
            {
                const int c0 = Rd<uint16_t>(d, bi + coff), c1 = Rd<uint16_t>(d, bi + coff + 2);
                const uint32_t cbits = Rd<uint32_t>(d, bi + coff + 4);
                Rgb pal[4];
                BcColours(c0, c1, fmt != "PF_DXT1", pal);
                bi += (size_t)step;
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                    {
                        const int px = bx + x, py = by + y;
                        if (px >= w || py >= h)
                            continue;
                        const Rgb &c = pal[(cbits >> (2 * (4 * y + x))) & 3];
                        out[(size_t)py * w + px] = (uint8_t)((c.r * 299 + c.g * 587 + c.b * 114) / 1000);
                    }
            }
        return {};
    }
    if (fmt == "PF_B8G8R8A8" || fmt == "PF_R8G8B8A8")
    {
        const size_t n = (size_t)w * h;
        if (d.size() < n * 4)
            return "short pixel data";
        std::vector<uint8_t> lum(n), alp(n);
        int amin = 255, amax = 0;
        for (size_t i = 0; i < n; ++i)
        {
            int b = d[i * 4], g = d[i * 4 + 1], r = d[i * 4 + 2];
            const int a = d[i * 4 + 3];
            if (fmt == "PF_R8G8B8A8")
                std::swap(r, b);
            lum[i] = (uint8_t)((r * 299 + g * 587 + b * 114) / 1000);
            alp[i] = (uint8_t)a;
            amin = std::min(amin, a);
            amax = std::max(amax, a);
        }
        out = (amax - amin) > 32 ? alp : lum;
        return {};
    }
    if (fmt == "PF_G8")
    {
        if (d.size() < out.size())
            return "short pixel data";
        std::copy(d.begin(), d.begin() + (ptrdiff_t)out.size(), out.begin());
        return {};
    }
    return "no decoder for " + fmt;
}

void NormalisePolarity(std::vector<uint8_t> &g)
{
    if (g.empty())
        return;
    uint64_t sum = 0;
    for (uint8_t v : g)
        sum += v;
    if (sum / g.size() > 127)
        for (auto &v : g)
            v = (uint8_t)(255 - v);
}

bool WriteFile(const fs::path &p, const void *data, size_t n)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(data), (std::streamsize)n);
    return (bool)f;
}

void Be32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
void Chunk(std::vector<uint8_t> &png, const char *type, const std::vector<uint8_t> &data)
{
    Be32(png, (uint32_t)data.size());
    const size_t at = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    Be32(png, (uint32_t)mz_crc32(MZ_CRC32_INIT, png.data() + at, png.size() - at));
}

bool WritePngGrey(const fs::path &p, int w, int h, std::vector<uint8_t> grey)
{
    NormalisePolarity(grey);
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w + 1) * h);
    for (int y = 0; y < h; ++y)
    {
        raw.push_back(0);
        raw.insert(raw.end(), grey.begin() + (ptrdiff_t)y * w, grey.begin() + (ptrdiff_t)(y + 1) * w);
    }
    mz_ulong zl = mz_compressBound((mz_ulong)raw.size());
    std::vector<uint8_t> z(zl);
    if (mz_compress2(z.data(), &zl, raw.data(), (mz_ulong)raw.size(), 9) != MZ_OK)
        return false;
    z.resize(zl);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    Be32(ihdr, (uint32_t)w);
    Be32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0});
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    return WriteFile(p, png.data(), png.size());
}

// --------------------------------------------------------------- name rules ----
const std::map<std::string, std::string> kPartAlias = {
    {"hd", "head"}, {"h", "head"}, {"head", "head"}, {"body", "ct"}, {"ct", "ct"}, {"lt", "lt"}, {"rt", "rt"},
    {"la", "la"}, {"ra", "ra"}, {"ll", "ll"}, {"rl", "rl"}, {"rct", "rct"}, {"rlt", "rlt"}, {"rrt", "rrt"},
    {"ctr", "rct"}, {"ltr", "rlt"}, {"rtr", "rrt"}, {"ctrear", "rct"}, {"ltrear", "rlt"}, {"rtrear", "rrt"},
    {"rearct", "rct"}, {"rearlt", "rlt"}, {"rearrt", "rrt"}, {"hp", "head"}, {"cp", "head"}, {"cockpit", "head"}};
const std::map<std::pair<std::string, std::string>, std::string> kPartPairs = {
    {{"arm", "left"}, "la"}, {{"left", "arm"}, "la"}, {{"l", "arm"}, "la"},
    {{"arm", "right"}, "ra"}, {{"right", "arm"}, "ra"}, {{"r", "arm"}, "ra"},
    {{"leg", "left"}, "ll"}, {{"left", "leg"}, "ll"}, {{"l", "leg"}, "ll"},
    {{"leg", "right"}, "rl"}, {{"right", "leg"}, "rl"}, {{"r", "leg"}, "rl"},
    {{"left", "torso"}, "lt"}, {{"torso", "left"}, "lt"}, {{"l", "shoulders"}, "lt"},
    {{"right", "torso"}, "rt"}, {{"torso", "right"}, "rt"}, {{"r", "shoulders"}, "rt"},
    {{"center", "torso"}, "ct"}, {{"centre", "torso"}, "ct"}, {{"torso", "center"}, "ct"},
    {{"center", "body"}, "ct"}, {{"front", "body"}, "ct"}};
const std::set<std::string> kArmor = {"armor", "armour", "arnor", "ext", "exterior"};
const std::set<std::string> kInternal = {"internal", "internals", "int", "interior"};
const std::set<std::string> kVuln = {"vulnerability", "vulnerable", "vuln"};
const std::set<std::string> kNoise = {"paperdoll", "target", "texture", "textures", "hdtextures", "yagdm", "yaml",
                                      "f", "front", "mech", "battlemech", "omnimech", "dmg", "damage"};
const std::set<std::string> kMechParts = {"head", "ct", "lt", "rt", "la", "ra", "ll", "rl", "rct", "rlt", "rrt"};

const std::regex &PaperdollFileRx()
{
    static const std::regex rx(R"((paperdoll|_armor|_armour|_arnor|_internal|_internals|_ext|_int|body_side|body_top|_dd_\d+x\d+_))",
                               std::regex::icase);
    return rx;
}
const std::regex &NotAMaskRx()
{
    static const std::regex rx(R"((_clr|_mrs|_orm|_ems|_nrm|_msk|_bcr|_met|_dirt|_wear|_rgh|_ao|_builtdata|_bakedown|_lod\d|lod\d_))",
                               std::regex::icase);
    return rx;
}

std::vector<std::string> Tokens(const std::string &s) // re.split(r'[^a-z0-9]+', s) minus empties
{
    std::vector<std::string> t;
    std::string cur;
    for (char c : s)
    {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            cur += c;
        else if (!cur.empty())
        {
            t.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty())
        t.push_back(cur);
    return t;
}

// <part>_<layer>; "" for a vehicle body view (the caller keeps the short name); else t unchanged.
std::string CanonName(const std::string &shortName, const std::string &chassis, bool &isNone)
{
    isNone = false;
    std::string t = Lower(shortName);
    for (auto &c : t)
        if (c == ' ' || c == '-')
            c = '_';
    std::vector<std::string> toks = Tokens(t);
    std::set<std::string> chtoks;
    for (auto &x : Tokens(Lower(chassis)))
        chtoks.insert(x);
    std::vector<std::string> kept;
    for (auto &x : toks)
        if (!chtoks.count(x) && !kNoise.count(x))
            kept.push_back(x);
    std::vector<std::string> stripped;
    for (auto &x : kept)
    {
        size_t e = x.size();
        while (e > 0 && x[e - 1] >= '0' && x[e - 1] <= '9')
            --e;
        stripped.push_back(e ? x.substr(0, e) : x);
    }
    std::vector<std::string> chSorted(chtoks.begin(), chtoks.end());
    std::stable_sort(chSorted.begin(), chSorted.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    for (auto &x : stripped)
        for (auto &c : chSorted)
            if (c.size() > 2 && StartsWith(x, c) && x.size() > c.size())
            {
                x = x.substr(c.size());
                break;
            }
    std::string layer;
    for (auto &x : stripped)
    {
        if (kVuln.count(x)) { layer = "vulnerability"; break; }
        if (kArmor.count(x)) { layer = "armor"; break; }
        if (kInternal.count(x)) { layer = "internal"; break; }
    }
    std::string part;
    for (size_t i = 0; i + 1 < stripped.size(); ++i)
    {
        auto it = kPartPairs.find({stripped[i], stripped[i + 1]});
        if (it != kPartPairs.end()) { part = it->second; break; }
    }
    if (part.empty())
        for (auto &x : stripped)
        {
            auto it = kPartAlias.find(x);
            if (it != kPartAlias.end()) { part = it->second; break; }
        }
    const bool hasArm = std::find(stripped.begin(), stripped.end(), "arm") != stripped.end();
    if (layer.empty() && hasArm && !part.empty() && part != "la" && part != "ra")
        layer = "armor";
    auto has = [&](const char *w) { return std::find(stripped.begin(), stripped.end(), w) != stripped.end(); };
    if (!part.empty() && !layer.empty())
    {
        if (has("rear") && (part == "ct" || part == "lt" || part == "rt"))
            part = part == "ct" ? "rct" : part == "lt" ? "rlt" : "rrt";
        return part + "_" + layer;
    }
    if (part == "ct" && layer.empty() && (has("side") || has("top")))
    {
        isNone = true;
        return {};
    }
    return t;
}

struct Asset { int32_t Ex[3] = {-1, -1, -1}; }; // uasset, uexp, ubulk: offsets into the encoded entries

// chassis (lower) -> asset stem -> its files. std::map: sorted as Python's sorted() (byte order).
void FindTextures(const Index &ix, std::map<std::string, std::map<std::string, Asset>> &out)
{
    static const std::set<std::string> texdir = {"textures", "texture", "hdtextures", "hdtexture", "target", "materials"};
    for (auto &[dir, files] : ix.Tree)
    {
        std::vector<std::string> seg;
        {
            std::string norm = dir, cur;
            for (char c : norm)
            {
                if (c == '/' || c == '\\') { if (!cur.empty()) seg.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) seg.push_back(cur);
        }
        bool ui = false, underTarget = false;
        for (auto &s : seg)
        {
            const std::string l = Lower(s);
            if (l == "ui") ui = true;
            if (l == "target" || l == "targets") underTarget = true;
        }
        if (ui)
            continue;
        size_t k = seg.size();
        while (k > 0 && texdir.count(Lower(seg[k - 1])))
            --k;
        if (k == 0)
            continue;
        const std::string folder = seg[k - 1];
        for (auto &[fn, ei] : files)
        {
            const std::string lfn = Lower(fn);
            if (lfn.find("paperdoll") != std::string::npos)
                ;
            else if (underTarget && std::regex_search(fn, PaperdollFileRx()) && !std::regex_search(fn, NotAMaskRx()))
                ;
            else
                continue;
            const size_t dot = fn.rfind('.');
            const std::string stem = dot == std::string::npos ? fn : fn.substr(0, dot);
            const std::string ext = dot == std::string::npos ? "" : Lower(fn.substr(dot + 1));
            int slot = ext == "uasset" ? 0 : ext == "uexp" ? 1 : ext == "ubulk" ? 2 : -1;
            if (slot < 0)
                continue;
            std::string chassis = folder;
            // Beyond the Python tool (done by hand for the 2026-10-08 DLC9 update): a folder that is a whole
            // CATEGORY (DLC9/Objects/Tanks/Textures holds paperdoll_Behemoth_Side_UIX, paperdoll_Bulldog_...)
            // names no unit either, so the unit comes from the file, as for a "_common" folder.
            static const std::set<std::string> category = {"tanks", "tank", "vtols", "vtol", "turrets", "vehicles"};
            if (!folder.empty() && (folder[0] == '_' || category.count(Lower(folder))))
            {
                std::string t = Lower(stem);
                if (StartsWith(t, "paperdoll_"))
                    t = t.substr(10);
                for (const char *suf : {"_side_uix", "_top_uix", "_front_uix", "_body_side", "_body_top", "_side", "_top", "_front", "_back"})
                    if (EndsWith(t, suf)) { t = t.substr(0, t.size() - std::strlen(suf)); break; }
                if (!t.empty())
                    chassis = t;
            }
            out[Lower(chassis)][stem].Ex[slot] = ei;
        }
    }
}

std::string ReadAll(const fs::path &p, std::vector<uint8_t> &out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return "cannot open";
    out.assign(std::istreambuf_iterator<char>(f), {});
    return {};
}
} // namespace

// ================================================================ public ====
bool Extract(const std::vector<fs::path> &paks, const fs::path &outDir, Result &res, const ProgressFn &progress)
{
    std::error_code ec;
    fs::create_directories(outDir, ec);
    std::set<fs::path> written; // an earlier pak / earlier stem wins a clash (the Python tool's resume rule)
    std::map<std::string, std::vector<std::string>> manifest;
    struct Job { Pak *P; Index *Ix; std::string Chassis, Stem; Asset A; std::string PakName; };
    std::vector<std::unique_ptr<Pak>> open;
    std::vector<std::unique_ptr<Index>> indexes;
    std::vector<Job> jobs;
    for (auto &pp : paks)
    {
        auto pak = std::make_unique<Pak>();
        auto ix = std::make_unique<Index>();
        const std::string name = pp.filename().string();
        if (!pak->Open(pp))
        {
            res.Failures.push_back(name + ": cannot open");
            continue;
        }
        const std::string err = LoadIndex(*pak, *ix);
        if (!err.empty())
        {
            res.Failures.push_back(name + ": " + err);
            continue;
        }
        ++res.Paks;
        std::map<std::string, std::map<std::string, Asset>> tex;
        FindTextures(*ix, tex);
        for (auto &[chassis, assets] : tex)
            for (auto &[stem, a] : assets)
                jobs.push_back({pak.get(), ix.get(), chassis, stem, a, name});
        open.push_back(std::move(pak));
        indexes.push_back(std::move(ix));
    }
    int done = 0;
    for (auto &j : jobs)
    {
        if (progress && !progress(done, (int)jobs.size(), j.Chassis))
            return false;
        ++done;
        const std::string what = j.Chassis + "/" + j.Stem + ": ";
        if (j.A.Ex[0] < 0 || j.A.Ex[1] < 0)
        {
            res.Failures.push_back(what + "missing uasset/uexp pair");
            continue;
        }
        std::vector<uint8_t> uasset, uexp, ubulk;
        std::string err = Payload(*j.P, DecodeEntry(j.Ix->Entries, (size_t)j.A.Ex[1]), uexp);
        if (err.empty())
            err = Payload(*j.P, DecodeEntry(j.Ix->Entries, (size_t)j.A.Ex[0]), uasset);
        if (err.empty() && j.A.Ex[2] >= 0)
            err = Payload(*j.P, DecodeEntry(j.Ix->Entries, (size_t)j.A.Ex[2]), ubulk);
        std::string fmt;
        int w = 0, h = 0;
        std::vector<uint8_t> mip, grey;
        if (err.empty())
            err = ParseTexture(uasset, uexp, j.A.Ex[2] >= 0 ? &ubulk : nullptr, fmt, w, h, mip);
        if (err.empty())
            err = DecodeLuma(fmt, mip, w, h, grey);
        if (!err.empty())
        {
            res.Failures.push_back(what + err);
            continue;
        }
        std::string shortName = Lower(j.Stem);
        for (const std::string &pre : {j.Chassis + "_", std::string("paperdoll_")})
            if (StartsWith(shortName, pre))
                shortName = shortName.substr(pre.size());
        bool none = false;
        std::string canon = CanonName(shortName, j.Chassis, none);
        if (!none && !canon.empty())
            shortName = canon;
        const fs::path cd = outDir / j.Chassis;
        fs::create_directories(cd, ec);
        const fs::path dst = cd / (shortName + ".png");
        manifest[j.Chassis].push_back(shortName);
        if (written.count(dst))
            continue;
        if (!WritePngGrey(dst, w, h, grey))
        {
            res.Failures.push_back(what + "cannot write " + dst.string());
            continue;
        }
        written.insert(dst);
        ++res.Files;
    }
    res.Chassis = (int)manifest.size();
    // manifest.json, as the Python tool: chassis -> sorted names
    {
        std::string js = "{";
        bool first = true;
        for (auto &[c, names] : manifest)
        {
            std::sort(names.begin(), names.end());
            js += first ? "\n" : ",\n";
            first = false;
            js += " \"" + c + "\": [";
            for (size_t i = 0; i < names.size(); ++i)
                js += (i ? ",\n  \"" : "\n  \"") + names[i] + "\"";
            js += names.empty() ? "]" : "\n ]";
        }
        js += "\n}";
        WriteFile(outDir / "manifest.json", js.data(), js.size());
    }
    res.Bodies = Canonicalise(outDir);
    if (progress)
        progress(done, (int)jobs.size(), "done");
    return true;
}

int Canonicalise(const fs::path &outDir)
{
    static const char *pref[] = {"_body_side", "_side_uix", "_side", "_front_uix", "_front", "_body_top", "_top_uix", "_top", "_back"};
    int made = 0;
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (auto &de : fs::directory_iterator(outDir, ec))
        if (de.is_directory() && de.path().filename().string()[0] != '_')
            dirs.push_back(de.path());
    std::sort(dirs.begin(), dirs.end());
    for (auto &cd : dirs)
    {
        std::vector<std::string> names;
        for (auto &f : fs::directory_iterator(cd, ec))
        {
            const std::string n = f.path().filename().string();
            if (EndsWith(n, ".png"))
                names.push_back(n.substr(0, n.size() - 4));
        }
        if (names.empty())
            continue;
        std::sort(names.begin(), names.end());
        std::string pick;
        for (const char *suf : pref)
        {
            for (auto &n : names)
                if (EndsWith(n, suf) && !kMechParts.count(n.substr(0, n.find('_'))))
                {
                    pick = n;
                    break;
                }
            if (!pick.empty())
                break;
        }
        if (pick.empty())
            continue;
        fs::copy_file(cd / (pick + ".png"), cd / "body.png", fs::copy_options::overwrite_existing, ec);
        ++made;
    }
    const std::pair<const char *, const char *> markers[] = {
        {"artillery", "markers/artillery_fct_armor.png"},
        {"satellite", "markers/radar_fct_armor.png"},
        {"radar", "markers/radar_fct_armor.png"}, // the dish's art is NAMED radar - an art key read from it says "radar"
        {"radartower", "markers/generic_comstower_dd_130x132_side.png"},
        {"generator", "markers/generic_generator_dd_130x132_side.png"}};
    for (auto &[unit, src] : markers)
    {
        const fs::path sp = outDir / fs::path(src);
        if (!fs::exists(sp, ec))
            continue;
        fs::create_directories(outDir / unit, ec);
        fs::copy_file(sp, outDir / unit / "body.png", fs::copy_options::overwrite_existing, ec);
        ++made;
    }
    const std::pair<const char *, const char *> generic[] = {
        {"artillery", "artillery"}, {"generator", "generator"}, {"radartower", "radartower"}, {"satellite", "satellite"},
        {"tank", "demolisher"}, {"turret", "turret_generic"}, {"vtol", "warrior"}, {"vtoldual", "igor"}};
    fs::create_directories(outDir / "_generic", ec);
    for (auto &[kind, src] : generic)
    {
        const fs::path sp = outDir / src / "body.png";
        if (fs::exists(sp, ec))
            fs::copy_file(sp, outDir / "_generic" / (std::string(kind) + ".png"), fs::copy_options::overwrite_existing, ec);
    }
    return made;
}

std::vector<fs::path> FindPaks(const fs::path &gameDir, bool includeMods)
{
    std::vector<fs::path> out;
    std::error_code ec;
    auto addDir = [&](const fs::path &d) {
        std::vector<fs::path> v;
        for (auto &f : fs::directory_iterator(d, ec))
            if (f.is_regular_file() && Lower(f.path().extension().string()) == ".pak")
                v.push_back(f.path());
        std::sort(v.begin(), v.end());
        out.insert(out.end(), v.begin(), v.end());
    };
    addDir(gameDir / "MW5Mercs" / "Content" / "Paks");
    if (includeMods)
    {
        auto addMods = [&](const fs::path &root) {
            std::vector<fs::path> mods;
            for (auto &m : fs::directory_iterator(root, ec))
                if (m.is_directory())
                    mods.push_back(m.path());
            std::sort(mods.begin(), mods.end());
            for (auto &m : mods)
                addDir(m / "Paks");
        };
        addMods(gameDir / "MW5Mercs" / "Mods");
        addMods(gameDir.parent_path().parent_path() / "workshop" / "content" / "784080");
    }
    return out;
}
std::string DecoderTag() { return "decoder " + std::to_string(kDecoderVersion) + "\n"; }
std::string PakSignature(const std::vector<fs::path> &paks)
{
    std::string sig = DecoderTag();
    std::error_code ec;
    for (auto &p : paks)
    {
        const auto sz = fs::file_size(p, ec);
        const auto t = fs::last_write_time(p, ec).time_since_epoch().count();
        sig += p.generic_u8string() + "|" + std::to_string((unsigned long long)sz) + "|" + std::to_string((long long)t) + "\n";
    }
    return sig;
}

bool IsGameDir(const fs::path &dir)
{
    std::error_code ec;
    return !dir.empty() && fs::exists(dir / "MW5Mercs" / "Content" / "Paks" / "MW5Mercs-WindowsNoEditor.pak", ec);
}

#ifdef _WIN32
fs::path CacheDir()
{
    return PaperdollCacheDir(); // PaperdollCache.h - the one definition the readers use too
}

fs::path FindGameDir()
{
    std::vector<fs::path> steams;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS)
        steams.push_back(buf);
    n = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, nullptr, buf, &n) ==
        ERROR_SUCCESS)
        steams.push_back(buf);
    std::vector<fs::path> libs;
    for (auto &st : steams)
    {
        libs.push_back(st);
        // libraryfolders.vdf: lines like   "path"		"D:\\SteamLibrary"
        std::ifstream f(st / "steamapps" / "libraryfolders.vdf");
        std::string line;
        while (std::getline(f, line))
        {
            const size_t k = line.find("\"path\"");
            if (k == std::string::npos)
                continue;
            const size_t a = line.find('"', k + 6), b = a == std::string::npos ? a : line.rfind('"');
            if (a == std::string::npos || b <= a)
                continue;
            std::string v = line.substr(a + 1, b - a - 1), u;
            for (size_t i = 0; i < v.size(); ++i)
            {
                if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\')
                    ++i;
                u += v[i];
            }
            const int wl = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
            std::wstring w((size_t)std::max(0, wl), L'\0');
            if (wl > 0)
                MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, w.data(), wl);
            w.resize(wcslen(w.c_str()));
            libs.push_back(w);
        }
    }
    for (auto &l : libs)
    {
        const fs::path g = l / "steamapps" / "common" / "MechWarrior 5 Mercenaries";
        if (IsGameDir(g))
            return g;
    }
    return {};
}
#endif
} // namespace pde
