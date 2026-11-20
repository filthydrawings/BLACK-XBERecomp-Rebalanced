#pragma once
#include <string>
#include <vector>

// research.cpp: the temporary research logger, logs\research.log
namespace research {

void Tick();  // game thread, once a frame of play
void OverlayLines(std::vector<std::string>& lines);  // what it shows under the FPS counter, in play

}  // namespace research
