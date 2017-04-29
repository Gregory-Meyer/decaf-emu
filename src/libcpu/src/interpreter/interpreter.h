#pragma once

#include "cpu.h"

namespace cpu {
	namespace interpreter {
		void initialise();

		Core* one(Core *core);

		void resume();
	} // namespace interpreter
} // namespace cpu