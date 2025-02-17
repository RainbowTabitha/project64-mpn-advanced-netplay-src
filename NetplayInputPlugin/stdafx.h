// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <codecvt>
#include <cstdint>
#include <ctime>
#include <exception>
#include <functional>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <utility>

#ifdef _WIN32
#include <commctrl.h>
#include <qos2.h>
#include <richedit.h>
#include <strsafe.h>
#include <windows.h>
#include <windowsx.h>
#include <winuser.h>
#endif

#ifdef __GNUC__
#if !defined(__MINGW32__) && !defined(__MINGW64__)
#include <execinfo.h>
#endif
#endif

#ifdef DEBUG
#include <fstream>
#include <iomanip>
#endif
