#include "win_app_launcher.h"

#include <gdiplus.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace droidscreen {

namespace {

std::wstring utf8_to_wstring(const std::string &value) {
  if (value.empty())
    return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (len <= 1)
    return {};
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), len);
  out.resize(static_cast<size_t>(len - 1));
  return out;
}

std::once_flag g_gdiplus_once;
ULONG_PTR g_gdiplus_token = 0;

void ensure_gdiplus() {
  std::call_once(g_gdiplus_once, []() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr);
  });
}

int get_png_encoder_clsid(CLSID *clsid) {
  UINT num = 0;
  UINT size = 0;
  if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0) {
    return -1;
  }

  std::vector<uint8_t> buffer(size);
  auto *codecs = reinterpret_cast<Gdiplus::ImageCodecInfo *>(buffer.data());
  if (Gdiplus::GetImageEncoders(num, size, codecs) != Gdiplus::Ok) {
    return -1;
  }

  for (UINT i = 0; i < num; ++i) {
    if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
      *clsid = codecs[i].Clsid;
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::string base64_encode(const uint8_t *data, size_t size) {
  if (!data || size == 0)
    return {};

  DWORD out_len = 0;
  if (!CryptBinaryToStringA(data, static_cast<DWORD>(size),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
                            &out_len)) {
    return {};
  }

  std::string out(out_len ? out_len : 0, '\0');
  if (!CryptBinaryToStringA(data, static_cast<DWORD>(size),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                            out.data(), &out_len)) {
    return {};
  }
  if (out_len > 0) {
    out.resize(out_len - 1);
  }
  return out;
}

std::string bitmap_to_png_base64(HBITMAP bitmap) {
  if (!bitmap)
    return {};

  ensure_gdiplus();

  Gdiplus::Bitmap src(bitmap, nullptr);
  if (src.GetLastStatus() != Gdiplus::Ok) {
    return {};
  }

  Gdiplus::Bitmap scaled(64, 64, PixelFormat32bppARGB);
  Gdiplus::Graphics graphics(&scaled);
  graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
  graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
  graphics.DrawImage(&src, Gdiplus::Rect(0, 0, 64, 64));

  CLSID png_clsid{};
  if (get_png_encoder_clsid(&png_clsid) < 0) {
    return {};
  }

  IStream *stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
    return {};
  }

  std::string out;
  if (scaled.Save(stream, &png_clsid, nullptr) == Gdiplus::Ok) {
    HGLOBAL hglobal = nullptr;
    if (SUCCEEDED(GetHGlobalFromStream(stream, &hglobal)) && hglobal) {
      SIZE_T size = GlobalSize(hglobal);
      if (void *data = GlobalLock(hglobal)) {
        out = base64_encode(static_cast<const uint8_t *>(data),
                            static_cast<size_t>(size));
        GlobalUnlock(hglobal);
      }
    }
  }

  stream->Release();
  return out;
}

std::string hicon_to_png_base64(HICON icon) {
  if (!icon)
    return {};

  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(BITMAPV5HEADER);
  bi.bV5Width = 64;
  bi.bV5Height = -64;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;

  void *bits = nullptr;
  HDC dc = GetDC(nullptr);
  HBITMAP bitmap = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&bi),
                                    DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap) {
    ReleaseDC(nullptr, dc);
    return {};
  }

  HDC mem_dc = CreateCompatibleDC(dc);
  HGDIOBJ old_bitmap = SelectObject(mem_dc, bitmap);
  PatBlt(mem_dc, 0, 0, 64, 64, BLACKNESS);
  DrawIconEx(mem_dc, 0, 0, icon, 64, 64, 0, nullptr, DI_NORMAL);

  SelectObject(mem_dc, old_bitmap);
  DeleteDC(mem_dc);
  ReleaseDC(nullptr, dc);

  std::string out = bitmap_to_png_base64(bitmap);
  DeleteObject(bitmap);
  return out;
}

std::string
shell_item_icon_base64_from_parsing_name(const std::wstring &parsing_name) {
  if (parsing_name.empty())
    return {};

  PIDLIST_ABSOLUTE pidl = nullptr;
  SFGAOF attrs = 0;
  if (FAILED(SHParseDisplayName(parsing_name.c_str(), nullptr, &pidl, 0,
                                &attrs)) ||
      !pidl) {
    return {};
  }

  IShellItem *item = nullptr;
  std::string out;
  if (SUCCEEDED(SHCreateShellItem(nullptr, nullptr, pidl, &item)) && item) {
    IShellItemImageFactory *factory = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&factory))) && factory) {
      SIZE size{64, 64};
      HBITMAP bitmap = nullptr;
      if (SUCCEEDED(factory->GetImage(
              size, SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY, &bitmap)) &&
          bitmap) {
        out = bitmap_to_png_base64(bitmap);
        DeleteObject(bitmap);
      }
      factory->Release();
    }
    item->Release();
  }

  CoTaskMemFree(pidl);
  return out;
}

std::wstring resolve_protocol_icon_source(const std::wstring &protocol_target) {
  size_t colon = protocol_target.find(L':');
  if (colon == std::wstring::npos)
    return {};
  std::wstring scheme = protocol_target.substr(0, colon + 1);

  DWORD len = 0;
  AssocQueryStringW(0, ASSOCSTR_DEFAULTICON, scheme.c_str(), nullptr, nullptr,
                    &len);
  if (len == 0)
    return {};

  std::wstring icon_spec(len, L'\0');
  if (FAILED(AssocQueryStringW(0, ASSOCSTR_DEFAULTICON, scheme.c_str(), nullptr,
                               icon_spec.data(), &len))) {
    return {};
  }

  icon_spec.resize(wcsnlen(icon_spec.c_str(), icon_spec.size()));
  size_t comma = icon_spec.find(L',');
  return (comma == std::wstring::npos) ? icon_spec : icon_spec.substr(0, comma);
}

std::string fallback_app_icon_base64() {
  HICON icon = LoadIconW(nullptr, IDI_APPLICATION);
  return hicon_to_png_base64(icon);
}

} // namespace

std::wstring
WinAppLauncher::resolve_executable_target(const std::string &target) {
  std::wstring wide_target = utf8_to_wstring(target);
  if (wide_target.empty())
    return {};

  wchar_t resolved[MAX_PATH] = {};
  DWORD len = SearchPathW(nullptr, wide_target.c_str(), nullptr, MAX_PATH,
                          resolved, nullptr);
  if (len > 0 && len < MAX_PATH) {
    return resolved;
  }
  return wide_target;
}

bool WinAppLauncher::launch(const std::string &kind, const std::string &target,
                            const std::string &args,
                            const std::string &working_dir,
                            std::string *error) {
  SHELLEXECUTEINFOW exec{};
  exec.cbSize = sizeof(exec);
  exec.fMask = SEE_MASK_NOASYNC;
  exec.nShow = SW_SHOWNORMAL;

  const std::wstring wide_kind = utf8_to_wstring(kind);
  const std::wstring wide_target = utf8_to_wstring(target);
  const std::wstring wide_args = utf8_to_wstring(args);
  const std::wstring wide_workdir = utf8_to_wstring(working_dir);

  if (wide_kind == L"uwp") {
    std::wstring appsfolder = L"shell:AppsFolder\\";
    appsfolder += wide_target;
    exec.lpFile = L"explorer.exe";
    exec.lpParameters = appsfolder.c_str();
  } else if (wide_kind == L"protocol") {
    exec.lpFile = wide_target.c_str();
  } else {
    const std::wstring resolved = resolve_executable_target(target);
    exec.lpFile = resolved.empty() ? wide_target.c_str() : resolved.c_str();
    exec.lpParameters = wide_args.empty() ? nullptr : wide_args.c_str();
    exec.lpDirectory = wide_workdir.empty() ? nullptr : wide_workdir.c_str();
  }

  if (!ShellExecuteExW(&exec)) {
    if (error) {
      *error = "ShellExecuteExW failed";
    }
    return false;
  }
  return true;
}

std::string WinAppLauncher::icon_png_base64(const std::string &kind,
                                            const std::string &target) {
  const std::wstring wide_kind = utf8_to_wstring(kind);
  const std::wstring wide_target = utf8_to_wstring(target);

  if (wide_kind == L"uwp" && !wide_target.empty()) {
    std::string out = shell_item_icon_base64_from_parsing_name(
        L"shell:AppsFolder\\" + wide_target);
    if (!out.empty())
      return out;
  }

  if (wide_kind == L"protocol" && !wide_target.empty()) {
    std::wstring icon_source = resolve_protocol_icon_source(wide_target);
    if (!icon_source.empty()) {
      std::string out = shell_item_icon_base64_from_parsing_name(icon_source);
      if (!out.empty())
        return out;
    }
  }

  if (!wide_target.empty()) {
    std::wstring resolved = resolve_executable_target(target);
    if (!resolved.empty()) {
      std::string out = shell_item_icon_base64_from_parsing_name(resolved);
      if (!out.empty())
        return out;
    }
  }

  return fallback_app_icon_base64();
}

} // namespace droidscreen
