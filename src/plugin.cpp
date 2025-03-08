#include "plugin.hpp"


Plugin *pluginInstance;


void init(Plugin *p) {
	pluginInstance = p;

	p->addModel(modelRecorder);
}


std::string lastRecordingsDirectory;


json_t* settingsToJson() {
	json_t* rootJ = json_object();

	json_object_set_new(rootJ, "lastRecordingsDirectory", json_string(lastRecordingsDirectory.c_str()));

	return rootJ;
}


void settingsFromJson(json_t* rootJ) {
	json_t* lastRecordingsDirectoryJ = json_object_get(rootJ, "lastRecordingsDirectory");
	if (lastRecordingsDirectoryJ)
		lastRecordingsDirectory = json_string_value(lastRecordingsDirectoryJ);
}
