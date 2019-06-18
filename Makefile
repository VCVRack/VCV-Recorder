RACK_DIR ?= ../..

include $(RACK_DIR)/arch.mk

FLAGS += -Idep/include
CFLAGS +=
CXXFLAGS +=
ifdef ARCH_LIN
	# No idea what this does. Recommended by https://ffmpeg.org/platform.html#Advanced-linking-configuration
	LDFLAGS += -Wl,-Bsymbolic
endif

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)


ffmpeg := dep/lib/libavcodec.a
lame := dep/lib/libmp3lame.a

# Order matters here
OBJECTS += dep/lib/libavformat.a
OBJECTS += dep/lib/libavcodec.a
OBJECTS += dep/lib/libavutil.a
OBJECTS += dep/lib/libswscale.a
OBJECTS += $(lame)

DEP_LOCAL := dep
DEPS += $(ffmpeg)


FFMPEG_FORMATS += --enable-muxer=wav --enable-encoder=pcm_s16le --enable-encoder=pcm_s24le
FFMPEG_FORMATS += --enable-muxer=aiff --enable-encoder=pcm_s16be --enable-encoder=pcm_s24be
FFMPEG_FORMATS += --enable-libmp3lame --enable-muxer=mp3 --enable-encoder=libmp3lame
FFMPEG_FORMATS += --enable-muxer=flac --enable-encoder=flac
FFMPEG_FORMATS += --enable-muxer=ipod --enable-encoder=alac
FFMPEG_FORMATS += --enable-muxer=mpeg1system --enable-encoder=mpeg2video --enable-encoder=mp2
# FFMPEG_FORMATS += --enable-muxer=avi --enable-encoder=huffyuv
FFMPEG_FORMATS += --enable-muxer=avi --enable-encoder=ffv1
ifdef ARCH_MAC
# 	FFMPEG_FORMATS += --enable-videotoolbox --enable-muxer=mp4 --enable-encoder=h264_videotoolbox --enable-encoder=mp2
endif

$(ffmpeg): $(lame)
	# Don't use $(CONFIGURE) because this is a handwritten configure script
	cd dep/ffmpeg && ./configure --prefix="$(DEP_PATH)" --enable-pic --enable-gpl \
		--disable-programs --disable-doc --disable-avdevice --disable-swresample --disable-postproc --disable-avfilter --disable-network --disable-iconv --disable-alsa --disable-autodetect --disable-everything \
		--enable-protocol=file \
		$(FFMPEG_FORMATS)
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
