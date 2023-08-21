RACK_DIR ?= ../..

include $(RACK_DIR)/arch.mk

FLAGS += -Idep/include
CFLAGS +=
CXXFLAGS +=
ifdef ARCH_LIN
	# No idea what this does. Recommended by https://ffmpeg.org/platform.html#Advanced-linking-configuration
	LDFLAGS += -Wl,-Bsymbolic
endif
ifdef ARCH_WIN
	# Windows DLLs need to link explicitly to other DLLs
	LDFLAGS += -lopengl32 -lbcrypt
endif

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)


ffmpeg := dep/lib/libavcodec.a
lame := dep/lib/libmp3lame.a
libopus := dep/lib/libopus.a
DEPS += $(ffmpeg)

# Order matters here
OBJECTS += dep/lib/libavformat.a
OBJECTS += dep/lib/libavcodec.a
OBJECTS += dep/lib/libavutil.a
OBJECTS += dep/lib/libswscale.a
OBJECTS += $(lame)
OBJECTS += $(libopus)

# ffmpeg
# ffmpeg's configure script doesn't respect --prefix for finding dependencies, so add the paths manually here
DEP_FLAGS += -I$(DEP_PATH)/include
DEP_LDFLAGS += -L$(DEP_PATH)/lib

FFMPEG_FORMATS += --enable-muxer=wav --enable-encoder=pcm_s16le --enable-encoder=pcm_s24le
FFMPEG_FORMATS += --enable-muxer=aiff --enable-encoder=pcm_s16be --enable-encoder=pcm_s24be
FFMPEG_FORMATS += --enable-libmp3lame --enable-muxer=mp3 --enable-encoder=libmp3lame
FFMPEG_FORMATS += --enable-libopus --enable-muxer=opus --enable-encoder=libopus
FFMPEG_FORMATS += --enable-muxer=flac --enable-encoder=flac
FFMPEG_FORMATS += --enable-muxer=ipod --enable-encoder=alac
FFMPEG_FORMATS += --enable-muxer=mpeg1system --enable-encoder=mpeg2video --enable-encoder=mp2
FFMPEG_FORMATS += --enable-muxer=avi --enable-encoder=huffyuv
FFMPEG_FORMATS += --enable-muxer=avi --enable-encoder=ffv1
ifdef ARCH_MAC
# 	FFMPEG_FORMATS += --enable-videotoolbox --enable-muxer=mp4 --enable-encoder=h264_videotoolbox --enable-encoder=mp2
endif

$(ffmpeg): $(lame) $(libopus)
	# Don't use $(CONFIGURE) because this is a handwritten configure script
	# ffmpeg bug: The pkgconfig dir is not set from pkgconfigdir at all. Set it with PKG_CONFIG_PATH instead.
	cd ffmpeg && PKG_CONFIG_PATH="$(DEP_PATH)/lib/pkgconfig" ./configure \
		--prefix="$(DEP_PATH)" \
		--extra-cflags="$(MAC_SDK_FLAGS)" \
		--extra-ldflags="$(MAC_SDK_FLAGS)" \
		--enable-pic \
		--enable-gpl \
		--disable-programs --disable-doc --disable-avdevice --disable-swresample --disable-postproc --disable-avfilter --disable-network --disable-iconv --disable-alsa --disable-autodetect --disable-everything \
		--enable-protocol=file \
		$(FFMPEG_FORMATS)
	cd ffmpeg && $(MAKE)
	cd ffmpeg && $(MAKE) install


# lame
$(lame):
	# Use -nc because Sourceforce mirrors don't understand -c if the file already exists
	$(WGET) -nc "https://sourceforge.net/projects/lame/files/lame/3.100/lame-3.100.tar.gz"
	$(SHA256) lame-3.100.tar.gz ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
	cd dep && $(UNTAR) ../lame-3.100.tar.gz
	# Remove nonexistent symbol from symbols list
	cd dep && $(SED) "s/lame_init_old\n//g" lame-3.100/include/libmp3lame.sym
	cd dep/lame-3.100 && $(CONFIGURE) --enable-nasm --disable-gtktest --disable-decoder --disable-frontend --enable-shared=no
	cd dep/lame-3.100 && $(MAKE) install


# libopus
$(libopus):
	$(WGET) -nc "https://archive.mozilla.org/pub/opus/opus-1.3.1.tar.gz"
	$(SHA256) opus-1.3.1.tar.gz 65b58e1e25b2a114157014736a3d9dfeaad8d41be1c8179866f144a2fb44ff9d
	cd dep && $(UNTAR) ../opus-1.3.1.tar.gz
	cd dep/opus-1.3.1 && $(CONFIGURE)
	cd dep/opus-1.3.1 && $(MAKE) install


include $(RACK_DIR)/plugin.mk
