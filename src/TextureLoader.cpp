#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

struct RwMemory {
    uint8_t* start;
    uint32_t length;
};

static DWORD g_addrLoadTxdFile        = 0;
static DWORD g_addrFindTxdSlot        = 0x00580D70;
static DWORD g_addrStartLoadTxd       = 0x00580BF0;
static DWORD g_addrTexDictStreamRead  = 0x0061E710;
static DWORD g_addrAddTexture         = 0x0064E020;
static DWORD g_addrRwStreamOpen       = 0x006459C0;
static DWORD g_addrRwStreamClose      = 0x0064FAC0;

static uint8_t* g_trampFindTxdSlot    = nullptr;
static uint8_t* g_trampStartLoadTxd   = nullptr;
static uint8_t* g_trampAddTexture     = nullptr;

// ---------------------------------------------------------------- globals
static std::string g_dir, g_exeDir, g_ini, g_log;
static std::map<std::string, std::string> g_patchedFileCache;
static std::map<std::string, std::vector<uint8_t>> g_patchedMemCache;
static std::map<int, std::string> g_slotToStem;

// Keyed by live RwTexDictionary* address so every reload gets fresh rasters safely
static std::map<uintptr_t, void*> g_dictToPatchedDict;
static std::set<uintptr_t> g_swappedTextures;
static std::string g_lastStreamStem;
static bool g_inInternalRead = false;

struct TxdConfig {
    bool enabled = true;
    std::string folder;
    std::map<std::string, std::string> explicitPngs;
    std::map<std::string, std::string> resolvedPngs;
};

static bool g_globalEnable = true;
static std::map<std::string, TxdConfig> g_txdConfigs;
static std::map<std::string, std::string> g_texToStem;

static void Log(const char* fmt, ...) {
    FILE* f = fopen(g_log.c_str(), "a"); if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

static uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t U16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void Put32(std::vector<uint8_t>& v, uint32_t x) { uint8_t t[4]; memcpy(t, &x, 4); v.insert(v.end(), t, t + 4); }

static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::string ExtractTxdStem(std::string p) {
    for (auto& c : p) if (c == '/') c = '\\';
    size_t slash = p.find_last_of('\\');
    if (slash != std::string::npos) p = p.substr(slash + 1);
    size_t dot = p.find_last_of('.');
    if (dot != std::string::npos && Lower(p.substr(dot)) == ".txd") p = p.substr(0, dot);
    return Lower(p);
}

static std::string DirOf(const std::string& p) {
    size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? "" : p.substr(0, i + 1);
}

static std::string ModPath(HMODULE h) {
    char b[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(h, b, sizeof b);
    return n ? std::string(b, n) : "";
}

static std::string ResolvePath(std::string path, const std::string& baseDir) {
    if (path.empty()) return "";
    for (auto& c : path) if (c == '/') c = '\\';
    bool isAbs = (path.size() > 1 && path[1] == ':') || path[0] == '\\';
    return isAbs ? path : (baseDir + path);
}

static bool ReadFileAll(const std::string& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (256LL << 20)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, NULL);
    CloseHandle(h);
    return ok && got == out.size();
}

static bool WriteFileAll(const char* path, const std::vector<uint8_t>& d) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, d.data(), (DWORD)d.size(), &wr, NULL);
    CloseHandle(h);
    return ok && wr == d.size();
}

static void CleanupTempTxds(bool onlyUnowned) {
    char tmpDir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmpDir);
    if (n == 0 || n >= MAX_PATH) return;

    std::string pattern = std::string(tmpDir) + "vc_txd_*.txd";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string full = std::string(tmpDir) + fd.cFileName;
        if (onlyUnowned) {
            DeleteFileA(full.c_str());
        } else {
            DeleteFileA(full.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void CollectImgFiles(const std::string& dir, std::vector<std::string>& outImgs) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string full = dir + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectImgFiles(full + "\\", outImgs);
        } else {
            std::string l = Lower(fd.cFileName);
            if (l.size() > 4 && l.substr(l.size() - 4) == ".img") {
                outImgs.push_back(full);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static bool FindLooseTxdInDir(const std::string& dir, const std::string& targetLower, std::vector<uint8_t>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string full = dir + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (FindLooseTxdInDir(full + "\\", targetLower, out)) {
                found = true;
                break;
            }
        } else if (Lower(fd.cFileName) == targetLower) {
            if (ReadFileAll(full, out) && !out.empty()) {
                Log("[modloader] found loose '%s' (%zu bytes) at %s", targetLower.c_str(), out.size(), full.c_str());
                found = true;
                break;
            }
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return found;
}

static bool ExtractEntryFromImgAndDir(const std::string& imgPath, const std::string& dirPath,
                                      const std::string& targetLower, const std::string& noExt,
                                      std::vector<uint8_t>& out) {
    HANDLE hImg = CreateFileA(imgPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hImg == INVALID_HANDLE_VALUE) return false;

    std::vector<uint8_t> dirBuf;
    if (!dirPath.empty() && GetFileAttributesA(dirPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!ReadFileAll(dirPath, dirBuf) || dirBuf.size() < 32) {
            CloseHandle(hImg);
            return false;
        }
    } else {
        uint8_t hdr[8] = {0};
        DWORD got = 0;
        if (!ReadFile(hImg, hdr, 8, &got, NULL) || got != 8 || memcmp(hdr, "VER2", 4) != 0) {
            CloseHandle(hImg);
            return false;
        }
        uint32_t count = U32(hdr + 4);
        if (count == 0 || count > 200000) {
            CloseHandle(hImg);
            return false;
        }
        dirBuf.resize((size_t)count * 32);
        if (!ReadFile(hImg, dirBuf.data(), (DWORD)dirBuf.size(), &got, NULL) || got != dirBuf.size()) {
            CloseHandle(hImg);
            return false;
        }
    }

    size_t count = dirBuf.size() / 32;
    for (size_t i = 0; i < count; i++) {
        const uint8_t* ent = dirBuf.data() + i * 32;
        uint32_t offsetSectors = U32(ent + 0);
        uint32_t sizeSectors   = U32(ent + 4);
        char nameBuf[25] = {0};
        memcpy(nameBuf, ent + 8, 24);
        std::string entryLower = Lower(nameBuf);

        if (entryLower == targetLower || entryLower == noExt) {
            uint64_t byteOff = (uint64_t)offsetSectors * 2048ULL;
            uint64_t byteSz  = (uint64_t)sizeSectors * 2048ULL;
            if (byteSz == 0 || byteSz > (64ULL << 20)) break;

            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)byteOff;
            if (!SetFilePointerEx(hImg, li, NULL, FILE_BEGIN)) break;

            out.resize((size_t)byteSz);
            DWORD rd = 0;
            BOOL ok = ReadFile(hImg, out.data(), (DWORD)byteSz, &rd, NULL);
            CloseHandle(hImg);
            if (ok && rd >= 12 && U32(out.data()) == 0x16) {
                uint32_t exactSz = 12 + U32(out.data() + 4);
                if (exactSz <= rd) out.resize(exactSz);
            } else if (!(ok && rd == (DWORD)byteSz)) {
                return false;
            }
            Log("[img] extracted '%s' (%zu bytes) from %s", targetLower.c_str(), out.size(), imgPath.c_str());
            return true;
        }
    }

    CloseHandle(hImg);
    return false;
}

static bool ReadFromImgArchive(const std::string& targetLower, std::vector<uint8_t>& out) {
    // 1. Check loose .txd files inside modloader\ first
    std::string modloaderDir = g_exeDir + "modloader\\";
    if (GetFileAttributesA(modloaderDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (FindLooseTxdInDir(modloaderDir, targetLower, out)) {
            return true;
        }
    }

    // 2. Check .img archives (modloader\ first, then models\gta3.img and models\*.img)
    std::vector<std::string> imgs;
    CollectImgFiles(modloaderDir, imgs);
    std::string mainImg = g_exeDir + "models\\gta3.img";
    if (GetFileAttributesA(mainImg.c_str()) != INVALID_FILE_ATTRIBUTES) imgs.push_back(mainImg);
    CollectImgFiles(g_exeDir + "models\\", imgs);

    std::string noExt = targetLower;
    size_t dot = noExt.rfind('.');
    if (dot != std::string::npos) noExt = noExt.substr(0, dot);

    for (const auto& imgPath : imgs) {
        std::string dirPath = imgPath.substr(0, imgPath.size() - 4) + ".dir";
        if (ExtractEntryFromImgAndDir(imgPath, dirPath, targetLower, noExt, out)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- image helpers
static std::vector<uint8_t> Resize(const uint8_t* s, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> d((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * sh / dh));
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * sw / dw));
            uint64_t wsum[3] = {0, 0, 0}, plain[3] = {0, 0, 0}, a = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint8_t* p = s + ((size_t)yy * sw + xx) * 4;
                    a += p[3]; cnt++;
                    for (int c = 0; c < 3; c++) { wsum[c] += (uint64_t)p[c] * p[3]; plain[c] += p[c]; }
                }
            uint8_t* o = &d[((size_t)y * dw + x) * 4];
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)(a ? wsum[c] / a : plain[c] / cnt);
            o[3] = (uint8_t)(a / cnt);
        }
    }
    return d;
}

static void FillTransparent(uint8_t* img, int w, int h) {
    uint64_t sum[3] = {0, 0, 0}, a = 0;
    for (int i = 0; i < w * h; i++) {
        uint8_t* p = img + (size_t)i * 4;
        if (p[3]) { a += p[3]; for (int c = 0; c < 3; c++) sum[c] += (uint64_t)p[c] * p[3]; }
    }
    if (!a) return;
    uint8_t col[3]; for (int c = 0; c < 3; c++) col[c] = (uint8_t)(sum[c] / a);
    for (int i = 0; i < w * h; i++) {
        uint8_t* p = img + (size_t)i * 4;
        if (!p[3]) memcpy(p, col, 3);
    }
}

static int SnapPow2(int v) {
    v = std::max(1, std::min(v, 4096));
    int p = 1; while (p * 2 <= v) p *= 2;
    return (v - p > p * 2 - v) ? p * 2 : p;
}

// Formats:
//  0  = 32-bit BGRA8888
//  1  = DXT1
//  3  = DXT3
//  5  = DXT5
//  16 = 16-bit ARGB1555
//  17 = 16-bit RGB565
static size_t ExpectedSize(int fmt, int w, int h) {
    if (fmt == 0)  return (size_t)w * h * 4;
    if (fmt == 16 || fmt == 17) return (size_t)w * h * 2;
    size_t bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    return bw * bh * (fmt == 1 ? 8 : 16);
}

static void EncodeLevel(uint8_t* dst, const uint8_t* rgba, int w, int h, int fmt) {
    if (fmt == 0) {
        for (int i = 0; i < w * h; i++) {
            dst[i*4+0] = rgba[i*4+2]; dst[i*4+1] = rgba[i*4+1];
            dst[i*4+2] = rgba[i*4+0]; dst[i*4+3] = rgba[i*4+3];
        }
        return;
    }
    if (fmt == 16) {
        for (int i = 0; i < w * h; i++) {
            uint16_t r = (rgba[i*4+0] >> 3) & 0x1F;
            uint16_t g = (rgba[i*4+1] >> 3) & 0x1F;
            uint16_t b = (rgba[i*4+2] >> 3) & 0x1F;
            uint16_t a = (rgba[i*4+3] >= 128) ? 0x8000 : 0x0000;
            uint16_t px = a | (r << 10) | (g << 5) | b;
            memcpy(dst + i * 2, &px, 2);
        }
        return;
    }
    if (fmt == 17) {
        for (int i = 0; i < w * h; i++) {
            uint16_t r = (rgba[i*4+0] >> 3) & 0x1F;
            uint16_t g = (rgba[i*4+1] >> 2) & 0x3F;
            uint16_t b = (rgba[i*4+2] >> 3) & 0x1F;
            uint16_t px = (r << 11) | (g << 5) | b;
            memcpy(dst + i * 2, &px, 2);
        }
        return;
    }
    int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            uint8_t blk[64];
            for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    int x = std::min(bx * 4 + px, w - 1), y = std::min(by * 4 + py, h - 1);
                    memcpy(blk + (py * 4 + px) * 4, rgba + ((size_t)y * w + x) * 4, 4);
                }
            if (fmt == 5) {
                stb_compress_dxt_block(dst, blk, 1, STB_DXT_HIGHQUAL); dst += 16;
            } else if (fmt == 3) {
                for (int py = 0; py < 4; py++) {
                    uint16_t row = 0;
                    for (int px = 0; px < 4; px++) {
                        int a4 = std::min(15, (blk[(py * 4 + px) * 4 + 3] + 8) / 17);
                        row |= (uint16_t)(a4 << (px * 4));
                    }
                    memcpy(dst + py * 2, &row, 2);
                }
                for (int i = 0; i < 16; i++) blk[i * 4 + 3] = 255;
                stb_compress_dxt_block(dst + 8, blk, 0, STB_DXT_HIGHQUAL); dst += 16;
            } else {
                for (int i = 0; i < 16; i++) blk[i * 4 + 3] = 255;
                stb_compress_dxt_block(dst, blk, 0, STB_DXT_HIGHQUAL); dst += 8;
            }
        }
}

static std::map<std::string, std::string> GatherPngsForConfig(const TxdConfig& cfg) {
    std::map<std::string, std::string> pngs = cfg.explicitPngs;
    if (!cfg.folder.empty()) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((cfg.folder + "*.png").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::string nm = fd.cFileName;
                size_t dot = nm.find_last_of('.');
                if (dot == std::string::npos || Lower(nm.substr(dot)) != ".png") continue;
                std::string texStem = Lower(nm.substr(0, dot));
                if (pngs.find(texStem) == pngs.end()) pngs[texStem] = cfg.folder + nm;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    return pngs;
}

// ---------------------------------------------------------------- INI parsing
static void LoadIniConfig() {
    g_globalEnable = GetPrivateProfileIntA("Settings", "Enable", 1, g_ini.c_str()) != 0;
    if (!g_globalEnable) return;

    std::vector<char> secBuf(32768, 0);
    DWORD len = GetPrivateProfileSectionNamesA(secBuf.data(), (DWORD)secBuf.size(), g_ini.c_str());
    for (const char* sec = secBuf.data(); *sec && sec < secBuf.data() + len; sec += strlen(sec) + 1) {
        if (_stricmp(sec, "Settings") == 0) continue;

        std::string stem = ExtractTxdStem(sec);
        if (stem.empty()) continue;

        TxdConfig cfg;
        cfg.enabled = GetPrivateProfileIntA(sec, "Enable", 1, g_ini.c_str()) != 0;
        if (!cfg.enabled) continue;

        char folderBuf[MAX_PATH] = {0};
        GetPrivateProfileStringA(sec, "Folder", "", folderBuf, MAX_PATH, g_ini.c_str());
        if (folderBuf[0]) {
            cfg.folder = ResolvePath(folderBuf, g_dir);
            if (!cfg.folder.empty() && cfg.folder.back() != '\\')
                cfg.folder += '\\';
        }

        std::vector<char> kvBuf(32768, 0);
        DWORD kvLen = GetPrivateProfileSectionA(sec, kvBuf.data(), (DWORD)kvBuf.size(), g_ini.c_str());
        for (const char* kv = kvBuf.data(); *kv && kv < kvBuf.data() + kvLen; kv += strlen(kv) + 1) {
            const char* eq = strchr(kv, '=');
            if (!eq) continue;
            std::string key(kv, eq - kv);
            std::string val(eq + 1);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);

            if (_stricmp(key.c_str(), "Enable") == 0 || _stricmp(key.c_str(), "Folder") == 0) continue;
            if (val == "0" || val.empty()) continue;

            std::string base = cfg.folder.empty() ? g_dir : cfg.folder;
            cfg.explicitPngs[Lower(key)] = ResolvePath(val, base);
        }

        cfg.resolvedPngs = GatherPngsForConfig(cfg);
        for (const auto& kv : cfg.resolvedPngs) {
            g_texToStem[kv.first] = stem;
        }

        g_txdConfigs[stem] = std::move(cfg);
    }
    Log("Loaded %zu TXD rule(s) (%zu texture target(s)) from INI", g_txdConfigs.size(), g_texToStem.size());
}

// ---------------------------------------------------------------- TXD binary rebuild
static bool BuildPatchedTxd(const std::string& txdName, const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                            const std::map<std::string, std::string>& pngs, int& patched) {
    patched = 0;
    if (in.size() < 28 || U32(&in[0]) != 0x16 || U32(&in[12]) != 0x01) { Log("[%s] not a valid TXD", txdName.c_str()); return false; }
    size_t pos = 24 + (size_t)U32(&in[16]);
    if (pos > in.size()) { Log("[%s] truncated TXD header", txdName.c_str()); return false; }
    uint16_t count = U16(&in[24]);
    out.assign(in.begin(), in.begin() + pos);

    for (int t = 0; t < count; t++) {
        if (pos + 12 > in.size()) break;
        uint32_t type = U32(&in[pos]), size = U32(&in[pos + 4]), ver = U32(&in[pos + 8]);
        size_t next = pos + 12 + (size_t)size;
        if (type != 0x15 || next > in.size() || next < pos + 112) break;

        auto keep = [&]() { out.insert(out.end(), in.begin() + pos, in.begin() + next); pos = next; };

        uint32_t structSize = U32(&in[pos + 16]);
        if (U32(&in[pos + 12]) != 0x01 || structSize < 88 || pos + 24 + (size_t)structSize > next) { keep(); continue; }

        const uint8_t* p = &in[pos + 24];
        uint32_t platform = U32(p), rf = U32(p + 72);
        char name[33], mask[33];
        memcpy(name, p + 8, 32); name[32] = 0;
        memcpy(mask, p + 40, 32); mask[32] = 0;
        int w = U16(p + 80), h = U16(p + 82), depth = p[84], levels = p[85];

        auto it = pngs.find(Lower(name));
        if (it == pngs.end()) { keep(); continue; }

        int fmt = -1;
        if (platform == 8) {
            int comp = p[87];
            if (comp == 1 || comp == 3 || comp == 5) fmt = comp;
            else if (comp == 0 && depth == 32 && !(rf & 0x6000)) fmt = 0;
            else if (comp == 0 && depth == 16 && !(rf & 0x6000)) {
                uint32_t pxFmt = rf & 0x0F00;
                if (pxFmt == 0x0100) fmt = 16;
                else if (pxFmt == 0x0200) fmt = 17;
            }
        } else if (platform == 9) {
            uint32_t d3d = U32(p + 76);
            if (d3d == 0x31545844) fmt = 1;
            else if (d3d == 0x33545844) fmt = 3;
            else if (d3d == 0x35545844) fmt = 5;
            else if ((d3d == 21 || d3d == 22) && depth == 32) fmt = 0;
            else if (d3d == 25 && depth == 16) fmt = 16;
            else if (d3d == 23 && depth == 16) fmt = 17;
        }
        if (fmt < 0) { Log("[%s] %s: unsupported format (plat=%u, depth=%d, rf=0x%X) - skipped", txdName.c_str(), name, platform, depth, rf); keep(); continue; }

        std::vector<uint8_t> file;
        int sw = 0, sh = 0, ch = 0;
        uint8_t* img = nullptr;
        if (ReadFileAll(it->second, file)) img = stbi_load_from_memory(file.data(), (int)file.size(), &sw, &sh, &ch, 4);
        if (!img) { Log("[%s] %s: cannot read/decode %s", txdName.c_str(), name, it->second.c_str()); keep(); continue; }

        int nw = SnapPow2(sw), nh = SnapPow2(sh);
        std::vector<uint8_t> base = (nw == sw && nh == sh) ? std::vector<uint8_t>(img, img + (size_t)sw * sh * 4)
                                                           : Resize(img, sw, sh, nw, nh);
        stbi_image_free(img);
        FillTransparent(base.data(), nw, nh);

        int newLevels = 1;
        if (levels > 1) for (int m = std::max(nw, nh); m > 1; m >>= 1) newLevels++;

        std::vector<uint8_t> mips;
        for (int l = 0; l < newLevels; l++) {
            int lw = std::max(1, nw >> l), lh = std::max(1, nh >> l);
            std::vector<uint8_t> lvl = (l == 0) ? base : Resize(base.data(), nw, nh, lw, lh);
            size_t sz = ExpectedSize(fmt, lw, lh), off = mips.size();
            mips.resize(off + 4 + sz);
            uint32_t sz32 = (uint32_t)sz; memcpy(&mips[off], &sz32, 4);
            EncodeLevel(&mips[off + 4], lvl.data(), lw, lh, fmt);
        }

        std::vector<uint8_t> hdr(p, p + 88);
        uint16_t w16 = (uint16_t)nw, h16 = (uint16_t)nh;
        memcpy(&hdr[80], &w16, 2); memcpy(&hdr[82], &h16, 2); hdr[85] = (uint8_t)newLevels;

        size_t restBeg = pos + 24 + structSize;
        uint32_t newStruct = (uint32_t)(hdr.size() + mips.size());
        uint32_t newNative = 12 + newStruct + (uint32_t)(next - restBeg);

        Put32(out, 0x15); Put32(out, newNative); Put32(out, ver);
        Put32(out, 0x01); Put32(out, newStruct); Put32(out, U32(&in[pos + 20]));
        out.insert(out.end(), hdr.begin(), hdr.end());
        out.insert(out.end(), mips.begin(), mips.end());
        out.insert(out.end(), in.begin() + restBeg, in.begin() + next);

        Log("[%s] %s (mask='%s', rf=0x%X): patched %dx%d -> %dx%d (fmt %d, %d mips)",
            txdName.c_str(), name, mask, rf, w, h, nw, nh, fmt, newLevels);
        patched++;
        pos = next;
    }
    if (pos < in.size()) out.insert(out.end(), in.begin() + pos, in.end());
    uint32_t rootSize = (uint32_t)(out.size() - 12);
    memcpy(&out[4], &rootSize, 4);
    return true;
}

static bool EnsurePatchedMemCached(const std::string& stem, const TxdConfig& cfg) {
    if (g_patchedMemCache.find(stem) != g_patchedMemCache.end()) return true;
    if (cfg.resolvedPngs.empty()) return false;

    std::vector<uint8_t> rawTxd;
    if (!ReadFromImgArchive(stem + ".txd", rawTxd)) return false;

    std::vector<uint8_t> patchedTxd;
    int n = 0;
    if (!BuildPatchedTxd(stem, rawTxd, patchedTxd, cfg.resolvedPngs, n) || n <= 0) {
        Log("[%s] BuildPatchedTxd matched 0 target texture(s) in base TXD (%zu bytes)", stem.c_str(), rawTxd.size());
        return false;
    }

    g_patchedMemCache[stem] = std::move(patchedTxd);
    Log("[%s] cached patched TXD binary (%zu bytes, %d texture(s))", stem.c_str(), g_patchedMemCache[stem].size(), n);
    return true;
}

static void* CreateFreshPatchedDict(const std::string& stem, const TxdConfig& cfg) {
    if (!EnsurePatchedMemCached(stem, cfg)) return nullptr;
    auto& buf = g_patchedMemCache[stem];
    if (buf.size() <= 12) return nullptr;

    RwMemory mem;
    mem.start  = buf.data() + 12;
    mem.length = (uint32_t)(buf.size() - 12);

    auto fnRwStreamOpen       = (void*(__cdecl*)(int, int, const void*))g_addrRwStreamOpen;
    auto fnRwStreamClose      = (int(__cdecl*)(void*, void*))g_addrRwStreamClose;
    auto fnTexDictStreamRead  = (void*(__cdecl*)(void*))g_addrTexDictStreamRead;

    g_inInternalRead = true;
    void* memStream = fnRwStreamOpen(3, 1, &mem);
    void* dict = memStream ? fnTexDictStreamRead(memStream) : nullptr;
    if (memStream) fnRwStreamClose(memStream, NULL);
    g_inInternalRead = false;

    if (dict) {
        Log("[%s] parsed fresh patched dictionary=0x%08X", stem.c_str(), (uint32_t)(uintptr_t)dict);
    }
    return dict;
}

static uint8_t* FindTextureInDict(void* dict, const std::string& targetLower) {
    if (!dict || IsBadReadPtr(dict, 20)) return nullptr;
    const uint8_t* d = (const uint8_t*)dict;
    const uint8_t* listHead = d + 8;
    const uint8_t* cur = *(const uint8_t* const*)listHead;
    int steps = 0;
    while (cur && cur != listHead && steps < 512 && !IsBadReadPtr(cur, 0x50)) {
        uint8_t* tex = (uint8_t*)(cur - 8);
        char nm[33] = {0};
        memcpy(nm, tex + 0x10, 32);
        nm[32] = 0;
        if (Lower(nm) == targetLower) {
            return tex;
        }
        const uint8_t* nextNode = *(const uint8_t* const*)(cur + 0);
        if (!nextNode || nextNode == cur) break;
        cur = nextNode;
        steps++;
    }
    return nullptr;
}

// ---------------------------------------------------------------- Hook 1: File-based CTxdStore::LoadTxd(slot, filename)
static bool __cdecl HookedLoadTxdFile(int index, const char* filename) {
    if (g_globalEnable && filename) {
        try {
            std::string rawPath = filename;
            std::string stem = ExtractTxdStem(rawPath);

            auto cached = g_patchedFileCache.find(stem);
            if (cached != g_patchedFileCache.end()) {
                filename = cached->second.c_str();
            } else {
                auto cfgIt = g_txdConfigs.find(stem);
                if (cfgIt != g_txdConfigs.end() && !cfgIt->second.resolvedPngs.empty()) {
                    std::string fullTxdPath = ResolvePath(rawPath, g_exeDir);
                    std::vector<uint8_t> txd, patchedTxd;
                    int n = 0;
                    if (ReadFileAll(fullTxdPath, txd) && BuildPatchedTxd(stem, txd, patchedTxd, cfgIt->second.resolvedPngs, n) && n > 0) {
                        char tmpDir[MAX_PATH], tmpFile[MAX_PATH];
                        GetTempPathA(MAX_PATH - 64, tmpDir);
                        snprintf(tmpFile, MAX_PATH, "%svc_txd_%s.txd", tmpDir, stem.c_str());
                        if (WriteFileAll(tmpFile, patchedTxd)) {
                            g_patchedFileCache[stem] = tmpFile;
                            filename = g_patchedFileCache[stem].c_str();
                            Log("[%s] file-hook redirected -> %s (%d texture(s))", stem.c_str(), tmpFile, n);
                        }
                    }
                }
            }
        } catch (...) {
            Log("exception in HookedLoadTxdFile");
        }
    }
    return ((bool(__cdecl*)(int, const char*))g_addrLoadTxdFile)(index, filename);
}

// ---------------------------------------------------------------- Hook 2: CTxdStore::FindTxdSlot(const char* name)
static int __cdecl HookedFindTxdSlot(const char* name) {
    int slot = ((int(__cdecl*)(const char*))g_trampFindTxdSlot)(name);
    if (slot >= 0 && name && name[0]) {
        std::string stem = Lower(name);
        if (g_txdConfigs.find(stem) != g_txdConfigs.end()) {
            g_slotToStem[slot] = stem;
        }
    }
    return slot;
}

// ---------------------------------------------------------------- Hook 3: CTxdStore::StartLoadTxd(slot, stream)
static int __cdecl HookedStartLoadTxd(int slot, void* rwStream) {
    auto slotIt = g_slotToStem.find(slot);
    if (slotIt != g_slotToStem.end()) {
        g_lastStreamStem = slotIt->second;
    }
    return ((int(__cdecl*)(int, void*))g_trampStartLoadTxd)(slot, rwStream);
}

// ---------------------------------------------------------------- Hook 4: FUN_0064E020 (RwTexDictionaryAddTexture)
static void* __cdecl HookedAddTexture(void* dictionary, void* texture) {
    void* ret = ((void*(__cdecl*)(void*, void*))g_trampAddTexture)(dictionary, texture);

    if (!g_globalEnable || g_inInternalRead || !dictionary || !texture)
        return ret;

    if (IsBadReadPtr(dictionary, 16) || IsBadReadPtr(texture, 0x30))
        return ret;

    uintptr_t dictAddr = (uintptr_t)dictionary;
    uintptr_t texAddr  = (uintptr_t)texture;
    if (g_swappedTextures.find(texAddr) != g_swappedTextures.end())
        return ret;

    char nm[33] = {0};
    memcpy(nm, (const void*)(texAddr + 0x10), 32);
    nm[32] = 0;
    std::string low = Lower(nm);

    auto targetIt = g_texToStem.find(low);
    if (targetIt == g_texToStem.end())
        return ret;

    const std::string stem = targetIt->second;
    if (g_patchedFileCache.find(stem) != g_patchedFileCache.end())
        return ret;

    auto cfgIt = g_txdConfigs.find(stem);
    if (cfgIt == g_txdConfigs.end())
        return ret;

    void* patchedDict = nullptr;
    auto dIt = g_dictToPatchedDict.find(dictAddr);
    if (dIt != g_dictToPatchedDict.end() && dIt->second != nullptr) {
        patchedDict = dIt->second;
    } else {
        patchedDict = CreateFreshPatchedDict(stem, cfgIt->second);
        if (patchedDict) {
            g_dictToPatchedDict[dictAddr] = patchedDict;
        }
    }
    if (!patchedDict) {
        Log("[%s] failed to obtain patchedDict for '%s' (dict=0x%08X)", stem.c_str(), nm, (uint32_t)dictAddr);
        return ret;
    }

    uint8_t* patchedTex = FindTextureInDict(patchedDict, low);
    if (!patchedTex) {
        Log("[%s] '%s' not found in patchedDict=0x%08X", stem.c_str(), nm, (uint32_t)(uintptr_t)patchedDict);
        return ret;
    }

    void* oldRast = *(void**)(texAddr + 0x00);
    void* newRast = *(void**)(patchedTex + 0x00);
    if (newRast && newRast != oldRast) {
        *(void**)(texAddr + 0x00) = newRast;
        *(void**)(patchedTex + 0x00) = oldRast;
        g_swappedTextures.insert(texAddr);
        Log("[%s] swapped raster for '%s' (dict=0x%08X, tex=0x%08X): 0x%08X -> 0x%08X",
            stem.c_str(), nm, (uint32_t)dictAddr, (uint32_t)texAddr,
            (uint32_t)(uintptr_t)oldRast, (uint32_t)(uintptr_t)newRast);
    }

    return ret;
}

// ---------------------------------------------------------------- dynamic symbol discovery & detour helpers
static int PatchCallSites(DWORD targetFunc, DWORD replacementFunc) {
    int hooked = 0;
    for (DWORD addr = 0x00401000; addr < 0x00650000; addr++) {
        BYTE* ins = (BYTE*)addr;
        if (ins[0] == 0xE8 && (addr + 5 + *(DWORD*)(ins + 1)) == targetFunc) {
            DWORD old;
            VirtualProtect(ins, 5, PAGE_EXECUTE_READWRITE, &old);
            DWORD rel = replacementFunc - (addr + 5);
            memcpy(ins + 1, &rel, 4);
            VirtualProtect(ins, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), ins, 5);
            hooked++;
            addr += 4;
        }
    }
    return hooked;
}

static size_t CalcStolenLen(const uint8_t* p) {
    size_t len = 0;
    while (len < 5) {
        uint8_t op = p[len];
        if (op >= 0x50 && op <= 0x57) { len += 1; }
        else if (op == 0x8B && p[len + 1] == 0xEC) { len += 2; }
        else if (op == 0x8B && (p[len + 1] & 0xC7) == 0x05) { len += 6; }
        else if (op == 0x8B && p[len + 1] == 0x44 && p[len + 2] == 0x24) { len += 4; }
        else if (op == 0x8B && p[len + 1] == 0x74 && p[len + 2] == 0x24) { len += 4; }
        else if (op == 0x8B && p[len + 1] == 0x5C && p[len + 2] == 0x24) { len += 4; }
        else if (op == 0x8B && (p[len + 1] & 0xC0) == 0x40 && (p[len + 1] & 0x07) != 0x04) { len += 3; }
        else if (op == 0x83 && p[len + 1] == 0xEC) { len += 3; }
        else if (op == 0x81 && p[len + 1] == 0xEC) { len += 6; }
        else if (op == 0xA1) { len += 5; }
        else { return 0; }
    }
    return len;
}

static bool DetourFunction(DWORD targetAddr, void* hookFn, uint8_t** outTramp) {
    uint8_t* src = (uint8_t*)targetAddr;
    size_t stolen = CalcStolenLen(src);
    if (stolen < 5) {
        Log("Detour failed at 0x%08X: bytes %02X %02X %02X %02X %02X %02X %02X %02X",
            targetAddr, src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7]);
        return false;
    }

    uint8_t* tramp = (uint8_t*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;

    memcpy(tramp, src, stolen);
    tramp[stolen] = 0xE9;
    *(DWORD*)(tramp + stolen + 1) = (targetAddr + stolen) - ((DWORD)tramp + stolen + 5);

    DWORD old;
    VirtualProtect(src, stolen, PAGE_EXECUTE_READWRITE, &old);
    src[0] = 0xE9;
    *(DWORD*)(src + 1) = (DWORD)hookFn - (targetAddr + 5);
    for (size_t i = 5; i < stolen; i++) src[i] = 0x90;
    VirtualProtect(src, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), src, stolen);

    *outTramp = tramp;
    return true;
}

static bool InstallHooks() {
    BYTE* hudPush = (BYTE*)0x0055C8AF;
    if (!IsBadReadPtr(hudPush, 20) && hudPush[0] == 0x68 && *(DWORD*)(hudPush + 1) == 0x00697C68) {
        for (int off = 5; off <= 16; off++) {
            if (hudPush[off] == 0xE8) {
                g_addrLoadTxdFile = (0x0055C8AF + off) + 5 + *(DWORD*)(hudPush + off + 1);
                break;
            }
        }
    }
    if (g_addrLoadTxdFile) {
        int nFileHooks = PatchCallSites(g_addrLoadTxdFile, (DWORD)&HookedLoadTxdFile);
        Log("Hooked %d LoadTxd(file) call site(s) -> 0x%08X", nFileHooks, g_addrLoadTxdFile);
    }

    if (DetourFunction(g_addrFindTxdSlot, (void*)&HookedFindTxdSlot, &g_trampFindTxdSlot)) {
        Log("Detoured CTxdStore::FindTxdSlot at 0x%08X", g_addrFindTxdSlot);
    }

    if (DetourFunction(g_addrStartLoadTxd, (void*)&HookedStartLoadTxd, &g_trampStartLoadTxd)) {
        Log("Detoured CTxdStore::StartLoadTxd at 0x%08X", g_addrStartLoadTxd);
    }

    if (DetourFunction(g_addrAddTexture, (void*)&HookedAddTexture, &g_trampAddTexture)) {
        Log("Detoured FUN_0064E020 (AddTexture) at 0x%08X", g_addrAddTexture);
    }

    return g_addrLoadTxdFile != 0 || g_trampAddTexture != nullptr;
}

BOOL WINAPI DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_dir    = DirOf(ModPath(h));
        g_exeDir = DirOf(ModPath(NULL));
        g_ini    = g_dir + "TextureLoader.ini";
        g_log    = g_dir + "TextureLoader.log";
        DeleteFileA(g_log.c_str());
        CleanupTempTxds(true);

        LoadIniConfig();
        if (InstallHooks()) Log("Ready");
    } else if (reason == DLL_PROCESS_DETACH) {
        CleanupTempTxds(false);
    }
    return TRUE;
}