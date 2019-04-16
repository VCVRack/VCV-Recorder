RACK_DIR ?= ../..

FLAGS +=
CFLAGS +=
CXXFLAGS +=
# No idea what this does. Recommended by https://ffmpeg.org/platform.html#Advanced-linking-configuration
LDFLAGS += -Wl,-Bsymbolic

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)


ffmpeg := dep/lib/libavcodec.a
lame := dep/lib/libmp3lame.a

# Order matters here
OBJECTS += dep/lib/libavformat.a
OBJECTS += dep/lib/libavcodec.a
OBJECTS += dep/lib/libavutil.a
OBJECTS += dep/lib/libswresample.a
OBJECTS += $(lame)

DEP_LOCAL := dep
DEPS += $(ffmpeg)

$(ffmpeg): $(lame)
	cd dep/ffmpeg && $(CONFIGURE) --enable-pic --enable-gpl \
		--disable-programs --disable-doc --disable-avdevice --disable-swscale --disable-postproc --disable-avfilter --disable-network --disable-iconv --disable-alsa --disable-autodetect --disable-everything \
		--enable-protocol=file \
		--enable-muxer=wav --enable-encoder=pcm_s16le --enable-encoder=pcm_s24le \
		--enable-muxer=aiff --enable-encoder=pcm_s16be --enable-encoder=pcm_s24be \
		--enable-libmp3lame --enable-muxer=mp3 --enable-encoder=libmp3lame \
		--enable-muxer=flac --enable-encoder=flac \
		--enable-muxer=mpeg1system --enable-encoder=mpeg2video --enable-encoder=mp2
	cd dep/ffmpeg && $(MAKE)
	cd dep/ffmpeg && $(MAKE) install

$(lame):
	cd dep && $(WGET) "https://sourceforge.net/projects/lame/files/lame/3.100/lame-3.100.tar.gz"
	cd dep && $(UNTAR) "lame-3.100.tar.gz"
	cd dep/lame-3.100 && $(CONFIGURE)
	cd dep/lame-3.100 && $(MAKE) install


include $(RACK_DIR)/plugin.mk
