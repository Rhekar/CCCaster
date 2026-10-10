#ifndef REPLAYHEADER
#define REPLAYHEADER


#define FILE_HEADER "MBAAReplayFile\0\0"
#define UNK1 0x040000c0

#include <stdint.h>
#include <iostream>
#include <vector>
#include <string>
#include "Logger.hpp"

#define BUTTON_UP(BEFORE,AFTER) uint8_t (~((~BEFORE)|AFTER))
#define BUTTON_DOWN(BEFORE,AFTER) uint8_t (~((~AFTER)|BEFORE))

class ReplayCreator
{
public:

    enum MOON {C, F, H};

    struct CharData
    {
        int isCpu;
        int character;
        int moon;
        int color;
    };
    struct Input
    {
      //00000000
      //000EDCBA
      uint8_t duration;
      uint8_t direction;
      uint8_t buttonHold;
      uint8_t buttonDown;
      uint8_t buttonUp;
      uint8_t button4;

      bool operator==( const Input& in2) const
      {
          return this->duration ==in2.duration &&
                 this->direction == in2.direction &&
                 this->buttonHold == in2.buttonHold &&
                 this->buttonDown == in2.buttonDown &&
              ((this->buttonUp == in2.buttonUp)// ||
               //(this->buttonUp == 7 && in2.buttonUp == 9) ||
               //(this->buttonUp == 9 && in2.buttonUp == 3) ||
               // (this->buttonUp == 9 && in2.buttonUp == 0)
               ) &&
                 this->button4 == in2.button4;
      }

      bool operator!=( const Input& in2) const
      {
          return !(*this == in2);
      }
    };

    struct Round
    {
      //seems to affect CPU AI
      char unkrng1[44];
      char z2[88];
      char fdataStart[8];
      int lenp1Inputs;
      std::vector<Input> p1Inputs;
      int lenp2Inputs;
      std::vector<Input> p2Inputs;
      int lenp3Inputs;
      std::vector<Input> p3Inputs;
      int lenp4Inputs;
      std::vector<Input> p4Inputs;
      int lenRng;
      std::vector<uint32_t> rngstates;
      char nine[4];
      //maybe draw state?Only on draw final round, blackscreens if FF->0
      char zeroOrFF[4];
      char thirtyfour[4];
      char unk5[12*4]; //4 ??? 0x28 ???? 4 ???
      char thirtyfour2[4];
      char unk6[12*4];
      char eight1[4];
      //0x1027 = 100.00%
      char p1StartingMeter[4];
      char eight2[4];
      char p2StartingMeter[4];
      char eight3[4];
      char p3StartingMeter[4];
      char eight4[4];
      char p4StartingMeter[4];
    };

    struct ReplayFile
    {
      char headername[16];
      char unk1[4];
      char z1[4];
      uint16_t year;
      uint16_t month;
      uint16_t day;
      uint16_t hour;
      uint16_t minute;
      uint16_t second;
      char z0[4];
      int stage;
      int numPlayers;
      int damage;
      int numWins;
      int speed;
      CharData p1;
      CharData p2;
      int numRounds;
      std::vector<Round> rounds;
    };

    struct MoveData
    {
      int duration = 0;
      int numInputs = 0;
      uint8_t button = 0;
      uint8_t lastButton = 0;
      uint8_t direction = 0;
      uint8_t lastDirection = 0;
      uint8_t down = 0;
      uint8_t lastDown = 0;
      uint8_t up = 0;
      uint8_t lastUp = 0;
      bool last = true;
    };

    void dump( ReplayFile rf, char* fname );
    void load( ReplayFile* rf, char* fname );
    void fixReplay( ReplayFile* rf, char* fname, MoveData* prior );
    uint8_t getButton( unsigned int x );
    uint8_t getDirection( unsigned int x );
    std::string getDirIcon( uint8_t x );
    std::string getButtonIcon( uint8_t x );
    void printinfo( ReplayFile* rf );
    void printrepdiff( ReplayFile* rf1, ReplayFile* rf2, int p );
    void findrepdiff( ReplayFile* rf1, ReplayFile* rf2, int tol, int round );
    void findrngdiff( ReplayFile* rf1, ReplayFile* rf2 );
    void printInput( Input input );
    void printInput2( Input input );
    void printInput3( Input input );
    void printReplay( ReplayFile* rf, int start, int end, int round);
    void readFrameInput( MoveData* m, std::string frameinput );
    void copyToInput( MoveData* m, Input &input );
    void writeInputs( std::vector<Input> &inputs,
                      std::vector<std::string> &rawinputs,
                      MoveData* prior );
    void fixRng( ReplayFile* rf, char* fname );

private:
    int test;
};


#endif
