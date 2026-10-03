#include "AddonProtocol.h"

#include <cstdio>

namespace Stobe {
namespace AddonProtocol {
namespace {

const size_t kMaxNameBytes = 48;
const char kExtPrefix[] = "extcmd";
const size_t kExtPrefixLength = 6;

bool IsAsciiAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

} // namespace

std::string LowerAscii(const std::string &value) {
  std::string out = value;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') {
      out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
  }
  return out;
}

bool IsExtCommand(const std::string &rawCommand) {
  return rawCommand.size() > kExtPrefixLength &&
         LowerAscii(rawCommand.substr(0, kExtPrefixLength)) == kExtPrefix;
}

bool ParseExtCommand(const std::string &rawCommand, ExtCommand &out) {
  if (!IsExtCommand(rawCommand)) {
    return false;
  }
  const size_t separator = rawCommand.find('_', kExtPrefixLength);
  if (separator == std::string::npos || separator == kExtPrefixLength ||
      separator + 1 >= rawCommand.size()) {
    return false;
  }
  const std::string bridge =
      rawCommand.substr(kExtPrefixLength, separator - kExtPrefixLength);
  const std::string action = rawCommand.substr(separator + 1);
  if (!IsValidBridgeName(bridge) || !IsValidToken(action)) {
    return false;
  }
  out.command = rawCommand;
  out.bridge = bridge;
  out.action = action;
  return true;
}

bool IsValidBridgeName(const std::string &value) {
  if (value.empty() || value.size() > kMaxNameBytes) {
    return false;
  }
  for (size_t i = 0; i < value.size(); ++i) {
    if (!IsAsciiAlnum(value[i])) {
      return false;
    }
  }
  return true;
}

bool ParseStrictSerial(const std::string &value, unsigned int &out) {
  out = 0;
  if (value.empty()) {
    return false;
  }
  unsigned long long parsed = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + static_cast<unsigned long long>(c - '0');
    if (parsed > 0xFFFFFFFFull) {
      return false;
    }
  }
  if (parsed == 0) {
    return false;
  }
  out = static_cast<unsigned int>(parsed);
  return true;
}

bool IsValidToken(const std::string &value) {
  if (value.empty() || value.size() > kMaxNameBytes || value[0] == '.' ||
      value[0] == '-') {
    return false;
  }
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (!IsAsciiAlnum(c) && c != '_' && c != '.' && c != '-') {
      return false;
    }
  }
  return true;
}

// Server rule: ^[A-Za-z0-9][A-Za-z0-9 ._-]{0,63}$, no trailing '.' or ' '.
bool IsSafePackageName(const std::string &value) {
  if (value.empty() || value.size() > 64 || !IsAsciiAlnum(value[0])) {
    return false;
  }
  for (size_t i = 1; i < value.size(); ++i) {
    const char c = value[i];
    if (!IsAsciiAlnum(c) && c != ' ' && c != '.' && c != '_' && c != '-') {
      return false;
    }
  }
  const char last = value[value.size() - 1];
  return last != '.' && last != ' ';
}

// Server rule: ^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$
bool IsSafePackageVersion(const std::string &value) {
  if (value.empty() || value.size() > 64 || !IsAsciiAlnum(value[0])) {
    return false;
  }
  for (size_t i = 1; i < value.size(); ++i) {
    const char c = value[i];
    if (!IsAsciiAlnum(c) && c != '.' && c != '_' && c != '+' && c != '-') {
      return false;
    }
  }
  return true;
}

bool SplitPackageArchiveName(const std::string &fileName, std::string &stemOut) {
  const size_t dot = fileName.rfind('.');
  if (dot == std::string::npos || dot == 0) {
    return false;
  }
  const std::string extension = LowerAscii(fileName.substr(dot));
  if (extension != ".dwpkg" && extension != ".zip") {
    return false;
  }
  stemOut = fileName.substr(0, dot);
  return true;
}

std::string JsonStringValue(const std::string &body, const std::string &key) {
  const std::string needle = "\"" + key + "\"";
  const size_t keyPosition = body.find(needle);
  if (keyPosition == std::string::npos) {
    return "";
  }
  const size_t colon = body.find(':', keyPosition + needle.size());
  if (colon == std::string::npos) {
    return "";
  }
  const size_t quote = body.find_first_not_of(" \t\r\n", colon + 1);
  if (quote == std::string::npos || body[quote] != '"') {
    return "";
  }
  std::string value;
  bool escaped = false;
  for (size_t i = quote + 1; i < body.size(); ++i) {
    const char c = body[i];
    if (escaped) {
      if (c == 'n') {
        value.push_back('\n');
      } else if (c == 'r') {
        value.push_back('\r');
      } else if (c == 't') {
        value.push_back('\t');
      } else {
        value.push_back(c);
      }
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      return value;
    } else {
      value.push_back(c);
    }
  }
  return "";
}

bool JsonBooleanValue(const std::string &body, const std::string &key,
                      bool fallback) {
  const std::string needle = "\"" + key + "\"";
  const size_t keyPosition = body.find(needle);
  if (keyPosition == std::string::npos) {
    return fallback;
  }
  const size_t colon = body.find(':', keyPosition + needle.size());
  if (colon == std::string::npos) {
    return fallback;
  }
  const size_t value = body.find_first_not_of(" \t\r\n", colon + 1);
  if (value == std::string::npos) {
    return fallback;
  }
  if (body.compare(value, 4, "true") == 0) {
    return true;
  }
  if (body.compare(value, 5, "false") == 0) {
    return false;
  }
  return fallback;
}

std::string JsonEscape(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20) {
      char buffer[8];
      sprintf_s(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
      out += buffer;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

} // namespace AddonProtocol
} // namespace Stobe
