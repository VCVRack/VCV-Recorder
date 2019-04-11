#include "plugin.hpp"
#include <libavcodec/avcodec.h>


static const int MAX_BUFFER_LEN = 1024;


struct Encoder {
	Encoder() {

	}

	~Encoder() {

	}

	void processBuffer(float *input) {

	}
};


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
		ENUMS(VU_LIGHTS, 2*6),
		REC_LIGHT,
		NUM_LIGHTS
	};

	dsp::BooleanTrigger recTrigger;
	dsp::SchmittTrigger trigTrigger;
	dsp::VuMeter2 vuMeter[2];
	bool gate = false;
	Encoder *encoder;
	float inputBuffer[MAX_BUFFER_LEN] = {};
	dsp::Counter inputCounter;

	Recorder() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(GAIN_PARAM, 0.f, 2.f, 1.f, "Gain", " dB", -10, 20);
		configParam(REC_PARAM, 0.f, 1.f, 0.f, "Record");

		encoder = new Encoder;
		inputCounter.setPeriod(MAX_BUFFER_LEN);
	}

	~Recorder() {
		delete encoder;
	}

	void process(const ProcessArgs &args) override {
		// Record state
		if (recTrigger.process(params[REC_PARAM].getValue())) {
			gate ^= true;
		}
		if (trigTrigger.process(rescale(inputs[TRIG_INPUT].getVoltage(), 0.1, 2.0, 0.0, 1.0))) {
			gate ^= true;
		}
		if (inputs[GATE_INPUT].isConnected()) {
			gate = (inputs[GATE_INPUT].getVoltage() >= 2.f);
		}

		// Input
		float gain = params[GAIN_PARAM].getValue();
		float in[2];
		in[0] = inputs[LEFT_INPUT].getVoltage() / 10.f * gain;
		in[1] = inputs[RIGHT_INPUT].getVoltage() / 10.f * gain;
		inputBuffer[inputCounter.getCount()] = in[0];

		// Process
		if (inputCounter.process()) {
			encoder->processBuffer(inputBuffer);
		}

		// Lights
		for (int i = 0; i < 2; i++) {
			vuMeter[i].process(args.sampleTime, in[i]);
			lights[VU_LIGHTS + i*6 + 0].setBrightness(vuMeter[i].getBrightness(0, 0));
			lights[VU_LIGHTS + i*6 + 1].setBrightness(vuMeter[i].getBrightness(-3, 0));
			lights[VU_LIGHTS + i*6 + 2].setBrightness(vuMeter[i].getBrightness(-6, -3));
			lights[VU_LIGHTS + i*6 + 3].setBrightness(vuMeter[i].getBrightness(-12, -6));
			lights[VU_LIGHTS + i*6 + 4].setBrightness(vuMeter[i].getBrightness(-24, -12));
			lights[VU_LIGHTS + i*6 + 5].setBrightness(vuMeter[i].getBrightness(-36, -24));
		}

		lights[REC_LIGHT].setBrightness(gate);
	}
};


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

		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(6.7, 34.758)), module, Recorder::VU_LIGHTS + 0*6 + 0));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(18.7, 34.758)), module, Recorder::VU_LIGHTS + 1*6 + 0));
		addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(6.7, 39.884)), module, Recorder::VU_LIGHTS + 0*6 + 1));
		addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(18.7, 39.884)), module, Recorder::VU_LIGHTS + 1*6 + 1));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(6.7, 45.009)), module, Recorder::VU_LIGHTS + 0*6 + 2));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(18.7, 45.009)), module, Recorder::VU_LIGHTS + 1*6 + 2));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(6.7, 50.134)), module, Recorder::VU_LIGHTS + 0*6 + 3));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(18.7, 50.134)), module, Recorder::VU_LIGHTS + 1*6 + 3));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(6.7, 55.259)), module, Recorder::VU_LIGHTS + 0*6 + 4));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(18.7, 55.259)), module, Recorder::VU_LIGHTS + 1*6 + 4));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(6.7, 60.384)), module, Recorder::VU_LIGHTS + 0*6 + 5));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(18.7, 60.384)), module, Recorder::VU_LIGHTS + 1*6 + 5));
		addChild(createLightCentered<RecLight>(mm2px(Vec(12.699, 73.624)), module, Recorder::REC_LIGHT));
	}
};


Model *modelRecorder = createModel<Recorder, RecorderWidget>("Recorder");