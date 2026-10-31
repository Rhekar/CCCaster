#include "DllChatManager.hpp"
#include "DllOverlayUi.hpp"
#include "NetLogger.hpp"
#include "KeyboardState.hpp"
#include "DllNetplayManager.hpp"
#include "DllOverlayPrimitives.hpp"

/*

issues:
need to see while typing
setting keys just fucks it.
switch to my own text renderer so i dont have to deal with this bs
move version numbers into the dll so i can check shit. bc i know for a fact i wont get everyone to update, and ppl should be able to use the chat without being scared of crash

*/

void DllChatManager::keyboardEvent ( uint32_t vkCode, uint32_t scanCode, bool isExtended, bool isDown ) {

	//log("%08X %08X %d %d", vkCode, scanCode, isExtended, isDown);

	//log("ugh");
	//log("%08X %08X %08X", code, wParam, lParam);

	if(isDown) {
		return;
	}

	// how the hell does this thing even access controllermanager?
	Lock lock ( ControllerManager::get().mutex );

	bool shiftState = KeyboardState::isDown(VK_LSHIFT) || KeyboardState::isDown(VK_RSHIFT);
	
	// ToAscii gave me trust issues. i just cant force myself to care.

	if(vkCode >= '0' && vkCode <= '9') {
		if(shiftState && vkCode == '1') {
			typingMessage += '!';
		} else {
			typingMessage += (char)vkCode;
		}
	} else if(vkCode >= 'A' && vkCode <= 'Z') {
		typingMessage += (char)vkCode + (shiftState ? 0 : 0x20);
	} else if(vkCode >= VK_OEM_1 && vkCode <= VK_OEM_2) {
		typingMessage += (char)(vkCode - (shiftState ? 0x80 : 0x90));
	} else if(vkCode == VK_SPACE) {
		typingMessage += (char)vkCode;
	} else if(vkCode == VK_BACK) {
		if(typingMessage.size() >= 1) {
			typingMessage.pop_back();
		} else {
			shouldStop = true; // if someone backspaces when theres nothing left, just dont send shit.
		}
	} else if(vkCode == VK_ENBABLE_CHAT) {
		shouldStop = true;
	}
}

void DllChatManager::frameStep(SocketPtr& dataSocket) {

	if(disable) { return; }

	while(!recvMsg.empty()) { // display recieved messages.
		ChatMessage m = recvMsg.front();
		recvMsg.pop_front();
		m.framesToDisplay = framesToDisplayMsg;
		displayQueue.push_front(m);
	}

	while(!sendMsg.empty()) { 
		ChatMessage m = sendMsg.front();
		sendMsg.pop_front();
		m.player = netManPtr->_localPlayer;
		if(dataSocket && dataSocket->isConnected()) {
			dataSocket->send(m);
		} 
		m.framesToDisplay = framesToDisplayMsg;
		displayQueue.push_front(m);	
	}

}

void DllChatManager::displayChat() {

	if(disable) { return; }

	int drawIndex = 1 + isTyping; // index 0 is below the screen, 1 is left for the typing dialogue. move the messages down if not typing.

	for(auto it = displayQueue.begin(); it != displayQueue.end(); ) {
		if(it->framesToDisplay < 0) {
			it = displayQueue.erase(it);
			continue;
		}
		DllOverlayUi::showChatMessage(drawIndex, *it);
		it->framesToDisplay--;
		++it;
		drawIndex++;
	}

	if(isTyping) {
		DllOverlayUi::showTypingDialogue(typingMessage);
	}
}

void DllChatManager::startTyping() {

	if(disable) { return; }

	if(isTyping) {
		return;
	}
	
	if(KeyboardState::isDown(VK_LMENU) || KeyboardState::isDown(VK_RMENU) || KeyboardState::isDown(VK_MENU) ||
	   KeyboardState::isDown(VK_LCONTROL) || KeyboardState::isDown(VK_RCONTROL) || KeyboardState::isDown(VK_CONTROL)) {
		return;
	}

	typingMessage = "";
	typeStartTime = lastCheckTime = TimerManager::get().getNow ( true );
	isTyping = true;
	shouldStop = false;

	KeyboardManager::get().matchedKeys.clear();
	KeyboardManager::get().ignoredKeys.clear();
	KeyboardManager::get().hook ( this, true );
}

void DllChatManager::recvMessage(const ChatMessage& m) {
	recvMsg.push_back(m);
}

void DllChatManager::sendMessage(const ChatMessage& m) {
	if(m.msg.size() >= 1) {
		if(LocalVersion.code != remoteVersion.code) {
			ChatMessage m;
			m.player = 0;
			m.msg = "Straight up, ur opponent hasnt updated, so i cant message them. deepest apologies.";
			recvMsg.push_back(m);
			return;
		}
		sendMsg.push_back(m);
	}
}

void DllChatManager::typeMessage() {

	// caster already has some keyboard manager thing. i should use that. 
	// issue. multiple keypresses in the space of a frame should all record, would suck if they didnt
	// keyboardstate.cpp seems to be what i want
	// god, in the pursuit of perfection, i end up with a bunch of comments and nothing else

	typingMessage += KeyboardState::getRecentPresses(lastCheckTime);
	lastCheckTime = TimerManager::get().getNow ( true );

	if(shouldStop) {
		if(lastCheckTime - typeStartTime > 100) {
			isTyping = false;
			ChatMessage newMsg;
			newMsg.msg = typingMessage;
			sendMessage(newMsg);
			KeyboardManager::get().unhook();
		} else {
			shouldStop = false;
		}
	}
}
