#pragma once

#include <map>
#include <string>
#include <vector>

/* Audio is always encoded at this bitrate. It isn't a setting: streams only go
 * lower where the platform accepts less. */
constexpr int kFixedAudioBitrateKbps = 320;

const std::map<int, std::string> &GetSimpleAACEncoderBitrateMap();
const char *GetSimpleAACEncoderForBitrate(int bitrate);
int FindClosestAvailableSimpleAACBitrate(int bitrate);

const std::map<int, std::string> &GetSimpleOpusEncoderBitrateMap();
const char *GetSimpleOpusEncoderForBitrate(int bitrate);
int FindClosestAvailableSimpleOpusBitrate(int bitrate);

const std::vector<int> &GetAudioEncoderBitrates(const char *id);
int FindClosestAvailableAudioBitrate(const char *id, int bitrate);
