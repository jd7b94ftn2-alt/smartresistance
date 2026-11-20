#pragma once

#include <cstdint>

// 色环方块 (Band Blocks): a falling-block puzzle on the board screen. This file is the pure game
// state; ui.cpp draws it (pieces in resistor colour-code colours, score in ohms) and maps the knob
// and the touch screen onto it.
//
// Rules: a full row is cleared. Now and then a one-cell silver fuse falls; where it lands it burns
// out its whole row, full or not. 10 cleared rows = next level (faster fall), up to level 15.
namespace game {

constexpr int kCols = 10;
constexpr int kRows = 19;
constexpr int kPieceTypes = 8;        // 7 four-cell pieces + the fuse
constexpr int kFuse = 7;

enum class State : std::uint8_t { Ready, Playing, Paused, Clearing, Over };

// view() cells: low nibble 0 = empty, 1..8 = piece type + 1; flags below.
constexpr std::uint8_t kGhost = 0x40;   // where the falling piece would land
constexpr std::uint8_t kFlash = 0x80;   // row being cleared

void begin();                           // loads the best score from NVS (idempotent)
void newGame(std::uint32_t now);
void pause(std::uint32_t now);
void resume(std::uint32_t now);
State state();

bool tick(std::uint32_t now);           // gravity, lock delay, clear animation; true = picture changed
bool move(int dx, std::uint32_t now);
bool rotate(std::uint32_t now);         // clockwise, with small wall / floor kicks
bool drop(std::uint32_t now);           // straight to the bottom and lock

void view(std::uint8_t out[kRows][kCols]);
int nextType();
int shape(int type, int rotation, std::int8_t xs[4], std::int8_t ys[4]);   // cell count
std::uint32_t score();                  // in ohms
std::uint32_t best();
int lines();
int level();
bool newBest();                         // the game that just ended beat the best score

}  // namespace game
