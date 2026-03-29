#pragma once
// Force-included into mcut's compilation units to work around upstream bugs.
// mcut/internal/frontend.h uses std::chrono without #including <chrono>.
#ifdef __cplusplus
#include <chrono>
#endif
