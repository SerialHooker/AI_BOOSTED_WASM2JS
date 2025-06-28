#pragma once


#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <stack>
#include <fstream>
#include <tuple>


#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOCRYPT
#define NOGDI
#include <winsock2.h>
#include <ws2tcpip.h>

#include <Windows.h>


#ifdef ZeroMemory
#  undef ZeroMemory
#endif
#ifdef CopyMemory
#  undef CopyMemory
#endif
#ifdef MoveMemory
#  undef MoveMemory
#endif
#ifdef FillMemory
#  undef FillMemory
#endif
#ifdef memset
#  undef memset         #endif

#pragma comment(lib, "Ws2_32.lib")
