#include "StobeText.h"

#include <cctype>

namespace Stobe {
namespace Text {
namespace {

std::string TrimCopy(const std::string &value) {
  if (value.empty()) {
    return "";
  }
  size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return "";
  }
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

} // namespace

std::string EscapeJSON(const std::string &value) {
  std::string result = "";
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '\"') {
      result += "\\\"";
    } else if (c == '\\') {
      result += "\\\\";
    } else if (c == '\n') {
      result += "\\n";
    } else if (c == '\r') {
      result += "\\r";
    } else {
      result += c;
    }
  }
  return result;
}

std::string UnescapeJSON(const std::string &value) {
  std::string result = "";
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] == '\\' && i + 1 < value.length()) {
      if (value[i + 1] == 'n') {
        result += '\n';
        ++i;
      } else if (value[i + 1] == 'r') {
        result += '\r';
        ++i;
      } else if (value[i + 1] == '\"') {
        result += '\"';
        ++i;
      } else if (value[i + 1] == '\\') {
        result += '\\';
        ++i;
      } else if (value[i + 1] == 'u' && i + 5 < value.length()) {
        unsigned int cp = 0;
        bool valid = true;
        for (int j = 0; j < 4; ++j) {
          char c = value[i + 2 + j];
          cp <<= 4;
          if (c >= '0' && c <= '9') {
            cp += static_cast<unsigned int>(c - '0');
          } else if (c >= 'a' && c <= 'f') {
            cp += static_cast<unsigned int>(10 + c - 'a');
          } else if (c >= 'A' && c <= 'F') {
            cp += static_cast<unsigned int>(10 + c - 'A');
          } else {
            valid = false;
            break;
          }
        }

        if (valid) {
          if (cp <= 0x7F) {
            result += static_cast<char>(cp);
          } else if (cp <= 0x7FF) {
            result += static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            result += static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
          }
          i += 5;
        } else {
          result += value[i];
        }
      } else {
        result += value[i];
      }
    } else {
      result += value[i];
    }
  }
  return result;
}

std::string JsonReadField(const std::string &json, const std::string &key) {
  std::string keyQuery = "\"" + key + "\":";
  size_t pos = json.find(keyQuery);
  if (pos == std::string::npos) {
    return "";
  }

  size_t valStart = json.find_first_not_of(" \t\r\n", pos + keyQuery.length());
  if (valStart == std::string::npos) {
    return "";
  }

  if (json[valStart] == '\"') {
    ++valStart;
    std::string result = "";
    for (size_t i = valStart; i < json.length(); ++i) {
      if (json[i] == '\\' && i + 1 < json.length()) {
        result += json[i];
        result += json[i + 1];
        ++i;
      } else if (json[i] == '\"') {
        return UnescapeJSON(result);
      } else {
        result += json[i];
      }
    }
  } else if (json[valStart] == '[' || json[valStart] == '{') {
    char open = json[valStart];
    char close = (open == '[') ? ']' : '}';
    int bracketCount = 0;
    bool inString = false;
    size_t i = valStart;
    for (; i < json.length(); ++i) {
      if (json[i] == '"' && (i == 0 || json[i - 1] != '\\')) {
        inString = !inString;
      } else if (!inString) {
        if (json[i] == open) {
          ++bracketCount;
        } else if (json[i] == close) {
          --bracketCount;
          if (bracketCount == 0) {
            break;
          }
        }
      }
    }
    if (i < json.length() && json[i] == close) {
      return json.substr(valStart, i - valStart + 1);
    }
  } else {
    size_t end = json.find_first_of(",}", valStart);
    if (end != std::string::npos) {
      return json.substr(valStart, end - valStart);
    }
  }

  return "";
}

std::string SanitizeDialogueForEventStream(const std::string &value) {
  std::string cleaned = TrimCopy(value);
  if (cleaned.empty()) {
    return "";
  }

  size_t nulPos = cleaned.find('\0');
  if (nulPos != std::string::npos) {
    cleaned = TrimCopy(cleaned.substr(0, nulPos));
  }

  size_t end = cleaned.size();
  size_t questionCount = 0;
  while (end > 0 && cleaned[end - 1] == '?') {
    --end;
    ++questionCount;
  }
  if (questionCount >= 2) {
    while (end > 0 &&
           std::isspace(static_cast<unsigned char>(cleaned[end - 1])) != 0) {
      --end;
    }
    cleaned = TrimCopy(cleaned.substr(0, end));
  }

  if (cleaned.length() >= 2 && cleaned[cleaned.length() - 1] == '7' &&
      std::isspace(static_cast<unsigned char>(cleaned[cleaned.length() - 2])) !=
          0) {
    size_t prefixEnd = cleaned.length() - 2;
    while (prefixEnd > 0 &&
           std::isspace(static_cast<unsigned char>(cleaned[prefixEnd - 1])) !=
               0) {
      --prefixEnd;
    }
    if (prefixEnd > 0) {
      cleaned = TrimCopy(cleaned.substr(0, prefixEnd));
    }
  }

  return TrimCopy(cleaned);
}

} // namespace Text
} // namespace Stobe
