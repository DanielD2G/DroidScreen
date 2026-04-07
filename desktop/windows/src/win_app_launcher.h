#pragma once

#include <string>

namespace droidscreen {

class WinAppLauncher {
public:
  static bool launch(const std::string &kind, const std::string &target,
                     const std::string &args, const std::string &working_dir,
                     std::string *error = nullptr);

  static std::string icon_png_base64(const std::string &kind,
                                     const std::string &target);

  static std::wstring resolve_executable_target(const std::string &target);
};

} // namespace droidscreen
