#include "plugin.hpp"
#include "osdialog.h"
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
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


struct Encoder {
	AVIOContext *io = NULL;
	AVFormatContext *formatCtx = NULL;

	AVCodec *audioCodec = NULL;
	AVCodecContext *audioCtx = NULL;
	AVStream *audioStream = NULL;
	AVFrame *audioFrame = NULL;
	struct SwrContext *swr = NULL;
	int frameIndex = 0;

	AVCodec *videoCodec = NULL;
	AVCodecContext *videoCtx = NULL;
	AVStream *videoStream = NULL;
	AVFrame *videoFrame = NULL;
	struct SwsContext *sws = NULL;

	// Double buffer of RGBA8888 video data
	uint8_t *videoData[2] = {};
	bool videoDataFlipped = false;

	~Encoder() {
		close();
	}

	void openWAV(const std::string &path, int channels, int sampleRate, int depth) {
		close();
		if (!initIO(path))
			return;
		initMuxer("wav");

		if (depth == 16) {
			initAudio("pcm_s16le");
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S16;
		}
		else if (depth == 24) {
			initAudio("pcm_s24le");
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S32;
		}
		else {
			assert(0);
		}

		initChannels(channels);
		initSampleRate(sampleRate);
		open();
	}

	void openAIFF(const std::string &path, int channels, int sampleRate, int depth) {
		close();
		if (!initIO(path))
			return;
		initMuxer("aiff");

		if (depth == 16) {
			initAudio("pcm_s16be");
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S16;
		}
		else if (depth == 24) {
			initAudio("pcm_s24be");
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S32;
		}
		else {
			assert(0);
		}

		initChannels(channels);
		initSampleRate(sampleRate);
		open();
	}

	void openFLAC(const std::string &path, int channels, int sampleRate, int depth) {
		close();
		if (!initIO(path))
			return;
		initMuxer("flac");
		initAudio("flac");
		initChannels(channels);
		initSampleRate(sampleRate);

		if (depth == 16) {
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S16;
		}
		else if (depth == 24) {
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S32;
		}
		else {
			assert(0);
		}

		open();
	}

	void openALAC(const std::string &path, int channels, int sampleRate, int depth) {
		close();
		if (!initIO(path))
			return;
		initMuxer("ipod");
		initAudio("alac");
		initChannels(channels);
		initSampleRate(sampleRate);

		if (depth == 16) {
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S16P;
		}
		else if (depth == 24) {
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S32P;
		}
		else {
			assert(0);
		}

		open();
	}

	void openMP3(const std::string &path, int channels, int sampleRate, int bitRate) {
		close();
		if (!initIO(path))
			return;
		initMuxer("mp3");
		initAudio("libmp3lame");
		initChannels(channels);
		initSampleRate(sampleRate);

		audioCtx->bit_rate = bitRate;
		audioCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;

		open();
	}

	void openMPEG2(const std::string &path, int channels, int sampleRate, int bitRate, int width, int height) {
		close();
		if (!(width > 0 && height > 0))
			return;
		if (!initIO(path))
			return;

		initMuxer("mpeg");
		initAudio("mp2");
		initVideo("mpeg2video");
		initChannels(channels);
		initSampleRate(sampleRate);

		audioCtx->bit_rate = bitRate;
		audioCtx->sample_fmt = AV_SAMPLE_FMT_S16;

		// Round down to nearest even size
		videoCtx->bit_rate = 2 * 1000000 * 8;
		videoCtx->width = (width / 2) * 2;
		videoCtx->height = (height / 2) * 2;
		videoCtx->gop_size = 10;
		videoCtx->pix_fmt = AV_PIX_FMT_YUV420P;
		videoCtx->framerate = (AVRational) {30, 1};
		videoCtx->max_b_frames = 2;

		open();
	}

	void close() {
		if (formatCtx) {
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
		if (swr)
			swr_free(&swr);

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

	bool isOpen() {
		return formatCtx;
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
		videoDataFlipped ^= 1;

		// Advance frame
		videoFrame->pts++;
	}

	uint8_t *getProducerVideoData() {
		return videoData[videoDataFlipped];
	}

	uint8_t *getConsumerVideoData() {
		return videoData[!videoDataFlipped];
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

	// Private methods

	void initMuxer(const std::string &formatName) {
		int err;
		assert(!formatCtx);
		err = avformat_alloc_output_context2(&formatCtx, NULL, formatName.c_str(), NULL);
		assert(err >= 0);
		assert(formatCtx);
		assert(io);
		formatCtx->pb = io;
	}

	void initAudio(const std::string &codecName) {
		assert(!audioCodec);
		audioCodec = avcodec_find_encoder_by_name(codecName.c_str());
		assert(audioCodec);

		assert(!audioStream);
		audioStream = avformat_new_stream(formatCtx, NULL);
		assert(audioStream);

		assert(!audioCtx);
		audioCtx = avcodec_alloc_context3(audioCodec);
		assert(audioCtx);

		assert(!audioFrame);
		audioFrame = av_frame_alloc();
		assert(audioFrame);

		assert(!swr);
		swr = swr_alloc();
		assert(swr);
	}

	void initVideo(const std::string &codecName) {
		assert(!videoCodec);
		videoCodec = avcodec_find_encoder_by_name(codecName.c_str());
		assert(videoCodec);

		assert(!videoStream);
		videoStream = avformat_new_stream(formatCtx, NULL);
		assert(videoStream);

		assert(!videoCtx);
		videoCtx = avcodec_alloc_context3(videoCodec);
		assert(videoCtx);

		assert(!videoFrame);
		videoFrame = av_frame_alloc();
		assert(videoFrame);
	}

	bool initIO(const std::string &path) {
		int err;
		assert(!io);
		std::string url = "file:" + path;
		err = avio_open(&io, url.c_str(), AVIO_FLAG_WRITE);
		if (err < 0)
			return false;
		assert(io);
		return true;
	}

	void initChannels(int channels) {
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
	}

	void initSampleRate(int sampleRate) {
		audioCtx->sample_rate = sampleRate;
	}

	void open() {
		int err;
		assert(formatCtx && audioCodec && audioCtx && audioStream && audioFrame);

		audioCtx->time_base = (AVRational) {1, audioCtx->sample_rate};

		if (videoCtx) {
			// Inverse of framerate
			videoCtx->time_base = (AVRational) {videoCtx->framerate.den, videoCtx->framerate.num};
		}

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

		err = avcodec_open2(audioCtx, audioCodec, NULL);
		assert(err >= 0);

		if (videoCtx) {
			err = avcodec_open2(videoCtx, videoCodec, NULL);
			assert(err >= 0);
		}

		err = avcodec_parameters_from_context(audioStream->codecpar, audioCtx);
		assert(err >= 0);

		if (videoStream) {
			err = avcodec_parameters_from_context(videoStream->codecpar, videoCtx);
			assert(err >= 0);
		}

		av_dump_format(formatCtx, 0, NULL, true);

		// Set up audio frame
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

		// Set up video frame
		if (videoFrame) {
			videoFrame->pts = 0;
			videoFrame->format = videoCtx->pix_fmt;
			videoFrame->width = videoCtx->width;
			videoFrame->height = videoCtx->height;

			err = av_frame_get_buffer(videoFrame, 0);
			assert(err >= 0);
		}

		// Set up resampler
		swr_alloc_set_opts(swr,
		                   audioFrame->channel_layout, audioCtx->sample_fmt, audioFrame->sample_rate,
		                   audioFrame->channel_layout, audioCtx->sample_fmt, audioFrame->sample_rate,
		                   0, NULL);

		// Set up rescaler
		if (videoCtx) {
			assert(!sws);
			// TODO Change this to SWS_POINT if that means nearest-neighbor
			sws = sws_getContext(videoCtx->width, videoCtx->height, AV_PIX_FMT_RGBA,
			                     videoCtx->width, videoCtx->height, videoCtx->pix_fmt,
			                     SWS_BILINEAR, NULL, NULL, NULL);
			assert(sws);

			// Allocate videoData
			int videoDataSize = videoCtx->width * videoCtx->height * 4;
			videoData[0] = (uint8_t*) malloc(videoDataSize);
			videoData[1] = (uint8_t*) malloc(videoDataSize);
			memset(videoData[0], 0, videoDataSize);
			memset(videoData[1], 0, videoDataSize);
		}

		// Write format header to file
		err = avformat_write_header(formatCtx, NULL);
		assert(err >= 0);
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
	Encoder *encoder;
	std::mutex encoderMutex;

	// Format settings
	std::string format;
	std::string path;
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
		encoder = new Encoder;
		onReset();
	}

	~Recorder() {
		delete encoder;
	}

	void onReset() override {
		format = "wav";
		path = "out.wav";
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
		json_object_set_new(rootJ, "sampleRate", json_integer(sampleRate));
		json_object_set_new(rootJ, "depth", json_integer(depth));
		json_object_set_new(rootJ, "bitRate", json_integer(bitRate));
		return rootJ;
	}

	void dataFromJson(json_t *rootJ) override {
		stop();

		json_t *formatJ = json_object_get(rootJ, "format");
		if (formatJ)
			format = json_string_value(formatJ);

		json_t *pathJ = json_object_get(rootJ, "path");
		if (pathJ)
			path = json_string_value(pathJ);

		json_t *sampleRateJ = json_object_get(rootJ, "sampleRate");
		if (sampleRateJ)
			sampleRate = json_integer_value(sampleRateJ);

		json_t *depthJ = json_object_get(rootJ, "depth");
		if (depthJ)
			depth = json_integer_value(depthJ);

		json_t *bitRateJ = json_object_get(rootJ, "bitRate");
		if (bitRateJ)
			bitRate = json_integer_value(bitRateJ);
	}

	void process(const ProcessArgs &args) override {
		// Record state
		bool gate = encoder->isOpen();
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
		gate = encoder->isOpen();

		// Input
		float gain = params[GAIN_PARAM].getValue();
		float in[2];
		in[0] = inputs[LEFT_INPUT].getVoltage() / 10.f * gain;
		in[1] = inputs[RIGHT_INPUT].getVoltage() / 10.f * gain;

		// Process
		{
			std::lock_guard<std::mutex> lock(encoderMutex);
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
		std::lock_guard<std::mutex> lock(encoderMutex);
		if (format == "wav")
			encoder->openWAV(path, channels, sampleRate, depth);
		else if (format == "aiff")
			encoder->openAIFF(path, channels, sampleRate, depth);
		else if (format == "flac")
			encoder->openFLAC(path, channels, sampleRate, depth);
		else if (format == "alac")
			encoder->openALAC(path, channels, sampleRate, depth);
		else if (format == "mp3")
			encoder->openMP3(path, channels, sampleRate, bitRate);
		else if (format == "mpeg2")
			encoder->openMPEG2(path, channels, sampleRate, bitRate, width, height);
		else
			assert(0);
	}

	void stop() {
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
	}

	void writeVideo(uint8_t *data, int width, int height) {
		std::lock_guard<std::mutex> lock(encoderMutex);
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
		return (encoder->getProducerVideoData() != NULL);
	}

	// Settings

	void setFormat(std::string format) {
		if (this->format == format)
			return;
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
		this->format = format;
	}

	std::vector<std::string> getFormats() {
		return {"wav", "aiff", "flac", "alac", "mp3", "mpeg2"};
	}

	std::string getFormatName(std::string format) {
		static const std::map<std::string, std::string> names = {
			{"wav", "WAV (.wav)"},
			{"aiff", "AIFF (.aif)"},
			{"flac", "FLAC (.flac)"},
			{"alac", "ALAC (.m4a)"},
			{"mp3", "MP3 (.mp3)"},
			{"mpeg2", "MPEG-2 video (.mpg)"},
		};
		return names.at(format);
	}

	void setPath(std::string path) {
		if (this->path == path)
			return;
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
		this->path = path;
	}

	void setSampleRate(int sampleRate) {
		if (this->sampleRate == sampleRate)
			return;
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
		this->sampleRate = sampleRate;
	}

	std::vector<int> getSampleRates() {
		return {44100, 48000};
	}

	void setDepth(int depth) {
		if (this->depth == depth)
			return;
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
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
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
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
		// Don't close encoder, just prepare it for next start()
		this->width = width;
		this->height = height;
	}
};


////////////////////
// Widgets
////////////////////


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
};


struct RecLight : RedLight {
	RecLight() {
		bgColor = nvgRGB(0x66, 0x66, 0x66);
		box.size = mm2px(Vec(12.700, 12.700));
	}
};


struct PathItem : MenuItem {
	Recorder *module;
	void onAction(const widget::ActionEvent &e) override {
		std::string dir = asset::user("");
		char *path = osdialog_file(OSDIALOG_SAVE, dir.c_str(), "Untitled", NULL);
		if (path) {
			module->setPath(path);
			free(path);
		}
	}
};


struct FormatItem : MenuItem {
	Recorder *module;
	std::string format;
	void onAction(const widget::ActionEvent &e) override {
		module->setFormat(format);
	}
};


struct SampleRateValueItem : MenuItem {
	Recorder *module;
	int sampleRate;
	void onAction(const widget::ActionEvent &e) override {
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
	void onAction(const widget::ActionEvent &e) override {
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
	void onAction(const widget::ActionEvent &e) override {
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

		PathItem *pathItem = new PathItem;
		pathItem->text = "Select output file";
		pathItem->module = module;
		menu->addChild(pathItem);

		std::string path = string::ellipsizePrefix(module->path, 25);
		menu->addChild(createMenuLabel(path));

		menu->addChild(new MenuEntry);
		menu->addChild(createMenuLabel("Format"));

		for (const std::string &format : module->getFormats()) {
			FormatItem *formatItem = new FormatItem;
			formatItem->text = module->getFormatName(format);
			formatItem->rightText = CHECKMARK(format == module->format);
			formatItem->module = module;
			formatItem->format = format;
			menu->addChild(formatItem);
		}

		menu->addChild(new MenuEntry);
		menu->addChild(createMenuLabel("Settings"));

		SampleRateItem *sampleRateItem = new SampleRateItem;
		sampleRateItem->text = "Sample rate";
		sampleRateItem->rightText = RIGHT_ARROW;
		sampleRateItem->module = module;
		menu->addChild(sampleRateItem);

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