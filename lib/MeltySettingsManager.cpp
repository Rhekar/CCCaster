 
#include "MeltySettingsManager.hpp"
#include <fstream>

/*

this file is here to manage _AAGameData.dat and _AAPlayData.dat

*/



void writeData(const std::string& fileName, DWORD offset, auto val) {

    std::ifstream inFile(fileName, std::ios::binary | std::ios::ate);
	if (!inFile.good()) {
		return;
	}

	int bufferSize = inFile.tellg();
	inFile.seekg(0, std::ios::beg);
	BYTE* buffer = (BYTE*)malloc(bufferSize);
	inFile.read((char*)buffer, bufferSize);
    inFile.close();

    using T = decltype(val);
    *(T*)(buffer+offset) = val;

    std::ofstream outFile(fileName, std::ios::binary | std::ios::ate);
    outFile.write((char*)buffer, bufferSize);
    outFile.close();

    free(buffer);

}

void writeBGMVolume(int v) {

    if(v == 0) {
        v = 21;
    } else {
        v = 20 - v;
    }

    writeData("./System/_AAGameData.dat", 0x144, v);
}

void writeSFXVolume(int v) {

    if(v == 0) {
        v = 21;
    } else {
        v = 20 - v;
    }

    writeData("./System/_AAGameData.dat", 0x148, v);

}
