#include "rack.hpp"


using namespace rack;

extern Plugin *pluginInstance;

extern Model *modelRecorder;


/** From VCV Free */
struct VCVBezelBig : app::SvgSwitch {
	VCVBezelBig() {
		momentary = true;
		addFrame(Svg::load(asset::plugin(pluginInstance, "res/VCVBezelBig.svg")));
	}
};

template <typename TBase>
struct VCVBezelLightBig : TBase {
	VCVBezelLightBig() {
		this->borderColor = color::BLACK_TRANSPARENT;
		this->bgColor = color::BLACK_TRANSPARENT;
		this->box.size = mm2px(math::Vec(11.1936, 11.1936));
	}
};
