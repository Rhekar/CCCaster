#include "DllExceptionHandler.hpp"
#include <windows.h>
#include "NetLogger.hpp"
#include <time.h>
#include <string>
#include <filesystem>
#include <psapi.h>
#include "Version.hpp"
#include <intrin.h>
#include <vector>
#include <array>
#include <d3dx9.h>
#include <fstream>
#include <winhttp.h>
#include <dbghelp.h>
#include <cxxabi.h>
#include <cstdlib>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dbghelp.lib")

BOOL CALLBACK symCallback( HANDLE hProcess, ULONG actionCode, ULONG64 callbackData, ULONG64 userContext) {

	static char buffer[4096];
	if (actionCode == CBA_DEBUG_INFO) {
		const char* msg = reinterpret_cast<const char*>(callbackData);
		snprintf(buffer, sizeof(buffer), "``CBA_DEBUG_INFO: %s", msg);
		log(buffer);
	} else if(actionCode == CBA_EVENT) {
		IMAGEHLP_CBA_EVENT* event = reinterpret_cast<IMAGEHLP_CBA_EVENT*>(callbackData);
		log("`CBA_EVENT:                       severity=%lu code=%lu desc=%s", event->severity, event->code,	event->desc ? event->desc : "");
	} else if(actionCode == CBA_DEFERRED_SYMBOL_LOAD_CANCEL) {
		IMAGEHLP_CBA_EVENT* e = reinterpret_cast<IMAGEHLP_CBA_EVENT*>(callbackData);
		log("`CBA_DEFERRED_SYMBOL_LOAD_CANCEL: severity=%lu code=%lu desc=%s", e->severity, e->code, e->desc ? e->desc : "");
	} else {
		log("```unchecked actionCode: %08X", actionCode);
	}

	return TRUE;
}

typedef struct CV_INFO_PDB70 {
    DWORD cvSig;
    GUID guid;
    DWORD age;
    BYTE pdbFilename[];
} CV_INFO_PDB70;

namespace ExceptionHandler {

	int resSymInitialize = 0;
	int resSymRegisterCallback = 0;

	bool wasInitCalled = false;

	typedef const char* (*getETMVersionType)();
	getETMVersionType getETMVersion = NULL;

	std::string numToHex(DWORD v) {
		// i could avoid using this to compress shit further but... who cares
		char buffer[32];
		snprintf(buffer, 32, "0x%08X", v);
		return std::string(buffer);
	}

	std::string toUTF8(const std::wstring& wide_string) { //https://json.nlohmann.me/home/faq/#parse-errors-reading-non-ascii-characters
		static std::wstring_convert<std::codecvt_utf8<wchar_t>> utf8_conv;
		return utf8_conv.to_bytes(wide_string);
	}

	bool GetPdbInfo(HMODULE module, GUID& guid, DWORD& age, char* pdbName) {
		auto* base = reinterpret_cast<BYTE*>(module);

		auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;

		auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS32>(
			base + dos->e_lfanew);

		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		const auto& debugDir =
			nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];

		if (debugDir.VirtualAddress == 0 ||
			debugDir.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
			return false;

		auto* debug = reinterpret_cast<PIMAGE_DEBUG_DIRECTORY>(
			base + debugDir.VirtualAddress);

		size_t count =
			debugDir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);

		for (size_t i = 0; i < count; ++i)
		{
			if (debug[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW)
				continue;

			auto* cv = base + debug[i].AddressOfRawData;

			// RSDS signature
			if (*reinterpret_cast<DWORD*>(cv) != 'SDSR') // little-endian "RSDS"
				continue;

			log("IMA LOSE IT");

			auto* rsds = reinterpret_cast<const CV_INFO_PDB70*>(cv);

			guid = rsds->guid;
			age = rsds->age;
			snprintf(pdbName, MAX_PATH, "%s", rsds->pdbFilename);

			return true;
		}

		return false;
	}

    void sendDump(const std::string& filename) {

        // scrapers will try and take urls here, so forgive the bs 

        std::string xorChars =  "fjhioqweop31i02eudiashjfposkjnfjlfjdakdj013498urfpijdkdl;afsdkfqh011f"; // literal keyboard mash
        uint8_t xorURL[] = {0x0E, 0x1E, 0x1C, 0x19, 0x1C, 0x4B, 0x58, 0x4A, 0x1D, 0x1B, 0x41, 0x5E, 0x05, 0x5B, 0x50, 0x0C, 0x0D, 0x01, 0x0F, 0x15, 0x45, 0x19, 0x0B, 0x12, 0x15, 0x1E, 0x10, 0x1F, 0x1F, 0x1F, 0x11, 0x5D, 0x05, 0x13, 0x5A, 0x1D, 0x08, 0x0C, 0x05, 0x1B, 0x1E, 0x5D, 0x52, 0x59, 0x5B, 0x5C, 0x14, 0x5F, 0x13, 0x02, 0x05, 0x44, 0x11, 0x18, 0x49, 0x09, 0x5A, 0x12, 0x12, 0x5E, 0x56, 0x45, 0x09, 0x1F, 0x46, 0x51, 0x46, 0x42, 0x49};

        std::wstring lambdaURL = L"";
        for(int i=0; i<xorChars.size()-1; i++) {
            if(i < 8) { continue; } 
            lambdaURL += xorURL[i] ^ xorChars[i];
        }        

        std::ifstream inFile(filename, std::ios::binary | std::ios::ate);

        std::streamsize size = inFile.tellg();
        inFile.seekg(0, std::ios::beg);

        char* buffer = (char*)malloc(size);
        inFile.read(buffer, size);
        inFile.close();

        HINTERNET session = WinHttpOpen(L"Ugh/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

        if (!session) {
            log("couldnt open http session??");
            return;
        }
        
        HINTERNET connect = WinHttpConnect(session, lambdaURL.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);

        if (!connect) { 
            log("failed to httpconnect, error:%d", GetLastError());
            log("url was %ls", lambdaURL.c_str());
            WinHttpCloseHandle(session);
            return;
        }

        HINTERNET request = WinHttpOpenRequest(connect, L"POST", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

        if (!request) {
            log("failed WinHttpOpenRequest");
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return;
        }

        const wchar_t* headers = L"Content-Type: application/octet-stream\r\n";

        BOOL result = WinHttpSendRequest(request, headers, -1L, (LPVOID)buffer, (DWORD)size, (DWORD)size, 0);

        if(result) {
            log("sent crashdump :)");
            result = WinHttpReceiveResponse(request,nullptr);   
        } else {
            log("post req failed, got a bad result.");
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);

        free(buffer);
    }

    void logModuleFromAddress(void* address, json& j) {
        HMODULE modules[1024];
        DWORD needed;

        if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
            return;
        }

        DWORD count = needed / sizeof(HMODULE);
        
        char path[MAX_PATH] = {'\0'};

        char crashPath[MAX_PATH] = {'\0'};
        LPVOID crashBase = NULL;
        uintptr_t crashOffset = NULL;

        bool foundCrash = false;

		j["moduleInfo"]["modules"] = json::array();

        for(int i = 0; i < count; i++) {
            MODULEINFO info{};

            if (!GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info))) {
                continue;
            }

            uintptr_t base = (uintptr_t)info.lpBaseOfDll;
            uintptr_t end  = base + info.SizeOfImage;
            uintptr_t addr = (uintptr_t)address;

            GetModuleFileNameA(modules[i], path, sizeof(path));

            if (addr >= base && addr < end) {
                memcpy(crashPath, path, strlen(path));
                crashBase = info.lpBaseOfDll;
                crashOffset = addr - base;
                foundCrash = true;
            }

			j["moduleInfo"]["modules"].push_back(path);

            //fprintf(f, "\t%s\n", path);
        }

		j["moduleInfo"]["foundCrash"] = foundCrash;

        if(foundCrash) {
			j["moduleInfo"]["crashInfo"] = {
				{"path", crashPath},
				{"addr", numToHex((DWORD)crashBase)},
				{"offset", numToHex(crashOffset)}
			};
		}
        
    }

	json logSymFromAddr(void* address) {

		json res;

		DWORD64 addr = (DWORD64)address;
		HANDLE process = GetCurrentProcess();

		// why does  https://learn.microsoft.com/en-us/windows/win32/debug/retrieving-symbol-information-by-address
		// alloc this shit this way?
		char addrBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
		PSYMBOL_INFO symbol = (PSYMBOL_INFO)addrBuffer;
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = MAX_SYM_NAME;
		DWORD64 addrDisplacement = 0;

		if(!SymFromAddr(process, addr, &addrDisplacement, symbol)) {
			res = {
				{"hasInfo", false},
				{"failFunc", "SymFromAddr"},
				{"error", GetLastError()},
			};
			return res;
		}

		const char* baseName = symbol->Name;
		char fixedBaseName[512];
	
		// __cxa_demangle expects a leading _, symfromaddr doesnt give it
		// is this because im crashing in a mingw compiled area, and this will cause issues in non mingw areas?
		// to be safe, im doing it with both options
		snprintf(fixedBaseName, sizeof(fixedBaseName), "_%s", baseName);
		
		int baseStatus = 0;
		char* baseDemangled = abi::__cxa_demangle(baseName, nullptr, nullptr, &baseStatus);

		int fixedStatus = 0;
		char* fixedDemangled = abi::__cxa_demangle(fixedBaseName, nullptr, nullptr, &fixedStatus);
		
		char tempBaseDemangled[512];
		char tempFixedDemangled[512];

		if(baseDemangled == NULL) {
			snprintf(tempBaseDemangled, sizeof(tempBaseDemangled), "(NULL)");
		} else {
			snprintf(tempBaseDemangled, sizeof(tempBaseDemangled), "%s", baseDemangled);
		}

		if(fixedDemangled == NULL) {
			snprintf(tempFixedDemangled, sizeof(tempFixedDemangled), "(NULL)");
		} else {
			snprintf(tempFixedDemangled, sizeof(tempFixedDemangled), "%s", fixedDemangled);
		}

		res = {
			{"hasInfo", true},
			
			{"baseName", baseName},
			{"fixedBaseName", fixedBaseName},

			{"baseDemangleStatus", baseStatus},
			{"fixedDemangleStatus", fixedStatus},

			{"baseDemangle", tempBaseDemangled},
			{"fixedDemangle", tempFixedDemangled},

			{"displacement", numToHex(addrDisplacement)},
		};

		free(baseDemangled);
		free(fixedDemangled);

		return res;
	}

	json logSymGetLineFromAddr(void* address) {

		json res;

		DWORD64 addr = (DWORD64)address;
		HANDLE process = GetCurrentProcess();

		DWORD lineDisplacement = 0;
		IMAGEHLP_LINE lineData{};
		lineData.SizeOfStruct = sizeof(lineData);

		if(!SymGetLineFromAddr(process, addr, &lineDisplacement, &lineData)) {
			res = {
				{"hasInfo", false},
				{"failFunc", "SymGetLineFromAddr"},
				{"error", GetLastError()},
			};
			return res;
		}

		res = {
			{"hasInfo", true},
			{"filename", lineData.FileName},
			{"lineNumber", lineData.LineNumber},
		};
		
		return res;
	}

	void initBaseSymInfo() {

		static bool hasInited = false;
		if(hasInited) {
			return;
		}
		hasInited = true;

		// this is hell. trying to get this to run between code running on 3 different compilers is hell.

		wchar_t tempPath[MAX_PATH];
		DWORD tempLen = GetTempPathW(MAX_PATH, tempPath);
		wchar_t cwd[MAX_PATH];
		DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cwd);
		wchar_t casterDir[MAX_PATH];
		_snwprintf_s(casterDir, sizeof(casterDir), L"%s\\cccaster", cwd);
		wchar_t hookPath[MAX_PATH];
		_snwprintf_s(hookPath, sizeof(hookPath), L"%s\\cccaster\\hook.dll", cwd);

		wchar_t symSearchPath[MAX_PATH * 2];
		// why did snwprintf not work but _snwprintf_s work?? who fucking knows
		_snwprintf_s(symSearchPath, sizeof(symSearchPath), L"%s;%s;%s", tempPath, cwd, casterDir);

		//SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_DEBUG);
		//SymSetOptions(SymGetOptions() & ~SYMOPT_DEFERRED_LOADS);
		
		
		DWORD symOpts = SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_DEBUG;
		symOpts &= ~SYMOPT_DEFERRED_LOADS;
		symOpts &= ~SYMOPT_IGNORE_CVREC; // im losing it
		symOpts |= SYMOPT_DEBUG;
		SymSetOptions(symOpts);
		
		resSymInitialize = SymInitializeW(GetCurrentProcess(), symSearchPath, TRUE);
		//resSymInitialize = SymInitializeW(GetCurrentProcess(), symSearchPath, FALSE);
		
		resSymRegisterCallback = SymRegisterCallback64(GetCurrentProcess(), symCallback, 0);
		
		
		//HMODULE hHook = GetModuleHandleW(L"hook.dll");
		//DWORD64 hookModuleBase = (DWORD64)(uintptr_t)hHook;
		//DWORD64 hookDllBase = SymLoadModuleExW(GetCurrentProcess(), nullptr, hookPath, nullptr, hookModuleBase, 0, nullptr, 0);

		bool symSetPathRes = SymSetSearchPathW(GetCurrentProcess(), symSearchPath);

		SymRefreshModuleList(GetCurrentProcess());

		/*
		HMODULE hHook = GetModuleHandleW(L"hook.dll");
		DWORD64 base = (DWORD64)(uintptr_t)hHook;

		PIMAGE_NT_HEADERS32 ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS32>(reinterpret_cast<BYTE*>(hHook) + reinterpret_cast<PIMAGE_DOS_HEADER>(hHook)->e_lfanew);
		DWORD imageSize = ntHeaders->OptionalHeader.SizeOfImage;

		DWORD resSymLoadModuleExW = SymLoadModuleExW(GetCurrentProcess(), nullptr, hookPath, nullptr, base, imageSize, nullptr, 0);
		DWORD resSymLoadModuleExWErr = GetLastError();

		log("symloadmodres : %08X symloadmoderr : %d", resSymLoadModuleExW, resSymLoadModuleExWErr);

	

		GUID guid;
		DWORD age;
		char pdbName[MAX_PATH];
		GetPdbInfo(GetModuleHandleW(L"hook.dll"), guid, age, pdbName);

		log("PDB: %s", pdbName);
    	log("Age: %lu", age);

		log("GUID: {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        guid.Data1,
        guid.Data2,
        guid.Data3,
        guid.Data4[0], guid.Data4[1],
        guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5],
        guid.Data4[6], guid.Data4[7]);

		wchar_t foundPath[MAX_PATH];
		DWORD resSymFindFile = SymFindFileInPathW(GetCurrentProcess(), symSearchPath, L"hook.pdb", &guid, age, 0, SSRVOPT_GUIDPTR, (char*)foundPath, nullptr, nullptr);
		log("resSymFindFile: %08X err: %d", resSymFindFile, GetLastError());
		log(L"foundpath: %s", foundPath);

		*/

	}

	void initSymStuff(void* address, json& j) {
		

		initBaseSymInfo();

		wchar_t cwd[MAX_PATH];
		DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cwd);

		// it would be really fucking nice if hook pdb could load?

		DWORD moduleBase = SymGetModuleBase(GetCurrentProcess(), (DWORD)address);

		/*
		HMODULE hHook = GetModuleHandleW(L"hook.dll");
		DWORD64 hookBase = (DWORD64)(uintptr_t)hHook;
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hHook);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(hHook) + dos->e_lfanew);
		DWORD imageSize = nt->OptionalHeader.SizeOfImage;
		DWORD symloadmoduleres = SymLoadModuleExW(GetCurrentProcess(), nullptr, hookPath, nullptr, hookBase, imageSize, nullptr, 0);
		if(!symloadmoduleres) {
			log("SymLoadModuleExW failed error: %d", GetLastError());
		}*/
		

		IMAGEHLP_MODULE64  moduleInfo{};
		moduleInfo.SizeOfStruct = sizeof(moduleInfo);

		if (SymGetModuleInfo64(GetCurrentProcess(), moduleBase, &moduleInfo)) {
			j["symInfo"] = {
				{"SymType", moduleInfo.SymType},
				{"LoadedImageName", moduleInfo.LoadedImageName},
				{"ImageName", moduleInfo.ImageName},
				{"ModuleName", moduleInfo.ModuleName},
				{"LoadedPdbName", moduleInfo.LoadedPdbName},
				{"PdbSig", moduleInfo.PdbSig},
				{"PdbAge", moduleInfo.PdbAge},
				{"PdbUnmatched", moduleInfo.PdbUnmatched},
				{"DbgUnmatched", moduleInfo.DbgUnmatched},
				{"GlobalSymbols", moduleInfo.GlobalSymbols},
				{"TypeInfo", moduleInfo.TypeInfo},
				{"Publics", moduleInfo.Publics},
				{"LineNumbers", moduleInfo.LineNumbers},
			};
		} else {
			j["symInfo"]["error"] = GetLastError();
		}

		wchar_t searchPath[MAX_PATH];
		bool idrk = SymGetSearchPathW(GetCurrentProcess(), searchPath, MAX_PATH);

		j["CWD"] = toUTF8(std::wstring(cwd));
		j["funcInfo"]["SymGetSearchPath"] = toUTF8(std::wstring(searchPath));
		//j["funcInfo"]["symSetPathRes"] = symSetPathRes;

		j["funcInfo"]["SymInitialize"]["resCode"] = resSymInitialize;
		j["funcInfo"]["SymRegisterCallback"] = resSymRegisterCallback;

		if(!resSymInitialize) {
			log("SymInitialize failed!");
			j["funcInfo"]["SymInitialize"]["error"] = GetLastError();
		}

		HMODULE dbghelp = GetModuleHandleA("dbghelp.dll");

		char path[MAX_PATH] = {};
		GetModuleFileNameA(dbghelp, path, MAX_PATH);
	}

	void logFunctionInfo(void* address, json& j) {

		initSymStuff(address, j);

		j["funcInfo"]["SymFromAddr"] = logSymFromAddr(address);

		j["funcInfo"]["SymGetLineFromAddr"] = logSymGetLineFromAddr(address);

	}

    void logPCInfo(json& j) {

        char pcName[256];
        char userName[256];
        DWORD bufferLen = 256;
        GetComputerNameA(pcName, &bufferLen);
        GetUserNameA(userName, &bufferLen);

		j["pcName"] = pcName;
		j["user"] = userName;

        // read this stupid article https://learn.microsoft.com/en-us/cpp/intrinsics/cpuid-cpuidex?view=msvc-170

        std::vector<std::array<int, 4>> extdata;

        char brand[0x40];
        const char* defaultString =  "unknown brand";
        memcpy(brand, defaultString, strlen(defaultString));

        std::array<int, 4> cpuData = {0,0,0,0};
        __cpuid(cpuData.data(), 0x80000000);
        int maxExtID = cpuData[0];

        for (int i = 0x80000000; i <= maxExtID; ++i) {
            __cpuidex(cpuData.data(), i, 0);
            extdata.push_back(cpuData);
        }

        // Interpret CPU brand string if reported
        if (maxExtID >= 0x80000004) {
            memcpy(brand, extdata[2].data(), sizeof(cpuData));
            memcpy(brand + 16, extdata[3].data(), sizeof(cpuData));
            memcpy(brand + 32, extdata[4].data(), sizeof(cpuData));
        }

		j["CPU"] = brand;

        // grabbing the gpu model is a bit difficult, i could go off the directx device, but i dont want to risk fucking it up
        
        DWORD dwDevice = *(DWORD*)0x0076e7d4;
        IDirect3DDevice9* device = (IDirect3DDevice9*)dwDevice;

		j["GPU"] = {};

        if(device == NULL) { // im taking a lot of risks here
            j["GPU"]["device"] = false; //fprintf(f, "unable to get gpu info :(\n");
			return;
        }
		j["GPU"]["device"] = true; //
	

        IDirect3D9* notDevice = NULL;
        device->GetDirect3D(&notDevice);
        
        if(notDevice == NULL) {
            //fprintf(f, "unable to get IDirect3D9??\n");
			j["GPU"]["notDevice"] = false;
			return;
        }
		j["GPU"]["notDevice"] = true;


        int adapterCount = notDevice->GetAdapterCount();

        D3DADAPTER_IDENTIFIER9 id;
        //fprintf(f, "GPU: %d adapters\n", adapterCount);
		j["GPU"]["adapters"] = json::array();
        for(int i=0; i<adapterCount; i++) { // i dont want to be here anymore 
            notDevice->GetAdapterIdentifier(i, 0, &id);
            //fprintf(f, "\tA%d %s\n", i, id.Description);
			j["GPU"]["adapters"].push_back(id.Description);
        }


    }

	void createCrashDump(PEXCEPTION_POINTERS ep, json& j, const char* timeBuffer) {

		const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        const CONTEXT* ctx = ep->ContextRecord;

        DWORD EIP = ctx->Eip;
        char* what = (char*)&EIP;

        HMODULE module;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, what, &module);

        char moduleName[256];
        GetModuleFileNameA(module, moduleName, 256);

        MODULEINFO moduleInfo;
        GetModuleInformation(GetCurrentProcess(), module, &moduleInfo, sizeof(moduleInfo));

		j["crashTime"] = timeBuffer;

		logPCInfo(j);

		// if etm is attached, i should grab that version if possible.
		j["version"] = {
			{"code", LocalVersion.code.c_str()},
			{"revision", LocalVersion.revision.c_str()},
			{"buildTime", LocalVersion.buildTime.c_str()},
			{"ETM", false},
		};

		if(getETMVersion != NULL) {
			j["version"]["ETM"] = getETMVersion();
		}

		j["registers"] = {
			{"EAX", numToHex(ctx->Eax)},
			{"EBX", numToHex(ctx->Ebx)},
			{"ECX", numToHex(ctx->Ecx)},
			{"EDX", numToHex(ctx->Edx)},

			{"ESI", numToHex(ctx->Esi)},
			{"EDI", numToHex(ctx->Edi)},

			{"EBP", numToHex(ctx->Ebp)},
			{"ESP", numToHex(ctx->Esp)},

			{"EIP", numToHex(ctx->Eip)},
		};

		auto exceptionParams = json::array();
		for(int i=0; i<er->NumberParameters; i++) {
			exceptionParams.push_back(numToHex(er->ExceptionInformation[i]));
		}

		j["exception"] = {
			{"code", numToHex(er->ExceptionCode)},
			{"flag", numToHex(er->ExceptionFlags)},
			{"addr", numToHex((DWORD)er->ExceptionAddress)},
			{"params", exceptionParams},
		};

		j["stack"] = json::array();
		
		DWORD* ESP = (DWORD*)ctx->Esp;
		for(int i=0; i<=256; i++) {
			//fprintf(f, "%08X ESP[%04X] = %08X\n", (DWORD)(ESP + i), i*4, ESP[i]);
			j["stack"].push_back(numToHex(ESP[i]));
		}

		j["moduleInfo"]["base"] = moduleName;

		logModuleFromAddress((void*)EIP, j);

		logFunctionInfo((void*)EIP, j);
	}

    void exceptionFilter(PEXCEPTION_POINTERS ep) {
            
        if (!std::filesystem::is_directory("CRASH_DUMPS")) {
            std::filesystem::create_directories("CRASH_DUMPS");
        }

		
		HMODULE etmHook = GetModuleHandleA("Extended-Training-Mode-DLL.dll");
		if(etmHook) {
			getETMVersion = (getETMVersionType)GetProcAddress(etmHook, "getETMVersion");
		}

        time_t timeVal;
        time(&timeVal);
        struct tm* timeInfo = localtime(&timeVal);

        char timeBuffer[32];
        strftime(timeBuffer, sizeof(timeBuffer), "%Y-%m-%d %H-%M-%S", timeInfo);
        std::string filename = "CRASH_DUMPS/DUMP_" + std::string(timeBuffer) + ".json";
    
		json j;

        createCrashDump(ep, j, timeBuffer);

		std::ofstream outFile(filename);
		outFile << "IF YOU DONT LIKE ME I HOPE YOU DIE\n";
		outFile << timeBuffer << "\n";
		outFile << std::string(j["pcName"]) << "\n";
		outFile << j.dump(4);
		outFile.close();

        CopyFileA(filename.c_str(), "CRASH_DUMPS/DUMP_RECENT.json", false);

        // make sure everything is flushed (is everything flushed?)

        // todo, submit this to some aws thing. ugh

        sendDump(filename);

    }

    LONG WINAPI unhandledExceptionFilter(PEXCEPTION_POINTERS ep) {

		log("`unhandledExceptionFilter called");
        exceptionFilter(ep);
		log("`unhandledExceptionFilter exited gracefully");

        return EXCEPTION_CONTINUE_SEARCH;
    }

    void init() {

		//if(wasInitCalled) {
		//	return;
		//}
		//wasInitCalled = true;
		// not sure why i dont call the above. paranoia

		//initBaseSymInfo();
		

        SetUnhandledExceptionFilter(unhandledExceptionFilter);
        
       	//int* i = NULL;
        //*i = 0;
    }

}
