RACK_DIR ?= ../..

FLAGS += -Idep/include
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
OBJECTS += dep/lib/libswscale.a
OBJECTS += $(lame)

DEP_LOCAL := dep
DEPS += $(ffmpeg)

$(ffmpeg): $(lame)
	cd dep/ffmpeg && $(CONFIGURE) --enable-pic --enable-gpl \
		--disable-programs --disable-doc --disable-avdevice --disable-postproc --disable-avfilter --disable-network --disable-iconv --disable-alsa --disable-autodetect --disable-everything \
		--enable-protocol=file \
		--enable-muxer=wav --enable-encoder=pcm_s16le --enable-encoder=pcm_s24le \
		--enable-muxer=aiff --enable-encoder=pcm_s16be --enable-encoder=pcm_s24be \
		--enable-libmp3lame --enable-muxer=mp3 --enable-encoder=libmp3lame \
		--enable-muxer=flac --enable-encoder=flac \
		--enable-muxer=ipod --enable-encoder=alac \
		--enable-muxer=mpeg1system --enable-encoder=mpeg2video --enable-encoder=mp2
	cd dep/ffmpeg && $(MAKE)
	cd dep/ffmpeg && $(MAKE) install

$(lame):
	# Use -nc because Sourceforce mirrors don't understand -c if the file already exists
	cd dep && $(WGET) -nc "https://sourceforge.net/projects/lame/files/lame/3.100/lame-3.100.tar.gz"
	cd dep && $(SHA256) lame-3.100.tar.gz ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
	cd dep && $(UNTAR) "lame-3.100.tar.gz"
	# Remove nonexistent symbol from symbols list
	cd dep && $(SED) "s/lame_init_old\n//g" lame-3.100/include/libmp3lame.sym
	cd dep/lame-3.100 && $(CONFIGURE) --enable-nasm --disable-gtktest --disable-decoder --disable-frontend --enable-shared=no
	cd dep/lame-3.100 && $(MAKE) install


include $(RACK_DIR)/plugin.mk
