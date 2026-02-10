/*
 * jff_proxy.cpp - WebP format support for Paint Shop Pro 7
 *
 * Copyright (c) 2026 - Released under the MIT License
 * See LICENSE file for details.
 *
 * This proxy DLL replaces jff.dll in PSP 7.04, forwarding all original
 * exports to the renamed jff_orig.dll while intercepting 6 functions
 * to add native WebP read/write support via Google's libwebp.
 *
 * The IFF (Amiga) format slot is repurposed for WebP since IFF is
 * rarely used. All other formats remain fully functional.
 *
 * Build: compile as C++ (x86/32-bit) with MSVC to produce correct
 *        C++ name mangling compatible with PSP's import table.
 */

extern "C" {
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "webp/decode.h"
#include "webp/encode.h"
}

/* ===== Types matching PSP 7's internal C++ ABI ===== */

enum JascErrorCode {
    JascOK          = 0,
    JascError_General = 0x05,
    JascError_Param   = 0x65
};

enum tagJascFileFormats {
    JFF_FMT_BMP = 0x01,
    JFF_FMT_IFF = 0x0B
};

struct tSFILEREQUEST;
class  vcJFile;

/* ===== Request structure offsets (PSP 7.04) ===== */

#define OFFSET_FILENAME  0x70
#define OFFSET_JFILE     0x48E
#define OFFSET_BMPINFO   0x492

/* ===== Function pointer types ===== */

typedef JascErrorCode (__cdecl *fn_DecodeFormat)(tSFILEREQUEST*);
typedef JascErrorCode (__cdecl *fn_EncodeFormat)(tSFILEREQUEST*);
typedef JascErrorCode (__cdecl *fn_ImageTypeByFilename)(const char*, tagJascFileFormats*);
typedef JascErrorCode (__cdecl *fn_ImageTypeByJFile)(vcJFile*, tagJascFileFormats*);
typedef char*         (__cdecl *fn_LoadJFFString)(tagJascFileFormats, char*, int);

typedef void (__fastcall *fn_MemFile_Ctor)(void* pThis, void* edx);
typedef int  (__fastcall *fn_MemFile_SetPtr)(void* pThis, void* edx, BYTE* data, DWORD sz);

/* ===== C++ mangled names for GetProcAddress ===== */

#define MN_DecodeIFF  "?DecodeIFF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z"
#define MN_EncodeIFF  "?EncodeIFF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z"
#define MN_DecodeBMP  "?DecodeBMP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z"
#define MN_EncodeBMP  "?EncodeBMP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z"
#define MN_ImgByFile  "?ImageTypeByFilename@@YA?AW4JascErrorCode@@PBDPAW4tagJascFileFormats@@@Z"
#define MN_ImgByJFile "?ImageTypeByJFile@@YA?AW4JascErrorCode@@PAVvcJFile@@PAW4tagJascFileFormats@@@Z"
#define MN_FilterStr  "?LoadJFFFilterString@@YAPADW4tagJascFileFormats@@PADH@Z"
#define MN_FileIDStr  "?LoadJFFFileIDString@@YAPADW4tagJascFileFormats@@PADH@Z"
#define MN_MemCtor    "??0CJFileMemory@@QAE@XZ"
#define MN_MemSetPtr  "?SetPointerMemoryMode@CJFileMemory@@QAEHPAEK@Z"

/* ===== Globals ===== */

static HMODULE   g_hOrigDll = NULL;
static HINSTANCE g_hSelf    = NULL;

static fn_DecodeFormat        g_origDecodeIFF           = NULL;
static fn_EncodeFormat        g_origEncodeIFF           = NULL;
static fn_DecodeFormat        g_origDecodeBMP           = NULL;
static fn_EncodeFormat        g_origEncodeBMP           = NULL;
static fn_ImageTypeByFilename g_origImageTypeByFilename = NULL;
static fn_ImageTypeByJFile    g_origImageTypeByJFile    = NULL;
static fn_LoadJFFString       g_origLoadJFFFilterString = NULL;
static fn_LoadJFFString       g_origLoadJFFFileIDString = NULL;
static fn_MemFile_Ctor        g_origMemFileCtor         = NULL;
static fn_MemFile_SetPtr      g_origMemFileSetPtr       = NULL;

/* ===== Optional debug logging ===== */

#ifndef ENABLE_LOG
#define ENABLE_LOG 0
#endif

static char g_logPath[MAX_PATH] = {0};

static void ProxyLog(const char* fmt, ...)
{
#if ENABLE_LOG
    FILE* f;
    va_list ap;
    if (!g_logPath[0]) {
        GetModuleFileNameA(NULL, g_logPath, MAX_PATH);
        char* s = strrchr(g_logPath, '\\');
        if (s) *(s + 1) = 0;
        strcat(g_logPath, "jff_proxy.log");
    }
    f = fopen(g_logPath, "a");
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
#else
    (void)fmt;
#endif
}

/* ===== Utilities ===== */

static BOOL HasExtension(const char* path, const char* ext)
{
    const char* dot = strrchr(path, '.');
    if (!dot) return FALSE;
    return (_stricmp(dot, ext) == 0);
}

static BOOL IsWebPMagic(const BYTE* hdr, int len)
{
    if (len < 12) return FALSE;
    return (hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F' &&
            hdr[8] == 'W' && hdr[9] == 'E' && hdr[10] == 'B' && hdr[11] == 'P');
}

/* vcJFile vtable wrappers (__thiscall via __fastcall trick) */

typedef DWORD (__fastcall *fn_JFile_Read)(void*, void*, void*, DWORD, BOOL);
typedef DWORD (__fastcall *fn_JFile_Seek)(void*, void*, long, int);

static DWORD JFile_Read(void* pJFile, void* buf, DWORD len, BOOL flag)
{
    void** vt = *(void***)pJFile;
    return ((fn_JFile_Read)vt[4])(pJFile, NULL, buf, len, flag);
}

static DWORD JFile_Seek(void* pJFile, long offset, int origin)
{
    void** vt = *(void***)pJFile;
    return ((fn_JFile_Seek)vt[8])(pJFile, NULL, offset, origin);
}

/* ===== BMP construction from RGBA pixels ===== */

static BYTE* CreateBMPFromRGBA(int w, int h, const BYTE* rgba, DWORD* pBmpSz)
{
    int   rowBytes = ((w * 3 + 3) / 4) * 4;
    DWORD pixelSz  = (DWORD)rowBytes * h;
    DWORD totalSz  = 14 + 40 + pixelSz;
    BYTE* bmp;
    int   x, y;

    bmp = (BYTE*)VirtualAlloc(NULL, totalSz, MEM_COMMIT, PAGE_READWRITE);
    if (!bmp) return NULL;

    /* BITMAPFILEHEADER */
    bmp[0] = 'B'; bmp[1] = 'M';
    *(DWORD*)(bmp + 2)  = totalSz;
    *(DWORD*)(bmp + 6)  = 0;
    *(DWORD*)(bmp + 10) = 14 + 40;

    /* BITMAPINFOHEADER */
    *(DWORD*)(bmp + 14) = 40;
    *(LONG* )(bmp + 18) = w;
    *(LONG* )(bmp + 22) = h;
    *(WORD* )(bmp + 26) = 1;
    *(WORD* )(bmp + 28) = 24;
    *(DWORD*)(bmp + 30) = 0;
    *(DWORD*)(bmp + 34) = pixelSz;

    /* Pixel data: RGBA top-down -> BGR bottom-up */
    for (y = 0; y < h; y++) {
        const BYTE* src = rgba + (h - 1 - y) * w * 4;
        BYTE*       dst = bmp + 54 + y * rowBytes;
        for (x = 0; x < w; x++) {
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }

    *pBmpSz = totalSz;
    return bmp;
}

/* ===== WebP Decode ===== */

static JascErrorCode DecodeWebP(tSFILEREQUEST* pReq)
{
    char* filename = (char*)((BYTE*)pReq + OFFSET_FILENAME);
    FILE* fp;
    long  fileSz;
    BYTE* fileData    = NULL;
    BYTE* rgbaPixels  = NULL;
    BYTE* bmpData     = NULL;
    DWORD bmpSz;
    BYTE* pMemFileObj = NULL;
    void* pOrigJFile;
    int   w, h;
    JascErrorCode result = JascError_General;

    ProxyLog("DecodeWebP: %s", filename);

    fp = fopen(filename, "rb");
    if (!fp) return g_origDecodeIFF(pReq);

    fseek(fp, 0, SEEK_END);
    fileSz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSz < 12 || fileSz > 100 * 1024 * 1024) {
        fclose(fp);
        return g_origDecodeIFF(pReq);
    }

    fileData = (BYTE*)VirtualAlloc(NULL, fileSz, MEM_COMMIT, PAGE_READWRITE);
    if (!fileData) { fclose(fp); return JascError_General; }
    fread(fileData, 1, fileSz, fp);
    fclose(fp);

    if (!IsWebPMagic(fileData, (int)fileSz)) {
        VirtualFree(fileData, 0, MEM_RELEASE);
        return g_origDecodeIFF(pReq);
    }

    rgbaPixels = WebPDecodeRGBA(fileData, (size_t)fileSz, &w, &h);
    VirtualFree(fileData, 0, MEM_RELEASE);

    if (!rgbaPixels || w <= 0 || h <= 0) {
        if (rgbaPixels) WebPFree(rgbaPixels);
        return JascError_General;
    }

    bmpData = CreateBMPFromRGBA(w, h, rgbaPixels, &bmpSz);
    WebPFree(rgbaPixels);
    if (!bmpData) return JascError_General;

    pMemFileObj = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 4096);
    if (!pMemFileObj) {
        VirtualFree(bmpData, 0, MEM_RELEASE);
        return JascError_General;
    }

    g_origMemFileCtor(pMemFileObj, NULL);
    g_origMemFileSetPtr(pMemFileObj, NULL, bmpData, bmpSz);

    pOrigJFile = *(void**)((BYTE*)pReq + OFFSET_JFILE);
    *(void**)((BYTE*)pReq + OFFSET_JFILE) = pMemFileObj;

    result = g_origDecodeBMP(pReq);
    ProxyLog("DecodeWebP: DecodeBMP returned %d", (int)result);

    *(void**)((BYTE*)pReq + OFFSET_JFILE) = pOrigJFile;

    HeapFree(GetProcessHeap(), 0, pMemFileObj);
    VirtualFree(bmpData, 0, MEM_RELEASE);

    return result;
}

/* ===== WebP Encode ===== */

static JascErrorCode EncodeWebP(tSFILEREQUEST* pReq)
{
    char* filename = (char*)((BYTE*)pReq + OFFSET_FILENAME);
    char  origFilename[520];
    FILE* fp;
    BYTE* bmpData  = NULL;
    BYTE* rgbaData = NULL;
    BYTE* webpOut  = NULL;
    size_t webpSz;
    long  bmpSz;
    int   w, h, rowBytes, x, y;
    JascErrorCode result;

    strncpy(origFilename, filename, sizeof(origFilename) - 1);
    origFilename[sizeof(origFilename) - 1] = 0;
    ProxyLog("EncodeWebP: %s", origFilename);

    /* Let PSP write a BMP via its internal encoder */
    result = g_origEncodeBMP(pReq);
    if (result != JascOK) {
        result = g_origEncodeIFF(pReq);
        if (result != JascOK) return result;
    }

    /* Read the intermediate file back */
    fp = fopen(origFilename, "rb");
    if (!fp) return JascOK;

    fseek(fp, 0, SEEK_END);
    bmpSz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (bmpSz < 54) { fclose(fp); return JascOK; }

    bmpData = (BYTE*)VirtualAlloc(NULL, bmpSz, MEM_COMMIT, PAGE_READWRITE);
    if (!bmpData) { fclose(fp); return JascOK; }
    fread(bmpData, 1, bmpSz, fp);
    fclose(fp);

    if (bmpData[0] != 'B' || bmpData[1] != 'M') {
        VirtualFree(bmpData, 0, MEM_RELEASE);
        return JascOK;
    }

    w = *(int*)(bmpData + 18);
    h = *(int*)(bmpData + 22);
    if (h < 0) h = -h;

    {
        WORD  bpp        = *(WORD*)(bmpData + 28);
        DWORD dataOffset = *(DWORD*)(bmpData + 10);

        if (bpp != 24 && bpp != 32) {
            VirtualFree(bmpData, 0, MEM_RELEASE);
            return JascOK;
        }

        rgbaData = (BYTE*)VirtualAlloc(NULL, w * h * 4, MEM_COMMIT, PAGE_READWRITE);
        if (!rgbaData) {
            VirtualFree(bmpData, 0, MEM_RELEASE);
            return JascOK;
        }

        if (bpp == 24) {
            rowBytes = ((w * 3 + 3) / 4) * 4;
            for (y = 0; y < h; y++) {
                BYTE* src = bmpData + dataOffset + (h - 1 - y) * rowBytes;
                BYTE* dst = rgbaData + y * w * 4;
                for (x = 0; x < w; x++) {
                    dst[x * 4 + 0] = src[x * 3 + 2];
                    dst[x * 4 + 1] = src[x * 3 + 1];
                    dst[x * 4 + 2] = src[x * 3 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
        } else {
            rowBytes = w * 4;
            for (y = 0; y < h; y++) {
                BYTE* src = bmpData + dataOffset + (h - 1 - y) * rowBytes;
                BYTE* dst = rgbaData + y * w * 4;
                for (x = 0; x < w; x++) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = src[x * 4 + 3];
                }
            }
        }
    }

    VirtualFree(bmpData, 0, MEM_RELEASE);

    webpSz = WebPEncodeRGBA(rgbaData, w, h, w * 4, 85.0f, &webpOut);
    VirtualFree(rgbaData, 0, MEM_RELEASE);

    if (webpSz == 0 || !webpOut) return JascOK;

    fp = fopen(origFilename, "wb");
    if (!fp) { WebPFree(webpOut); return JascOK; }
    fwrite(webpOut, 1, webpSz, fp);
    fclose(fp);

    ProxyLog("EncodeWebP: %dx%d -> %zu bytes", w, h, webpSz);
    WebPFree(webpOut);
    return JascOK;
}

/* ===== Intercepted exports ===== */

#define JFFEXPORT __declspec(dllexport)

JFFEXPORT JascErrorCode __cdecl DecodeIFF(tSFILEREQUEST* pReq)
{
    if (!pReq) return g_origDecodeIFF(pReq);

    char* filename = (char*)((BYTE*)pReq + OFFSET_FILENAME);
    if (filename[0] != '\0' && HasExtension(filename, ".webp"))
        return DecodeWebP(pReq);

    return g_origDecodeIFF(pReq);
}

JFFEXPORT JascErrorCode __cdecl EncodeIFF(tSFILEREQUEST* pReq)
{
    char* filename = (char*)((BYTE*)pReq + OFFSET_FILENAME);
    if (HasExtension(filename, ".webp"))
        return EncodeWebP(pReq);

    return g_origEncodeIFF(pReq);
}

JFFEXPORT JascErrorCode __cdecl ImageTypeByFilename(const char* path, tagJascFileFormats* pFmt)
{
    if (path && HasExtension(path, ".webp")) {
        if (pFmt) *pFmt = JFF_FMT_IFF;
        return JascOK;
    }
    return g_origImageTypeByFilename(path, pFmt);
}

JFFEXPORT JascErrorCode __cdecl ImageTypeByJFile(vcJFile* pJFile, tagJascFileFormats* pFmt)
{
    if (pJFile) {
        BYTE hdr[12] = {0};
        JFile_Seek((void*)pJFile, 0, 0);
        JFile_Read((void*)pJFile, hdr, 12, 0);
        JFile_Seek((void*)pJFile, 0, 0);
        if (IsWebPMagic(hdr, 12)) {
            if (pFmt) *pFmt = JFF_FMT_IFF;
            return JascOK;
        }
    }
    return g_origImageTypeByJFile(pJFile, pFmt);
}

JFFEXPORT char* __cdecl LoadJFFFilterString(tagJascFileFormats fmt, char* buf, int len)
{
    if (fmt == JFF_FMT_IFF) {
        strncpy(buf, "WebP Image Format|*.WEBP|", len - 1);
        buf[len - 1] = 0;
        return buf;
    }
    return g_origLoadJFFFilterString(fmt, buf, len);
}

JFFEXPORT char* __cdecl LoadJFFFileIDString(tagJascFileFormats fmt, char* buf, int len)
{
    if (fmt == JFF_FMT_IFF) {
        strncpy(buf, "WebP", len - 1);
        buf[len - 1] = 0;
        return buf;
    }
    return g_origLoadJFFFileIDString(fmt, buf, len);
}

/* ===== Initialization ===== */

static BOOL LoadOriginalDll(void)
{
    char dllPath[MAX_PATH];
    char* pSlash;

    GetModuleFileNameA(g_hSelf, dllPath, MAX_PATH);
    pSlash = strrchr(dllPath, '\\');
    if (!pSlash) return FALSE;
    *(pSlash + 1) = 0;
    strcat(dllPath, "jff_orig.dll");

    g_hOrigDll = LoadLibraryA(dllPath);
    if (!g_hOrigDll) {
        char msg[512];
        wsprintfA(msg, "Cannot load %s\nError: %lu", dllPath, GetLastError());
        MessageBoxA(NULL, msg, "WebP Plugin - Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    g_origDecodeIFF           = (fn_DecodeFormat)GetProcAddress(g_hOrigDll, MN_DecodeIFF);
    g_origEncodeIFF           = (fn_EncodeFormat)GetProcAddress(g_hOrigDll, MN_EncodeIFF);
    g_origDecodeBMP           = (fn_DecodeFormat)GetProcAddress(g_hOrigDll, MN_DecodeBMP);
    g_origEncodeBMP           = (fn_EncodeFormat)GetProcAddress(g_hOrigDll, MN_EncodeBMP);
    g_origImageTypeByFilename = (fn_ImageTypeByFilename)GetProcAddress(g_hOrigDll, MN_ImgByFile);
    g_origImageTypeByJFile    = (fn_ImageTypeByJFile)GetProcAddress(g_hOrigDll, MN_ImgByJFile);
    g_origLoadJFFFilterString = (fn_LoadJFFString)GetProcAddress(g_hOrigDll, MN_FilterStr);
    g_origLoadJFFFileIDString = (fn_LoadJFFString)GetProcAddress(g_hOrigDll, MN_FileIDStr);
    g_origMemFileCtor         = (fn_MemFile_Ctor)GetProcAddress(g_hOrigDll, MN_MemCtor);
    g_origMemFileSetPtr       = (fn_MemFile_SetPtr)GetProcAddress(g_hOrigDll, MN_MemSetPtr);

    if (!g_origDecodeIFF || !g_origDecodeBMP || !g_origEncodeBMP ||
        !g_origImageTypeByFilename || !g_origImageTypeByJFile ||
        !g_origLoadJFFFilterString || !g_origLoadJFFFileIDString ||
        !g_origMemFileCtor || !g_origMemFileSetPtr) {
        MessageBoxA(NULL, "Failed to resolve exports from jff_orig.dll",
                    "WebP Plugin - Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    return TRUE;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID)
{
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hSelf = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        if (!LoadOriginalDll()) return FALSE;
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_hOrigDll) FreeLibrary(g_hOrigDll);
    }
    return TRUE;
}
