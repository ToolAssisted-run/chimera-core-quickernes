// miniHawk Level A/B cross-validation dumper.
// Replays a quickerNES .test sequence (Simple cycle) and writes the final
// low memory (2KB) as raw bytes, for byte-comparison against EmuHawk dumps.
// Built against the quickerNES tree; see tests/native/README.

#include "nesInstance.hpp"
#include <jaffarCommon/deserializers/contiguous.hpp>
#include <jaffarCommon/file.hpp>
#include <jaffarCommon/json.hpp>
#include <jaffarCommon/serializers/contiguous.hpp>
#include <jaffarCommon/string.hpp>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char *argv[])
{
  if (argc != 3)
  {
    fprintf(stderr, "usage: dumper <testFile> <outFile>\n");
    return 1;
  }
  const std::string scriptFilePath = argv[1];
  const std::string outFilePath = argv[2];

  std::string scriptJsonRaw;
  if (!jaffarCommon::file::loadStringFromFile(scriptJsonRaw, scriptFilePath))
  {
    fprintf(stderr, "could not read test file\n");
    return 1;
  }
  const auto scriptJson = nlohmann::json::parse(scriptJsonRaw);

  NESInstance e(scriptJson);

  std::string romFileData;
  if (!jaffarCommon::file::loadStringFromFile(romFileData, scriptJson["Rom File"].get<std::string>()))
  {
    fprintf(stderr, "could not read rom file\n");
    return 1;
  }
  e.loadROM((uint8_t *)romFileData.data(), romFileData.size());

  const auto initialStateFilePath = scriptJson["Initial State File"].get<std::string>();
  if (initialStateFilePath != "")
  {
    fprintf(stderr, "initial-state tests not supported by dumper\n");
    return 1;
  }

  e.disableRendering();

  std::string sequenceRaw;
  if (!jaffarCommon::file::loadStringFromFile(sequenceRaw, scriptJson["Sequence File"].get<std::string>()))
  {
    fprintf(stderr, "could not read sequence file\n");
    return 1;
  }
  const auto sequence = jaffarCommon::string::split(sequenceRaw, '\n');

  // Optional: every N inputs, append a low-mem checkpoint to <outFile>.ckpt
  int checkpointInterval = 0;
  if (const char *ci = getenv("MINIHAWK_CHECKPOINT")) checkpointInterval = atoi(ci);
  FILE *ckpt = nullptr;
  if (checkpointInterval > 0) ckpt = fopen((outFilePath + ".ckpt").c_str(), "wb");

  // MINIHAWK_CYCLE=rerecord: full-state load/advance/save around every frame,
  // mirroring EmuHawk's IStatable round-trip (unlike the tester, which cycles a
  // REDUCED state honoring the test's Disable State Blocks list).
  const bool rerecord = getenv("MINIHAWK_CYCLE") != nullptr
    && std::string(getenv("MINIHAWK_CYCLE")) == "rerecord";
  uint8_t *cycleState = nullptr;
  size_t cycleStateSize = 0;
  if (rerecord)
  {
    cycleStateSize = e.getFullStateSize();
    cycleState = (uint8_t *)malloc(cycleStateSize);
    jaffarCommon::serializer::Contiguous s(cycleState, cycleStateSize);
    e.serializeState(s);
  }

  const auto inputParser = e.getInputParser();
  size_t inputIdx = 0;
  for (const auto &inputString : sequence)
  {
    if (inputString.empty()) continue;
    if (rerecord)
    {
      jaffarCommon::deserializer::Contiguous d(cycleState, cycleStateSize);
      e.deserializeState(d);
    }
    e.advanceState(inputParser->parseInputString(inputString));
    if (rerecord)
    {
      jaffarCommon::serializer::Contiguous s(cycleState, cycleStateSize);
      e.serializeState(s);
    }
    inputIdx++;
    if (ckpt != nullptr && inputIdx % checkpointInterval == 0)
      fwrite(e.getLowMem(), 1, e.getLowMemSize(), ckpt);
  }
  free(cycleState);
  if (ckpt != nullptr) fclose(ckpt);

  FILE *f = fopen(outFilePath.c_str(), "wb");
  if (!f)
  {
    fprintf(stderr, "could not open output file\n");
    return 1;
  }
  fwrite(e.getLowMem(), 1, e.getLowMemSize(), f);
  fclose(f);

  printf("dumped %lu bytes after %lu inputs\n", (unsigned long)e.getLowMemSize(), (unsigned long)sequence.size());
  return 0;
}
