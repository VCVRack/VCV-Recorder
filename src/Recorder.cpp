#include "plugin.hpp"
#include "osdialog.h"
#include <mutex>
#include <regex>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}


////////////////////
// DSP
////////////////////


static void printFfmpegError(int err) {
	char str[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(err, str, sizeof(str));
	DEBUG("ffmpeg error: %s", str);
}


struct FormatInfo {
	std::string name;
	std::string extension;
};


static const std::vector<std::string> AUDIO_FORMATS = {"wav", "aiff", "flac", "alac", "mp3"};
static const std::vector<std::string> VIDEO_FORMATS = {"mpeg2"};

static const std::map<std::string, FormatInfo> FORMAT_INFO = {
	{"wav", {"WAV", "wav"}},
	{"aiff", {"AIFF", "aif"}},
	{"flac", {"FLAC", "flac"}},
	{"alac", {"ALAC", "m4a"}},
	{"mp3", {"MP3", "mp3"}},
	{"mpeg2", {"MPEG-2 video", "mpg"}},
	{"h264", {"H.264", "mp4"}},
};


struct Encoder {
	bool initialized = false;
	bool opened = false;

	AVFormatContext *formatCtx = NULL;
	AVIOContext *io = NULL;

	AVCodec *audioCodec = NULL;
	AVCodecContext *audioCtx = NULL;
	AVStream *audioStream = NULL;
	AVFrame *audioFrame = NULL;
	int frameIndex = 0;

	AVCodec *videoCodec = NULL;
	AVCodecContext *videoCtx = NULL;
	AVStream *videoStream = NULL;
	AVFrame *videoFrame = NULL;
	struct SwsContext *sws = NULL;

	// Double buffer of RGBA8888 video data
	uint8_t *videoData[2] = {};
	std::atomic<int> videoDataIndex{0};

	~Encoder() {
		close();
	}

	void open(std::string format, std::string path, int channels, int sampleRate, int depth, int bitRate, int width, int height) {
		int err;
		// This method can only be called once per instance.
		assert(!initialized);
		initialized = true;

		// Create muxer
		std::string formatName;
		if (format == "wav") formatName = "wav";
		else if (format == "aiff") formatName = "aiff";
		else if (format == "flac") formatName = "flac";
		else if (format == "alac") formatName = "ipod";
		else if (format == "mp3") formatName = "mp3";
		else if (format == "mpeg2") formatName = "mpeg";
		else if (format == "h264") formatName = "mp4";
		else assert(0);

		err = avformat_alloc_output_context2(&formatCtx, NULL, formatName.c_str(), path.c_str());
		assert(err >= 0);
		assert(formatCtx);

		// Create IO
		std::string url = "file:" + path;
		err = avio_open(&io, url.c_str(), AVIO_FLAG_WRITE);
		if (err < 0) {
			printFfmpegError(err);
			return;
		}
		assert(io);
		formatCtx->pb = io;

		// Find audio encoder
		std::string audioEncoderName;
		if (format == "wav") {
			if (depth == 16) audioEncoderName = "pcm_s16le";
			else if (depth == 24) audioEncoderName = "pcm_s24le";
			else assert(0);
		}
		else if (format == "aiff") {
			if (depth == 16) audioEncoderName = "pcm_s16be";
			else if (depth == 24) audioEncoderName = "pcm_s24be";
			else assert(0);
		}
		else if (format == "flac") audioEncoderName = "flac";
		else if (format == "alac") audioEncoderName = "alac";
		else if (format == "mp3") audioEncoderName = "libmp3lame";
		else if (format == "mpeg2" || format == "h264") audioEncoderName = "mp2";
		else assert(0);

		audioCodec = avcodec_find_encoder_by_name(audioEncoderName.c_str());
		assert(audioCodec);

		// Create audio context
		audioCtx = avcodec_alloc_context3(audioCodec);
		assert(audioCtx);

		// Set audio channels
		audioCtx->channels = channels;
		if (channels == 1) {
			audioCtx->channel_layout = AV_CH_LAYOUT_MONO;
		}
		else if (channels == 2) {
			audioCtx->channel_layout = AV_CH_LAYOUT_STEREO;
		}
		else {
			assert(0);
		}

		// Set audio sample format
		if (format == "wav" || format == "aiff" || format == "flac") {
			if (depth == 16) audioCtx->sample_fmt = AV_SAMPLE_FMT_S16;
			else if (depth == 24) audioCtx->sample_fmt = AV_SAMPLE_FMT_S32;
			else assert(0);
		}
		else if (format == "alac") {
			if (depth == 16) audioCtx->sample_fmt = AV_SAMPLE_FMT_S16P;
			else if (depth == 24) audioCtx->sample_fmt = AV_SAMPLE_FMT_S32P;
			else assert(0);
		}
		else if (format == "mp3") audioCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
		else if (format == "mpeg2" || format == "h264") audioCtx->sample_fmt = AV_SAMPLE_FMT_S16;
		else assert(0);

		// Set bitrate
		if (format == "mp3" || format == "mpeg2" || format == "h264") {
			audioCtx->bit_rate = bitRate;
		}

		// Set sample rate
		audioCtx->sample_rate = sampleRate;
		audioCtx->time_base = (AVRational) {1, audioCtx->sample_rate};

		// Open audio encoder
		err = avcodec_open2(audioCtx, audioCodec, NULL);
		if (err < 0) {
			printFfmpegError(err);
			return;
		}

		// Create audio stream
		audioStream = avformat_new_stream(formatCtx, NULL);
		assert(audioStream);

		err = avcodec_parameters_from_context(audioStream->codecpar, audioCtx);
		assert(err >= 0);

		// Create audio frame
		audioFrame = av_frame_alloc();
		assert(audioFrame);

		audioFrame->pts = 0;
		audioFrame->format = audioCtx->sample_fmt;
		audioFrame->channel_layout = audioCtx->channel_layout;
		audioFrame->sample_rate = audioCtx->sample_rate;
		audioFrame->nb_samples = audioCtx->frame_size;
		// PCM doesn't set nb_samples, so use a sane default.
		if (audioFrame->nb_samples == 0)
			audioFrame->nb_samples = 16;

		err = av_frame_get_buffer(audioFrame, 0);
		assert(err >= 0);

		// Video
		if (format == "mpeg2" || format == "h264") {
			// Find video encoder
			std::string videoEncoderName;
			if (format == "mpeg2") videoEncoderName = "mpeg2video";
			else if (format == "h264") videoEncoderName = "h264";
			else assert(0);

			videoCodec = avcodec_find_encoder_by_name(videoEncoderName.c_str());
			assert(videoCodec);

			// Create video encoder
			videoCtx = avcodec_alloc_context3(videoCodec);
			assert(videoCtx);

			videoCtx->bit_rate = 20 * 1000 * 1000 * 8;
			videoCtx->width = (width / 2) * 2;
			videoCtx->height = (height / 2) * 2;
			videoCtx->gop_size = 10;
			videoCtx->pix_fmt = AV_PIX_FMT_YUV420P;
			videoCtx->framerate = (AVRational) {30, 1};
			videoCtx->max_b_frames = 2;

			videoCtx->time_base = (AVRational) {videoCtx->framerate.den, videoCtx->framerate.num};

			// Open video encoder
			err = avcodec_open2(videoCtx, videoCodec, NULL);
			assert(err >= 0);

			// Create video stream
			videoStream = avformat_new_stream(formatCtx, NULL);
			assert(videoStream);

			err = avcodec_parameters_from_context(videoStream->codecpar, videoCtx);
			assert(err >= 0);

			// Create video frame
			videoFrame = av_frame_alloc();
			assert(videoFrame);

			videoFrame->pts = 0;
			videoFrame->format = videoCtx->pix_fmt;
			videoFrame->width = videoCtx->width;
			videoFrame->height = videoCtx->height;

			err = av_frame_get_buffer(videoFrame, 0);
			assert(err >= 0);

			// Create video rescaler
			// TODO Change this to SWS_POINT if that means nearest-neighbor
			sws = sws_getContext(videoCtx->width, videoCtx->height, AV_PIX_FMT_RGBA, videoCtx->width, videoCtx->height, videoCtx->pix_fmt, SWS_BILINEAR, NULL, NULL, NULL);
			assert(sws);

			// Allocate videoData
			int videoDataSize = videoCtx->width * videoCtx->height * 4;
			videoData[0] = (uint8_t*) malloc(videoDataSize);
			videoData[1] = (uint8_t*) malloc(videoDataSize);
			memset(videoData[0], 0, videoDataSize);
			memset(videoData[1], 0, videoDataSize);
		}

		av_dump_format(formatCtx, 0, url.c_str(), true);

		#if 0
		// Supported sample rates
		for (const int *x = audioCodec->supported_samplerates; x && *x != 0; x++) {
			DEBUG("sample rate: %d", *x);
		}

		// Supported sample formats
		for (const enum AVSampleFormat *x = audioCodec->sample_fmts; x && *x != -1; x++) {
			DEBUG("sample format: %s", av_get_sample_fmt_name(*x));
		}

		if (videoCodec) {
			// Supported framerates
			for (const AVRational *x = videoCodec->supported_framerates; x && x->num != 0; x++) {
				DEBUG("framerate: %d/%d", x->num, x->den);
			}

			// Supported pixel formats
			for (const enum AVPixelFormat *x = videoCodec->pix_fmts; x && *x != -1; x++) {
				DEBUG("pixel format: %d", *x);
			}
		}
		#endif

		// Write format header to file
		err = avformat_write_header(formatCtx, NULL);
		assert(err >= 0);

		opened = true;
	}

	bool isOpen() {
		return opened;
	}

	void close() {
		if (opened) {
			// Flush a NULL frame to end the stream.
			flushFrame(audioCtx, audioStream, NULL);
			if (videoCtx)
				flushFrame(videoCtx, videoStream, NULL);
			// Write trailer to file
			av_write_trailer(formatCtx);
		}

		// Clean up audio
		if (audioFrame)
			av_frame_free(&audioFrame);
		if (audioCtx)
			avcodec_free_context(&audioCtx);
		audioCodec = NULL;
		audioStream = NULL;

		// Clean up video
		if (videoFrame)
			av_frame_free(&videoFrame);
		if (videoCtx)
			avcodec_free_context(&videoCtx);
		videoCodec = NULL;
		videoStream = NULL;
		if (sws) {
			sws_freeContext(sws);
			sws = NULL;
		}
		if (videoData[0]) {
			free(videoData[0]);
			videoData[0] = NULL;
			free(videoData[1]);
			videoData[1] = NULL;
		}

		// Clean up IO and format
		if (io) {
			avio_close(io);
			io = NULL;
		}
		if (formatCtx) {
			avformat_free_context(formatCtx);
			formatCtx = NULL;
		}
	}

	/** `input` must be `audioCtx->channels` length and between -1 and 1.
	*/
	void writeAudio(float *input) {
		int err;
		if (!audioCtx)
			return;

		err = av_frame_make_writable(audioFrame);
		assert(err >= 0);

		// Set output
		if (audioCtx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
			float **output = (float**) audioFrame->data;
			for (int i = 0; i < audioCtx->channels; i++) {
				float v = clamp(input[i], -1.f, 1.f);
				output[i][frameIndex] = v;
			}
		}
		else if (audioCtx->sample_fmt == AV_SAMPLE_FMT_S16) {
			int16_t **output = (int16_t**) audioFrame->data;
			for (int i = 0; i < audioCtx->channels; i++) {
				float v = clamp(input[i], -1.f, 1.f);
				output[0][frameIndex * audioCtx->channels + i] = (int16_t) std::round(v * 0x7fff);
			}
		}
		else if (audioCtx->sample_fmt == AV_SAMPLE_FMT_S32) {
			int32_t **output = (int32_t**) audioFrame->data;
			for (int i = 0; i < audioCtx->channels; i++) {
				float v = clamp(input[i], -1.f, 1.f);
				output[0][frameIndex * audioCtx->channels + i] = (int32_t) std::round(v * 0x7fffffff);
			}
		}
		else if (audioCtx->sample_fmt == AV_SAMPLE_FMT_S16P) {
			int16_t **output = (int16_t**) audioFrame->data;
			for (int i = 0; i < audioCtx->channels; i++) {
				float v = clamp(input[i], -1.f, 1.f);
				output[i][frameIndex] = (int16_t) std::round(v * 0x7fff);
			}
		}
		else if (audioCtx->sample_fmt == AV_SAMPLE_FMT_S32P) {
			int32_t **output = (int32_t**) audioFrame->data;
			for (int i = 0; i < audioCtx->channels; i++) {
				float v = clamp(input[i], -1.f, 1.f);
				output[i][frameIndex] = (int32_t) std::round(v * 0x7fffffff);
			}
		}
		else {
			assert(0);
		}

		// Write a video frame if needed
		if (videoCtx && av_compare_ts(videoFrame->pts, videoCtx->time_base, audioFrame->pts, audioCtx->time_base) <= 0) {
			// DEBUG("%f %f", (float) videoFrame->pts * videoCtx->time_base.num / videoCtx->time_base.den, (float) audioFrame->pts * audioCtx->time_base.num / audioCtx->time_base.den);
			writeVideo();
		}

		// Flush the frame if the frame buffer is full
		frameIndex++;
		if (frameIndex >= audioFrame->nb_samples) {
			frameIndex = 0;
			flushFrame(audioCtx, audioStream, audioFrame);
		}

		// Advance frame
		audioFrame->pts++;
	}

	void writeVideo() {
		int err;
		if (!videoCtx)
			return;

		assert(videoFrame);
		err = av_frame_make_writable(videoFrame);
		assert(err >= 0);

		uint8_t *videoData = getConsumerVideoData();
		if (!videoData)
			return;

		// Rescale video data
		int stride = videoCtx->width * 4;
		int height = videoCtx->height;
		sws_scale(sws, &videoData, &stride, 0, height, videoFrame->data, videoFrame->linesize);

		// Flush packets to file
		flushFrame(videoCtx, videoStream, videoFrame);

		// Flip double buffer
		videoDataIndex ^= 1;

		// Advance frame
		videoFrame->pts++;
	}

	uint8_t *getProducerVideoData() {
		return videoData[videoDataIndex];
	}

	uint8_t *getConsumerVideoData() {
		return videoData[!videoDataIndex];
	}

	int getVideoWidth() {
		if (!videoCtx)
			return 0;
		return videoCtx->width;
	}

	int getVideoHeight() {
		if (!videoCtx)
			return 0;
		return videoCtx->height;
	}

	void flushFrame(AVCodecContext *ctx, AVStream *stream, AVFrame *frame) {
		int err;
		assert(formatCtx);

		// frame may be NULL to signal the end of the stream.
		err = avcodec_send_frame(ctx, frame);
		assert(err >= 0);

		while (1) {
			AVPacket pkt = {};
			av_init_packet(&pkt);

			err = avcodec_receive_packet(ctx, &pkt);
			if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
				break;
			assert(err >= 0);

			pkt.stream_index = stream->index;
			av_packet_rescale_ts(&pkt, ctx->time_base, stream->time_base);

			err = av_interleaved_write_frame(formatCtx, &pkt);
			assert(err >= 0);
		}
	}
};


////////////////////
// Modules
////////////////////


struct Recorder : Module {
	enum ParamIds {
		GAIN_PARAM,
		REC_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		GATE_INPUT,
		TRIG_INPUT,
		LEFT_INPUT,
		RIGHT_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		NUM_OUTPUTS
	};
	enum LightIds {
		ENUMS(VU_LIGHTS, 2 * 6),
		REC_LIGHT,
		NUM_LIGHTS
	};

	dsp::BooleanTrigger recTrigger;
	dsp::SchmittTrigger trigTrigger;
	dsp::VuMeter2 vuMeter[2];
	dsp::ClockDivider lightDivider;
	Encoder *encoder = NULL;
	std::mutex encoderMutex;

	// Settings. Copied to Encoder when created.
	std::string format;
	std::string path;
	std::string directory;
	std::string basename;
	bool incrementPath;
	int channels;
	int sampleRate;
	int depth;
	int bitRate;
	int width, height;

	Recorder() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(GAIN_PARAM, 0.f, 2.f, 1.f, "Gain", " dB", -10, 20);
		configParam(REC_PARAM, 0.f, 1.f, 0.f, "Record");

		lightDivider.setDivision(512);
		onReset();
	}

	~Recorder() {
		stop();
	}

	void onReset() override {
		stop();
		format = "wav";
		path = "";
		directory = "";
		basename = "";
		incrementPath = true;
		channels = 2;
		sampleRate = 44100;
		depth = 16;
		bitRate = 320000;
		width = height = 0;
	}

	json_t *dataToJson() override {
		json_t *rootJ = json_object();
		json_object_set_new(rootJ, "format", json_string(format.c_str()));
		json_object_set_new(rootJ, "path", json_string(path.c_str()));
		json_object_set_new(rootJ, "incrementPath", json_boolean(incrementPath));
		json_object_set_new(rootJ, "sampleRate", json_integer(sampleRate));
		json_object_set_new(rootJ, "depth", json_integer(depth));
		json_object_set_new(rootJ, "bitRate", json_integer(bitRate));
		return rootJ;
	}

	void dataFromJson(json_t *rootJ) override {
		json_t *formatJ = json_object_get(rootJ, "format");
		if (formatJ)
			setFormat(json_string_value(formatJ));

		json_t *pathJ = json_object_get(rootJ, "path");
		if (pathJ)
			setPath(json_string_value(pathJ));

		json_t *incrementPathJ = json_object_get(rootJ, "incrementPath");
		if (incrementPathJ)
			incrementPath = json_boolean_value(incrementPathJ);

		json_t *sampleRateJ = json_object_get(rootJ, "sampleRate");
		if (sampleRateJ)
			setSampleRate(json_integer_value(sampleRateJ));

		json_t *depthJ = json_object_get(rootJ, "depth");
		if (depthJ)
			setDepth(json_integer_value(depthJ));

		json_t *bitRateJ = json_object_get(rootJ, "bitRate");
		if (bitRateJ)
			setBitRate(json_integer_value(bitRateJ));
	}

	void process(const ProcessArgs &args) override {
		// Record state
		bool gate = isRecording();
		bool oldGate = gate;
		if (recTrigger.process(params[REC_PARAM].getValue())) {
			gate ^= true;
		}
		if (trigTrigger.process(rescale(inputs[TRIG_INPUT].getVoltage(), 0.1, 2.0, 0.0, 1.0))) {
			gate ^= true;
		}
		if (inputs[GATE_INPUT].isConnected()) {
			gate = (inputs[GATE_INPUT].getVoltage() >= 2.f);
		}

		// Start/stop
		if (gate && !oldGate) {
			channels = inputs[RIGHT_INPUT].isConnected() ? 2 : 1;
			start();
		}
		else if (!gate && oldGate) {
			stop();
		}
		gate = isRecording();

		// Input
		float gain = params[GAIN_PARAM].getValue();
		float in[2];
		in[0] = inputs[LEFT_INPUT].getVoltage() / 10.f * gain;
		in[1] = inputs[RIGHT_INPUT].getVoltage() / 10.f * gain;

		// Process
		setSampleRate((int) args.sampleRate);
		{
			std::lock_guard<std::mutex> lock(encoderMutex);
			if (encoder)
				encoder->writeAudio(in);
		}

		// Lights
		for (int i = 0; i < 2; i++) {
			vuMeter[i].process(args.sampleTime, in[i]);
		}
		if (lightDivider.process()) {
			for (int i = 0; i < 2; i++) {
				lights[VU_LIGHTS + i * 6 + 0].setBrightness(vuMeter[i].getBrightness(0, 0));
				lights[VU_LIGHTS + i * 6 + 1].setBrightness(vuMeter[i].getBrightness(-3, 0));
				lights[VU_LIGHTS + i * 6 + 2].setBrightness(vuMeter[i].getBrightness(-6, -3));
				lights[VU_LIGHTS + i * 6 + 3].setBrightness(vuMeter[i].getBrightness(-12, -6));
				lights[VU_LIGHTS + i * 6 + 4].setBrightness(vuMeter[i].getBrightness(-24, -12));
				lights[VU_LIGHTS + i * 6 + 5].setBrightness(vuMeter[i].getBrightness(-36, -24));
			}

			lights[REC_LIGHT].setBrightness(gate);
		}
	}

	void start() {
		stop();
		std::lock_guard<std::mutex> lock(encoderMutex);

		if (path == "")
			return;

		std::string newPath = path;
		if (incrementPath) {
			std::string extension = FORMAT_INFO.at(format).extension;
			for (int i = 0; i <= 999; i++) {
				newPath = directory + "/" + basename;
				if (i > 0)
					newPath += string::f("-%03d", i);
				newPath += "." + extension;
				// Skip if file exists
				if (!system::isFile(newPath))
					break;
			}
		}

		encoder = new Encoder;
		encoder->open(format, newPath, channels, sampleRate, depth, bitRate, width, height);
		if (!encoder->isOpen()) {
			delete encoder;
			encoder = NULL;
		}
	}

	void stop() {
		std::lock_guard<std::mutex> lock(encoderMutex);
		if (encoder) {
			delete encoder;
			encoder = NULL;
		}
	}

	bool isRecording() {
		return !!encoder;
	}

	void writeVideo(uint8_t *data, int width, int height) {
		std::lock_guard<std::mutex> lock(encoderMutex);
		if (!encoder)
			return;
		uint8_t *videoData = encoder->getProducerVideoData();
		if (!videoData)
			return;

		int videoWidth = encoder->getVideoWidth();
		int videoHeight = encoder->getVideoHeight();

		// Copy video
		for (int videoY = 0; videoY < videoHeight; videoY++) {
			int y = videoHeight - videoY;
			int w = (y < height) ? std::min(width, videoWidth) : 0;
			// Copy horizontal line
			if (w > 0)
				memcpy(&videoData[videoY * videoWidth * 4], &data[y * width * 4], w * 4);
			// Fill black pixels
			if (w < videoWidth)
				memset(&videoData[(videoY * videoWidth + w) * 4], 0, (videoWidth - w) * 4);
		}
	}

	bool needsVideo() {
		if (!encoder)
			return false;
		return (encoder->getProducerVideoData() != NULL);
	}

	void fixPathExtension() {
		if (basename == "") {
			path = "";
			return;
		}
		std::string extension = FORMAT_INFO.at(format).extension;
		path = directory + "/" + basename + "." + extension;
	}

	// Settings

	void setFormat(std::string format) {
		if (this->format == format)
			return;
		stop();
		this->format = format;
		fixPathExtension();
	}

	void setPath(std::string path) {
		if (this->path == path)
			return;
		stop();

		if (path == "") {
			this->path = "";
			directory = "";
			basename = "";
			return;
		}

		directory = string::directory(path);
		basename = string::filenameBase(string::filename(path));
		fixPathExtension();
	}

	void setSampleRate(int sampleRate) {
		if (this->sampleRate == sampleRate)
			return;
		stop();
		this->sampleRate = sampleRate;
	}

	std::vector<int> getSampleRates() {
		return {44100, 48000};
	}

	void setDepth(int depth) {
		if (this->depth == depth)
			return;
		stop();
		this->depth = depth;
	}

	std::vector<int> getDepths() {
		return {16, 24};
	}

	bool showDepth() {
		return (format == "wav" || format == "aiff" || format == "flac" || format == "alac");
	}

	void setBitRate(int bitRate) {
		if (this->bitRate == bitRate)
			return;
		stop();
		this->bitRate = bitRate;
	}

	std::vector<int> getBitRates() {
		return {128000, 160000, 192000, 224000, 256000, 320000};
	}

	bool showBitRate() {
		return (format == "mp3" || format == "mpeg2");
	}

	void setSize(int width, int height) {
		if (this->width == width && this->height == height)
			return;
		// Don't stop recording, just prepare the size for next start.
		this->width = width;
		this->height = height;
	}
};


////////////////////
// Widgets
////////////////////


static void selectPath(Recorder *module) {
	std::string dir;
	std::string filename;
	if (module->path != "") {
		dir = string::directory(module->path);
		filename = string::filename(module->path);
	}
	else {
		dir = asset::user("");
		filename = "Untitled";
	}

	char *path = osdialog_file(OSDIALOG_SAVE, dir.c_str(), filename.c_str(), NULL);
	if (path) {
		module->setPath(path);
		free(path);
	}
}

struct BlackKnob : RoundKnob {
	BlackKnob() {
		setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/BlackKnob.svg")));
	}
};


struct RecButton : SvgSwitch {
	RecButton() {
		momentary = true;
		addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/RecButton.svg")));
	}

	void onDragStart(const event::DragStart &e) override {
		Recorder *module = dynamic_cast<Recorder*>(paramQuantity->module);
		if (module && module->path == "")
			selectPath(module);

		SvgSwitch::onDragStart(e);
	}
};


struct RecLight : RedLight {
	RecLight() {
		bgColor = nvgRGB(0x66, 0x66, 0x66);
		box.size = mm2px(Vec(12.700, 12.700));
	}
};


struct PathItem : MenuItem {
	Recorder *module;
	void onAction(const event::Action &e) override {
		selectPath(module);
	}
};


struct IncrementPathItem : MenuItem {
	Recorder *module;
	void onAction(const event::Action &e) override {
		module->incrementPath ^= true;
	}
};


struct FormatItem : MenuItem {
	Recorder *module;
	std::string format;
	void onAction(const event::Action &e) override {
		module->setFormat(format);
	}
};


struct SampleRateValueItem : MenuItem {
	Recorder *module;
	int sampleRate;
	void onAction(const event::Action &e) override {
		module->setSampleRate(sampleRate);
	}
};


struct SampleRateItem : MenuItem {
	Recorder *module;
	Menu *createChildMenu() override {
		Menu *menu = new Menu;
		for (int sampleRate : module->getSampleRates()) {
			SampleRateValueItem *item = new SampleRateValueItem;
			item->text = string::f("%g kHz", sampleRate / 1000.0);
			item->rightText = CHECKMARK(module->sampleRate == sampleRate);
			item->module = module;
			item->sampleRate = sampleRate;
			menu->addChild(item);
		}
		return menu;
	}
};


struct DepthValueItem : MenuItem {
	Recorder *module;
	int depth;
	void onAction(const event::Action &e) override {
		module->setDepth(depth);
	}
};


struct DepthItem : MenuItem {
	Recorder *module;
	Menu *createChildMenu() override {
		Menu *menu = new Menu;
		for (int depth : module->getDepths()) {
			DepthValueItem *item = new DepthValueItem;
			item->text = string::f("%d bit", depth);
			item->rightText = CHECKMARK(module->depth == depth);
			item->module = module;
			item->depth = depth;
			menu->addChild(item);
		}
		return menu;
	}
};


struct BitRateValueItem : MenuItem {
	Recorder *module;
	int bitRate;
	void onAction(const event::Action &e) override {
		module->setBitRate(bitRate);
	}
};


struct BitRateItem : MenuItem {
	Recorder *module;
	Menu *createChildMenu() override {
		Menu *menu = new Menu;
		for (int bitRate : module->getBitRates()) {
			BitRateValueItem *item = new BitRateValueItem;
			item->text = string::f("%d kbps", bitRate / 1000);
			item->rightText = CHECKMARK(module->bitRate == bitRate);
			item->module = module;
			item->bitRate = bitRate;
			menu->addChild(item);
		}
		return menu;
	}
};


////////////////////
// ModuleWidgets
////////////////////


struct RecorderWidget : ModuleWidget {
	RecorderWidget(Recorder *module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Recorder.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<BlackKnob>(mm2px(Vec(12.7, 21.417)), module, Recorder::GAIN_PARAM));
		addParam(createParamCentered<RecButton>(mm2px(Vec(12.7, 73.624)), module, Recorder::REC_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.697, 97.253)), module, Recorder::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.703, 97.253)), module, Recorder::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.696, 112.253)), module, Recorder::LEFT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.703, 112.253)), module, Recorder::RIGHT_INPUT));

		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(6.7, 34.758)), module, Recorder::VU_LIGHTS + 0 * 6 + 0));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(18.7, 34.758)), module, Recorder::VU_LIGHTS + 1 * 6 + 0));
		addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(6.7, 39.884)), module, Recorder::VU_LIGHTS + 0 * 6 + 1));
		addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(18.7, 39.884)), module, Recorder::VU_LIGHTS + 1 * 6 + 1));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(6.7, 45.009)), module, Recorder::VU_LIGHTS + 0 * 6 + 2));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(18.7, 45.009)), module, Recorder::VU_LIGHTS + 1 * 6 + 2));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(6.7, 50.134)), module, Recorder::VU_LIGHTS + 0 * 6 + 3));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(18.7, 50.134)), module, Recorder::VU_LIGHTS + 1 * 6 + 3));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(6.7, 55.259)), module, Recorder::VU_LIGHTS + 0 * 6 + 4));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(18.7, 55.259)), module, Recorder::VU_LIGHTS + 1 * 6 + 4));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(6.7, 60.384)), module, Recorder::VU_LIGHTS + 0 * 6 + 5));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(18.7, 60.384)), module, Recorder::VU_LIGHTS + 1 * 6 + 5));
		addChild(createLightCentered<RecLight>(mm2px(Vec(12.699, 73.624)), module, Recorder::REC_LIGHT));
	}

	void appendContextMenu(Menu *menu) override {
		Recorder *module = dynamic_cast<Recorder*>(this->module);

		menu->addChild(new MenuEntry);

		menu->addChild(createMenuLabel("Output file"));

		PathItem *pathItem = new PathItem;
		std::string path = string::ellipsizePrefix(module->path, 30);
		pathItem->text = (path != "") ? path : "Select...";
		pathItem->module = module;
		menu->addChild(pathItem);

		IncrementPathItem *incrementPathItem = new IncrementPathItem;
		incrementPathItem->text = "Overwrite existing file";
		incrementPathItem->rightText = CHECKMARK(!module->incrementPath);
		incrementPathItem->module = module;
		menu->addChild(incrementPathItem);

		menu->addChild(new MenuEntry);
		menu->addChild(createMenuLabel("Audio"));

		for (const std::string &format : AUDIO_FORMATS) {
			const FormatInfo &fi = FORMAT_INFO.at(format);
			FormatItem *formatItem = new FormatItem;
			formatItem->text = fi.name + " (." + fi.extension + ")";
			formatItem->rightText = CHECKMARK(format == module->format);
			formatItem->module = module;
			formatItem->format = format;
			menu->addChild(formatItem);
		}

		menu->addChild(new MenuEntry);
		menu->addChild(createMenuLabel("Video (experimental)"));

		for (const std::string &format : VIDEO_FORMATS) {
			const FormatInfo &fi = FORMAT_INFO.at(format);
			FormatItem *formatItem = new FormatItem;
			formatItem->text = fi.name + " (." + fi.extension + ")";
			formatItem->rightText = CHECKMARK(format == module->format);
			formatItem->module = module;
			formatItem->format = format;
			menu->addChild(formatItem);
		}

		menu->addChild(new MenuEntry);
		menu->addChild(createMenuLabel("Encoder settings"));

		// SampleRateItem *sampleRateItem = new SampleRateItem;
		// sampleRateItem->text = "Sample rate";
		// sampleRateItem->rightText = RIGHT_ARROW;
		// sampleRateItem->module = module;
		// menu->addChild(sampleRateItem);

		if (module->showDepth()) {
			DepthItem *depthItem = new DepthItem;
			depthItem->text = "Bit depth";
			depthItem->rightText = RIGHT_ARROW;
			depthItem->module = module;
			menu->addChild(depthItem);
		}

		if (module->showBitRate()) {
			BitRateItem *bitRateItem = new BitRateItem;
			bitRateItem->text = "Bit rate";
			bitRateItem->rightText = RIGHT_ARROW;
			bitRateItem->module = module;
			menu->addChild(bitRateItem);
		}
	}

	void draw(const DrawArgs &args) override {
		ModuleWidget::draw(args);
		if (!this->module)
			return;
		Recorder *module = dynamic_cast<Recorder*>(this->module);

		int width, height;
		glfwGetFramebufferSize(APP->window->win, &width, &height);
		module->setSize(width, height);

		if (module->needsVideo()) {
			uint8_t data[height * width * 4];
			glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data);
			module->writeVideo(data, width, height);
		}
	}
};


Model *modelRecorder = createModel<Recorder, RecorderWidget>("Recorder");