#include "game.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>

namespace game {
namespace {

struct Cell {
    std::int8_t x;
    std::int8_t y;
};

struct Shape {
    std::uint8_t size;      // rotation box is size x size
    std::uint8_t count;
    Cell cells[4];          // rotation 0, y grows downwards
};

const Shape kShapes[kPieceTypes] = {
    {4, 4, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}},   // bar
    {2, 4, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}},   // square
    {3, 4, {{1, 0}, {0, 1}, {1, 1}, {2, 1}}},   // tee
    {3, 4, {{1, 0}, {2, 0}, {0, 1}, {1, 1}}},   // step right
    {3, 4, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}},   // step left
    {3, 4, {{0, 0}, {0, 1}, {1, 1}, {2, 1}}},   // hook left
    {3, 4, {{2, 0}, {0, 1}, {1, 1}, {2, 1}}},   // hook right
    {1, 1, {{0, 0}, {0, 0}, {0, 0}, {0, 0}}},   // fuse
};

constexpr std::uint32_t kLockDelayMs = 450;
constexpr std::uint32_t kClearMs = 260;
constexpr int kMaxLockResets = 12;
constexpr int kMaxLevel = 15;
// Points per clear in ohms, E6 resistor values: 1 row 100, 2 rows 330, 3 rows 680, 4 rows 1.5k; x level.
const std::uint16_t kRowPoints[5] = {0, 100, 330, 680, 1500};
constexpr std::uint16_t kFusePoints = 47;

std::uint8_t board[kRows][kCols];
State current = State::Ready;
int pieceType = 0;
int pieceRot = 0;
int pieceX = 0;
int pieceY = 0;
int upcoming = 0;
int lastType = -1;
int sinceFuse = 0;
bool landed = false;
std::uint32_t landedMs = 0;
int lockResets = 0;
std::uint32_t nextFallMs = 0;
std::uint32_t clearMask = 0;           // bit y = row y is being cleared
std::uint32_t clearDoneMs = 0;
int clearFullRows = 0;
std::uint32_t points = 0;
std::uint32_t bestPoints = 0;
int rowsCleared = 0;
int currentLevel = 1;
bool beatBest = false;
bool loaded = false;

int cellsOf(int type, int rot, Cell out[4]) {
    const Shape& s = kShapes[type];
    for (int i = 0; i < s.count; ++i) {
        int x = s.cells[i].x;
        int y = s.cells[i].y;
        for (int k = 0; k < (rot & 3); ++k) {     // clockwise: (x, y) -> (size - 1 - y, x)
            const int nx = s.size - 1 - y;
            y = x;
            x = nx;
        }
        out[i] = {static_cast<std::int8_t>(x), static_cast<std::int8_t>(y)};
    }
    return s.count;
}

bool fits(int type, int rot, int px, int py) {
    Cell c[4];
    const int n = cellsOf(type, rot, c);
    for (int i = 0; i < n; ++i) {
        const int x = px + c[i].x;
        const int y = py + c[i].y;
        if (x < 0 || x >= kCols || y >= kRows) {
            return false;
        }
        if (y >= 0 && board[y][x] != 0) {
            return false;
        }
    }
    return true;
}

std::uint32_t fallInterval() {
    float ms = 800.0f;
    for (int i = 1; i < currentLevel; ++i) {
        ms *= 0.85f;
    }
    return ms < 70.0f ? 70U : static_cast<std::uint32_t>(ms);
}

int randomType() {
    if (sinceFuse >= 6 && esp_random() % 14 == 0) {
        sinceFuse = 0;
        return kFuse;
    }
    ++sinceFuse;
    int t = static_cast<int>(esp_random() % 7);
    if (t == lastType) {
        t = static_cast<int>(esp_random() % 7);   // one re-roll: straight repeats get rarer
    }
    lastType = t;
    return t;
}

void saveBest() {
    Preferences prefs;
    prefs.begin("game", false);
    prefs.putUInt("best", bestPoints);
    prefs.end();
}

void finish() {
    current = State::Over;
    beatBest = points > bestPoints;
    if (beatBest) {
        bestPoints = points;
        saveBest();
    }
}

void spawn(std::uint32_t now) {
    pieceType = upcoming;
    upcoming = randomType();
    pieceRot = 0;
    pieceX = (kCols - kShapes[pieceType].size) / 2;
    pieceY = pieceType == 0 ? -1 : 0;          // the bar's cells sit in row 1 of its box
    landed = false;
    lockResets = 0;
    nextFallMs = now + fallInterval();
    if (!fits(pieceType, pieceRot, pieceX, pieceY)) {
        finish();
    }
}

void collapse(std::uint32_t now) {
    int write = kRows - 1;
    for (int y = kRows - 1; y >= 0; --y) {
        if (clearMask & (1UL << y)) {
            continue;
        }
        if (write != y) {
            memcpy(board[write], board[y], kCols);
        }
        --write;
    }
    for (; write >= 0; --write) {
        memset(board[write], 0, kCols);
    }
    rowsCleared += clearFullRows;
    currentLevel = std::min(kMaxLevel, 1 + rowsCleared / 10);
    clearMask = 0;
    clearFullRows = 0;
    current = State::Playing;
    spawn(now);
}

void lockPiece(std::uint32_t now) {
    Cell c[4];
    const int n = cellsOf(pieceType, pieceRot, c);
    bool above = false;
    int fuseRow = -1;
    for (int i = 0; i < n; ++i) {
        const int x = pieceX + c[i].x;
        const int y = pieceY + c[i].y;
        if (y < 0) {
            above = true;
            continue;
        }
        board[y][x] = static_cast<std::uint8_t>(pieceType + 1);
        if (pieceType == kFuse) {
            fuseRow = y;
        }
    }
    if (above) {                               // stacked past the top
        finish();
        return;
    }
    clearMask = 0;
    clearFullRows = 0;
    for (int y = 0; y < kRows; ++y) {
        bool full = true;
        for (int x = 0; x < kCols && full; ++x) {
            full = board[y][x] != 0;
        }
        if (full) {
            clearMask |= 1UL << y;
            ++clearFullRows;
        }
    }
    points += static_cast<std::uint32_t>(kRowPoints[std::min(clearFullRows, 4)]) * currentLevel;
    if (fuseRow >= 0 && !(clearMask & (1UL << fuseRow))) {
        clearMask |= 1UL << fuseRow;           // the fuse burns its row out, full or not
        points += static_cast<std::uint32_t>(kFusePoints) * currentLevel;
    }
    if (clearMask != 0) {
        current = State::Clearing;
        clearDoneMs = now + kClearMs;
    } else {
        spawn(now);
    }
}

void touchedWhileLanded(std::uint32_t now) {
    if (landed && lockResets < kMaxLockResets) {
        ++lockResets;
        landedMs = now;
    }
}

}  // namespace

void begin() {
    if (loaded) {
        return;
    }
    loaded = true;
    Preferences prefs;
    if (prefs.begin("game", true)) {
        bestPoints = prefs.getUInt("best", 0);
        prefs.end();
    }
}

void newGame(std::uint32_t now) {
    begin();
    memset(board, 0, sizeof(board));
    points = 0;
    rowsCleared = 0;
    currentLevel = 1;
    beatBest = false;
    clearMask = 0;
    clearFullRows = 0;
    sinceFuse = 0;
    lastType = -1;
    upcoming = randomType();
    current = State::Playing;
    spawn(now);
}

void quit() {
    memset(board, 0, sizeof(board));
    points = 0;
    rowsCleared = 0;
    currentLevel = 1;
    beatBest = false;
    clearMask = 0;
    clearFullRows = 0;
    current = State::Ready;
}

void pause(std::uint32_t now) {
    if (current == State::Clearing) {
        collapse(now);
    }
    if (current == State::Playing) {
        current = State::Paused;
    }
}

void resume(std::uint32_t now) {
    if (current != State::Paused) {
        return;
    }
    current = State::Playing;
    nextFallMs = now + fallInterval();
    landedMs = now;
}

State state() {
    return current;
}

bool tick(std::uint32_t now) {
    if (current == State::Clearing) {
        if (static_cast<std::int32_t>(now - clearDoneMs) >= 0) {
            collapse(now);
            return true;
        }
        return false;
    }
    if (current != State::Playing) {
        return false;
    }
    if (landed) {
        if (fits(pieceType, pieceRot, pieceX, pieceY + 1)) {
            landed = false;                    // slid off the edge it was resting on
            nextFallMs = now + fallInterval();
        } else if (now - landedMs >= kLockDelayMs) {
            lockPiece(now);
            return true;
        }
        return false;
    }
    if (static_cast<std::int32_t>(now - nextFallMs) >= 0) {
        nextFallMs = now + fallInterval();
        if (fits(pieceType, pieceRot, pieceX, pieceY + 1)) {
            ++pieceY;
            return true;
        }
        landed = true;
        landedMs = now;
    }
    return false;
}

bool move(int dx, std::uint32_t now) {
    if (current != State::Playing || !fits(pieceType, pieceRot, pieceX + dx, pieceY)) {
        return false;
    }
    pieceX += dx;
    touchedWhileLanded(now);
    return true;
}

bool rotate(std::uint32_t now) {
    if (current != State::Playing || pieceType == 1 || pieceType == kFuse) {
        return false;
    }
    static const std::int8_t kKicks[][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {-2, 0}, {2, 0}};
    const int rot = (pieceRot + 1) & 3;
    for (const auto& k : kKicks) {
        if (fits(pieceType, rot, pieceX + k[0], pieceY + k[1])) {
            pieceRot = rot;
            pieceX += k[0];
            pieceY += k[1];
            touchedWhileLanded(now);
            return true;
        }
    }
    return false;
}

bool drop(std::uint32_t now) {
    if (current != State::Playing) {
        return false;
    }
    int rows = 0;
    while (fits(pieceType, pieceRot, pieceX, pieceY + 1)) {
        ++pieceY;
        ++rows;
    }
    points += static_cast<std::uint32_t>(rows);     // 1 ohm per row dropped
    lockPiece(now);
    return true;
}

void view(std::uint8_t out[kRows][kCols]) {
    memcpy(out, board, sizeof(board));
    if (current == State::Clearing) {
        for (int y = 0; y < kRows; ++y) {
            if (clearMask & (1UL << y)) {
                for (int x = 0; x < kCols; ++x) {
                    out[y][x] |= kFlash;
                }
            }
        }
        return;
    }
    if (current != State::Playing && current != State::Paused) {
        return;
    }
    Cell c[4];
    const int n = cellsOf(pieceType, pieceRot, c);
    int ghostY = pieceY;
    while (fits(pieceType, pieceRot, pieceX, ghostY + 1)) {
        ++ghostY;
    }
    const std::uint8_t value = static_cast<std::uint8_t>(pieceType + 1);
    if (ghostY != pieceY) {
        for (int i = 0; i < n; ++i) {
            const int y = ghostY + c[i].y;
            if (y >= 0) {
                out[y][pieceX + c[i].x] = static_cast<std::uint8_t>(kGhost | value);
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        const int y = pieceY + c[i].y;
        if (y >= 0) {
            out[y][pieceX + c[i].x] = value;
        }
    }
}

int nextType() {
    return upcoming;
}

int shape(int type, int rotation, std::int8_t xs[4], std::int8_t ys[4]) {
    if (type < 0 || type >= kPieceTypes) {
        return 0;
    }
    Cell c[4];
    const int n = cellsOf(type, rotation, c);
    for (int i = 0; i < n; ++i) {
        xs[i] = c[i].x;
        ys[i] = c[i].y;
    }
    return n;
}

std::uint32_t score() {
    return points;
}

std::uint32_t best() {
    return std::max(bestPoints, points);
}

int lines() {
    return rowsCleared;
}

int level() {
    return currentLevel;
}

bool newBest() {
    return beatBest;
}

}  // namespace game
