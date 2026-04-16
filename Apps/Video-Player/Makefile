# Video Player V3 Makefile

CXX = g++
CXXFLAGS = -Wall -O3 -std=c++17
TARGET = video_player_v3

# FFmpeg: video decode + swresample (for resampling decoded audio to 48kHz F32)
FFMPEG_LIBS = -lavformat -lavcodec -lavutil -lswscale -lswresample
# -lrt for shm_open (audio driver ring buffer)
LIBS = -lrt -lpthread $(FFMPEG_LIBS)

SOURCES = video_player_v3.cpp
OBJECTS = $(SOURCES:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@echo "Cleaning video player..."
	@rm -f $(OBJECTS) $(TARGET)

install: $(TARGET)
	@echo "Installing $(TARGET) to /usr/local/bin/"
	@sudo cp $(TARGET) /usr/local/bin/
	@sudo chmod +x /usr/local/bin/$(TARGET)

uninstall:
	@echo "Removing $(TARGET) from /usr/local/bin/"
	@sudo rm -f /usr/local/bin/$(TARGET)

.PHONY: all clean install uninstall
