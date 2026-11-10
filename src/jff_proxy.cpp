/*
 * jff_proxy.cpp - WebP format support for Paint Shop Pro 7
 *
 * Copyright (c) 2026 - Released under the MIT License
 * See LICENSE file for details.
 *
 * This proxy DLL replaces jff.dll in PSP 7.04, forwarding all original
 * exports to the renamed jff_orig.dll while intercepting functions
 * to add native WebP read/write support via Google's libwebp.
 *
 * The IFF (Amiga) format slot is repurposed for WebP since IFF is
 * rarely used. All other formats remain fully functional.
 *
 * PATCHED VERSION (changes marked with [PATCH]):
 *  1. EncodeIFF no longer depends on the file extension. "Save" makes PSP
 *     write to a temporary name, so the extension check failed and the
 *     original IFF encoder wrote an ILBM file.
 *  2. DecodeIFF decides by file content, not by extension.
 *  3. DecodeWebP falls back to the original IFF decoder (or to the PNG
 *     decoder if the file really is a PNG) instead of always using PNG.
 *  4. EncodeWebP returns an error (and removes the intermediate file)
 *     instead of reporting success when something fails.
 *  5. "Paste As New Image" keeps the transparency of a PNG on the clipboard
 *     (see the section before DllMain).
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

#include <wincodec.h>

/* ===== Types matching PSP 7's internal C++ ABI ===== */

enum JascErrorCode {
    JascOK            = 0,
    JascError_General = 0x05,
    JascError_Param   = 0x65
};

enum tagJascFileFormats {
    JFF_FMT_BMP = 0x01,
    JFF_FMT_IFF = 0x0B,
    JFF_FMT_PNG = 0x1C
};

struct tSFILEREQUEST;
class  vcJFile;

struct SFormatDesc  { unsigned char data[0xD2]; };
struct SEncodeParms { unsigned char data[0x282]; };

/* ===== Request structure offsets (PSP 7.04) ===== */

#define OFFSET_FILENAME        0x70
#define OFFSET_JFILE           0x48E
#define OFFSET_BMPINFO         0x492
#define FORMATDESC_MAGIC       0xD2
#define ENCODEPARMS_FORMAT_OFF 0x04

/* ===== Function pointer types ===== */

typedef JascErrorCode (__cdecl *fn_DecodeFormat)(tSFILEREQUEST*);
typedef JascErrorCode (__cdecl *fn_EncodeFormat)(tSFILEREQUEST*);
typedef JascErrorCode (__cdecl *fn_ImageTypeByFilename)(const char*, tagJascFileFormats*);
typedef JascErrorCode (__cdecl *fn_ImageTypeByJFile)(vcJFile*, tagJascFileFormats*);
typedef char*         (__cdecl *fn_LoadJFFString)(tagJascFileFormats, char*, int);
typedef JascErrorCode (__cdecl *fn_LoadJFFFormatDesc)(SFormatDesc*, SEncodeParms*);

typedef void (__fastcall *fn_MemFile_Ctor)(void* pThis, void* edx);
typedef int  (__fastcall *fn_MemFile_SetPtr)(void* pThis, void* edx, BYTE* data, DWORD sz);

/* ===== C++ mangled names for GetProcAddress ===== */

#define MN_DecodeIFF  "?DecodeIFF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z"
#define MN_EncodeIFF  "?EncodeIFF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z"
#define MN_DecodeBMP  "?DecodeBMP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z"
#define MN_EncodeBMP  "?EncodeBMP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z"
#define MN_DecodePNG  "?DecodePNG@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z"
#define MN_EncodePNG  "?EncodePNG@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z"
#define MN_ImgByFile  "?ImageTypeByFilename@@YA?AW4JascErrorCode@@PBDPAW4tagJascFileFormats@@@Z"
#define MN_ImgByJFile "?ImageTypeByJFile@@YA?AW4JascErrorCode@@PAVvcJFile@@PAW4tagJascFileFormats@@@Z"
#define MN_FilterStr  "?LoadJFFFilterString@@YAPADW4tagJascFileFormats@@PADH@Z"
#define MN_FileIDStr  "?LoadJFFFileIDString@@YAPADW4tagJascFileFormats@@PADH@Z"
#define MN_FormatDesc "?LoadJFFFormatDesc@@YA?AW4JascErrorCode@@PAUSFormatDesc@@PAUSEncodeParms@@@Z"
#define MN_MemCtor    "??0CJFileMemory@@QAE@XZ"
#define MN_MemSetPtr  "?SetPointerMemoryMode@CJFileMemory@@QAEHPAEK@Z"

/* ===== Globals ===== */

static HMODULE   g_hOrigDll = NULL;
static HINSTANCE g_hSelf    = NULL;

static fn_DecodeFormat        g_origDecodeIFF           = NULL;
static fn_EncodeFormat        g_origEncodeIFF           = NULL;
static fn_DecodeFormat        g_origDecodeBMP           = NULL;
static fn_EncodeFormat        g_origEncodeBMP           = NULL;
static fn_DecodeFormat        g_origDecodePNG           = NULL;
static fn_EncodeFormat        g_origEncodePNG           = NULL;
static fn_ImageTypeByFilename g_origImageTypeByFilename = NULL;
static fn_ImageTypeByJFile    g_origImageTypeByJFile    = NULL;
static fn_LoadJFFString       g_origLoadJFFFilterString = NULL;
static fn_LoadJFFString       g_origLoadJFFFileIDString = NULL;
static fn_LoadJFFFormatDesc   g_origLoadJFFFormatDesc   = NULL;
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

/* ===== WebP settings (persisted in registry) ===== */

static BOOL g_settingsLoaded    = FALSE;
static BOOL g_lossless          = TRUE;
static int  g_quality           = 100;
static BOOL g_preserveAlpha     = TRUE;

static BYTE* g_cuevaCodigo      = NULL;

static void EnsureSettingsLoaded(void);
static BOOL ShowWebPOptionsDialog(HWND parent);

static int __cdecl HookDialogoOpcionesFormato(SEncodeParms* parms)
{
    if (parms && *(unsigned int*)(parms->data + ENCODEPARMS_FORMAT_OFF) == JFF_FMT_IFF) {
        EnsureSettingsLoaded();
        if (ShowWebPOptionsDialog(GetActiveWindow())) {
            return 0;
        }
        return 100;
    }

    typedef int (__cdecl *fn_OrigOpcionesIFF)(SEncodeParms*);
    return ((fn_OrigOpcionesIFF)0x00545790)(parms);
}

static void EscribirMemoriaEjecutable(DWORD direccion, const BYTE* datos, SIZE_T tamano)
{
    DWORD proteccionAnterior = 0;
    if (VirtualProtect((LPVOID)direccion, tamano, PAGE_EXECUTE_READWRITE, &proteccionAnterior)) {
        memcpy((void*)direccion, datos, tamano);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)direccion, tamano);
        VirtualProtect((LPVOID)direccion, tamano, proteccionAnterior, &proteccionAnterior);
    }
}

static void EscribirSaltoRelativo(BYTE* destino, DWORD origenVA, DWORD destinoVA)
{
    destino[0] = 0xE9;
    *(DWORD*)(destino + 1) = destinoVA - (origenVA + 5);
}

static void EscribirLlamadaRelativa(BYTE* destino, DWORD origenVA, DWORD destinoVA)
{
    destino[0] = 0xE8;
    *(DWORD*)(destino + 1) = destinoVA - (origenVA + 5);
}

static void AplicarParchesMemoriaPSP(void)
{
    HMODULE hExe = GetModuleHandleA(NULL);
    if ((DWORD)hExe != 0x00400000) return;

    const BYTE origPA[5]  = { 0xE8, 0xF2, 0x91, 0x00, 0x00 };
    const BYTE origPB1[5] = { 0xE8, 0x36, 0x09, 0x00, 0x00 };
    const BYTE origPB2[5] = { 0xE8, 0xFC, 0x07, 0x00, 0x00 };
    const BYTE origPC[6]  = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
    const BYTE origPD[6]  = { 0x0F, 0x85, 0x1B, 0x01, 0x00, 0x00 };

    if (*(BYTE*)0x006B4609 == 0xE8 && *(DWORD*)0x006B460A == 0x001B39B7)
        EscribirMemoriaEjecutable(0x006B4609, origPA, 5);
    if (*(BYTE*)0x00499A05 == 0xE8 && *(DWORD*)0x00499A06 == 0x003CE5CA)
        EscribirMemoriaEjecutable(0x00499A05, origPB1, 5);
    if (*(BYTE*)0x00499B3F == 0xE8 && *(DWORD*)0x00499B40 == 0x003CE490)
        EscribirMemoriaEjecutable(0x00499B3F, origPB2, 5);
    if (*(BYTE*)0x00656E30 == 0xE9 && *(DWORD*)0x00656E31 == 0x00218B5B)
        EscribirMemoriaEjecutable(0x00656E30, origPC, 6);
    if (*(BYTE*)0x00499ACD == 0xE9 && *(DWORD*)0x00499ACE == 0x003D5EFE)
        EscribirMemoriaEjecutable(0x00499ACD, origPD, 6);

    const BYTE firmaExtraccion[6] = { 0x83, 0x79, 0x04, 0x1C, 0x75, 0x22 };
    const BYTE firmaAviso[5]      = { 0x83, 0xF9, 0x1C, 0x74, 0x14 };
    const BYTE firmaOpciones[5]   = { 0xE8, 0x77, 0xE1, 0xFF, 0xFF };

    if (memcmp((const void*)0x005DB40E, firmaExtraccion, 6) == 0 &&
        memcmp((const void*)0x005DB7C6, firmaAviso, 5) == 0) {

        g_cuevaCodigo = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (g_cuevaCodigo) {
            BYTE* cueva1 = g_cuevaCodigo;
            cueva1[0]  = 0x83; cueva1[1]  = 0x79; cueva1[2] = 0x04; cueva1[3] = 0x1C;
            cueva1[4]  = 0x74; cueva1[5]  = 0x06;
            cueva1[6]  = 0x83; cueva1[7]  = 0x79; cueva1[8] = 0x04; cueva1[9] = 0x0B;
            cueva1[10] = 0x75; cueva1[11] = 0x05;
            EscribirSaltoRelativo(cueva1 + 12, (DWORD)(cueva1 + 12), 0x005DB414);
            EscribirSaltoRelativo(cueva1 + 17, (DWORD)(cueva1 + 17), 0x005DB436);

            BYTE* cueva2 = g_cuevaCodigo + 64;
            cueva2[0] = 0x83; cueva2[1] = 0xF9; cueva2[2] = 0x1C;
            cueva2[3] = 0x74; cueva2[4] = 0x0A;
            cueva2[5] = 0x83; cueva2[6] = 0xF9; cueva2[7] = 0x0B;
            cueva2[8] = 0x74; cueva2[9] = 0x0A;
            EscribirSaltoRelativo(cueva2 + 10, (DWORD)(cueva2 + 10), 0x005DB7CB);
            EscribirSaltoRelativo(cueva2 + 15, (DWORD)(cueva2 + 15), 0x005DB7DF);
            EscribirSaltoRelativo(cueva2 + 20, (DWORD)(cueva2 + 20), 0x005DB8E8);

            FlushInstructionCache(GetCurrentProcess(), g_cuevaCodigo, 4096);

            BYTE parche1[6];
            EscribirSaltoRelativo(parche1, 0x005DB40E, (DWORD)cueva1);
            parche1[5] = 0x90;
            EscribirMemoriaEjecutable(0x005DB40E, parche1, 6);

            BYTE parche2[5];
            EscribirSaltoRelativo(parche2, 0x005DB7C6, (DWORD)cueva2);
            EscribirMemoriaEjecutable(0x005DB7C6, parche2, 5);
        }
    }

    if (memcmp((const void*)0x00547614, firmaOpciones, 5) == 0) {
        BYTE parche3[5];
        EscribirLlamadaRelativa(parche3, 0x00547614, (DWORD)HookDialogoOpcionesFormato);
        EscribirMemoriaEjecutable(0x00547614, parche3, 5);
    }
}

static void LoadSettings(void)
{
    HKEY hKey;
    DWORD val = 0, sz = sizeof(DWORD), type = 0;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Jasc\\Paint Shop Pro\\WebP",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        g_settingsLoaded = TRUE;
        return;
    }

    sz = sizeof(DWORD);
    if (RegQueryValueExA(hKey, "Lossless", NULL, &type, (LPBYTE)&val, &sz) == ERROR_SUCCESS)
        g_lossless = (val != 0);
    sz = sizeof(DWORD);
    if (RegQueryValueExA(hKey, "Quality", NULL, &type, (LPBYTE)&val, &sz) == ERROR_SUCCESS)
        if ((int)val >= 0 && (int)val <= 100) g_quality = (int)val;
    sz = sizeof(DWORD);
    if (RegQueryValueExA(hKey, "PreserveAlpha", NULL, &type, (LPBYTE)&val, &sz) == ERROR_SUCCESS)
        g_preserveAlpha = (val != 0);

    RegCloseKey(hKey);
    g_settingsLoaded = TRUE;
}

static void SaveSettings(void)
{
    HKEY hKey;
    DWORD val;

    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Jasc\\Paint Shop Pro\\WebP",
                        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;

    val = g_lossless ? 1 : 0;
    RegSetValueExA(hKey, "Lossless", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
    val = (DWORD)g_quality;
    RegSetValueExA(hKey, "Quality", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
    val = g_preserveAlpha ? 1 : 0;
    RegSetValueExA(hKey, "PreserveAlpha", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));

    RegCloseKey(hKey);
}

static void EnsureSettingsLoaded(void)
{
    if (!g_settingsLoaded) LoadSettings();
}

/* ===== WebP options dialog (built in memory, no .rc needed) ===== */

#define IDC_WEBP_LOSSLESS  1001
#define IDC_WEBP_ALPHA     1002
#define IDC_WEBP_QUALITY   1003

static void UpdateQualityEnabled(HWND hDlg)
{
    BOOL lossless = (IsDlgButtonChecked(hDlg, IDC_WEBP_LOSSLESS) == BST_CHECKED);
    EnableWindow(GetDlgItem(hDlg, IDC_WEBP_QUALITY), !lossless);
}

static INT_PTR CALLBACK WebPOptionsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(hDlg, IDC_WEBP_LOSSLESS, g_lossless ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_WEBP_ALPHA, g_preserveAlpha ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(hDlg, IDC_WEBP_QUALITY, (UINT)g_quality, FALSE);
        UpdateQualityEnabled(hDlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_WEBP_LOSSLESS:
            UpdateQualityEnabled(hDlg);
            return TRUE;
        case IDOK: {
            BOOL ok = FALSE;
            UINT q = GetDlgItemInt(hDlg, IDC_WEBP_QUALITY, &ok, FALSE);
            g_lossless = (IsDlgButtonChecked(hDlg, IDC_WEBP_LOSSLESS) == BST_CHECKED);
            g_preserveAlpha = (IsDlgButtonChecked(hDlg, IDC_WEBP_ALPHA) == BST_CHECKED);
            if (!ok || q > 100) q = 100;
            g_quality = (int)q;
            SaveSettings();
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static BYTE* AlignDlgPtr(BYTE* p)
{
    ULONG_PTR v = (ULONG_PTR)p;
    return (BYTE*)((v + 3) & ~(ULONG_PTR)3);
}

static BYTE* AddDlgStringW(BYTE* p, const wchar_t* s)
{
    while (*s) { *(wchar_t*)p = *s++; p += sizeof(wchar_t); }
    *(wchar_t*)p = 0; p += sizeof(wchar_t);
    return p;
}

static BYTE* AddDlgItem(BYTE* p, DWORD style, short x, short y, short cx, short cy,
                        WORD id, WORD classAtom, const wchar_t* title)
{
    p = AlignDlgPtr(p);
    DLGITEMTEMPLATE* it = (DLGITEMTEMPLATE*)p;
    it->style = style;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy;
    it->id = id;
    p += sizeof(DLGITEMTEMPLATE);
    *(WORD*)p = 0xFFFF; p += sizeof(WORD);
    *(WORD*)p = classAtom; p += sizeof(WORD);
    p = AddDlgStringW(p, title);
    *(WORD*)p = 0; p += sizeof(WORD);
    return p;
}

static BOOL ShowWebPOptionsDialog(HWND parent)
{
    void* mem = GlobalAlloc(GPTR, 2048);
    if (!mem) return FALSE;

    DLGTEMPLATE* dlg = (DLGTEMPLATE*)mem;
    dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dlg->cdit = 6;
    dlg->x = 10; dlg->y = 10; dlg->cx = 200; dlg->cy = 92;

    BYTE* p = (BYTE*)(dlg + 1);
    *(WORD*)p = 0; p += sizeof(WORD);
    *(WORD*)p = 0; p += sizeof(WORD);
    p = AddDlgStringW(p, L"WebP Options");
    *(WORD*)p = 8; p += sizeof(WORD);
    p = AddDlgStringW(p, L"MS Shell Dlg");

    p = AddDlgItem(p, WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                   8,8,180,12, IDC_WEBP_LOSSLESS, 0x0080, L"Lossless");
    p = AddDlgItem(p, WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                   8,24,180,12, IDC_WEBP_ALPHA, 0x0080, L"Preserve transparency (alpha)");
    p = AddDlgItem(p, WS_CHILD|WS_VISIBLE|SS_LEFT,
                   8,42,90,10, 0, 0x0082, L"Quality (0-100):");
    p = AddDlgItem(p, WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER,
                   105,40,40,12, IDC_WEBP_QUALITY, 0x0081, L"");
    p = AddDlgItem(p, WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
                   40,66,50,14, IDOK, 0x0080, L"OK");
    p = AddDlgItem(p, WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                   110,66,50,14, IDCANCEL, 0x0080, L"Cancel");

    INT_PTR res = DialogBoxIndirectParamW(g_hSelf, (DLGTEMPLATE*)mem, parent, WebPOptionsDlgProc, 0);
    GlobalFree(mem);
    return (res == IDOK);
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

/* [PATCH] PNG signature check, used by the decode fallback */
static BOOL IsPngMagic(const BYTE* hdr, int len)
{
    if (len < 4) return FALSE;
    return (hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G');
}

/* ===== WIC-based PNG encode/decode (alpha-preserving intermediate) ===== */

static BYTE* CreatePNGFromRGBA(int w, int h, const BYTE* rgba, DWORD* pPngSz)
{
    IWICImagingFactory* factory = NULL;
    IWICStream* stream = NULL;
    IWICBitmapEncoder* encoder = NULL;
    IWICBitmapFrameEncode* frame = NULL;
    BYTE* result = NULL;
    HRESULT hr;
    char tmpDir[MAX_PATH], tmpFile[MAX_PATH];

    *pPngSz = 0;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return NULL;

    GetTempPathA(MAX_PATH, tmpDir);
    GetTempFileNameA(tmpDir, "wbp", 0, tmpFile);

    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
    if (FAILED(hr)) goto done;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) goto done;

    {
        WCHAR wTmp[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, tmpFile, -1, wTmp, MAX_PATH);
        hr = stream->InitializeFromFilename(wTmp, GENERIC_WRITE);
    }
    if (FAILED(hr)) goto done;

    hr = factory->CreateEncoder(GUID_ContainerFormatPng, NULL, &encoder);
    if (FAILED(hr)) goto done;
    hr = encoder->Initialize((IStream*)stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) goto done;
    hr = encoder->CreateNewFrame(&frame, NULL);
    if (FAILED(hr)) goto done;
    hr = frame->Initialize(NULL);
    if (FAILED(hr)) goto done;
    hr = frame->SetSize(w, h);
    if (FAILED(hr)) goto done;

    {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&fmt);
        if (FAILED(hr)) goto done;

        BYTE* row = (BYTE*)VirtualAlloc(NULL, w * 4, MEM_COMMIT, PAGE_READWRITE);
        if (!row) goto done;

        for (int y = 0; y < h; y++) {
            const BYTE* src = rgba + y * w * 4;
            for (int x = 0; x < w; x++) {
                row[x*4+0] = src[x*4+2];
                row[x*4+1] = src[x*4+1];
                row[x*4+2] = src[x*4+0];
                row[x*4+3] = src[x*4+3];
            }
            frame->WritePixels(1, w * 4, w * 4, row);
        }
        VirtualFree(row, 0, MEM_RELEASE);
    }

    frame->Commit();
    encoder->Commit();
    if (frame) frame->Release(); frame = NULL;
    if (encoder) encoder->Release(); encoder = NULL;
    if (stream) stream->Release(); stream = NULL;
    if (factory) factory->Release(); factory = NULL;

    {
        FILE* fp = fopen(tmpFile, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            *pPngSz = (DWORD)ftell(fp);
            fseek(fp, 0, SEEK_SET);
            result = (BYTE*)VirtualAlloc(NULL, *pPngSz, MEM_COMMIT, PAGE_READWRITE);
            if (result) fread(result, 1, *pPngSz, fp);
            fclose(fp);
        }
    }

done:
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    DeleteFileA(tmpFile);
    CoUninitialize();
    return result;
}

static BYTE* DecodePngToRGBA(const char* path, int* outW, int* outH)
{
    IWICImagingFactory* factory = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* converter = NULL;
    BYTE* pixels = NULL;
    UINT w = 0, h = 0;
    HRESULT hr;

    *outW = *outH = 0;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return NULL;

    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
    if (FAILED(hr)) goto done;

    {
        WCHAR wpath[MAX_PATH];
        int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
        if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
        if (n <= 0) { hr = E_FAIL; goto done; }
        hr = factory->CreateDecoderFromFilename(wpath, NULL, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (FAILED(hr)) goto done;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) goto done;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) goto done;

    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) goto done;
    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, NULL, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) goto done;

    pixels = (BYTE*)VirtualAlloc(NULL, w * h * 4, MEM_COMMIT, PAGE_READWRITE);
    if (!pixels) goto done;
    hr = converter->CopyPixels(NULL, w * 4, w * h * 4, pixels);
    if (FAILED(hr)) {
        VirtualFree(pixels, 0, MEM_RELEASE);
        pixels = NULL;
        goto done;
    }
    *outW = (int)w;
    *outH = (int)h;

done:
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    CoUninitialize();
    return pixels;
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

/* ===== WebP Decode ===== */

static JascErrorCode DecodeWebP(tSFILEREQUEST* pReq)
{
    char* filename = (char*)((BYTE*)pReq + OFFSET_FILENAME);
    FILE* fp;
    long  fileSz;
    BYTE  hdr[12] = {0};
    BYTE* fileData   = NULL;
    BYTE* rgbaPixels = NULL;
    BYTE* pngData    = NULL;
    DWORD pngSz;
    BYTE* pMemFileObj = NULL;
    void* pOrigJFile;
    int   w, h;
    JascErrorCode result = JascError_General;

    ProxyLog("DecodeWebP: %s", filename);

    /* [PATCH] Anything that is not a WebP goes to the original IFF decoder
       (or to the PNG decoder if it really is a PNG), never blindly to PNG. */
    fp = fopen(filename, "rb");
    if (!fp) {
        ProxyLog("DecodeWebP: cannot open file, -> orig DecodeIFF");
        return g_origDecodeIFF(pReq);
    }

    fseek(fp, 0, SEEK_END);
    fileSz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSz < 12 || fileSz > 100 * 1024 * 1024) {
        fclose(fp);
        ProxyLog("DecodeWebP: bad size %ld, -> orig DecodeIFF", fileSz);
        return g_origDecodeIFF(pReq);
    }

    if (fread(hdr, 1, 12, fp) != 12) {
        fclose(fp);
        return g_origDecodeIFF(pReq);
    }

    if (!IsWebPMagic(hdr, 12)) {
        fclose(fp);
        if (IsPngMagic(hdr, 12)) {
            ProxyLog("DecodeWebP: file is a PNG, -> orig DecodePNG");
            return g_origDecodePNG(pReq);
        }
        ProxyLog("DecodeWebP: not WebP, -> orig DecodeIFF");
        return g_origDecodeIFF(pReq);
    }

    fseek(fp, 0, SEEK_SET);
    fileData = (BYTE*)VirtualAlloc(NULL, fileSz, MEM_COMMIT, PAGE_READWRITE);
    if (!fileData) { fclose(fp); return JascError_General; }
    fread(fileData, 1, fileSz, fp);
    fclose(fp);

    rgbaPixels = WebPDecodeRGBA(fileData, (size_t)fileSz, &w, &h);
    VirtualFree(fileData, 0, MEM_RELEASE);

    if (!rgbaPixels || w <= 0 || h <= 0) {
        ProxyLog("DecodeWebP: libwebp failed to decode");
        if (rgbaPixels) WebPFree(rgbaPixels);
        return JascError_General;
    }

    pngData = CreatePNGFromRGBA(w, h, rgbaPixels, &pngSz);
    WebPFree(rgbaPixels);

    if (!pngData || pngSz == 0) {
        if (pngData) VirtualFree(pngData, 0, MEM_RELEASE);
        return JascError_General;
    }

    ProxyLog("DecodeWebP: PNG %d bytes (%dx%d)", (int)pngSz, w, h);

    pMemFileObj = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 4096);
    if (!pMemFileObj) {
        VirtualFree(pngData, 0, MEM_RELEASE);
        return JascError_General;
    }

    g_origMemFileCtor(pMemFileObj, NULL);
    g_origMemFileSetPtr(pMemFileObj, NULL, pngData, pngSz);

    pOrigJFile = *(void**)((BYTE*)pReq + OFFSET_JFILE);
    *(void**)((BYTE*)pReq + OFFSET_JFILE) = pMemFileObj;

    result = g_origDecodePNG(pReq);
    ProxyLog("DecodeWebP: DecodePNG -> %d", (int)result);

    *(void**)((BYTE*)pReq + OFFSET_JFILE) = pOrigJFile;
    HeapFree(GetProcessHeap(), 0, pMemFileObj);
    VirtualFree(pngData, 0, MEM_RELEASE);

    return result;
}

/* ===== WebP Encode ===== */

/* [PATCH] On failure: log, remove the intermediate PNG/BMP that PSP wrote
   under the target name (it is NOT a WebP), and report an error to PSP. */
static JascErrorCode EncodeFail(const char* path, const char* why)
{
    ProxyLog("EncodeWebP FAILED: %s", why);
    DeleteFileA(path);
    return JascError_General;
}

typedef int (__cdecl *fn_CallbackObtenerNumAlfa)(void* cb, int* pNum);
typedef int (__cdecl *fn_CallbackObtenerCanalAlfa)(void* cb, int idx, void* pInfo);

static void AplicarCanalAlfaDocumento(tSFILEREQUEST* pReq, BYTE* rgbaData, int w, int h)
{
    if (!pReq || !rgbaData || w <= 0 || h <= 0) return;

    int total = w * h;
    for (int i = 0; i < total; i++) {
        if (rgbaData[i * 4 + 3] < 255) return;
    }

    BYTE* cb = *(BYTE**)((BYTE*)pReq + OFFSET_BMPINFO);
    if (!cb || *(DWORD*)cb != 0x124) return;

    fn_CallbackObtenerNumAlfa fnNumAlfa = *(fn_CallbackObtenerNumAlfa*)(cb + 0x4C);
    fn_CallbackObtenerCanalAlfa fnCanalAlfa = *(fn_CallbackObtenerCanalAlfa*)(cb + 0x94);
    if (!fnNumAlfa || !fnCanalAlfa) return;

    int numCanales = 0;
    if (fnNumAlfa(cb, &numCanales) != 0 || numCanales <= 0) return;

    BYTE infoAlfa[0x120];
    memset(infoAlfa, 0, sizeof(infoAlfa));
    *(DWORD*)infoAlfa = 0x119;

    if (fnCanalAlfa(cb, 0, infoAlfa) != 0) return;

    HGLOBAL hAlfaDIB = *(HGLOBAL*)(infoAlfa + 0x104);
    if (!hAlfaDIB) return;

    BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)GlobalLock(hAlfaDIB);
    if (!bih) return;

    if (bih->biBitCount == 8 && bih->biWidth == w && abs(bih->biHeight) == h) {
        DWORD colores = bih->biClrUsed ? bih->biClrUsed : 256;
        BYTE* pixelesAlfa = (BYTE*)bih + bih->biSize + colores * 4;
        int strideAlfa = ((w + 3) / 4) * 4;
        BOOL invertido = (bih->biHeight > 0);

        for (int y = 0; y < h; y++) {
            int filaOrigen = invertido ? (h - 1 - y) : y;
            const BYTE* filaAlfa = pixelesAlfa + filaOrigen * strideAlfa;
            BYTE* filaDestino = rgbaData + y * w * 4;
            for (int x = 0; x < w; x++) {
                filaDestino[x * 4 + 3] = filaAlfa[x];
            }
        }
    }

    GlobalUnlock(hAlfaDIB);
}

static JascErrorCode EncodeWebP(tSFILEREQUEST* pReq)
{
    char* filename = (char*)((BYTE*)pReq + OFFSET_FILENAME);
    char  origFilename[520];
    FILE* fp;
    BYTE* rgbaData = NULL;
    BYTE* webpOut  = NULL;
    size_t webpSz  = 0;
    int   w = 0, h = 0;
    JascErrorCode result;

    strncpy(origFilename, filename, sizeof(origFilename) - 1);
    origFilename[sizeof(origFilename) - 1] = 0;
    ProxyLog("EncodeWebP: %s", origFilename);

    EnsureSettingsLoaded();

    /* Ask PSP to write an intermediate file via its PNG encoder */
    result = g_origEncodePNG(pReq);
    if (result != JascOK) {
        ProxyLog("EncodeWebP: EncodePNG -> %d, trying EncodeBMP", (int)result);
        result = g_origEncodeBMP(pReq);
        if (result != JascOK) {
            ProxyLog("EncodeWebP: EncodeBMP -> %d", (int)result);
            DeleteFileA(origFilename);
            return result;
        }
    }

    /* Read the intermediate back as RGBA */
    rgbaData = DecodePngToRGBA(origFilename, &w, &h);

    if (!rgbaData || w <= 0 || h <= 0) {
        if (rgbaData) { VirtualFree(rgbaData, 0, MEM_RELEASE); rgbaData = NULL; }

        /* Fallback: try reading as BMP */
        long bmpSz; int x, y;
        fp = fopen(origFilename, "rb");
        if (!fp) return EncodeFail(origFilename, "cannot reopen intermediate file");
        fseek(fp, 0, SEEK_END); bmpSz = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (bmpSz < 54) { fclose(fp); return EncodeFail(origFilename, "intermediate file too small"); }

        BYTE* bmpData = (BYTE*)VirtualAlloc(NULL, bmpSz, MEM_COMMIT, PAGE_READWRITE);
        if (!bmpData) { fclose(fp); return EncodeFail(origFilename, "out of memory (bmp)"); }
        fread(bmpData, 1, bmpSz, fp); fclose(fp);

        if (bmpData[0] != 'B' || bmpData[1] != 'M') {
            VirtualFree(bmpData, 0, MEM_RELEASE);
            return EncodeFail(origFilename, "intermediate is neither PNG nor BMP");
        }

        w = *(int*)(bmpData + 18);
        h = *(int*)(bmpData + 22);
        if (h < 0) h = -h;

        WORD  bpp        = *(WORD*)(bmpData + 28);
        DWORD dataOffset = *(DWORD*)(bmpData + 10);

        if (bpp != 24 && bpp != 32) {
            VirtualFree(bmpData, 0, MEM_RELEASE);
            return EncodeFail(origFilename, "unsupported BMP bit depth");
        }

        rgbaData = (BYTE*)VirtualAlloc(NULL, w * h * 4, MEM_COMMIT, PAGE_READWRITE);
        if (!rgbaData) {
            VirtualFree(bmpData, 0, MEM_RELEASE);
            return EncodeFail(origFilename, "out of memory (rgba)");
        }

        int rowBytes = (bpp == 24) ? ((w * 3 + 3) / 4) * 4 : w * 4;
        for (y = 0; y < h; y++) {
            BYTE* src = bmpData + dataOffset + (h - 1 - y) * rowBytes;
            BYTE* dst = rgbaData + y * w * 4;
            for (x = 0; x < w; x++) {
                if (bpp == 24) {
                    dst[x*4+0] = src[x*3+2]; dst[x*4+1] = src[x*3+1];
                    dst[x*4+2] = src[x*3+0]; dst[x*4+3] = 255;
                } else {
                    dst[x*4+0] = src[x*4+2]; dst[x*4+1] = src[x*4+1];
                    dst[x*4+2] = src[x*4+0]; dst[x*4+3] = src[x*4+3];
                }
            }
        }
        VirtualFree(bmpData, 0, MEM_RELEASE);
    }

    if (!rgbaData) return EncodeFail(origFilename, "no pixel data");

    if (g_preserveAlpha) {
        AplicarCanalAlfaDocumento(pReq, rgbaData, w, h);
    } else {
        int totalPixeles = w * h;
        for (int i = 0; i < totalPixeles; i++) {
            BYTE alfa = rgbaData[i * 4 + 3];
            if (alfa < 255) {
                rgbaData[i * 4 + 0] = (BYTE)((rgbaData[i * 4 + 0] * alfa + 255 * (255 - alfa) + 127) / 255);
                rgbaData[i * 4 + 1] = (BYTE)((rgbaData[i * 4 + 1] * alfa + 255 * (255 - alfa) + 127) / 255);
                rgbaData[i * 4 + 2] = (BYTE)((rgbaData[i * 4 + 2] * alfa + 255 * (255 - alfa) + 127) / 255);
                rgbaData[i * 4 + 3] = 255;
            }
        }
    }

    if (g_lossless) {
        webpSz = WebPEncodeLosslessRGBA(rgbaData, w, h, w * 4, &webpOut);
    } else {
        float q = (float)g_quality;
        if (q < 0.0f) q = 0.0f;
        if (q > 100.0f) q = 100.0f;
        webpSz = WebPEncodeRGBA(rgbaData, w, h, w * 4, q, &webpOut);
    }
    VirtualFree(rgbaData, 0, MEM_RELEASE);

    if (webpSz == 0 || !webpOut) {
        if (webpOut) WebPFree(webpOut);
        return EncodeFail(origFilename, "libwebp encode failed");
    }

    fp = fopen(origFilename, "wb");
    if (!fp) { WebPFree(webpOut); return EncodeFail(origFilename, "cannot write output file"); }
    if (fwrite(webpOut, 1, webpSz, fp) != webpSz) {
        fclose(fp);
        WebPFree(webpOut);
        return EncodeFail(origFilename, "short write");
    }
    fclose(fp);

    ProxyLog("EncodeWebP: %dx%d -> %zu bytes", w, h, webpSz);
    WebPFree(webpOut);
    return JascOK;
}

/* ===== Format descriptor ===== */

static void SetFormatDescStrings(SFormatDesc* desc, const char* name)
{
    lstrcpynA((char*)desc->data + 0x08, name, 0x31);
    lstrcpynA((char*)desc->data + 0x6D, name, 0x31);
    lstrcpynA((char*)desc->data + 0xA0, name, 0x31);
}

/* ===== Intercepted exports ===== */

#define JFFEXPORT __declspec(dllexport)

JFFEXPORT JascErrorCode __cdecl DecodeIFF(tSFILEREQUEST* pReq)
{
    if (!pReq) return g_origDecodeIFF(pReq);
    /* [PATCH] Decide by content inside DecodeWebP, not by extension. */
    char* filename = (char*)((BYTE*)pReq + OFFSET_FILENAME);
    ProxyLog("DecodeIFF: filename='%s'", filename);
    return DecodeWebP(pReq);
}

JFFEXPORT JascErrorCode __cdecl DecodePNG(tSFILEREQUEST* pReq)
{
    return g_origDecodePNG(pReq);
}

JFFEXPORT JascErrorCode __cdecl EncodeIFF(tSFILEREQUEST* pReq)
{
    char* filename = (char*)((BYTE*)pReq + OFFSET_FILENAME);
    ProxyLog("EncodeIFF: filename='%s'", filename);
    EnsureSettingsLoaded();
    return EncodeWebP(pReq);
}

JFFEXPORT JascErrorCode __cdecl EncodePNG(tSFILEREQUEST* pReq)
{
    return g_origEncodePNG(pReq);
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
        strncpy(buf, "WebP|*.WEBP|", len - 1);
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

JFFEXPORT JascErrorCode __cdecl LoadJFFFormatDesc(SFormatDesc* desc, SEncodeParms* parms)
{
    if (!desc || !parms || !g_origLoadJFFFormatDesc)
        return JascError_Param;

    if (*(unsigned int*)(parms->data + ENCODEPARMS_FORMAT_OFF) != JFF_FMT_IFF)
        return g_origLoadJFFFormatDesc(desc, parms);

    /* Borrow PNG capabilities so PSP treats WebP as an alpha-capable format */
    SEncodeParms tmpParms;
    memcpy(&tmpParms, parms, sizeof(SEncodeParms));
    *(unsigned int*)(tmpParms.data + ENCODEPARMS_FORMAT_OFF) = JFF_FMT_PNG;

    SFormatDesc tmpDesc;
    memset(&tmpDesc, 0, sizeof(SFormatDesc));
    *(unsigned int*)tmpDesc.data = FORMATDESC_MAGIC;

    JascErrorCode res = g_origLoadJFFFormatDesc(&tmpDesc, &tmpParms);
    if (res != JascOK) return res;

    memcpy(desc, &tmpDesc, sizeof(SFormatDesc));
    *(unsigned int*)desc->data = FORMATDESC_MAGIC;
    SetFormatDescStrings(desc, "WebP Image Format");
    return JascOK;
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
    g_origDecodePNG           = (fn_DecodeFormat)GetProcAddress(g_hOrigDll, MN_DecodePNG);
    g_origEncodePNG           = (fn_EncodeFormat)GetProcAddress(g_hOrigDll, MN_EncodePNG);
    g_origImageTypeByFilename = (fn_ImageTypeByFilename)GetProcAddress(g_hOrigDll, MN_ImgByFile);
    g_origImageTypeByJFile    = (fn_ImageTypeByJFile)GetProcAddress(g_hOrigDll, MN_ImgByJFile);
    g_origLoadJFFFilterString = (fn_LoadJFFString)GetProcAddress(g_hOrigDll, MN_FilterStr);
    g_origLoadJFFFileIDString = (fn_LoadJFFString)GetProcAddress(g_hOrigDll, MN_FileIDStr);
    g_origLoadJFFFormatDesc   = (fn_LoadJFFFormatDesc)GetProcAddress(g_hOrigDll, MN_FormatDesc);
    g_origMemFileCtor         = (fn_MemFile_Ctor)GetProcAddress(g_hOrigDll, MN_MemCtor);
    g_origMemFileSetPtr       = (fn_MemFile_SetPtr)GetProcAddress(g_hOrigDll, MN_MemSetPtr);

    if (!g_origDecodeIFF || !g_origDecodeBMP || !g_origEncodeBMP ||
        !g_origDecodePNG || !g_origEncodePNG ||
        !g_origImageTypeByFilename || !g_origImageTypeByJFile ||
        !g_origLoadJFFFilterString || !g_origLoadJFFFileIDString ||
        !g_origLoadJFFFormatDesc ||
        !g_origMemFileCtor || !g_origMemFileSetPtr) {
        MessageBoxA(NULL, "Failed to resolve exports from jff_orig.dll",
                    "WebP Plugin - Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    return TRUE;
}

/* ===== Paste As New Image keeping transparency =====
 *
 * psp.exe only reads CF_DIB when pasting, and Windows gives it a 24-bit
 * copy with the alpha already flattened. Apps such as Firefox also put a
 * real RGBA "PNG" on the clipboard. When that PNG has real transparency,
 * the "Paste As New Image" command writes it to a temporary file and opens
 * it through PSP's own OpenDocumentFile, so the transparency is imported
 * exactly as with File > Open. The temporary file is deleted right after
 * and is kept out of the recent files list. In every other case (no PNG,
 * opaque PNG, any failure) the original command runs untouched.
 *
 * Addresses are for psp.exe 7.04 (image base 0x00400000) and are checked
 * before patching:
 *   0x00896794  vtable slot holding the "Paste As New Image" handler
 *   0x006DF960  that handler (thiscall, one stack argument, RET 4)
 *   0x0069F740  PSP's override of CWinApp::OpenDocumentFile (thiscall, RET 4)
 *   0x00893760  slot of that override in the application's vtable
 *   0x00893764  next slot: CWinApp::AddToRecentFileList (MFC thunk 0x00829D20)
 *   0x0082907E  import thunk for MFC's AfxGetModuleState
 */

#define PASTE_NEWIMAGE_SLOT    0x00896794
#define PASTE_NEWIMAGE_ORIG    0x006DF960
#define PSP_APP_OPENDOC        0x0069F740
#define PSP_APP_OPENDOC_SLOT   0x00893760
#define PSP_APP_ADDRECENT_SLOT 0x00893764
#define PSP_APP_ADDRECENT_ORIG 0x00829D20
#define PSP_AFXGETMODULESTATE  0x0082907E

typedef int   (__fastcall *fn_PasteHandler)(void* pThis, void* edx, void* arg);
typedef void* (__stdcall  *fn_AfxGetModuleState)(void);
typedef void* (__fastcall *fn_OpenDocFile)(void* pApp, void* edx, const char* path);
typedef void  (__fastcall *fn_AddRecent)(void* pApp, void* edx, const char* path);

/* Set only while our own paste opens its temporary file. */
static BOOL g_suppressRecentFile = FALSE;

static void __fastcall HookAddToRecentFileList(void* pApp, void* edx, const char* path)
{
    if (g_suppressRecentFile) return;
    ((fn_AddRecent)PSP_APP_ADDRECENT_ORIG)(pApp, edx, path);
}

/* TRUE if the PNG decodes and at least one pixel is not fully opaque. */
static BOOL PngHasTransparency(const BYTE* png, SIZE_T pngSz)
{
    IWICImagingFactory* factory = NULL;
    IWICStream* wstream = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* conv = NULL;
    BYTE* row = NULL;
    UINT w = 0, h = 0, x, y;
    HRESULT hr;
    BOOL comInit = FALSE;
    BOOL found = FALSE;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    comInit = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return FALSE;

    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) goto done;
    hr = factory->CreateStream(&wstream);
    if (FAILED(hr)) goto done;
    hr = wstream->InitializeFromMemory((BYTE*)png, (DWORD)pngSz);
    if (FAILED(hr)) goto done;
    hr = factory->CreateDecoderFromStream(wstream, NULL, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) goto done;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) goto done;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0 || w > 65536 || h > 65536) goto done;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr)) goto done;
    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                          NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) goto done;

    row = (BYTE*)malloc((size_t)w * 4);
    if (!row) goto done;
    for (y = 0; y < h && !found; y++) {
        WICRect rc;
        rc.X = 0; rc.Y = (INT)y; rc.Width = (INT)w; rc.Height = 1;
        if (FAILED(conv->CopyPixels(&rc, w * 4, w * 4, row))) break;
        for (x = 0; x < w; x++) {
            if (row[x * 4 + 3] != 255) { found = TRUE; break; }
        }
    }

done:
    if (row) free(row);
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (wstream) wstream->Release();
    if (factory) factory->Release();
    if (comInit) CoUninitialize();
    return found;
}

/* Copies the clipboard's "PNG" format into a malloc'ed buffer (NULL if absent). */
static BYTE* ReadClipboardPng(SIZE_T* outSz)
{
    UINT fmt = RegisterClipboardFormatA("PNG");
    HANDLE h;
    const BYTE* p;
    SIZE_T sz;
    BYTE* copy = NULL;

    *outSz = 0;
    if (!fmt || !IsClipboardFormatAvailable(fmt)) return NULL;
    if (!OpenClipboard(NULL)) return NULL;
    h = GetClipboardData(fmt);
    if (h) {
        sz = GlobalSize(h);
        p = (const BYTE*)GlobalLock(h);
        if (p && sz >= 33 && sz <= 0x20000000 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) {
            copy = (BYTE*)malloc(sz);
            if (copy) { memcpy(copy, p, sz); *outSz = sz; }
        }
        if (p) GlobalUnlock(h);
    }
    CloseClipboard();
    return copy;
}

/* Deletes a file made by WriteTempPng and its two folders (each one is only
 * removed if it is empty, so another running PSP is never affected). */
static void RemoveTempPng(const char* path)
{
    char buf[MAX_PATH];
    char* slash;

    strncpy(buf, path, MAX_PATH - 1);
    buf[MAX_PATH - 1] = 0;
    if (!DeleteFileA(buf) && GetLastError() != ERROR_FILE_NOT_FOUND)
        ProxyLog("[PASTE] could not delete %s (error %lu)", buf, GetLastError());
    slash = strrchr(buf, '\\');
    if (slash) { *slash = 0; RemoveDirectoryA(buf); }     /* <random> folder */
    slash = strrchr(buf, '\\');
    if (slash) { *slash = 0; RemoveDirectoryA(buf); }     /* psp7_paste folder */
}

/* Writes the PNG to %TEMP%\psp7_paste\<random>\Pasted image.png. The random
 * folder keeps concurrent pastes apart; the fixed file name is what PSP shows
 * as the document title. */
static BOOL WriteTempPng(const BYTE* data, SIZE_T sz, char* outPath, size_t outLen)
{
    char root[MAX_PATH], sub[MAX_PATH];
    LARGE_INTEGER qpc;
    HANDLE hf;
    DWORD written = 0;
    DWORD n = GetTempPathA(MAX_PATH, root);

    if (n == 0 || n > MAX_PATH - 64) return FALSE;
    strcat(root, "psp7_paste\\");
    CreateDirectoryA(root, NULL);
    QueryPerformanceCounter(&qpc);
    _snprintf(sub, sizeof(sub), "%s%08lX%08lX%04lX\\", root,
              GetCurrentProcessId(), (unsigned long)qpc.LowPart, GetTickCount() & 0xFFFF);
    sub[sizeof(sub) - 1] = 0;
    if (!CreateDirectoryA(sub, NULL)) { RemoveDirectoryA(root); return FALSE; }
    _snprintf(outPath, outLen, "%sPasted image.png", sub);
    outPath[outLen - 1] = 0;

    hf = CreateFileA(outPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        RemoveDirectoryA(sub);
        RemoveDirectoryA(root);
        return FALSE;
    }
    if (!WriteFile(hf, data, (DWORD)sz, &written, NULL) || written != (DWORD)sz) {
        CloseHandle(hf);
        RemoveTempPng(outPath);
        return FALSE;
    }
    CloseHandle(hf);
    return TRUE;
}

/* The CWinApp object, validated by checking that its vtable holds PSP's
 * OpenDocumentFile override. NULL if anything looks different. */
static void* GetPspApp(void)
{
    void* app = NULL;
    __try {
        void* state = ((fn_AfxGetModuleState)PSP_AFXGETMODULESTATE)();
        if (state) {
            void* cand = *(void**)((BYTE*)state + 4);
            if (cand) {
                const DWORD* vt = *(const DWORD**)cand;
                int i;
                for (i = 0; i < 96; i++) {
                    if (vt[i] == PSP_APP_OPENDOC) { app = cand; break; }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        app = NULL;
    }
    return app;
}

static void* CallOpenDocumentFile(void* app, const char* path)
{
    void* doc = NULL;
    __try {
        doc = ((fn_OpenDocFile)PSP_APP_OPENDOC)(app, NULL, path);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        doc = NULL;
    }
    return doc;
}

static BOOL TryPasteAlphaAsNewImage(void)
{
    SIZE_T sz = 0;
    BYTE* png;
    char path[MAX_PATH];
    void* app;
    void* doc = NULL;

    png = ReadClipboardPng(&sz);
    if (!png) return FALSE;
    if (!PngHasTransparency(png, sz)) {
        ProxyLog("[PASTE] clipboard PNG (%u bytes) has no transparency -> original paste", (unsigned)sz);
        free(png);
        return FALSE;
    }
    ProxyLog("[PASTE] clipboard PNG with real alpha (%u bytes)", (unsigned)sz);
    if (!WriteTempPng(png, sz, path, sizeof(path))) {
        ProxyLog("[PASTE] cannot write the temporary PNG -> original paste");
        free(png);
        return FALSE;
    }
    free(png);

    app = GetPspApp();
    if (!app) {
        ProxyLog("[PASTE] application object not found -> original paste");
    } else {
        ProxyLog("[PASTE] opening %s through OpenDocumentFile (app=%p)", path, app);
        g_suppressRecentFile = TRUE;
        doc = CallOpenDocumentFile(app, path);
        g_suppressRecentFile = FALSE;
        ProxyLog("[PASTE] OpenDocumentFile returned %p", doc);
    }

    RemoveTempPng(path);
    return doc != NULL;
}

static int __fastcall HookPasteNewImage(void* pThis, void* edx, void* arg)
{
    static BOOL busy = FALSE;
    if (!busy) {
        BOOL done;
        busy = TRUE;
        done = TryPasteAlphaAsNewImage();
        busy = FALSE;
        if (done) return 0;
    }
    return ((fn_PasteHandler)PASTE_NEWIMAGE_ORIG)(pThis, edx, arg);
}

static void InstallPasteAlphaHook(void)
{
    DWORD hook;

    if ((DWORD)GetModuleHandleA(NULL) != 0x00400000) return;
    if (*(DWORD*)PASTE_NEWIMAGE_SLOT != PASTE_NEWIMAGE_ORIG ||
        *(DWORD*)PSP_APP_OPENDOC_SLOT != PSP_APP_OPENDOC ||
        *(WORD*)PSP_AFXGETMODULESTATE != 0x25FF) {
        ProxyLog("[PASTE] expected bytes not found, Paste As New Image NOT hooked");
        return;
    }

    /* Keep the temporary file out of the recent files list. If that slot
     * is not what we expect the paste still works, it just leaves an entry. */
    if (*(DWORD*)PSP_APP_ADDRECENT_SLOT == PSP_APP_ADDRECENT_ORIG) {
        hook = (DWORD)(DWORD_PTR)HookAddToRecentFileList;
        EscribirMemoriaEjecutable(PSP_APP_ADDRECENT_SLOT, (const BYTE*)&hook, 4);
        ProxyLog("[PASTE] AddToRecentFileList hooked (slot 0x%08X)", (unsigned)PSP_APP_ADDRECENT_SLOT);
    } else {
        ProxyLog("[PASTE] AddToRecentFileList slot differs, recent files list NOT protected");
    }

    hook = (DWORD)(DWORD_PTR)HookPasteNewImage;
    EscribirMemoriaEjecutable(PASTE_NEWIMAGE_SLOT, (const BYTE*)&hook, 4);
    ProxyLog("[PASTE] Paste As New Image hooked (slot 0x%08X)", (unsigned)PASTE_NEWIMAGE_SLOT);
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID)
{
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hSelf = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        if (!LoadOriginalDll()) return FALSE;
        AplicarParchesMemoriaPSP();
        InstallPasteAlphaHook();
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_cuevaCodigo) VirtualFree(g_cuevaCodigo, 0, MEM_RELEASE);
        if (g_hOrigDll) FreeLibrary(g_hOrigDll);
    }
    return TRUE;
}
