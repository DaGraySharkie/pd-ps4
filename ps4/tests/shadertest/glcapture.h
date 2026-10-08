#pragma once
#include <string>
#include <vector>
struct CapturedShader { unsigned type; std::string src; };
extern std::vector<CapturedShader> g_captured;
