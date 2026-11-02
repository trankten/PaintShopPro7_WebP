$ErrorActionPreference = "Stop"

Write-Host ""
Write-Host "====================================================="
Write-Host "  PSP7-WebP - Build Script"
Write-Host "====================================================="
Write-Host ""

# ---------------------------------------------------------------
#  Configuration
# ---------------------------------------------------------------

$LibWebPVersion = "1.0.3"
$LibWebPUrl     = "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${LibWebPVersion}-windows-x86.zip"

# ---------------------------------------------------------------
#  1) Find Visual Studio (x86 toolchain required)
# ---------------------------------------------------------------

$vcvars = $null
$vcPaths = @(
    "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat",
    "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars32.bat",
    "C:\Program Files\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
)
foreach ($p in $vcPaths) {
    if (Test-Path $p) { $vcvars = $p; break }
}
if (-not $vcvars) {
    Write-Host "[ERROR] Visual Studio with C++ x86 tools not found." -ForegroundColor Red
    Write-Host "        Install 'Desktop development with C++' workload." -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Visual Studio: $vcvars" -ForegroundColor Green

# ---------------------------------------------------------------
#  2) Download libwebp if needed
# ---------------------------------------------------------------

$libDir     = Join-Path $PSScriptRoot "lib"
$includeDir = Join-Path $PSScriptRoot "include"

if (-not (Test-Path (Join-Path $libDir "libwebp.lib")) -or -not (Test-Path (Join-Path $libDir "libwebpdemux.lib"))) {
    Write-Host "[*] Downloading libwebp ${LibWebPVersion}..." -ForegroundColor Yellow

    New-Item -ItemType Directory -Force -Path $libDir | Out-Null
    New-Item -ItemType Directory -Force -Path $includeDir | Out-Null

    $zip = Join-Path $PSScriptRoot "libwebp_dl.zip"
    $tmpDir = Join-Path $PSScriptRoot "libwebp_dl"

    Invoke-WebRequest -Uri $LibWebPUrl -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $tmpDir -Force

    $extracted = Get-ChildItem $tmpDir -Directory | Select-Object -First 1
    Copy-Item (Join-Path $extracted.FullName "lib\*") $libDir -Force
    Copy-Item (Join-Path $extracted.FullName "include\*") $includeDir -Recurse -Force

    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue

    Write-Host "[OK] libwebp downloaded" -ForegroundColor Green
} else {
    Write-Host "[OK] libwebp.lib found" -ForegroundColor Green
}

# ---------------------------------------------------------------
#  3) Generate the .def file (export forwarding table)
# ---------------------------------------------------------------

Write-Host "[*] Generating jff_proxy.def..." -ForegroundColor Yellow

$allExports = @(
    '??0CFileDiskBuffered@@QAE@XZ'
    '??0CJFileDisk@@QAE@PBD@Z'
    '??0CJFileDisk@@QAE@XZ'
    '??0CJFileMemory@@QAE@W4OpenReason@vcJFile@@@Z'
    '??0CJFileMemory@@QAE@XZ'
    '??1CFileDiskBuffered@@UAE@XZ'
    '??1CJFileDisk@@UAE@XZ'
    '??1CJFileMemory@@UAE@XZ'
    '?Append@vcJFile@@QAEXPAV1@PAVStatusControlBlock@@@Z'
    '?CMYKtoRGB@@YA?AW4JascErrorCode@@PAVStatusControlBlock@@PAXPAPAX@Z'
    '?CalculateCrc@@YAKPAEK@Z'
    '?CalculateCrc@@YAKPAX@Z'
    '?CalculateCrcBMP@@YA?AW4JascErrorCode@@PBDPAK@Z'
    '?CharsetToCodepage@@YAII@Z'
    '?Close@CFileDiskBuffered@@QAEXXZ'
    '?Close@CJFileDisk@@UAEXXZ'
    '?Close@CJFileMemory@@UAEXXZ'
    '?ConvertNumber@@YAGGGG@Z'
    '?Copy@vcJFile@@QAEXPAV1@PAVStatusControlBlock@@@Z'
    '?DecodeBMP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeCGM@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeCISRLE@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeCLP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeCUT@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeDCX@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeEMF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeEMFImport@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@PAUHENHMETAFILE__@@FF@Z'
    '?DecodeEMFThumb@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeEPS@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeFPX@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeFlatBMP@@YA?AW4JascErrorCode@@PBDPAPAX@Z'
    '?DecodeFlatGIFFromMemFile@@YA?AW4JascErrorCode@@PAVCJFileMemory@@PAPAXPAW4REQ_VARIANT@@PA_NPAF@Z'
    '?DecodeFlatJPG@@YA?AW4JascErrorCode@@PAEKPAPAXW4COLOR_ORDERS@@@Z'
    '?DecodeFlatJPGFromMemFile@@YA?AW4JascErrorCode@@PAVCJFileMemory@@PAPAXW4COLOR_ORDERS@@@Z'
    '?DecodeFlatJPGInfo@@YA?AW4JascErrorCode@@PAEKPAUSGraphicAttributes@@@Z'
    '?DecodeFlatJPGInfo@@YA?AW4JascErrorCode@@PAVvcJFile@@PAUSGraphicAttributes@@@Z'
    '?DecodeFlatJPGInfo@@YA?AW4JascErrorCode@@PBDPAUSGraphicAttributes@@@Z'
    '?DecodeFlatPNGFromMemFile@@YA?AW4JascErrorCode@@PAVCJFileMemory@@PAPAX1PAFPA_N@Z'
    '?DecodeGIF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeGRD@@YA?AW4JascErrorCode@@PAUSGradientRequest@@@Z'
    # DecodeIFF - intercepted, not forwarded
    # DecodePNG - intercepted (forwarded from code)
    '?DecodeIMG@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeJPG@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeKDC@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeLPIC@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeMAC@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeMSP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodePCD@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodePCX@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodePIC@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodePICT@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    # DecodePNG - intercepted (forwarded from code)
    '?DecodePSD@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodePSP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodePXM@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeRAS@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeRAW@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeRGB@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeSCT@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeTGA@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeTIF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeWMF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?DecodeWMFImport@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@PAUHMETAFILE__@@FF@Z'
    '?DecodeWPG@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?Delete@CJFileDisk@@UAEXXZ'
    '?Delete@CJFileMemory@@UAEXXZ'
    '?DetachGlobal@CJFileMemory@@QAEPAXXZ'
    '?EncodeBMP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeCISRLE@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeCLP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeCUT@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeEMF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeEPS@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeFPX@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeFlatBMP@@YA?AW4JascErrorCode@@PBDPAXE@Z'
    '?EncodeFlatGIFToJFile@@YA?AW4JascErrorCode@@PAXW4REQ_VARIANT@@_NFPAVvcJFile@@@Z'
    '?EncodeFlatJPG@@YA?AW4JascErrorCode@@PAVvcJFile@@PAXFW4REQ_VARIANT@@W4REQ_CMATH@@HW4COLOR_ORDERS@@@Z'
    '?EncodeFlatPNG@@YA?AW4JascErrorCode@@PAVvcJFile@@PAX1JPAUSCreatorData@@_N3@Z'
    '?EncodeFlatPNG@@YA?AW4JascErrorCode@@PAVvcJFile@@PAX1J_N2@Z'
    '?EncodeFlatPNG@@YA?AW4JascErrorCode@@PBDPAX1F_N2@Z'
    '?EncodeGIF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeGRD@@YA?AW4JascErrorCode@@PAUSGradientRequest@@@Z'
    # EncodeIFF - intercepted, not forwarded
    # EncodePNG - intercepted (forwarded from code)
    '?EncodeIMG@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeJPG@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeMAC@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeMSP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodePCX@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodePIC@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodePICT@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    # EncodePNG - intercepted (forwarded from code)
    '?EncodePSD@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodePSP@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodePXM@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeRAS@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeRAW@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeRGB@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeSCT@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeTGA@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeTIF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeWMF@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EncodeWPG@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@@Z'
    '?EnhancedMetaToDIB@@YA?AW4JascErrorCode@@PAUHWND__@@PAXPAUHENHMETAFILE__@@PAPAXUtagRECT@@HHHHH@Z'
    '?Flush@CFileDiskBuffered@@QAE?AW4JascErrorCode@@XZ'
    '?Flush@CJFileDisk@@UAEXXZ'
    '?Flush@CJFileMemory@@UAEXXZ'
    '?FreeGradientData@@YAXPAUSGradientData@@@Z'
    '?FreeGradientRequest@@YAXPAUSGradientRequest@@@Z'
    '?FreeLayer@@YA?AW4JascErrorCode@@PAUSLayerData@@@Z'
    '?FreeShape@@YA?AW4JascErrorCode@@PAUSShapeData@@@Z'
    '?GetBuffer@CFileDiskBuffered@@QAE?AW4JascErrorCode@@XZ'
    '?GetByte@CFileDiskBuffered@@QAE?AW4JascErrorCode@@AAE@Z'
    '?GetBytes@CFileDiskBuffered@@QAE?AW4JascErrorCode@@PAEK@Z'
    '?GetBytesWritten@CFileDiskBuffered@@QBEKXZ'
    '?GetCurrentReadPosition@CFileDiskBuffered@@QBEKXZ'
    '?GetDWord@CFileDiskBuffered@@QAE?AW4JascErrorCode@@AAK@Z'
    '?GetFilePathname@CFileDiskBuffered@@QBEPBDXZ'
    '?GetFilePathname@CJFileDisk@@QBEPBDXZ'
    '?GetFileSize@CFileDiskBuffered@@QBEKXZ'
    '?GetFileSize@CJFileDisk@@UBEKXZ'
    '?GetFileSize@CJFileMemory@@UBEKXZ'
    '?GetJFFFormatData@@YA?AW4JascErrorCode@@W4tagJascFileFormats@@PAG1@Z'
    '?GetLength@CFileDiskBufferedTagged@@QAE?AW4JascErrorCode@@AAK@Z'
    '?GetSystemACP@@YAIXZ'
    '?GetTaggedField@CFileDiskBufferedTagged@@QAE?AW4JascErrorCode@@PAEAAK@Z'
    '?GetTempMemPtr@CJFileMemory@@QBEPAEXZ'
    '?GetUsedRect@@YA?AW4JascErrorCode@@PAXJJPAUtagRECT@@H@Z'
    '?GetWord@CFileDiskBuffered@@QAE?AW4JascErrorCode@@AAG@Z'
    # ImageTypeByFilename - intercepted
    # ImageTypeByJFile - intercepted
    '?InitializeFPX@@YA?AW4JascErrorCode@@XZ'
    '?InvertDIB@@YA?AW4JascErrorCode@@PAVStatusControlBlock@@PAXPAPAX@Z'
    '?IsBackgroundLayer@@YA_NPAUSLayerData@@@Z'
    '?JffCheckVersion@@YA_NKKKK_N@Z'
    # LoadJFFFileIDString - intercepted
    # LoadJFFFilterString - intercepted
    # LoadJFFFormatDesc - intercepted (forwarded from code)
    '?LoadJFFString@@YAPADIPADH@Z'
    '?MetaToDIB@@YA?AW4JascErrorCode@@PAUHWND__@@PAXPAUHMETAFILE__@@PAPAXUtagRECT@@HHHHH@Z'
    '?Open@CFileDiskBuffered@@QAE?AW4JascErrorCode@@PBDKW4EFileOpenMode@1@@Z'
    '?Open@CJFileDisk@@UAEXW4OpenReason@vcJFile@@@Z'
    '?Open@CJFileMemory@@UAEXW4OpenReason@vcJFile@@@Z'
    '?OrderedDither2@@YA?AW4JascErrorCode@@PAVStatusControlBlock@@PAXPAPAXE@Z'
    '?PasteDIB@@YA?AW4JascErrorCode@@PAUHWND__@@PAXPAPAX@Z'
    '?PeekByte@CFileDiskBuffered@@QAE?AW4JascErrorCode@@AAE@Z'
    '?PickRLEType@@YA?AW4JascErrorCode@@PAUtSFILEREQUEST@@PAE@Z'
    '?PutBuffer@CFileDiskBuffered@@QAE?AW4JascErrorCode@@XZ'
    '?PutByte@CFileDiskBuffered@@QAE?AW4JascErrorCode@@E@Z'
    '?PutBytes@CFileDiskBuffered@@QAE?AW4JascErrorCode@@PBEK@Z'
    '?PutDWord@CFileDiskBuffered@@QAE?AW4JascErrorCode@@K@Z'
    '?PutLength@CFileDiskBufferedTagged@@QAE?AW4JascErrorCode@@K@Z'
    '?PutString@CFileDiskBufferedTagged@@QAE?AW4JascErrorCode@@EPBD@Z'
    '?PutTaggedField@CFileDiskBufferedTagged@@QAE?AW4JascErrorCode@@EKPBE@Z'
    '?PutWord@CFileDiskBuffered@@QAE?AW4JascErrorCode@@G@Z'
    '?RGBtoCMYK@@YA?AW4JascErrorCode@@PAVStatusControlBlock@@PAXPAPAX@Z'
    '?Read@CJFileDisk@@UAEKPAXK_N@Z'
    '?Read@CJFileMemory@@UAEKPAXK_N@Z'
    '?ReadDWord@vcJFile@@QAEKW4eByteOrder@1@@Z'
    '?ReadWord@vcJFile@@QAEGW4eByteOrder@1@@Z'
    '?ResizeDIB@@YA?AW4JascErrorCode@@PAVStatusControlBlock@@PAXPAPAXHH@Z'
    '?Seek@CJFileDisk@@UAEKJW4OriginForSeek@vcJFile@@@Z'
    '?Seek@CJFileMemory@@UAEKJW4OriginForSeek@vcJFile@@@Z'
    '?SetFilePathname@CJFileDisk@@QAEXPBD@Z'
    '?SetGranularity@CJFileMemory@@QAEXK@Z'
    '?SetGraphicAttributesFromDIB@@YA?AW4JascErrorCode@@PAXPAUSGraphicAttributes@@@Z'
    '?SetJFFLanguage@@YA?AW4JascErrorCode@@QAD@Z'
    '?SetMemoryMode@CJFileMemory@@QAEHW4FileMemoryMode@vcJFile@@PAX@Z'
    '?SetMemoryOrigin@CJFileMemory@@QAEHK@Z'
    '?SetPointerMemoryMode@CJFileMemory@@QAEHPAEK@Z'
    '?SetSize@CJFileMemory@@QAEXK@Z'
    '?SetSystemACP@@YAXI@Z'
    '?SkipBytes@CFileDiskBuffered@@QAE?AW4JascErrorCode@@K@Z'
    '?TerminateFPX@@YA?AW4JascErrorCode@@XZ'
    '?Write@CJFileDisk@@UAEXQAXK@Z'
    '?Write@CJFileMemory@@UAEXQAXK@Z'
    '?WriteByte@vcJFile@@QAEXE@Z'
    '?WriteDWord@vcJFile@@QAEXKW4eByteOrder@1@@Z'
    '?WriteWord@vcJFile@@QAEXGW4eByteOrder@1@@Z'
)

$defLines = @("LIBRARY jff", "EXPORTS")
foreach ($name in $allExports) {
    $defLines += "    $name = jff_orig.$name"
}
$defLines -join "`r`n" | Out-File -FilePath (Join-Path $PSScriptRoot "src\jff_proxy.def") -Encoding ASCII

Write-Host "[OK] jff_proxy.def generated" -ForegroundColor Green

# ---------------------------------------------------------------
#  4) Compile
# ---------------------------------------------------------------

Write-Host "[*] Compiling jff.dll..." -ForegroundColor Yellow

$srcDir  = Join-Path $PSScriptRoot "src"
$outDir  = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$buildBat = @"
@echo off
call "$vcvars"
cl /nologo /W3 /O2 /MT /EHsc /D WIN32 /D NDEBUG /D _WINDOWS /D _USRDLL /D _CRT_SECURE_NO_WARNINGS /I"$includeDir" /c /TP "$srcDir\jff_proxy.cpp" /Fo"$outDir\jff_proxy.obj"
if errorlevel 1 exit /b 1
link /nologo /DLL /DEF:"$srcDir\jff_proxy.def" /OUT:"$outDir\jff.dll" "$outDir\jff_proxy.obj" "$libDir\libwebp.lib" "$libDir\libwebpdemux.lib" user32.lib kernel32.lib ole32.lib oleaut32.lib uuid.lib windowscodecs.lib advapi32.lib shell32.lib gdi32.lib comdlg32.lib comctl32.lib
if errorlevel 1 exit /b 1
echo BUILD_OK
"@

$batFile = Join-Path $outDir "_build.bat"
$buildBat | Out-File -FilePath $batFile -Encoding ASCII
$output = cmd /c $batFile
$exitCode = $LASTEXITCODE
Remove-Item $batFile -Force -ErrorAction SilentlyContinue

$output | ForEach-Object { Write-Host $_ }

if ($exitCode -ne 0 -or -not ($output -match "BUILD_OK")) {
    Write-Host "`n[ERROR] Compilation failed." -ForegroundColor Red
    exit 1
}

$dllInfo = Get-Item (Join-Path $outDir "jff.dll")
Write-Host "[OK] jff.dll built successfully ($([math]::Round($dllInfo.Length/1KB)) KB)" -ForegroundColor Green

$targetPspDirs = @("L:\psp\psp", "I:\Programas\Paint Shop Pro 7")
foreach ($targetDir in $targetPspDirs) {
    if (Test-Path $targetDir) {
        try {
            Copy-Item -Path (Join-Path $outDir "jff.dll") -Destination (Join-Path $targetDir "jff.dll") -Force -ErrorAction Stop
            Write-Host "[OK] Copied jff.dll to $targetDir" -ForegroundColor Green
        } catch {
            Write-Host "[WARN] No se pudo copiar jff.dll a $targetDir (posiblemente bloqueado por psp.exe)." -ForegroundColor Yellow
        }
    }
}

# ---------------------------------------------------------------
#  5) Done
# ---------------------------------------------------------------

Write-Host ""
Write-Host "====================================================="
Write-Host "  Build complete: build\jff.dll"
Write-Host "====================================================="
Write-Host ""
Write-Host "Installation:" -ForegroundColor Cyan
Write-Host "  1. Rename the original jff.dll to jff_orig.dll"
Write-Host "  2. Copy build\jff.dll to the PSP directory"
Write-Host ""
