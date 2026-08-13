#pragma once

#include <string>

namespace Stobe {
namespace Voice {

struct Context {
  std::string speakerName;
  std::string speakerSerial;
  std::string targetName;
  std::string targetSerial;
  std::string mode;
};

bool Start(const Context &context);
void StopAndTranscribe();
void Cancel();
void Update();
bool IsRecording();
bool ConsumeResult(int resultId, Context &contextOut, std::string &textOut,
                   std::string &errorOut);

} // namespace Voice
} // namespace Stobe
