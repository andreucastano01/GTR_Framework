#pragma once
#include "framework.h"
#include <string>

namespace GTR {

	enum eLightType {
		POINT = 0,
		SPOT = 1,
		DIRECTIONAL = 2
	};

	class Light
	{
	public:
		std::string name;
		Vector3 color;
		float intensity;
		eLightType light_type;
		float max_distance;
		float cone_angle;
		float cone_exp;
		float area_size;
	};
}

