#include "plugin.hpp"
#include "osdialog.h"
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}


////////////////////
// DSP
////////////////////


struct Encoder {
	AVIOContext *io = NULL;
	AVFormatContext *formatCtx = NULL;
	AVCodec *audioCodec = NULL;
	AVCodecContext *audioCtx = NULL;
	AVStream *audioStream = NULL;
	AVFrame *audioFrame = NULL;
	int frameIndex = 0;

	~Encoder() {
		close();
	}

	void initFormat(const std::string &formatName) {
		int err;
		assert(!formatCtx);
		err = avformat_alloc_output_context2(&formatCtx, NULL, formatName.c_str(), NULL);
		assert(err >= 0);
		assert(formatCtx);
	}

	void initAudio(enum AVCodecID id) {
		assert(!audioCodec);
		audioCodec = avcodec_find_encoder(id);
		assert(audioCodec);

		assert(!audioCtx);
		audioCtx = avcodec_alloc_context3(audioCodec);
		assert(audioCtx);

		assert(!audioStream);
		audioStream = avformat_new_stream(formatCtx, audioCodec);
		assert(audioStream);

		assert(!audioFrame);
		audioFrame = av_frame_alloc();
		assert(audioFrame);

		// TODO allocate resampler
		// swr_alloc();
		// swr_init();
	}

	bool initIO(const std::string &path) {
		int err;
		assert(!io);
		std::string url = "file:" + path;
		err = avio_open(&io, url.c_str(), AVIO_FLAG_WRITE);
		if (err < 0)
			return false;
		assert(io);
		formatCtx->pb = io;
		return true;
	}

	void open() {
		int err;
		assert(formatCtx && audioCodec && audioCtx && audioStream && audioFrame);

		// audioStream->time_base = (AVRational) {1, audioCtx->sample_rate};

		err = avcodec_open2(audioCtx, audioCodec, NULL);
		assert(err >= 0);

		err = avcodec_parameters_from_context(audioStream->codecpar, audioCtx);
		assert(err >= 0);

#if 0
		av_dump_format(formatCtx, 0, NULL, true);

		// Supported sample rates
		for (const int *x = audioCodec->supported_samplerates; x && *x != 0; x++) {
			DEBUG("sample rate: %d", *x);
		}

		// Supported sample formats
		for (const enum AVSampleFormat *x = audioCodec->sample_fmts; x && *x != -1; x++) {
			DEBUG("sample format: %s", av_get_sample_fmt_name(*x));
		}
#endif

		// Set up frame
		audioFrame->format = audioCtx->sample_fmt;
		audioFrame->channel_layout = audioCtx->channel_layout;
		audioFrame->sample_rate = audioCtx->sample_rate;
		audioFrame->nb_samples = audioCtx->frame_size;
		// PCM doesn't set nb_samples, so use a sane default.
		if (audioFrame->nb_samples == 0)
			audioFrame->nb_samples = 256;

		err = av_frame_get_buffer(audioFrame, 0);
		assert(err >= 0);

		err = avformat_write_header(formatCtx, NULL);
		assert(err >= 0);
	}

	void close() {
		// Flush a NULL frame to end the stream.
		if (audioFrame)
			av_frame_free(&audioFrame);
		if (formatCtx && audioCtx)
			flush();
		// Write trailer of file
		if (formatCtx)
			av_write_trailer(formatCtx);
		// Clean up objects
		if (audioCtx)
			avcodec_free_context(&audioCtx);
		audioCodec = NULL;
		audioStream = NULL;
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

	void openWAV(const std::string &path, int channels, int sampleRate, int depth) {
		close();
		initFormat("wav");

		if (depth == 16) {
			initAudio(AV_CODEC_ID_PCM_S16LE);
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S16;
		}
		else if (depth == 24) {
			initAudio(AV_CODEC_ID_PCM_S24LE);
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S32;
		}
		else {
			assert(0);
		}
		audioCtx->sample_rate = sampleRate;
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
		// Frame size is not automatically set by the PCM codec.
		// audioCtx->frame_size = 64;

		if (!initIO(path)) {
			close();
			return;
		}
		open();
	}

	void openFLAC(const std::string &path, int channels, int sampleRate, int depth) {
		close();
		initFormat("flac");
		initAudio(AV_CODEC_ID_FLAC);

		if (depth == 16) {
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S16;
		}
		else if (depth == 24) {
			audioCtx->sample_fmt = AV_SAMPLE_FMT_S32;
		}
		else {
			assert(0);
		}
		audioCtx->sample_rate = sampleRate;
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

		if (!initIO(path)) {
			close();
			return;
		}
		open();
	}

	void openMP3(const std::string &path, int channels, int sampleRate, int bitRate) {
		close();
		initFormat("mp3");
		initAudio(AV_CODEC_ID_MP3);

		audioCtx->bit_rate = bitRate;
		audioCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
		audioCtx->sample_rate = sampleRate;
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

		if (!initIO(path)) {
			close();
			return;
		}
		open();
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
		else {
			assert(0);
		}

		frameIndex++;
		if (frameIndex >= audioFrame->nb_samples) {
			frameIndex = 0;
			flush();
		}
	}

	void flush() {
		int err;
		assert(formatCtx && audioCtx);

		// frame may be NULL to signal the end of the stream.
		err = avcodec_send_frame(audioCtx, audioFrame);
		assert(err >= 0);

		while (1) {
			AVPacket pkt;
			av_init_packet(&pkt);

			err = avcodec_receive_packet(audioCtx, &pkt);
			if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
				break;

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
		format = "WAV";
		path = "out.wav";
		channels = 2;
		sampleRate = 44100;
		depth = 16;
		bitRate = 320000;
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
		if (format == "WAV")
			encoder->openWAV(path, channels, sampleRate, depth);
		else if (format == "FLAC")
			encoder->openFLAC(path, channels, sampleRate, depth);
		else if (format == "MP3")
			encoder->openMP3(path, channels, sampleRate, bitRate);
	}

	void stop() {
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
	}

	// Settings

	void setFormat(std::string format) {
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
		this->format = format;
	}

	std::vector<std::string> getFormats() {
		return {"WAV", "FLAC", "MP3"};
	}

	std::string getFormatName(std::string format) {
		static const std::map<std::string, std::string> names = {
			{"WAV", "WAV"},
			{"FLAC", "FLAC"},
			{"MP3", "MP3"},
		};
		return names.at(format);
	}

	void setPath(std::string path) {
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
		this->path = path;
	}

	void setSampleRate(int sampleRate) {
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
		this->sampleRate = sampleRate;
	}

	std::vector<int> getSampleRates() {
		return {44100, 48000};
	}

	void setDepth(int depth) {
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
		this->depth = depth;
	}

	std::vector<int> getDepths() {
		return {16, 24};
	}

	bool showDepth() {
		return (format == "WAV" || format == "FLAC");
	}

	void setBitRate(int bitRate) {
		std::lock_guard<std::mutex> lock(encoderMutex);
		encoder->close();
		this->bitRate = bitRate;
	}

	std::vector<int> getBitRates() {
		return {128000, 160000, 192000, 224000, 256000, 320000};
	}

	bool showBitRate() {
		return (format == "MP3");
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
		bgColor = color::BLACK_TRANSPARENT;
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
};


Model *modelRecorder = createModel<Recorder, RecorderWidget>("Recorder");