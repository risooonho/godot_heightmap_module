#include "height_map.h"
#include <core/os/file_access.h>
#include <core/io/file_access_compressed.h>

#define DEFAULT_RESOLUTION 256
#define HEIGHTMAP_EXTENSION "heightmap"

const char *HeightMapData::SIGNAL_RESOLUTION_CHANGED = "resolution_changed";


HeightMapData::HeightMapData() {
}

void HeightMapData::load_default() {

	set_resolution(DEFAULT_RESOLUTION);

	// TODO Test, won't remain here
	Point2i size = heights.size();
	Point2i pos;
	for (pos.y = 0; pos.y < size.y; ++pos.y) {
		for (pos.x = 0; pos.x < size.x; ++pos.x) {
			float h = 8.0 * (Math::cos(pos.x * 0.2) + Math::sin(pos.y * 0.2));
			heights.set(pos, h);
		}
	}
	update_all_normals();
}

int HeightMapData::get_resolution() const {
	return heights.size().x;
}

void HeightMapData::set_resolution(int p_res) {

	if(p_res == get_resolution())
		return;

	if (p_res < HeightMap::CHUNK_SIZE)
		p_res = HeightMap::CHUNK_SIZE;

	// Power of two is important for LOD.
	// Also, grid data is off by one,
	// because for an even number of quads you need an odd number of vertices
	p_res = nearest_power_of_2(p_res - 1) + 1;

	Point2i size(p_res, p_res);
	heights.resize(size, true, 0);
	normals.resize(size, true, Vector3(0, 1, 0));
	colors.resize(size, true, Color(1, 1, 1, 1));

	for (int i = 0; i < TEXTURE_INDEX_COUNT; ++i) {
		// Sum of all weights must be 1, so we fill first slot with 1 and others with 0
		texture_weights[i].resize(size, true, i == 0 ? 1 : 0);
		texture_indices[i].resize(size, true, 0);
	}

	emit_signal(SIGNAL_RESOLUTION_CHANGED);
}

void HeightMapData::update_all_normals() {
	update_normals(Point2i(), heights.size());
}

void HeightMapData::update_normals(Point2i min, Point2i size) {

	Point2i max = min + size;
	Point2i pos;

	if (min.x < 0)
		min.x = 0;
	if (min.y < 0)
		min.y = 0;

	if (min.x > normals.size().x)
		min.x = normals.size().x;
	if (min.y > normals.size().y)
		min.y = normals.size().y;

	if (max.x < 0)
		max.x = 0;
	if (max.y < 0)
		max.y = 0;

	if (max.x > normals.size().x)
		max.x = normals.size().x;
	if (max.y > normals.size().y)
		max.y = normals.size().y;

	for (pos.y = min.y; pos.y < max.y; ++pos.y) {
		for (pos.x = min.x; pos.x < max.x; ++pos.x) {

			float left = heights.get_clamped(pos.x - 1, pos.y);
			float right = heights.get_clamped(pos.x + 1, pos.y);
			float fore = heights.get_clamped(pos.x, pos.y + 1);
			float back = heights.get_clamped(pos.x, pos.y - 1);

			normals.set(pos, Vector3(left - right, 2.0, back - fore).normalized());
		}
	}
}

void HeightMapData::_bind_methods() {

	ClassDB::bind_method(D_METHOD("set_resolution", "p_res"), &HeightMapData::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &HeightMapData::get_resolution);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution"), "set_resolution", "get_resolution");

	ADD_SIGNAL(MethodInfo(SIGNAL_RESOLUTION_CHANGED));
}


//---------------------------------------
// Serialization

const char *HEIGHTMAP_MAGIC_V1 = "GDHM";
const char *HEIGHTMAP_SUB_V1 = "v1__";

inline uint8_t encode_normal(float n) {
	return CLAMP(static_cast<int>(n * 127.f) + 128, 0, 255);
}

inline uint16_t encode_quantified(float h, float hmin, float hrange) {
	return CLAMP(static_cast<int>(65535.f * (h - hmin) / hrange), 0, 65535);
}

//inline uint8_t encode01(float v) {
//	return CLAMP(static_cast<int>(v * 255.f), 0, 255);
//}

inline float decode_normal(uint8_t n) {
	return static_cast<float>(n) / 255.f - 128.f;
}

inline float decode_quantified(uint16_t h, float hmin, float hrange) {
	return (static_cast<float>(h) / 65535.f) * hrange + hmin;
}

template <typename T>
void find_min_max(const T *data, int len, T& out_min, T& out_max) {

	if(len <= 0)
		return;

	T min = data[0];
	T max = min;

	for(int i = 1; i < len; ++i) {
		T v = data[i];
		if(v > max)
			max = v;
		else if(v < min)
			min = v;
	}

	out_min = min;
	out_max = max;
}

static void save_v1(HeightMapData &data, FileAccess &f) {

	// Sub-version
	f.store_buffer((const uint8_t*)HEIGHTMAP_SUB_V1, 4);

	// Size
	f.store_32(data.heights.size().x);
	f.store_32(data.heights.size().y);

	int area = data.heights.area();

	// Vertical bounds
	float hmin = 0.f;
	float hmax = 0.f;
	find_min_max(data.heights.raw(), area, hmin, hmax);
	f.store_float(hmin);
	f.store_float(hmax);

	// Heights
	float hrange = hmax - hmin;
	for(int i = 0; i < area; ++i) {
		float h = data.heights[i];
		uint16_t eh = encode_quantified(h, hmin, hrange);
		f.store_16(eh);
	}

	// Normals
	for(int i = 0; i < area; ++i) {
		Vector3 n = data.normals[i];
		f.store_8(encode_normal(n.x));
		f.store_8(encode_normal(n.y));
		f.store_8(encode_normal(n.z));
	}

	// Colors
	for(int i = 0; i < area; ++i) {
		Color c = data.colors[i];
		f.store_32(c.to_32());
	}

	// TODO Texture indices
	// TODO Texture weights

}

static Error load_v1(HeightMapData &data, FileAccess &f) {

	char version[5] = {0};
	f.get_buffer((uint8_t*)version, 4);

	if(strncmp(version, HEIGHTMAP_SUB_V1, 4) != 0) {
		print_line(String("Invalid version, found {0}").format(varray(version)));
		return ERR_FILE_UNRECOGNIZED;
	}

	Point2i size;
	size.x = f.get_32();
	size.y = f.get_32();

	int area = size.x * size.y;

	// Note: maybe some day non-square resolution will be supported
	data.set_resolution(size.x);

	float hmin = f.get_float();
	float hmax = f.get_float();

	float hrange = hmax - hmin;
	for(int i = 0; i < area; ++i) {
		data.heights[i] = decode_quantified(f.get_16(), hmin, hrange);
	}

	for(int i = 0; i < area; ++i) {
		Vector3 n;
		n.x = decode_normal(f.get_8());
		n.y = decode_normal(f.get_8());
		n.z = decode_normal(f.get_8());
		data.normals[i] = n;
	}

	for(int i = 0; i < area; ++i) {
		Color c = Color::hex(f.get_32());
		data.colors[i] = c;
	}

	// TODO Texture indices
	// TODO Texture weights

	return OK;
}


//---------------------------------------
// Saver

Error HeightMapDataSaver::save(const String &p_path, const Ref<Resource> &p_resource, uint32_t p_flags) {
	print_line("Saving heightmap data");

	Ref<HeightMapData> heightmap_data_ref = p_resource;
	ERR_FAIL_COND_V(heightmap_data_ref.is_null(), ERR_BUG);

	FileAccessCompressed *fac = memnew(FileAccessCompressed);
	fac->configure(HEIGHTMAP_MAGIC_V1);
	Error err = fac->_open(p_path, FileAccess::WRITE);
	if (err) {
		print_line("Error saving heightmap data");
		memdelete(fac);
		return err;
	}

	save_v1(**heightmap_data_ref, *fac);

	fac->close();

	return OK;
}

bool HeightMapDataSaver::recognize(const Ref<Resource> &p_resource) const {
	return p_resource->cast_to<HeightMapData>() != NULL;
}

void HeightMapDataSaver::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (p_resource->cast_to<HeightMapData>()) {
		p_extensions->push_back(HEIGHTMAP_EXTENSION);
	}
}


//---------------------------------------
// Loader

Ref<Resource> HeightMapDataLoader::load(const String &p_path, const String &p_original_path, Error *r_error) {
	print_line("Loading heightmap data");

	FileAccessCompressed *fac = memnew(FileAccessCompressed);
	fac->configure(HEIGHTMAP_MAGIC_V1);
	Error err = fac->_open(p_path, FileAccess::READ);
	if (err) {
		print_line("Error loading heightmap data");
		memdelete(fac);
		return err;
	}

	Ref<HeightMapData> heightmap_data_ref(memnew(HeightMapData));

	err = load_v1(**heightmap_data_ref, *fac);
	if(err != OK) {
		if(r_error)
			*r_error = err;
		return Ref<Resource>();
	}

	fac->close();

	if(r_error)
		*r_error = OK;
	return heightmap_data_ref;
}

void HeightMapDataLoader::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back(HEIGHTMAP_EXTENSION);
}
bool HeightMapDataLoader::handles_type(const String &p_type) const {
	return p_type == "HeightMapData";
}
String HeightMapDataLoader::get_resource_type(const String &p_path) const {
	String el = p_path.get_extension().to_lower();
	if (el == HEIGHTMAP_EXTENSION)
		return "HeightMapData";
	return "";
}




