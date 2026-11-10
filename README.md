# PSP7-WebP

**Native WebP support for Paint Shop Pro 7.04**

Open and save [WebP](https://developers.google.com/speed/webp) images directly in Jasc Paint Shop Pro 7.04 — with full native alpha channel and transparency support on both read and write.

![Paint Shop Pro 7 with WebP support](screenshot.png)

## Why this project?

In 2026, Jasc Paint Shop Pro 7 — originally released in 2001 — remains a remarkably capable image editing and management tool for Windows. It runs flawlessly on the latest versions of Windows 11, and its lightweight, fast interface still holds up against modern alternatives.

I grew up with this application. It was my go-to tool for everything from photo editing to batch processing. But as the web moved to newer formats like WebP, PSP 7 was left behind — Jasc Software was acquired by Corel, and version 7 was never updated again. Every time I downloaded a WebP image, I had to convert it in another program before opening it in PSP.

This project fixes that. If anyone out there is still using Jasc Paint Shop Pro 7.04 (and I suspect some of us are), this plugin gives it native WebP support — no external converters needed.

![WebP thumbnails in PSP 7 Browser](screenshot_browser.png)

## Features

- **Open WebP images** — including 32-bit RGBA images with alpha (transparency) channels
- **Save as WebP** — lossless or lossy, with configurable quality (0–100)
- **Full Alpha channel support** — preserves layer transparency and document alpha channels natively on both read and write (including newly created transparent images and resized images, without flattening warning dialogs)
- **Native Options button integration** — configure compression mode, quality, and alpha preservation directly via the **Options...** button in the **Save As...** dialog
- **Browser thumbnails** — WebP files display thumbnails in PSP's built-in Browser window
- **Settings persistence** — your encoding preferences are remembered in the Windows Registry for regular **Save** (Ctrl+S) operations and across sessions

**Non-WebP fix**:
- **Paste with transparency** — This is a fix for a bug that PSP 7 has had since forever: `Edit` > `Paste` > `Paste As New Image` now keeps the transparency of images copied from other applications (for [example](https://developers.google.com/speed/webp/gallery2) a web browser). Previously, PSP 7 only read the opaque bitmap Windows provides, so transparent areas were pasted as black. Now, alpha transparency is kept and the new image is shown as expected. Please note: `Paste As New Layer` and `Paste As Transparent Selection` are not affected by this fix, so they still paste the flattened bitmap.

## Quick Install (prebuilt)

1. Go to the PSP 7 installation directory (where `psp.exe` lives)
2. Rename the original `jff.dll` to `jff_orig.dll`
3. Copy the provided `jff.dll` into the same directory
4. Launch Paint Shop Pro — WebP now appears in Open/Save dialogs

To uninstall, delete the proxy `jff.dll` and rename `jff_orig.dll` back to `jff.dll`.

## How it works

This project creates a **proxy DLL** that replaces `jff.dll` — PSP 7's image format library. The proxy:

- Forwards all 178 original exports transparently to the renamed `jff_orig.dll`
- Intercepts 10 functions to inject WebP read/write support:
  - `DecodeIFF` / `EncodeIFF` — read and write image data
  - `DecodePNG` / `EncodePNG` — used as intermediate codec for alpha support
  - `ImageTypeByFilename` / `ImageTypeByJFile` — file type detection
  - `LoadJFFFilterString` / `LoadJFFFileIDString` — UI filter strings
  - `LoadJFFFormatDesc` — format capabilities (reports PNG-like capabilities for alpha)
- Applies targeted, non-destructive in-memory hooks at runtime (`DLL_PROCESS_ATTACH`) so PSP 7's internal save pipeline treats WebP (`0x0B`) identically to PNG (`0x1C`) for alpha channel extraction, suppresses the false *"limited to a merged image"* flatten warning dialog, and routes the **Save As... → Options...** button to the WebP configuration dialog.
- Hooks the `Paste As New Image` command: if the clipboard contains a PNG with real transparency, it is written to a temporary file, opened through PSP's own `Open` path (so transparency is imported exactly as with `File` > `Open`) and deleted right after, without adding it to the recent files list. Otherwise the original paste runs unchanged.

The rarely-used **IFF (Amiga)** format slot is repurposed for WebP. All other image formats (BMP, JPEG, PNG, GIF, TIFF, PSD, etc.) continue working normally through the original DLL.

The built-in **Browser** window also supports WebP — thumbnails are generated and displayed just like any other supported format.


### Decode flow

1. PSP calls `DecodeIFF` for the intercepted format
2. The proxy reads the `.webp` file directly from disk and verifies the RIFF/WEBP magic header
3. [libwebp](https://developers.google.com/speed/webp/docs/api) decodes the WebP data to 32-bit RGBA pixels
4. Pixels are converted to an in-memory PNG (32-bit BGRA) via WIC
5. The PNG is passed to PSP's internal `DecodePNG` via a `CJFileMemory` object, preserving full transparency

### Encode flow

1. PSP prepares the document for saving; the runtime hook enables unflattened RGB + 8-bit Alpha DIB extraction for the WebP format slot (`0x0B`) without triggering the flatten warning dialog
2. PSP calls `EncodeIFF` when saving via **Save** (Ctrl+S) or **Save As...**
3. The proxy invokes `EncodePNG` to write a 32-bit RGBA intermediate file and reads back the RGBA pixel buffer via WIC
4. If the document contains a dedicated Alpha channel in addition to layer transparency, it is merged directly from PSP's document callback table (or composited over white if alpha preservation is disabled in options)
5. libwebp encodes the RGBA buffer to WebP (lossless or lossy per saved settings)
6. The target file is written with the final WebP data

## WebP Options

In the **File → Save As...** dialog, select **WebP (\*.webp)** and click the **Options...** button to configure encoding settings:

| Option | Default | Description |
|--------|---------|-------------|
| Lossless | On | Encode without quality loss |
| Preserve transparency (alpha) | On | Preserve layer transparency and alpha channels (if unchecked, composites over white) |
| Quality (0-100) | 100 | Lossy compression quality (enabled when Lossless is unchecked) |

Settings are persisted in the Windows registry under `HKCU\Software\Jasc\Paint Shop Pro\WebP` and are automatically used during subsequent **Save** (Ctrl+S) and **Save As...** operations across sessions.

## Save and Save As Support

Both **File → Save** (Ctrl+S) and **File → Save As...** are fully supported. The plugin intercepts the encoding requests directly, properly handling PSP 7's temporary files created during regular saves while preserving full alpha transparency.

## Build from source

### Requirements

- **Windows** (32-bit output required)
- **Visual Studio 2019, 2022 or 2026** with "Desktop development with C++" workload (x86 tools)
- **PowerShell 5.1+**
- Internet connection (to download libwebp on first build)

### Steps

```powershell
git clone https://github.com/trankten/psp7_webp.git
cd psp7_webp
.\build.ps1
```

The build script will:
1. Auto-detect Visual Studio
2. Download [libwebp 1.0.3](https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.0.3-windows-x86.zip) (x86) if not present
3. Generate the export forwarding table (`jff_proxy.def`)
4. Compile `jff.dll` — the output is in `build\jff.dll`

### Debug logging

To enable diagnostic logging, compile with `/D ENABLE_LOG=1`. The proxy will write to `jff_proxy.log` in the PSP directory.

## Compatibility

| Software | Version | Status |
|----------|---------|--------|
| Paint Shop Pro | 7.04 | Tested and working |
| Paint Shop Pro | 7.00–7.03 | Format codec works; runtime alpha/options hooks target 7.04 addresses |
| Paint Shop Pro | 8+ | Not tested (different DLL structure) |

## Known Limitations

- The IFF (Amiga ILBM) format is no longer available through the Open/Save dialog.
- Maximum input file size: 100 MB.
- WebP metadata (EXIF, ICC profiles) is not preserved.

## Changelog

### v2.5.0 (current)

- **Full native Alpha channel & transparency support on save**: preserves raster layer transparency and document alpha channels on newly created transparent canvases, edited/resized images, and loaded files without relying on a decode cache
- **Eliminated false flatten warning dialog**: saving transparent WebP images no longer triggers the *"limited to a merged image"* dialog or flattens transparency onto a white background
- **Native "Options..." button integration**: the `WebP Options` dialog now opens exclusively when clicking the **Options...** button inside the **Save As...** modal dialog, matching native Paint Shop Pro formats
- **Persistent encoding settings**: `Lossless`, `Preserve transparency (alpha)`, and `Quality` are remembered in the Windows Registry across normal **Save** (Ctrl+S) operations, **Save As...**, and application restarts
- **Fixed crash when drawing on transparent layers**: removed legacy on-disk binary patches and added automatic in-memory restoration if legacy patches are detected in `psp.exe`

### v2.1.0

- Fixed "Save" (Ctrl+S) producing Amiga IFF/ILBM files instead of WebP (contributed by [@OinkEsFabuloso](https://github.com/OinkEsFabuloso) in [#1](https://github.com/trankten/PaintShopPro7_WebP/pull/1))
- `EncodeIFF` now unconditionally encodes WebP, correctly handling PSP's temporary save files
- `DecodeIFF` now detects format by file content and magic header bytes instead of extension
- `DecodeWebP` adds fallback to original `DecodeIFF` (or `DecodePNG` if PNG signature is detected) for non-WebP files
- `EncodeWebP` now cleans up intermediate files and reports errors properly to PSP on failure

### v2.0.0

- Full alpha channel support on decode (via PNG intermediate instead of BMP)
- Experimental alpha channel preservation on save (cache-based)
- Lossless WebP encoding support
- Configurable compression quality (0–100)
- Options dialog shown on save (can be disabled)
- Settings persisted in Windows registry
- Format descriptor reports PNG-like capabilities (alpha, 24/32-bit)
- Uses WIC (Windows Imaging Component) for intermediate PNG handling

### v1.0.0

- Initial release
- Open and save WebP images (24-bit RGB only)
- Fixed lossy encoding at quality 85
- Browser thumbnail support

## Roadmap

- [x] Full native alpha preservation on save (including new and resized images) (v2.5.0)
- [x] Native "Save As... → Options..." button integration (v2.5.0)
- [x] Support "Save" (Ctrl+S) in addition to "Save As" (v2.1.0)
- [ ] WebP metadata preservation (EXIF, ICC profiles)

## Credits & Acknowledgments

- Special thanks to [@OinkEsFabuloso](https://github.com/OinkEsFabuloso) for diagnosing the temporary file behavior in PSP 7's save routine and contributing the fix in [PR #1](https://github.com/trankten/PaintShopPro7_WebP/pull/1).

## Project structure

```
psp7-webp/
├── build.ps1          # Build script (downloads deps, compiles, links)
├── src/
│   └── jff_proxy.cpp  # Proxy DLL source (single file)
├── build/             # Build output (generated)
│   └── jff.dll        # The proxy DLL
├── lib/               # libwebp static libraries (auto-downloaded)
├── include/           # libwebp headers (auto-downloaded)
├── screenshot.png
├── screenshot_browser.png
├── LICENSE
└── README.md
```

## Legal notice

This project is an **independent, third-party enhancement** for Paint Shop Pro 7, a product originally developed by Jasc Software, later acquired by Corel Corporation.

**This project does not contain, distribute, or modify any proprietary code** from Paint Shop Pro or Jasc Software. The proxy DLL is original code that communicates with the unmodified original DLL through its documented public export interface.

Paint Shop Pro 7 (released 2000) is widely considered [abandonware](https://en.wikipedia.org/wiki/Abandonware) — it is no longer sold, updated, or supported by its rights holders. This plugin requires a legitimately obtained copy of PSP 7 to function.

The WebP codec is provided by Google's [libwebp](https://chromium.googlesource.com/webm/libwebp/), licensed under the BSD 3-Clause License.

**Trademark notice:** "Paint Shop Pro" and "Jasc" are trademarks of their respective owners. This project is not affiliated with, endorsed by, or associated with Jasc Software or Corel Corporation.

## License

This project is released under the [MIT License](LICENSE).

The bundled libwebp library is licensed under the [BSD 3-Clause License](https://chromium.googlesource.com/webm/libwebp/+/refs/heads/main/COPYING).
