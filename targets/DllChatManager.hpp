#pragma once

#include "Messages.hpp"
#include <string>
#include <deque>
#include <atomic>

#include "ControllerManager.hpp"

#define VK_ENBABLE_CHAT ( VK_RETURN ) // could have this be something different? rebindable maybe? idrek, and idrec. this is a lie. i care deeply.

class DllChatManager 
	: public KeyboardManager::Owner
	{
public:

	void sendMessage(const ChatMessage& m);	
	void recvMessage(const ChatMessage& m);
	
	void frameStep(SocketPtr& dataSocket);
	void displayChat();

	void startTyping();
	void typeMessage();
	void keyboardEvent ( uint32_t vkCode, uint32_t scanCode, bool isExtended, bool isDown ) override;

	uint64_t typeStartTime = 0;
	std::atomic<bool> isTyping = false;
	std::atomic<bool> shouldStop = false; // this is ass code. god. please maddy actually fix this 
	std::string typingMessage;
	uint64_t lastCheckTime = 0;

	bool disable = false;

private:

	const static int framesToDisplayMsg = 60 * 7;

	std::deque<ChatMessage> recvMsg;
	std::deque<ChatMessage> sendMsg;

	std::deque<ChatMessage> displayQueue;

};

extern DllChatManager* chatManPtr;

extern Version remoteVersion;
