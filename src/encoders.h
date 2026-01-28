// encoders.h
#pragma once

// AAC
bool startEncoderWithConfig();
void stopEncoder();

// MP3
bool startMp3EncoderWithConfig();
bool stopMp3EncoderWithConfig();

// HLS
bool startHlsWithConfig();
void stopHls();
