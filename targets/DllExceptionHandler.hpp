#pragma once 

namespace ExceptionHandler {

	extern int resSymInitialize;

	extern bool wasInitCalled;

    void init();

    // is it ok to not have the other funcs in here? it makes it such that i dont need extra includes
}

