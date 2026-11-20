#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct X86Context;

// research.cpp: the temporary research logger, logs\research.log
namespace research {

void Tick();  // game thread, once a frame of play
void BetweenFrames(X86Context& ctx);  // game thread, at the frame's end (D3DDevice_Swap): F5's weapon swap
void OverlayLines(std::vector<std::string>& lines);  // what it shows under the FPS counter, in play
// a file the game opened (game_files.cpp), NtCreateFile's status: logged, weapon files in black.log too
void FileOpened(const std::string& name, uint32_t root, uint32_t status);

}  // namespace research
