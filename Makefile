RACK_DIR ?= ../..

FLAGS +=
CFLAGS +=
CXXFLAGS +=

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)


x264 := dep/lib/libx264.a
ffmpeg := dep/lib/libavcodec.a

OBJECTS += $(x264)
OBJECTS += dep/lib/libavutil.a
OBJECTS += dep/lib/libavcodec.a
OBJECTS += dep/lib/libavformat.a

DEP_LOCAL := dep
DEPS += $(ffmpeg)

$(x264):
	cd dep/x264 && $(CONFIGURE) --disable-cli --enable-static --enable-pic
	cd dep/x264 && $(MAKE)
	cd dep/x264 && $(MAKE) install

$(ffmpeg): $(x264)
	cd dep/ffmpeg && $(CONFIGURE) --enable-pic --enable-shared --extra-cflags="-fPIC" \
		--enable-gpl --disable-programs --disable-doc --disable-avdevice --disable-swresample --disable-swscale --disable-postproc --disable-avfilter --disable-network --disable-autodetect --disable-everything \
		--enable-libx264 --enable-encoder=libx264rgb --enable-encoder=aac
	cd dep/ffmpeg && $(MAKE)
	cd dep/ffmpeg && $(MAKE) install


include $(RACK_DIR)/plugin.mk
