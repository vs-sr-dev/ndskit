// ndskit runtime — the host: the window, the compositor, pictures, the sound out.
#pragma once
#include "nds.h"
#include <string>

// The window (visible: real time, input from the keyboard and the mouse) or
// a hidden one (headless pictures: the 3D still needs OpenGL).
bool display_open(bool visible);
// After a VBlank: the frame just finished drawn (the 2D lines and the 3D
// picture under them) and presented, the host's input read, the pace kept.
// False once the window is closed.
bool display_frame();
// Both screens of the frame just finished, upper above lower, as a PNG at the DS's size.
void display_shot(const std::string& path);
void write_png(const std::string& path, int w, int h, const uint8_t* rgb);   // RGB8, top row first

// The sound to the host (audio.cpp): the audio device (a window's run) and a WAV file.
bool audio_open(bool device, const std::string& wav);
void audio_frame();                     // after a VBlank: the frame's samples out
void audio_close();
