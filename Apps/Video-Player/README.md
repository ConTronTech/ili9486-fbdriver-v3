# Video Player V3

FFmpeg-based video player for the ILI9486 framebuffer driver using double buffering.

## Features

- MP4, AVI, MKV, MOV support (via FFmpeg)
- Hardware-accelerated color conversion (YUV → RGB565)
- Double buffering for smooth playback
- Auto-scaling to 320x480 display
- Target: 15-30 FPS playback

## Dependencies

Install FFmpeg development libraries:
```bash
sudo apt-get install libavformat-dev libavcodec-dev libswscale-dev libavutil-dev
```

## Building

```bash
cd Apps/Video-Player
make
```

## Usage

```bash
# Basic playback
./video_player_v3 video.mp4

# With specific orientation
./video_player_v3 video.mp4 0      # Landscape (480x320)
./video_player_v3 video.mp4 90     # Portrait (320x480)
```

## Controls

- **Ctrl+C**: Stop playback and exit
- **Q key**: Quit (if implemented)

## Performance

- **320x480 @ 30 FPS**: ~10 MB/s bandwidth
- **Color conversion**: YUV420p → RGB565 (hardware accelerated)
- **Buffer swap**: <1ms (atomic operation)

## Technical Details

### Video Pipeline

```
FFmpeg Decode → YUV Frame → swscale (RGB565) → Back Buffer → Swap → Display
```

### Double Buffering Flow

1. Decode frame to YUV format
2. Convert/scale to RGB565 in **back buffer**
3. Swap buffers (instant, atomic)
4. Repeat for next frame

### Supported Formats

- **Video codecs**: H.264, H.265, VP9, MPEG-4
- **Containers**: MP4, AVI, MKV, MOV, WebM
- **Color spaces**: YUV420p, YUV422p, RGB24

## Limitations

- No audio playback (video only)
- No seeking/pause controls (basic playback)
- Limited to ~30 FPS due to SPI bandwidth

## Future Enhancements

- [ ] Audio playback via ALSA
- [ ] Pause/Resume controls
- [ ] Seeking support
- [ ] FPS display
- [ ] Hardware video decode (V4L2/MMAL on Pi)
