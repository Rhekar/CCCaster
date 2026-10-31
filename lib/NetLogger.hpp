#pragma once


void __stdcall ___log(const char* msg);

void __stdcall log(const char* format, ...);

void __stdcall ___log(const wchar_t* msg);

void __stdcall log(const wchar_t* format, ...);

void __stdcall logR(const char* format, ...);
void __stdcall logG(const char* format, ...);
void __stdcall logB(const char* format, ...);
void __stdcall logY(const char* format, ...);
