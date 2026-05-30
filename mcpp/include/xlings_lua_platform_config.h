#ifndef XLINGS_LUA_PLATFORM_CONFIG_H
#define XLINGS_LUA_PLATFORM_CONFIG_H

#if !defined(_WIN32) && !defined(LUA_USE_LINUX) && !defined(LUA_USE_MACOSX)
#if defined(__APPLE__)
#define LUA_USE_MACOSX
#else
#define LUA_USE_LINUX
#endif
#endif

#endif
