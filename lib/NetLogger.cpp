#include <stdio.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <winsock2.h>

#include "NetLogger.hpp" // why did i not do this?
#include <string>

void ___log(const char* msg)
{
	const char* ipAddress = "127.0.0.1";
	unsigned short port = 17474;
	const char* message = msg;
	WSADATA wsaData;
	int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (result != 0) 
	{
		return;
	}
	SOCKET sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sendSocket == INVALID_SOCKET) 
	{
		WSACleanup();
		return;
	}
	sockaddr_in destAddr;
	destAddr.sin_family = AF_INET;
	destAddr.sin_port = htons(port);
	if (inet_pton(AF_INET, ipAddress, &destAddr.sin_addr) <= 0) 
	{
		closesocket(sendSocket);
		WSACleanup();
		return;
	}
	int sendResult = sendto(sendSocket, message, strlen(message), 0, (sockaddr*)&destAddr, sizeof(destAddr));
	if (sendResult == SOCKET_ERROR) 
	{
		closesocket(sendSocket);
		WSACleanup();
		return;
	}
	closesocket(sendSocket);
	WSACleanup();
}

void ___log(const wchar_t* msg)
{
	const char* ipAddress = "127.0.0.1";
	unsigned short port = 17474;
	const wchar_t* message = msg;
	WSADATA wsaData;
	int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (result != 0) 
	{
		return;
	}
	SOCKET sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sendSocket == INVALID_SOCKET) 
	{
		WSACleanup();
		return;
	}
	sockaddr_in destAddr;
	destAddr.sin_family = AF_INET;
	destAddr.sin_port = htons(port);
	if (inet_pton(AF_INET, ipAddress, &destAddr.sin_addr) <= 0) 
	{
		closesocket(sendSocket);
		WSACleanup();
		return;
	}

	int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, message, -1, nullptr, 0, nullptr, nullptr);
	std::string s(utf8Length - 1, '\0');
	WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, msg, -1, s.data(), utf8Length, nullptr, nullptr);


	int sendResult = sendto(sendSocket, s.data(), s.size(), 0, (sockaddr*)&destAddr, sizeof(destAddr));
	if (sendResult == SOCKET_ERROR) 
	{
		closesocket(sendSocket);
		WSACleanup();
		return;
	}
	closesocket(sendSocket);
	WSACleanup();
}


void log(const char* format, ...) {
	static char buffer[1024]; // no more random char buffers everywhere.
	va_list args;
	va_start(args, format);
	vsnprintf_s(buffer, 1024, format, args);
	___log(buffer);
	va_end(args);
}

void log(const wchar_t* format, ...) {
	static wchar_t buffer[1024]; // no more random char buffers everywhere.
	va_list args;
	va_start(args, format);
	vswprintf_s(buffer, 1024, format, args);
	___log(buffer);
	va_end(args);
}

void logR(const char* format, ...) {
	constexpr int numBackTicks = 1;
	static char buffer[1024] = {'`'};
	va_list args;
	va_start(args, format);
	vsnprintf(buffer+numBackTicks, 1024-numBackTicks, format, args); // why cant i use the safe version here?? only god knows
	___log(buffer);
	va_end(args);
}

void logG(const char* format, ...) {
	constexpr int numBackTicks = 2;
	static char buffer[1024] = {'`', '`'};
	va_list args;
	va_start(args, format);
	vsnprintf(buffer+numBackTicks, 1024-numBackTicks, format, args); // why cant i use the safe version here?? only god knows
	___log(buffer);
	va_end(args);
}

void logB(const char* format, ...) {
	constexpr int numBackTicks = 3;
	static char buffer[1024] = {'`', '`', '`'};
	va_list args;
	va_start(args, format);
	vsnprintf(buffer+numBackTicks, 1024-numBackTicks, format, args); // why cant i use the safe version here?? only god knows
	___log(buffer);
	va_end(args);
}

void logY(const char* format, ...) {
	constexpr int numBackTicks = 4;
	static char buffer[1024] = {'`', '`', '`', '`'};
	va_list args;
	va_start(args, format);
	vsnprintf(buffer+numBackTicks, 1024-numBackTicks, format, args); // why cant i use the safe version here?? only god knows
	___log(buffer);
	va_end(args);
}

