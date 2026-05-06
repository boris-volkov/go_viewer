# Go Viewer

A minimal Go (Baduk) game viewer using SDL2, similar to the chess viewer but for Go games.

## Features

- **19x19 Go board** with traditional grid lines and star points (hoshi)
- **SGF file support** - loads games from Smart Game Format files
- **Animated stone placement** with fade-in effects
- **Game playback controls**:
  - Space: Pause/Resume
  - Left/Right arrows: Step through moves when paused
  - Up/Down arrows: Adjust playback speed
- **Analysis mode** (A key): Manually place stones on the board
- **Guess mode** (G key): Try to predict the next move
- **Catalog browser** (C key): Browse and select SGF files
- **Player names** and game results display
- **Fullscreen display** optimized for viewing

## Building

### Prerequisites

- SDL2 development libraries
- CMake (optional, for the provided CMakeLists.txt)

### Windows (MinGW)

```bash
gcc go_viewer.c -o go_viewer.exe -lmingw32 -lSDL2main -lSDL2
```

### Linux/Mac

```bash
gcc go_viewer.c -o go_viewer -lSDL2 -lm
```

### CMake (cross-platform)

```bash
cd go_viewer
mkdir build
cd build
cmake ..
make
```

## Usage

1. Place SGF files in the `go_viewer/games/` directory
2. Run the executable: `./go_viewer` (Linux/Mac) or `go_viewer.exe` (Windows)
3. The viewer will automatically load and play a random game
4. Use the controls to navigate through the game

## Controls

### General
- **Q**: Quit the application
- **N**: Load next random game
- **P**: Load previous game (if available)
- **R**: Restart current game
- **C**: Open catalog to browse games
- **ESC**: Toggle help overlay
- **F**: (Reserved for future features)

### Playback
- **Space**: Pause/Resume playback
- **Left/Right** (when paused): Step backward/forward through moves
- **Up/Down**: Increase/Decrease playback speed

### Special Modes
- **A**: Toggle analysis mode (place stones manually)
- **G**: Toggle guess mode (predict next moves)

### Catalog Browser
- **Up/Down**: Navigate through files
- **Enter**: Select and load a game
- **ESC**: Close catalog

## File Format

The viewer reads Go games in SGF (Smart Game Format). Example:

```
(;EV[2nd Tengen]RO[1]PB[Tainaka Shin]BR[9p]PW[Yoshida Yoichi]WR[8p]KM[5.5]RE[W+R]DT[1976-02-19]PC[Kansai Ki-in]
;B[qd];W[dd];B[pp];W[od];B[dp];W[cn];B[fp];W[eo];B[cp];W[dj]
;B[mc];W[ne];B[me];W[mf];B[md];W[pc];B[lf];W[ng];B[qg];W[lg]
;B[kf];W[gc];B[kg];W[lh];B[of];W[nf];B[kh];W[li];B[cd];W[ce]
;B[be];W[de];B[bd];W[bf];B[dc];W[ec];B[db];W[cg];B[eb];W[fb]
;B[ic];W[bb];B[ed];W[fc];B[cb];W[fe];B[qj];W[nq];B[em];W[en]
;B[gq];W[go];B[cm];W[dm];B[dl];W[dn];B[ej];W[bm];B[di];W[cl]
;B[ef];W[ee];B[jj];W[qc];B[iq];W[qn];B[np];W[mp];B[oq];W[no]
;B[op];W[jq];B[mq];W[ir];B[hq];W[lp];B[qo];W[pn];B[re];W[ei]
;B[fj];W[eh];B[oo];W[on];B[nn];W[mo];B[jp];W[kp];B[jr];W[kr]
;B[kq];W[lq];B[nr];W[jq];B[nm];W[pk];B[kq];W[lm];B[jq];W[mr]
;B[qk];W[ql];B[pj];W[ok];B[ro];W[hj];B[nk];W[oj];B[ll];W[ml]
;B[hk];W[rl];B[rc];W[jc];B[jd];W[kl];B[oi];W[pi])
```

## Stone Rendering

Stones are rendered as pixel-perfect circles directly in the viewer using the same resolution as the font. No external image files are required. The stones have a subtle fade-in animation when placed and can be enhanced with shading effects in future updates.

## Game Data

Place your SGF files in the `games/` directory. The viewer will automatically discover and load them. You can organize games in subdirectories - the catalog browser will show them all.

## Similarities to Chess Viewer

This Go viewer is designed with the same interface philosophy as the chess viewer:
- Minimal, distraction-free design
- Smooth animations and transitions
- Comprehensive keyboard controls
- Analysis and guess modes for learning
- Catalog system for game selection
- Cross-platform compatibility

Enjoy exploring Go games with the Go Viewer!