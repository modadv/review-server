#pragma once

#ifdef _WIN32
#   ifdef BUILDING_DLL
#       define DLL_EXPORT __declspec(dllexport)
#   else
#       define DLL_EXPORT __declspec(dllimport)
#   endif
#else
#define DLL_EXPORT __attribute__((visibility("default")))
#endif