

// ugh god 


// cd \\wsl.localhost\Ubuntu-24.04
// frida -n MBAA.exe -l fridaTest.js
// just type exit ig. i wish there was a better way

//const kernel32 = Process.getModuleByName("kernel32.dll");
//let qpcAddr = kernel32.getExportByName('QueryPerformanceCounter');
//var qpcFunc = new NativeFunction(qpcAddr, 'bool', ['pointer'], {abi: 'stdcall'});

Number.prototype.toHex = function() {
    return "0x" + this.toString(16);
};

Number.prototype.pad = function(n) {
    return this.toString().padEnd(n);
};

Number.prototype.padDec = function(n) {
    return this.toFixed(4).padStart(n);
};

String.prototype.pad = function(n) {
    return this.toString().padEnd(n);
};



const kernel32 = Process.getModuleByName("kernel32.dll");
const addr = kernel32.getExportByName("QueryPerformanceCounter");

const funcAddr = new NativePointer(0x0051b1d4)

function badSleep(ms) {
	let stop = Date.now() + ms;
    while(Date.now() < stop) {}
}

const funcAddr2 = new NativePointer(funcAddr.readU32())

const qpcFunc = new NativeFunction(
	funcAddr2,
	"bool",
	["pointer"],
	{ abi: "stdcall"}
);


//console.log("funcAddr ", funcAddr);
//console.log("funcData ", funcAddr2)
//console.log("func     ", qpcFunc)

//let counterData = Memory.alloc(8);
//let counterPointer = new NativePointer(counterData);
//
//counterData = 1;

function getTime() {
    //console.log("a  ", qpcFunc.toString());
	let counterData = Memory.alloc(8);
	let counterPointer = new NativePointer(counterData);
    qpcFunc(counterPointer);
    return counterPointer.readU64() / 10000.0;
    //return Process.getCurrentTime();
}

//console.log(counterPointer.readU64())
//console.log("time:", getTime())
//console.log(counterPointer.readU64())
//console.log("time:", getTime())
//console.log("time:", getTime())
//console.log(counterPointer.readU64())

let tick = 0;
let tickLimit = 100;

class Timer {

    constructor(addr, name, depth) {

        this.name = name;
        this.addr = addr;
		this.depth = depth;

        this.count = 0;
		this.startTime = 0
        this.time = 0.0;

		const timer = this; // this pisses me the fuck off

        Interceptor.attach(ptr(addr), {
            onEnter() {
                timer.count += 1;
                timer.startTime = getTime();
            }, 

            onLeave() {
				let temp = getTime();
				let delta = temp - timer.startTime;
                timer.time += delta;
            }
        });

    }

	display(totalTime) {
		//console.log(this.count.padEnd(16, " "), this.name.padEnd(16, " "), this.time.padEnd(16, " "));

		let t = this.time / tickLimit;
	
		let percent = 100 * this.time / totalTime;

		let offset = this.depth * 4;

		let offsetString = "    ".repeat(this.depth);
		if(this.depth == 0) {
			offsetString = "";
		}

		console.log(offsetString + this.count.pad(16) + this.name.pad(80-offset) + t.toFixed(5).pad(16) + percent.padDec(16))

		if(this.depth != 0) {
			return 0.0;
		}

		return percent;
	}

	reset() {
		this.count = 0;
		this.time = 0;
	}

}

let doPrint = true;
const hookModule = Process.getModuleByName("hook.dll");

const d3Module = Process.getModuleByName("d3d9.dll")

//doPrint = false;

if(!doPrint) {
	for (const exp of d3Module.enumerateExports()) {
		console.log(exp.type, exp.name, exp.address);
	}
}


//while(true) {}

let timers = [

	new Timer(hookModule.getExportByName("callback"), "casterCallback", 0),
		//new Timer(hookModule.getExportByName("_ZN12EventManager3getEv"), "_ZN12EventManager3getEv", 1),
		//new Timer(hookModule.getExportByName("_ZN14ProcessManager14writeGameInputEhtt"), "_ZN14ProcessManager14writeGameInputEhtt", 1),
		//new Timer(hookModule.getExportByName("_ZN16RefChangeMonitorI8VariablejE5checkEv"), "_ZN16RefChangeMonitorI8VariablejE5checkEv", 1),
		//	new Timer(hookModule.getExportByName("_ZN14NetplayManager11updateFrameEv"), "_ZN14NetplayManager11updateFrameEv", 2),
		//	new Timer(hookModule.getExportByName("_ZN14ProcessManager14writeGameInputEhtt"), "_ZN14ProcessManager14writeGameInputEhtt", 2),
		//	new Timer(hookModule.getExportByName("_ZN13ChangeMonitor3getEv"), "_ZN13ChangeMonitor3getEv", 2),
		//	new Timer(hookModule.getExportByName("_ZN14NetplayManager8getInputEh"), "_ZN14NetplayManager8getInputEh", 2),
		new Timer(hookModule.getExportByName("_ZN7DllMain12changedValueE8Variablejj"), "_ZN7DllMain12changedValueE8Variablejj", 1),
			new Timer(hookModule.getExportByName("_ZN7DllMain9frameStepEv"), "_ZN7DllMain9frameStepEv", 2),
				new Timer(hookModule.getExportByName("_ZN14NetplayManager11updateFrameEv"), "_ZN14NetplayManager11updateFrameEv", 3),
				new Timer(hookModule.getExportByName("_ZN14ProcessManager14writeGameInputEhtt"), "_ZN14ProcessManager14writeGameInputEhtt", 3),
				new Timer(hookModule.getExportByName("_ZN13ChangeMonitor3getEv"), "_ZN13ChangeMonitor3getEv", 3),
				new Timer(hookModule.getExportByName("_ZN13ChangeMonitor5checkEv"), "_ZN7DllMain12changedValueE8Variablejj", 3),
				new Timer(hookModule.getExportByName("_ZN14ProcessManager6isWineEv"), "_ZN14ProcessManager6isWineEv", 3),
				new Timer(hookModule.getExportByName("_ZN18DllRollbackManager15saveRerunSoundsEj"), "_ZN18DllRollbackManager15saveRerunSoundsEj", 3),
				new Timer(hookModule.getExportByName("_ZN7DllMain15frameStepNormalEv"), "_ZN7DllMain15frameStepNormalEv", 3),
					//new Timer(hookModule.getExportByName("_ZN7DllMain30frameStepNormalSwitchStatementEv"), "_ZN7DllMain30frameStepNormalSwitchStatementEv", 4),
					new Timer(hookModule.getExportByName("_ZN14DllChatManager9frameStepERSt10shared_ptrI6SocketE"), "_ZN14DllChatManager9frameStepERSt10shared_ptrI6SocketE", 4),
					//new Timer(hookModule.getExportByName("_ZN7DllMain16frameStepPollingEv"), "_ZN7DllMain16frameStepPollingEv", 4),
					
	

				new Timer(hookModule.getExportByName("_ZN16SpectatorManager19frameStepSpectatorsEv"), "_ZN16SpectatorManager19frameStepSpectatorsEv", 3),
				new Timer(hookModule.getExportByName("_ZN14NetplayManager8getInputEh"), "_ZN14NetplayManager8getInputEh", 3),
					
				

				//new Timer(hookModule.getExportByName("_ZN13KeyboardState6isDownEj"), "_ZN13KeyboardState6isDownEj", 5),
				
				
			
	new Timer(0x0040e390, "loop0", 0),
	
	//new Timer(0x0048e0a0, "dawgNoClue"),
    //new Timer(0x00432c50, "advanceFrame"),
		//new Timer(0x0043b8d0, "somethingRelatedToAnimSpeed?"),
		//new Timer(0x00432b40, "GoesToGameLoop2"),
			//new Timer(0x00433770, "u_GameLoop"),
				//new Timer(0x004de200, "FUN_004de200"),
				//new Timer(0x00433ad0, "FUN_00433ad0"),
				//new Timer(0x004337e0, "UpdateGame"),
					//new Timer(0x00423570, "BattleMode"),
						//new Timer(0x004235c0, "BattleScene"),
						//	new Timer(0x00423630, "UpdateBattleScene"),
						//	new Timer(0x00423860, "DrawBattleScene"),
				//new Timer(0x004bc800, "u_GetSystemTime"),
		//new Timer(0x0040dae0, "setsRenderTargets"),
		//new Timer(0x0043b950, "somethingTimeRelated"), 
			//new Timer(0x00421750, "FUN_00421750"),

	//new Timer(0x0043b8d0, "animSpeedd"),

	//new Timer(0x004330c0, "doesAlot"),
		//new Timer(0x0040e100, "linkedListSomethingMystery"),
		//new Timer(0x00432ce0, "callsTakeScreenshot"),
		//new Timer(0x004c04e0, "callWhichLeadsToPrimDraw2"), 
		//new Timer(0X0040dc50, "onlyFuncWhichCallsTheDrawPrims_MAYBE"),
		//new Timer(0x0048e470, "FUN_0048e470"),
		//new Timer(0x00432d30, "displayFPSVal"),	
		//new Timer(0x00432e30, "callsImportantDraw8VeryImportant"),
		//new Timer(0x00414e20, "linkedListAppend"),
	//new Timer(0x00433490, "callsDraw1"),

	//new Timer(0x00433310, "callsPresent2"),
		//new Timer(0x0041fd60, "framestepPauseHookFunc"),
		//new Timer(0x0041fcf0, "doesSomethingRelatingToFps2"),
		new Timer(0x004bdbc0, "callsDirectXPresent", 2),
			new Timer(hookModule.getExportByName("DX9_Present@20"), "DX9_Present@20", 3),
				//new Timer(hookModule.getExportByName("SwapOld"), "SwapOld", 4),
				new Timer(hookModule.getExportByName("_Z17PresentFrameBeginP16IDirect3DDevice9"), "_Z17PresentFrameBeginP16IDirect3DDevice9", 4),
				//new Timer(hookModule.getExportByName("_Z15PresentFrameEndP16IDirect3DDevice9"), "_Z15PresentFrameEndP16IDirect3DDevice9", 4),

				new Timer(hookModule.getExportByName("_Z15DX9_HooksVerifyP16IDirect3DDevice9"), "_Z15DX9_HooksVerifyP16IDirect3DDevice9", 4),

				new Timer(hookModule.getExportByName("_Z17actuallyDoPresentP16IDirect3DDevice9PK7tagRECTS3_P6HWND__Pv"), "_Z17actuallyDoPresentP16IDirect3DDevice9PK7tagRECTS3_P6HWND__Pv", 4),
				
				new Timer(hookModule.getExportByName("_Z17renderOverlayTextP16IDirect3DDevice9RK13_D3DVIEWPORT9"), "_Z17renderOverlayTextP16IDirect3DDevice9RK13_D3DVIEWPORT9", 4),
				
				
			
	
		//new Timer(0x0043b950, "somethingTime"),
	//new Timer(0x004bf970, "unknownFunc"),
	//new Timer(0x004151f0, "unknownFunc2"),
	//new Timer(0x00406680, "unknownFunc3"), // some issues with this func?
	//new Timer(0x004be8b0, "unknownFunc4"),
	//new Timer(0x0040e220, "unknownFunc5"),
]


let prevTime = 0;
let fpsMeasure = 0;

let totalTime = 0;

function actuallyDisplayShit() {
    
    console.log('\u001b[2J')
    console.log('\u001b[H');

	//console.log(casterCallback)
	let totalPercent = 0.0

    for(const t of timers) {
        totalPercent += t.display(totalTime)
    }

	console.log("FPS: ", 100000.0/fpsMeasure)
	console.log("totalPercent: ", totalPercent.toFixed(4).pad(16))
	
}

let isInit = true;


/*
Interceptor.attach(ptr(0x0040e390), {

    onEnter(args) {

		let tempTime = getTime()
		fpsMeasure += tempTime - prevTime
		prevTime = tempTime

		if(tick == 0) {
			fpsMeasure = 0
			for(let t of timers) {
				t.reset();
        	}
		}
       

        //console.log(sprintf("value = %d, address = %p", 12, 2));  
    },

    onLeave() {

        tick++;
        if(tick < tickLimit) {
            return;
        }
        tick = 0;

		totalTime = getTime() - totalTime;

        actuallyDisplayShit();

		totalTime = getTime();
    }


});
*/

Interceptor.attach(ptr(0x0040d330), function () {
    //console.log("instruction executed");

	let tempTime = getTime()
	fpsMeasure += tempTime - prevTime
	prevTime = tempTime

	tick++;
	if(tick < tickLimit) {
		return;
	}
	tick = 0;

	totalTime = getTime() - totalTime;

	if(doPrint) {
		actuallyDisplayShit();
	}
	

	totalTime = getTime();

	fpsMeasure = 0;
	for(let t of timers) {
		t.reset();
	}
});

