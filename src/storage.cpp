#include "storage.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include "ecg_isd_config.h"

const char* storage_error_to_str(StorageError error) {
	switch (error) {
	case StorageError::None:
		return "None";
	case StorageError::CanNotInitialize:
		return "CanNotInitialize";
	case StorageError::CanNotOpenFile:
		return "CanNotOpenFile";
	case StorageError::CanNotRemoveFile:
		return "CanNotRemoveFile";
	case StorageError::FileSystemError:
		return "FileSystemError";
	case StorageError::TooManyFiles:
		return "TooManyFiles";
	}

	return "<Error>";
}

const char* storage_state_to_str(StorageState state) {
	switch (state) {
	case StorageState::Idle:
		return "Idle";
	case StorageState::Error:
		return "Error";
	case StorageState::Recording:
		return "Recording";
	case StorageState::Reading:
		return "Reading";
	}

	return "<State>";
}

#define STORAGE_CHECK_STATE(CURRENT_STATE, EXPECTED_STATE, RETURN_VALUE) \
	if (CURRENT_STATE != (EXPECTED_STATE)) { \
		log_e( \
			"ERROR: not in %s state, current state: %s", \
			storage_state_to_str(EXPECTED_STATE), \
			storage_state_to_str(CURRENT_STATE)); \
		return RETURN_VALUE; \
	}

Storage::Storage(SPIClass& spi, std::mutex& spi_mutex)
	: _spi(spi), _spi_mutex(spi_mutex) {
	if (!init()) {
		log_e("First init failed");
	}

	// float data[9] = { 0.0, 1.0, 2.0 };
	// for (int i = 0; i < 3; i++) {
	// 	if (create_new_recording()) {
	// 		for (int j = 0; j < i + 1; j++) {
	// 			write_record(data, 3);
	// 		}
	// 		close_recording();
	// 	}
	// }
	//
	// Serial.println("List recs:");
	// for (auto& rec : list_recordings()) {
	// 	Serial.printf("- %s (%d)\n", rec.get_name(), rec.get_size());
	// 	remove_recording(rec.get_name());
	// 	if (open_recording(rec.get_name())) {
	// 		int len;
	// 		do {
	// 			len = read_record(data, 9);
	//
	// 			for (int i = 0; i < len; i++) {
	// 				Serial.printf("%f;", data[i]);
	// 			}
	//
	// 			if (len > 0) {
	// 				Serial.println();
	// 			}
	// 		} while (len >= 1);
	// 		close_recording();
	// 	}
	// }
}

Storage::~Storage() {}

bool Storage::init() {
	std::lock_guard<std::mutex> lock(_spi_mutex);

	if (!SD.begin(SD_CS, _spi)) {
		log_e("begin error");
		set_error(StorageError::CanNotInitialize);

		return false;
	}

	if (!SD.exists("/recordings")) {
		if (!SD.mkdir("/recordings")) {
			log_e("mkdir /recordings error");
			set_error(StorageError::FileSystemError);

			return false;
		}
	}

	_state = StorageState::Idle;

	return true;
}

void Storage::set_error(StorageError error) {
	log_e("%s", storage_error_to_str(error));
	_state = StorageState::Error;
	_error = error;
}

StorageError Storage::get_error() const {
	return _error;
}

bool Storage::clear_error() {
	if (!init()) {
		log_e("Clear error failed");
		return false;
	}
	_state = StorageState::Idle;
	_error = StorageError::None;

	return true;
}

std::vector<StorageEntry> Storage::list_recordings() {
	STORAGE_CHECK_STATE(_state, StorageState::Idle, {});

	std::lock_guard<std::mutex> lock(_spi_mutex);

	File dir = SD.open("/recordings");

	if (!dir) {
		log_e("Can not open /recordings dir");
		set_error(StorageError::CanNotOpenFile);
		return {};
	}

	File entry = dir.openNextFile();
	std::vector<StorageEntry> recordings;

	while (entry) {
		if (!entry.isDirectory()) {
			const char* entry_name = entry.name();
			log_d("checking entry path: %s", entry_name);

			if (strncmp(entry_name, "/recordings/", 12) == 0) {
				entry_name += 12;
				log_d("checking entry file name: %s", entry_name);

				int entry_name_len = strlen(entry_name);
				if ((entry_name_len >= 4) &&
					(0 ==
					 strcasecmp(entry_name + entry_name_len - 4, ".rec"))) {
					std::string recording_name(entry_name, entry_name_len - 4);
					log_d("found recording: %s", recording_name.data());
					recordings.emplace_back(
						StorageEntry(std::move(recording_name), entry.size()));
				}
			}
		}

		entry.close();
		entry = dir.openNextFile();
	}

	dir.close();

	return recordings;
}

static std::string build_recording_path(const char* name) {
	std::string path = "/recordings/";
	path += name;
	path += ".rec";
	return path;
}

bool Storage::remove_recording(const char* name) {
	STORAGE_CHECK_STATE(_state, StorageState::Idle, false);

	std::lock_guard<std::mutex> lock(_spi_mutex);

	auto path = build_recording_path(name);

	if (SD.exists(path.data())) {
		if (!SD.remove(path.data())) {
			log_e("can't remove file: %s", path.data());
			set_error(StorageError::CanNotRemoveFile);

			return false;
		}

		return true;
	}

	return false;
}

const char* Storage::create_new_recording() {
	STORAGE_CHECK_STATE(_state, StorageState::Idle, nullptr);

	std::lock_guard<std::mutex> lock(_spi_mutex);

	char recording_name[6];
	char recording_path[22];
	bool new_file_found = false;

	for (int i = _last_file_index; i < 10000; i++) {
		snprintf(recording_name, 6, "%05d", i);
		snprintf(recording_path, 22, "/recordings/%s.rec", recording_name);
		log_d("checking file path: %s", recording_path);

		if (!SD.exists(recording_path)) {
			log_d("file doesn't exist %s", recording_path);
			_current_recording_name = recording_name;
			new_file_found = true;
			break;
		}
	}

	if (!new_file_found) {
		log_e("can not find free filename");
		set_error(StorageError::TooManyFiles);
		return nullptr;
	}

	log_i("opening: %s", recording_path);
	_current_file = SD.open(recording_path, FILE_WRITE);

	if (!_current_file) {
		log_e("can not open file: %s", recording_path);
		set_error(StorageError::CanNotOpenFile);
		return nullptr;
	}

	_state = StorageState::Recording;

	log_i("created new recording: %s", recording_name);

	return _current_recording_name.data();
}

bool Storage::write_record(const float data[], uint8_t length) {
	STORAGE_CHECK_STATE(_state, StorageState::Recording, false);

	if (length == 0) {
		log_e("No data to write??");
		return false;
	}

	std::lock_guard<std::mutex> lock(_spi_mutex);

	if (!_current_file.write(length)) {
		log_e("couldn't write length to file");
		set_error(StorageError::FileSystemError);
		return false;
	}

	if (!_current_file.write((const uint8_t*) data, sizeof(float) * length)) {
		log_e("couldn't write data to file");
		set_error(StorageError::FileSystemError);
		return false;
	}

	return true;
}

bool Storage::open_recording(const char* name) {
	STORAGE_CHECK_STATE(_state, StorageState::Idle, false);

	std::lock_guard<std::mutex> lock(_spi_mutex);

	auto path = build_recording_path(name);

	if (SD.exists(path.data())) {
		if ((_current_file = SD.open(path.data()))) {
			_state = StorageState::Reading;

			return true;
		} else {
			log_e("can not open recording: %s", path.data());
		}
	} else {
		log_e("no such recording: %s", path.data());
	}

	return false;
}

int Storage::read_record(float data[], uint8_t length) {
	STORAGE_CHECK_STATE(_state, StorageState::Reading, 0);

	std::lock_guard<std::mutex> lock(_spi_mutex);

	int data_length;

	if ((data_length = _current_file.peek()) == -1) {
		// log_d("can't read length, assuming end of file");

		return 0;
	}

	// log_d("reading %d floats", data_length);

	if (data_length >= length) {
		log_w(
			"not enough space for reading, space: %d, needed: %d",
			length,
			data_length);

		return -data_length;
	}

	if ((data_length = _current_file.read()) == -1) {
		log_e("couldn't read data from file");
		set_error(StorageError::FileSystemError);

		return 0;
	}

	if (!_current_file.read((uint8_t*) data, sizeof(float) * data_length)) {
		set_error(StorageError::FileSystemError);

		return 0;
	}

	return data_length;
}

bool Storage::is_recording_open() const {
	return _state == StorageState::Recording || _state == StorageState::Reading;
}

bool Storage::close_recording() {
	switch (_state) {
	case StorageState::Recording:
		log_d("stopping recording");

		{
			std::lock_guard<std::mutex> lock(_spi_mutex);
			_current_file.close();
		}

		_state = StorageState::Idle;
		_last_file_index++;
		_current_recording_name.clear();

		return true;
	case StorageState::Reading:
		log_d("stopping reading");

		{
			std::lock_guard<std::mutex> lock(_spi_mutex);
			_current_file.close();
		}

		_state = StorageState::Idle;
		return true;
	default:
		log_e("ERROR: current state: %s", storage_state_to_str(_state));
		return false;
	}
}
