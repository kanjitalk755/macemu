# macemu WebRTC Streaming Server Makefile
# Generated from Makefile.in by configure

CXX = g++ -std=c++17
CXXFLAGS = -g -O2 -Wall -Wextra -g3 -O0 -fPIC -fno-omit-frame-pointer -rdynamic
INSTALL = /usr/bin/install -c

# Directories
SERVER_DIR = server
CLIENT_DIR = client
BUILD_DIR = build

# libdatachannel paths
LIBDATACHANNEL_DIR = libdatachannel
LIBDATACHANNEL_BUILD = libdatachannel/build
LIBDATACHANNEL_LIB = libdatachannel/build/libdatachannel.a
HAVE_LIBDATACHANNEL = yes
CMAKE = 

# Compiler and linker flags from configure
SERVER_CFLAGS = -I../BasiliskII/src/IPC -Ilibdatachannel/include -Ilibdatachannel/deps/json/include -Iserver  -I/usr/include/svt-av1 -DEB_DLL   -msse4.1 -mpclmul -I/usr/include/svt-av1 -DEB_DLL
SERVER_LIBS = libdatachannel/build/libdatachannel.a libdatachannel/build/deps/libjuice/libjuice.a libdatachannel/build/deps/libsrtp/libsrtp2.a libdatachannel/build/deps/usrsctp/usrsctplib/libusrsctp.a -lopenh264  -lssl -lcrypto  -lyuv -lpthread -lrt -lopus  -lSvtAv1Enc  -lvpx -lm  -lopus -lSvtAv1Enc -lwebp
FPNG_CFLAGS = -msse4.1 -mpclmul

# Source files
SERVER_SRCS = $(SERVER_DIR)/server.cpp \
              $(SERVER_DIR)/config/server_config.cpp \
              $(SERVER_DIR)/config/config_manager.cpp \
              $(SERVER_DIR)/utils/keyboard_map.cpp \
              $(SERVER_DIR)/utils/json_utils.cpp \
              $(SERVER_DIR)/ipc/ipc_connection.cpp \
              $(SERVER_DIR)/storage/file_scanner.cpp \
              $(SERVER_DIR)/storage/prefs_manager.cpp \
              $(SERVER_DIR)/http/http_server.cpp \
              $(SERVER_DIR)/http/api_handlers.cpp \
              $(SERVER_DIR)/http/static_files.cpp \
              $(SERVER_DIR)/h264_encoder.cpp \
              $(SERVER_DIR)/av1_encoder.cpp \
              $(SERVER_DIR)/opus_encoder.cpp \
              $(SERVER_DIR)/png_encoder.cpp \
              $(SERVER_DIR)/webp_encoder.cpp \
              $(SERVER_DIR)/fpng.cpp

# Object files
SERVER_OBJS = $(BUILD_DIR)/server.o \
              $(BUILD_DIR)/server_config.o \
              $(BUILD_DIR)/config_manager.o \
              $(BUILD_DIR)/keyboard_map.o \
              $(BUILD_DIR)/json_utils.o \
              $(BUILD_DIR)/ipc_connection.o \
              $(BUILD_DIR)/file_scanner.o \
              $(BUILD_DIR)/prefs_manager.o \
              $(BUILD_DIR)/http_server.o \
              $(BUILD_DIR)/api_handlers.o \
              $(BUILD_DIR)/static_files.o \
              $(BUILD_DIR)/h264_encoder.o \
              $(BUILD_DIR)/av1_encoder.o \
              $(BUILD_DIR)/vp9_encoder.o \
              $(BUILD_DIR)/opus_encoder.o \
              $(BUILD_DIR)/png_encoder.o \
              $(BUILD_DIR)/webp_encoder.o \
              $(BUILD_DIR)/fpng.o

# Target
SERVER = $(BUILD_DIR)/macemu-webrtc

# Default target
all: libdatachannel dirs $(SERVER)
	@echo ""
	@echo "Build complete!"
	@echo "Server: $(SERVER)"
	@echo ""
	@echo "Usage:"
	@echo "  0. Populate storage/roms and storage/images"
	@echo "  1. Build Basilisk II with: ./configure --enable-ipc-video --enable-ipc-audio && make"
	@echo "  2. Start the server: ./$(SERVER)"	
	@echo "  3. Open http://<yourip>:8000 in your browser"

# Create build directory
dirs:
	@mkdir -p $(BUILD_DIR)

# libdatachannel must be built before running configure
# This is just a placeholder target
libdatachannel:
	@echo "libdatachannel is already built (checked by configure)"

$(LIBDATACHANNEL_LIB):
	@echo "ERROR: libdatachannel not found."
	@echo "This should have been caught by configure. Please re-run ./configure"
	@exit 1

# Compile server
$(BUILD_DIR)/server.o: $(SERVER_DIR)/server.cpp \
                       ../BasiliskII/src/IPC/ipc_protocol.h \
                       $(SERVER_DIR)/codec.h \
                       $(SERVER_DIR)/h264_encoder.h \
                       $(LIBDATACHANNEL_LIB)
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

# Compile H.264 encoder
$(BUILD_DIR)/h264_encoder.o: $(SERVER_DIR)/h264_encoder.cpp \
                              $(SERVER_DIR)/h264_encoder.h \
                              $(SERVER_DIR)/codec.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

# Compile AV1 encoder
$(BUILD_DIR)/av1_encoder.o: $(SERVER_DIR)/av1_encoder.cpp \
                             $(SERVER_DIR)/av1_encoder.h \
                             $(SERVER_DIR)/codec.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

# Compile VP9 encoder
$(BUILD_DIR)/vp9_encoder.o: $(SERVER_DIR)/vp9_encoder.cpp \
                             $(SERVER_DIR)/vp9_encoder.h \
                             $(SERVER_DIR)/codec.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

# Compile Opus encoder
$(BUILD_DIR)/opus_encoder.o: $(SERVER_DIR)/opus_encoder.cpp \
                              $(SERVER_DIR)/opus_encoder.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

# Compile PNG encoder
$(BUILD_DIR)/png_encoder.o: $(SERVER_DIR)/png_encoder.cpp \
                             $(SERVER_DIR)/png_encoder.h \
                             $(SERVER_DIR)/codec.h \
                             $(SERVER_DIR)/fpng.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/webp_encoder.o: $(SERVER_DIR)/webp_encoder.cpp \
                              $(SERVER_DIR)/webp_encoder.h \
                              $(SERVER_DIR)/codec.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

# Compile fpng (fast PNG library)
$(BUILD_DIR)/fpng.o: $(SERVER_DIR)/fpng.cpp $(SERVER_DIR)/fpng.h
	$(CXX) $(CXXFLAGS) $(FPNG_CFLAGS) -c $< -o $@

# Compile server modules
$(BUILD_DIR)/server_config.o: $(SERVER_DIR)/config/server_config.cpp $(SERVER_DIR)/config/server_config.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/keyboard_map.o: $(SERVER_DIR)/utils/keyboard_map.cpp $(SERVER_DIR)/utils/keyboard_map.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/json_utils.o: $(SERVER_DIR)/utils/json_utils.cpp $(SERVER_DIR)/utils/json_utils.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ipc_connection.o: $(SERVER_DIR)/ipc/ipc_connection.cpp $(SERVER_DIR)/ipc/ipc_connection.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/file_scanner.o: $(SERVER_DIR)/storage/file_scanner.cpp $(SERVER_DIR)/storage/file_scanner.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/prefs_manager.o: $(SERVER_DIR)/storage/prefs_manager.cpp $(SERVER_DIR)/storage/prefs_manager.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/http_server.o: $(SERVER_DIR)/http/http_server.cpp $(SERVER_DIR)/http/http_server.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/api_handlers.o: $(SERVER_DIR)/http/api_handlers.cpp $(SERVER_DIR)/http/api_handlers.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(BUILD_DIR)/static_files.o: $(SERVER_DIR)/http/static_files.cpp $(SERVER_DIR)/http/static_files.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@

# Link server
$(SERVER): $(SERVER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(SERVER_OBJS) $(SERVER_LIBS)

# Run server
run: $(SERVER)
	./$(SERVER)

# Clean build files
clean:
	rm -rf $(BUILD_DIR)
	rm -f config.h config.log config.status

# Clean everything including libdatachannel and configure artifacts
distclean: clean
	rm -rf $(LIBDATACHANNEL_BUILD)
	rm -f Makefile

# Install server
install: $(SERVER)
	$(INSTALL) -d $(DESTDIR)/usr/local/bin
	$(INSTALL) -m 755 $(SERVER) $(DESTDIR)/usr/local/bin/

# Help
help:
	@echo "macemu WebRTC Streaming Server"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build the server (default)"
	@echo "  libdatachannel - Build libdatachannel library only"
	@echo "  run          - Build and run the server"
	@echo "  clean        - Remove build files"
	@echo "  distclean    - Remove all build files including libdatachannel"
	@echo "  install      - Install server to /usr/local/bin"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Quick Start:"
	@echo "  1. Configure: ./configure"
	@echo "  2. Build: make"
	@echo "  3. Run server: ./build/macemu-webrtc"
	@echo "     Server prints: 'Control socket: /tmp/macemu-<PID>.sock'"
	@echo "  4. Build Basilisk II:"
	@echo "     cd ../BasiliskII/src/Unix"
	@echo "     ./configure --enable-ipc-video"
	@echo "     make"
	@echo "  5. Run emulator with socket path:"
	@echo "     MACEMU_CONTROL_SOCK=/tmp/macemu-<PID>.sock ./BasiliskII"
	@echo "  6. Open http://localhost:8000 in browser"

.PHONY: all dirs run clean distclean help libdatachannel install

# Config manager
$(BUILD_DIR)/config_manager.o: $(SERVER_DIR)/config/config_manager.cpp \
                                $(SERVER_DIR)/config/config_manager.h \
                                $(SERVER_DIR)/utils/json_utils.h
	$(CXX) $(CXXFLAGS) $(SERVER_CFLAGS) -c $< -o $@
