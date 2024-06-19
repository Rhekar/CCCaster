#include "CharacterSelect.hpp"
#include "Constants.hpp"
#include "DllTrialManager.hpp"
#include "DllTrialManager.hpp"
#include "DllOverlayUi.hpp"
#include "DllNetplayManager.hpp"
#include "ProcessManager.hpp"
#include "StringUtils.hpp"
#include "Sequences.hpp"

#include <fstream>
#include <locale>
#include <codecvt>
#include <shlwapi.h>
#include <string>
#include <windows.h>
#include <mmsystem.h>

using namespace std;

#define SYSTEM_ALERT_PREFEX "System"

namespace TrialManager
{
wstring comboName;
vector<wstring> comboTrialText;
vector<wstring> fullStrings;
string dtext;
int comboTrialTextAlign = 0;
int comboTrialLength = 0;
uint32_t comboTrialPosition = 0;
int currentTrialIndex = 0;
bool hideText = false;
LPDIRECT3DTEXTURE9 trialTextures = NULL;
int trialBGTextures = 0;
int trialInputTextures = 0;

// rework
bool playDemo = false;
bool showCombo = true;
int demoPosition = 0;
bool isRecording = false;
//Trial* currentTrial;
vector<Trial> charaTrials;
int trialScale;

bool comboDrop = false;
bool comboStart = false;
bool inputGuideEnabled = false;
bool inputEditorEnabled = false;
int inputEditorX = 0;
int inputEditorY = 0;
int inputEditorPosition = 0;
int inputEditorSpeed = 1;
uint16_t inputEditorBuffer[5940] = { 0 };
bool playInputs = false;
int comboDropPos = -1;
int currentHitcount = 0;
int inputPosition = 0;

bool playAudioCue = false;
bool playScreenFlash = false;
string audioCueName = "SystemDefault";
uint32_t screenFlashColor = 0xff0000ff;

void loadTrialFolder() {
    LOG("Load trial folder");
    string fileNameBase = "cccaster/trials/";
    string moon;
    vector<string> trialFiles;
    switch ( *CC_P1_MOON_SELECTOR_ADDR )
        {
        case 0:
            moon = "C";
            break;
        case 1:
            moon = "F";
            break;
        case 2:
            moon = "H";
            break;
        default:
            break;
        }
    fileNameBase.append( moon );
    const char* cname = getShortCharaName ( *CC_P1_CHARACTER_ADDR );
    fileNameBase.append( "-" );
    fileNameBase.append( cname );
    WIN32_FIND_DATA fd;
    string searchPath = fileNameBase+"/*.txt";
    HANDLE hFind = ::FindFirstFile(searchPath.c_str(), &fd);
    DWORD ftyp = GetFileAttributesA( fileNameBase.c_str() );
    if (ftyp == INVALID_FILE_ATTRIBUTES)
        return;
    if (ftyp & FILE_ATTRIBUTE_DIRECTORY)
    {
        do {
            // read all (real) files in current folder
            if(! (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ) {
                trialFiles.push_back( fd.cFileName );
            }
        }while(::FindNextFile(hFind, &fd));
        ::FindClose(hFind);
    }
    for ( string file : trialFiles ) {
        TrialManager::handleTrialFile( fileNameBase + "/" + file );
    }
}

string getButtons(unsigned int x) {
    string output = "";
    if ((x & 0x4100) != 0) {
        output += "A";
    }
    if ((x & 0x8200) != 0) {
        output += "B";
    }
    if ((x & 0x0400) != 0) {
        output += "M";
    }
    if ((x & 0x0080) != 0) {
        output += "C";
    }
    if ((x & 0x0040) != 0) {
        output += "D";
    }
    if ((x & 0x0800) != 0) {
        output += "E";
    }
    return output;
}

vector<DemoInput> formatDemo( vector<uint16_t> demoInputs )
{
    vector<DemoInput> output;
    for ( uint16_t i = 0; i < demoInputs.size(); ++i ) {
        uint16_t input = demoInputs[i];
        LOG(input);
        if ( input != 0 && input != 0x20 ) {
            int direction = input & 0xF;
            if ( direction == 0 )
                direction = 5;
            string buttons = getButtons(input);
            LOG(buttons);
            output.push_back( DemoInput{ i, direction, buttons} );
        }
    }
    return output;
}

DemoInput stringToDemoInput( string input )
{
    vector<string> splitvals = split( input, ":" );
    string frame = splitvals[0];
    int dir = stoi( trimmed( splitvals[1] ).substr(0,1) );
    string inputs = trimmed( splitvals[1] ).substr( 1 );
    return DemoInput{ stoi( frame.substr( 1 ) ), dir, inputs };
}

uint16_t stringToButtons( string buttons ) {
    uint16_t output = 0;
    if ( buttons.find("A") != string::npos ) {
        output |= CC_BUTTON_A;
    }
    if ( buttons.find("B") != string::npos ) {
        output |= CC_BUTTON_B;
    }
    if ( buttons.find("C") != string::npos ) {
        output |= CC_BUTTON_C;
    }
    if ( buttons.find("D") != string::npos ) {
        output |= CC_BUTTON_D;
    }
    if ( buttons.find("E") != string::npos ) {
        output |= CC_BUTTON_E;
    }
    if ( buttons.find("M") != string::npos ) {
        output |= CC_BUTTON_AB;
    }
    return output;
}

uint16_t convertInputEditor( uint16_t input ) {
    uint16_t output = 0;
    int direction = input & 0xF;
    if ( direction == 0 )
        direction = 5;
    switch ( direction ) {
    case 9:
        output ^= 0b1100;
        break;
    case 6:
        output ^= 0b0100;
        break;
    case 3:
        output ^= 0b0110;
        break;
    case 8:
        output ^= 0b1000;
        break;
    case 5:
        output ^= 0b0000;
        break;
    case 2:
        output ^= 0b0010;
        break;
    case 7:
        output ^= 0b1001;
        break;
    case 4:
        output ^= 0b0001;
        break;
    case 1:
        output ^= 0b0011;
        break;
    }
    int base = 0b10000;
    input = input >> 4;
    if ((input & CC_BUTTON_A) != 0) {
        output ^= base;
    }
    base = base << 1;
    if ((input & CC_BUTTON_B) != 0) {
        output ^= base;
    }
    base = base << 1;
    if ((input & CC_BUTTON_C) != 0) {
        output ^= base;
    }
    base = base << 1;
    if ((input & CC_BUTTON_D) != 0) {
        output ^= base;
    }
    base = base << 1;
    if ((input & CC_BUTTON_E) != 0) {
        output ^= base;
    }
    return output;
}

uint16_t unconvertInputEditor( uint16_t input ) {
    uint16_t output = 0;
    uint16_t direction = input & 0b1111;
    switch ( direction ) {
    case 0b1100:
        output ^= 9;
        break;
    case 0b0100:
        output ^= 6;
        break;
    case 0b0110:
        output ^= 3;
        break;
    case 0b1000:
        output ^= 8;
        break;
    case 0:
        output ^= 0;
        break;
    case 0b0010:
        output ^= 2;
        break;
    case 0b1001:
        output ^= 7;
        break;
    case 0b0001:
        output ^= 4;
        break;
    case 0b0011:
        output ^= 1;
        break;
    }
    int value = 0b10000;
    if ((input & value) != 0) {
        output ^= gameInputBits[4].second >> 4;
    }
    value = value << 1;
    if ((input & value) != 0) {
        output ^= gameInputBits[5].second >> 4;
    }
    value = value << 1;
    if ((input & value) != 0) {
        output ^= gameInputBits[6].second >> 4;
    }
    value = value << 1;
    if ((input & value) != 0) {
        output ^= gameInputBits[7].second >> 4;
    }
    value = value << 1;
    if ((input & value) != 0) {
        output ^= gameInputBits[8].second >> 4;
    }
    return output;
}

vector<uint16_t> unformatDemo( vector<DemoInput> fdemoInputs )
{
    vector<uint16_t> output;
    int lastFrame = fdemoInputs.back().frame;
    int index = 0;
    for ( uint16_t i = 0; i < lastFrame; ++i ) {
        if ( i == fdemoInputs[index].frame) {
            DemoInput input = fdemoInputs[index];
            LOG( input.buttons );
            output.push_back( COMBINE_INPUT( input.direction,
                                             stringToButtons( input.buttons ) ) );
            LOG( stringToButtons( input.buttons ) );
            index++;
        } else {
            output.push_back( 0 );
        }
    }
    return output;
}

void handleTrialFile( string fileName ) {
    LOG("handling trial: %s", fileName );
    ifstream trialFile( fileName );
    array<int32_t, 3> startingPositions;
    // Default starting positions
    startingPositions[0] = -16384;
    startingPositions[1] = 16384;
    startingPositions[2] = 0;
    vector<string> comboText;
    vector<uint32_t> comboSeq;
    vector<int> comboHit;
    vector<uint16_t> demoInputs;
    if ( trialFile ) {
        string str;
        getline( trialFile, str );
        int numFields = stoi( str );
        for ( int i = 0; i < numFields; ++i ) {
            getline( trialFile, str);
            if ( str == "StartPos" ) {
                getline( trialFile, str );
                int readVal = stoi( str );
                startingPositions[0] = readVal;
                getline( trialFile, str );
                readVal = stoi( str );
                startingPositions[1] = readVal;
                getline( trialFile, str );
                readVal = stoi( str );
                startingPositions[2] = readVal;
            } else if ( str == "Combo" ) {
                getline( trialFile, str);
                int numInputs = stoi( str );
                for ( int j = 0; j < numInputs; ++j ) {
                    getline( trialFile, str );
                    comboText.push_back( str );
                    getline( trialFile, str );
                    comboSeq.push_back( stoi( str ) );
                    getline( trialFile, str );
                    comboHit.push_back( stoi( str ) );
                }
            } else if ( str == "Demo" ) {
                LOG("Demo");
                getline( trialFile, str);
                int numInputs = stoi( str );
                for ( int j = 0; j < numInputs; ++j ) {
                    getline( trialFile, str );
                    demoInputs.push_back( stoul( str, 0, 16 ) );
                }
            } else if ( str == "DemoFormatted" ) {
                LOG("DemoFormatted");
                getline( trialFile, str);
                int numInputs = stoi( str );
                vector<DemoInput> fdemo;
                for ( int j = 0; j < numInputs; ++j ) {
                    getline( trialFile, str );
                    fdemo.push_back( stringToDemoInput( str ) );
                }
                demoInputs = unformatDemo( fdemo );
            } else if ( str == "PartnerInputs" ) {
            }
        }
    }
    LOG("Making trial object");
    vector<string> nameFragments = split( fileName, "/" );
    fileName = nameFragments[nameFragments.size() - 1];
    Trial t = { fileName.substr( 0, fileName.size() - 4 ),
                startingPositions,
                comboText,
                comboSeq,
                comboHit,
                demoInputs,
                formatDemo( demoInputs ),
                tokenizeText( comboText )
    };
    LOG(t.name);
    for( Move m : t.tokens ) {
        for( Token t : m.text ) {
            LOG(t.text);
        }
    }
    charaTrials.push_back( t );
}

void saveTrial( Trial trial ) {
    trial.demoInputsFormatted = formatDemo( trial.demoInputs );
    string fileNameBase = "cccaster/trials/";
    string moon;
    switch ( *CC_P1_MOON_SELECTOR_ADDR )
    {
        case 0:
            moon = "C";
            break;
        case 1:
            moon = "F";
            break;
        case 2:
            moon = "H";
            break;
        default:
            break;
    }
    fileNameBase.append( moon );
    const char* cname = getShortCharaName ( *CC_P1_CHARACTER_ADDR );
    fileNameBase.append( "-" );
    fileNameBase.append( cname );
    fileNameBase.append( "/" );
    fileNameBase.append( trial.name );
    fileNameBase.append( ".txt" );
    ofstream trialfile( fileNameBase );
    if ( trialfile.is_open() ) {
        int numFields = 2;
        if ( trial.demoInputs.size() > 0 )
            numFields++;
        if ( trial.demoInputsFormatted.size() > 0 )
            numFields++;
        trialfile << numFields << endl;
        trialfile << "StartPos" << endl;
        trialfile << trial.startingPositions[0] << endl;
        trialfile << trial.startingPositions[1] << endl;
        trialfile << trial.startingPositions[2] << endl;
        trialfile << "Combo" << endl;
        trialfile << trial.comboSeq.size() << endl;
        for ( uint32_t i = 0; i < trial.comboSeq.size(); ++i ) {
            trialfile << trial.comboText[i] << endl;
            trialfile << trial.comboSeq[i] << endl;
            trialfile << trial.comboHit[i] << endl;
        }
        if ( trial.demoInputs.size() > 0 ) {
            trialfile << "Demo" << endl;
            trialfile << trial.demoInputs.size() << std::hex << endl;
            for ( uint16_t input : trial.demoInputs ) {
                trialfile << "0x" << input << endl;
            }
            trialfile << std::dec;
        }
        if ( trial.demoInputsFormatted.size() > 0 ) {
            trialfile << "DemoFormatted" << endl;
            trialfile << trial.demoInputsFormatted.size() << endl;
            for ( DemoInput input : trial.demoInputsFormatted ) {
                LOG(input.buttons);
                trialfile << "F" << input.frame << ": " << input.direction << input.buttons << endl;
            }
        }
    }
    trialfile.close();
}

void saveTrial() {
    saveTrial( charaTrials[currentTrialIndex] );
}

int getHitcount()
{
    return *(uint32_t*)((*CC_P1_COMBO_OFFSET_ADDR * 0x2C) / 4 + CC_P1_COMBO_HIT_BASE_ADDR );
}

void frameStepTrial()
{
    uint32_t seqAddr;
    uint32_t partnerAddr;
    if ( *CC_P1_PUPPET_STATE_ADDR ) {
        seqAddr = *CC_P3_SEQUENCE_ADDR;
        partnerAddr = *CC_P1_SEQUENCE_ADDR;
    } else {
        seqAddr = *CC_P1_SEQUENCE_ADDR;
        partnerAddr = *CC_P3_SEQUENCE_ADDR;
    }
    if ( charaTrials.empty() ) {
#ifdef LOGGING
        char buf[1000];
        sprintf(buf, "partnerSeq=%03d, currSeq=%03d", partnerAddr, seqAddr );
        dtext = buf;
#endif // LOGGING
        return;
    }

    Trial currentTrial = charaTrials[currentTrialIndex];
#ifdef LOGGING
    char buf[1000];
    /*
    sprintf(buf, "temp1=%d, temp=%d, ehitC=%d, rhitc=%02d,cCombo=%02d, p2cseq=%03d, currSeq=%03d, exSeq=%02d",
            0,0,
            currentHitcount,
            0,
            currentTrialIndex,
            *CC_P2_SEQUENCE_ADDR,
            seqAddr,
            currentTrial.comboSeq[comboTrialPosition]);
    */
    sprintf(buf, "ghc=%03d, hitcount=%03d, partnerSeq=%03d, currSeq=%03d, exSeq=%02d",
            getHitcount(),
            currentHitcount,
            partnerAddr,
            seqAddr,
            currentTrial.comboSeq[comboTrialPosition]);
    dtext = buf;
#endif // LOGGING
    if ( !comboDrop ) {
        if ( seqAddr == currentTrial.comboSeq[0] &&
             *CC_P2_SEQUENCE_ADDR != 0 && !comboStart ) {
            comboTrialPosition = 1;
            comboStart = true;
            currentHitcount = getHitcount();
        } else if ( ( seqAddr == currentTrial.comboSeq[comboTrialPosition] ||
                      ( ( currentTrial.comboSeq[comboTrialPosition] == 0 ) && ( seqAddr == CC_SEQ_CROUCH_TRANSITION ) ) ||
                      ( ( currentTrial.comboHit[comboTrialPosition] == 2 ) && ( partnerAddr == currentTrial.comboSeq[comboTrialPosition] ) ) )
                    && comboStart &&
                    ( ( currentTrial.comboHit[comboTrialPosition] == 1 ) ?
                      ( getHitcount() > currentHitcount ) : true ) ) {
            comboTrialPosition += 1;
            currentHitcount = getHitcount();
        } else if ( getHitcount() > currentHitcount ) {
            currentHitcount = getHitcount();
        }
        if ( *CC_P2_SEQUENCE_ADDR == 0 ) {
            comboDrop = true;
            comboDropPos = comboTrialPosition;
            comboStart = false;
            currentHitcount = 0;
        }
    } else if ( seqAddr == 0 &&
                *CC_P2_SEQUENCE_ADDR == 0 ) {
        if ( comboTrialPosition < currentTrial.comboSeq.size() )
            comboTrialPosition = 0;
    } else if ( seqAddr == currentTrial.comboSeq[0] &&
                *CC_P2_SEQUENCE_ADDR != 0 ) {
        comboTrialPosition = 1;
        comboDrop = false;
        comboStart = true;
        currentHitcount = getHitcount();
    }

    if ( isRecording ) {
        uint16_t rawinput = netManPtr->getRawInput( 1 );
        LOG( "newRecordInput: %d@%d", rawinput, currentTrial.demoInputs.size() );
        if ( currentTrial.demoInputs.size() < 60*30 ) {
            charaTrials[currentTrialIndex].demoInputs.push_back( rawinput );
        }
    }

    if ( ( GetAsyncKeyState ( VK_F2 ) & 0x1 ) == 1 ) {
        if ( inputGuideEnabled ) {
            inputPosition = 0;
            playInputs = !playInputs;
        }
    }
    if ( playInputs ) {
        inputPosition -= 3;
        int endPlayPosition = charaTrials[currentTrialIndex].demoInputs.size() * -3 - 200;
        if ( inputPosition < endPlayPosition ) {
            inputPosition = 0;
            playInputs = false;
        }
    }
}



vector<Move> tokenizeText( vector<string> text )
{
    string dirs = "123456789";
    string buttons = "ABCD";
    string brackets = "[]{}";
    vector<Move> moves;
    for( unsigned int j = 0; j < text.size(); ++j ) {
        string move = text[j];
        vector<Token> tokens;
        unsigned int i = 0;
        while( i < move.length() ) {
            char curMove = move[i];
            if ( curMove == 'd' ) {
                if ( move[i+1] == 'a' ) {
                    tokens.push_back( Token{ "dash", String, 4 } );
                    i += 3;
                } else if ( move[i+1] == 'l' ) {
                    tokens.push_back( Token{ "dl.", String, 3 } );
                    i+=2;
                } else {
                    tokens.push_back( Token{ "dj.", Symbol, 3 } );
                    i+=2;
                }
            } else if ( curMove == 'j' ) {
                tokens.push_back( Token{ "j.", Symbol, 2 } );
                i++;
            } else if ( curMove == 't' ) {
                tokens.push_back( Token{ "tk.", Symbol, 3 } );
                i += 2;
            } else if ( curMove == 'a' ) {
                if ( i+3 < move.length() && move[i+3] == 'd' ) {
                    if ( i+4 < move.length() && move[i+4] == 'o' ) {
                        tokens.push_back( Token{ "Airdodge", String, 8 } );
                        i += 7;
                    } else if ( i+4 < move.length() && move[i+4] == 'a' ) {
                        tokens.push_back( Token{ "Airdash", String, 7 } );
                        i += 6;
                    }
                } else if ( i+3 < move.length() && move[i+3] == 'b' ) {
                    tokens.push_back( Token{ "Airbackdash", String, 11 } );
                    i += 10;
                }
            } else if ( curMove == 'X' ) {
                tokens.push_back( Token{ "X", String, 1 } );
            } else if ( dirs.find( curMove ) != string::npos ) {
                if ( curMove != '5' ) {
                    tokens.push_back( Token{ string( 1, curMove ), Direction, 1 } );
                }
            } else if ( buttons.find( curMove ) != string::npos ) {
                if ( i+1 < move.length() && move[i+1] == 'T' ) {
                    tokens.push_back( Token{ "AT", String, 2 } );
                    i++;
                } else if ( i+1 < move.length() && move[i+1] == 'd' ) {
                    tokens.push_back( Token{ "Add.", Symbol, 4 } );
                    i += 4;
                } else if ( i+1 < move.length() && move[i+1] == 'i' ) {
                    tokens.push_back( Token{ "Airdash", String, 7 } );
                    i += 6;
                } else {
                    tokens.push_back( Token{ string( 1, curMove ), Button, 1 } );
                }
            } else if ( curMove == '(' ) {
                string textFrag;
                uint32_t len = 2;
                while( move[i] != ')' ){
                    textFrag += move[i];
                    i++;
                    len++;
                }
                textFrag += ')';
                tokens.push_back( Token{ textFrag, String, len } );
            } else if ( brackets.find( curMove ) != string::npos ) {
                tokens.push_back( Token{ string( 1, curMove ), String, 1 } );
            } else {
                LOG( "Unkown token: %s, %d", move, i );
                tokens.push_back( Token{ move, String, move.length() } );
                i = move.length();
            }
            i++;
        }
        MovePosition pos;
        if ( j == 0 ) {
            pos = Start;
        } else if ( j == text.size() - 1 ) {
            pos = Ending;
        } else {
            pos = Middle;
        }
        moves.push_back( Move{ tokens, pos } );
    }
    return moves;
}

} // namespace TrialManager

extern NetplayManager* netManPtr;

/*
void DllTrialManager::frameStepTrial()
{
    //tmp2 += 1;
    //tmp2 %= 300;
    if ( !initialized )
        return;

    //TrialManager::comboTrialText = { L"5A >", L"2A >", L"5B >", L"5C" };
    TrialManager::comboTrialText = comboText[TrialManager::currentTrialIndex];
    TrialManager::comboName = comboNames[TrialManager::currentTrialIndex];
    TrialManager::comboTrialLength = 4;

    //comboSeq = {{ 4, 1, 2, 3, -1 }};

    char buf[1000];
    sprintf(buf, "temp1=%d, temp=%d, ehitC=%d, rhitc=%02d,cCombo=%02d, p2cseq=%03d, currSeq=%03d, exSeq=%02d",
            //offaddrp=%d, offAddr=0x%08X,
            //comboDrop,
            //cStartcomboStart,
            //(int)TrialManager::trialTextures,
            //(int)&TrialManager::trialTextures,
            0,0,
            currentHitcount,
            getHitcount(),
            TrialManager::currentTrialIndex,
            //(*CC_P1_COMBO_OFFSET_ADDR * 0x2C),
            //((*CC_P1_COMBO_OFFSET_ADDR * 0x2C) / 4 + CC_P1_COMBO_HIT_BASE_ADDR ),
            *CC_P2_SEQUENCE_ADDR,
            *CC_P1_SEQUENCE_ADDR,
            comboSeq[TrialManager::currentTrialIndex][TrialManager::comboTrialPosition]);
    //TrialManager::dtext = buf;
    //cout << TrialManager::dtext << endl;
    if ( !comboDrop ) {
        if ( *CC_P1_SEQUENCE_ADDR == comboSeq[TrialManager::currentTrialIndex][0] &&
             *CC_P2_SEQUENCE_ADDR != 0 && !comboStart ) {
            TrialManager::comboTrialPosition = 1;
            comboStart = true;
            currentHitcount = getHitcount();
        } else if ( ( *CC_P1_SEQUENCE_ADDR == comboSeq[TrialManager::currentTrialIndex][TrialManager::comboTrialPosition] ||
                      ( ( comboSeq[TrialManager::currentTrialIndex][TrialManager::comboTrialPosition] == 0 ) && ( *CC_P1_SEQUENCE_ADDR == 12 ) ) )
                    && comboStart &&
                    ( (comboHit[TrialManager::currentTrialIndex][TrialManager::comboTrialPosition]) ?
                      (getHitcount() > currentHitcount) : true ) ) {
            TrialManager::comboTrialPosition += 1;
            currentHitcount = getHitcount();
        }
        if ( *CC_P2_SEQUENCE_ADDR == 0 ) {
            comboDrop = true;
            comboDropPos = TrialManager::comboTrialPosition;
            comboStart = false;
            currentHitcount = 0;
            //    TrialManager::comboTrialPosition = -1;
        }
    } else if ( *CC_P1_SEQUENCE_ADDR == 0 &&
                *CC_P2_SEQUENCE_ADDR == 0 ) {
        TrialManager::comboTrialPosition = 0;
    } else if ( *CC_P1_SEQUENCE_ADDR ==  comboSeq[TrialManager::currentTrialIndex][0] &&
                *CC_P2_SEQUENCE_ADDR != 0 ) {
        TrialManager::comboTrialPosition = 1;
        comboDrop = false;
        comboStart = true;
        currentHitcount = getHitcount();
    }
    //if ( ( GetAsyncKeyState ( VK_F3 ) & 0x1 ) == 1 ) {
    //    loadCombo( TrialManager::currentTrialIndex + 1 );
    //}
    if ( ( GetAsyncKeyState ( VK_F2 ) & 0x1 ) == 1 ) {
        loadCombo( TrialManager::currentTrialIndex - 1 );
    }
    if ( ( GetAsyncKeyState ( VK_F12 ) & 0x1 ) == 1 ) {
        if ( TrialManager::comboTrialTextAlign > 1 ) {
            TrialManager::comboTrialTextAlign = -1;
        }
        else if ( TrialManager::comboTrialTextAlign < 1 ) {
            TrialManager::comboTrialTextAlign = 0;
        }
        else if ( TrialManager::comboTrialTextAlign == 0 ) {
            TrialManager::comboTrialTextAlign = 1;
        }
    }




    if ( TrialManager::isRecording ) {
        uint16_t rawinput = netManPtr->getRawInput( 1 );
        LOG( "newRecordInput: %d@%d", rawinput, TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputs.size() );
        if ( TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputs.size() < 60*30 ) {
            TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputs.push_back( rawinput );
        }
    }
}
*/

wstring tmp( string s ){
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    //std::string narrow = converter.to_bytes(wide_utf16_source_string);
    std::wstring wide = converter.from_bytes(s);
    return wide;
}
void DllTrialManager::loadTrialFile()
{
    string fileNameBase = "cccaster/trials/";
    string moon;
    switch ( *CC_P1_MOON_SELECTOR_ADDR )
    {
        case 0:
            moon = "C";
            break;
        case 1:
            moon = "F";
            break;
        case 2:
            moon = "H";
            break;
        default:
            break;
    }
    fileNameBase.append( moon );
    const char* cname = getShortCharaName ( *CC_P1_CHARACTER_ADDR );
    fileNameBase.append( cname );
    fileNameBase.append( ".txt" );
    ifstream trialFile(  fileNameBase );
    if ( trialFile ) {
        string str;
        getline( trialFile, str );
        numCombos = stoi( str );
        for ( int i = 0; i < numCombos; ++i ) {
            getline( trialFile, str);
            wstring ncomboName = tmp( str );
            getline( trialFile, str);
            int numInputs = stoi( str );
            vector<wstring> ncomboText;
            vector<int> ncomboSeq;
            vector<int> ncomboHit;
            wstring nfullstring;
            for ( int j = 0; j < numInputs; ++j ) {
                getline( trialFile, str );
                //if ( j != numInputs - 1 )
                //    str.append( " >");
                wstring tstr = tmp( str );
                ncomboText.push_back( tstr );
                nfullstring.append( tstr );
                getline( trialFile, str );
                ncomboSeq.push_back( stoi( str ) );
                getline( trialFile, str );
                ncomboHit.push_back( stoi( str ) );
            }
            comboNames.push_back( ncomboName );
            comboText.push_back( ncomboText );
            comboSeq.push_back( ncomboSeq );
            comboHit.push_back( ncomboHit );
            TrialManager::fullStrings.push_back( nfullstring );
        }
        initialized = true;
    }
    //*CC_SHOW_ATTACK_DISPLAY = 1;
    //FILE* file = fopen ("coords.txt", "r");
    //fscanf (file, "%d %d %d %d %d %d", &i1, &i2, &i3, &i4, &i5, &i6);
    i1 = 4;
    i2 = 5;
    i3 = 7;
    i4 = 20;
    i5 = 300;
    i6 = 11;

    //fclose( file );
    //cout << TrialManager::fullStrings[0];
}

void DllTrialManager::loadCombo( int comboId )
{
    if ( comboId < 0 )
        comboId = numCombos - 1;
    TrialManager::currentTrialIndex = ( comboId % numCombos );
    TrialManager::comboTrialPosition = 0;
    currentHitcount = 0;
    comboDrop = false;
    comboStart = false;

    if ( comboId < numCombos ) {
    }
}

int DllTrialManager::getHitcount()
{
    return *(uint32_t*)((*CC_P1_COMBO_OFFSET_ADDR * 0x2C) / 4 + CC_P1_COMBO_HIT_BASE_ADDR );
}

extern "C" int CallDrawSprite ( int spriteWidth, int dxdevice, int texAddr, int screenXAddr, int screenYAddr, int spriteHeight, int texXAddr, int texYAddr, int texXSize, int texYSize, int flags, int unk, int layer );

extern "C" int CallDrawText ( int width, int height, int xAddr, int yAddr, char* text, int textAlpha, int textShade, int textShade2, void* font, int spacing, int unk, char* out );

extern "C" int CallDrawRect ( int screenXAddr, int screenYAddr, int width, int height, int A, int B, int C, int D, int layer );

void DllTrialManager::drawButton( int buttonId, int screenX, int screenY, int width, int height, int layer )
{
    // Buttons:
    // 0: A 1: B 2: C 3:D 4:E
    if ( buttonId == 4 ) {
        CallDrawSprite ( width, 0, (int)TrialManager::trialInputTextures, screenX, screenY, height, 0x19, 75, 0x19, 0x19, 0xFFFFFFFF, 0, layer );
    } else {
        CallDrawSprite ( width, 0, *(int*)BUTTON_SPRITE_TEX, screenX, screenY, height, 0x19*buttonId, 0x19, 0x19, 0x19, 0xFFFFFFFF, 0, layer );
    }
}

void DllTrialManager::drawArrow( int direction, int screenX, int screenY, int width, int height, int layer )
{
    CallDrawSprite ( width, 0, *(int*)BUTTON_SPRITE_TEX, screenX, screenY, height, 0x19*direction, 0, 0x19, 0x19, 0xFFFFFFFF, 0, layer );
}

void DllTrialManager::drawText( string text, int screenX, int screenY, int width, int height, int layer )
{
    vector<char> ctext(text.begin(), text.end());
    ctext.push_back(0);

    CallDrawText ( width, height, screenX, screenY, &ctext[0],
                   0xff, // alpha
                   0xff, // shade
                   layer, // also alpha?
                   (void*) FONT2,
                   0, 0, 0 );
}

void DllTrialManager::drawTextBorder( string text, int screenX, int screenY, int width, int height, int layer )
{
    vector<char> ctext(text.begin(), text.end());
    ctext.push_back(0);

    CallDrawText ( width, height, screenX-1, screenY, &ctext[0],
                   0xff, // alpha
                   0x0, // shade
                   layer,
                   (void*) FONT2,
                   0, 0, 0 );
    CallDrawText ( width, height, screenX+1, screenY, &ctext[0],
                   0xff, // alpha
                   0x0, // shade
                   layer,
                   (void*) FONT2,
                   0, 0, 0 );
    CallDrawText ( width, height, screenX, screenY-1, &ctext[0],
                   0xff, // alpha
                   0x0, // shade
                   layer,
                   (void*) FONT2,
                   0, 0, 0 );
    CallDrawText ( width, height, screenX, screenY+1, &ctext[0],
                   0xff, // alpha
                   0x0, // shade
                   layer,
                   (void*) FONT2,
                   0, 0, 0 );

}

void DllTrialManager::drawTextWithBorder( string text, int screenX, int screenY, int width, int height, int layer )
{
    drawText( text, screenX, screenY, width, height, layer );
    drawTextBorder( text, screenX, screenY, width, height, layer );
}

void DllTrialManager::drawShadowButton( int buttonId, int screenX, int screenY, int width, int height )
{
    // Buttons:
    // 0=A 1=B 2=C 3=D
    CallDrawSprite ( width, 0, (int) TrialManager::trialInputTextures, screenX, screenY, height, 0x19*buttonId, 125, 0x19, 0x19, 0xFFFFFFFF, 0, 0x2cc );
}

void DllTrialManager::drawShadowArrow( int direction, int screenX, int screenY, int width, int height )
{
    CallDrawSprite ( width, 0, (int) TrialManager::trialInputTextures, screenX, screenY, height, 0x19*direction, 100, 0x19, 0x19, 0xFFFFFFFF, 0, 0x2cc );
}

int DllTrialManager::drawComboBacking( MovePosition position, MoveStatus status, int screenX, int screenY, int width, int height )
{
    // Type: 0=Full, 1=Start, 2=End
    // Status: 0=Next, 1=Current, 3=Completed, 4=Fail Point
    int texStartX = 4;
    int centerTexWidth = 33;
    int texStartY = 2;
    int yoffset = status*32;
    int centerStartX = 18;
    int endStartX = 52;
    int nextX = screenX;
    bool drawStart = true;
    bool drawEnd = true;
    switch ( position ) {
        case Middle:
            break;
        case Start:
            drawStart = false;
            break;
        case Ending:
            drawEnd = false;
            break;
        default:
            break;
    }
    if ( drawStart ) {
        CallDrawSprite ( 15, 0, (int) TrialManager::trialBGTextures, screenX, screenY, height, texStartX, texStartY+yoffset, 15, 32, 0xFFFFFFFF, 0, 0x2cb );
        screenX +=15;
        nextX += 15;
    }
    CallDrawSprite ( width, 0, (int) TrialManager::trialBGTextures, screenX, screenY, height, centerStartX, texStartY+yoffset, centerTexWidth, 32, 0xFFFFFFFF, 0, 0x1cc );
    nextX += width;
    if ( drawEnd ) {
        CallDrawSprite ( 18, 0, (int) TrialManager::trialBGTextures, screenX+width, screenY, height, endStartX, texStartY+yoffset, 18, 32, 0xFFFFFFFF, 0, 0x2cb );
        nextX += 4;
    }

    return nextX ;
}
void DllTrialManager::drawInputs()
{
    int width = 640;
    int left = width / 2 - 12 - 25;
    int top = 480 - 50;
    uint16_t rawinput = netManPtr->getRawInput( 1 );
    uint16_t dir = 0xF & rawinput;
    int leftmod = 0;
    int topmod = 0;
    for ( uint16_t i=1; i < 10; ++i ){
        if ( dir == i ) {
            drawArrow( i, left+leftmod, top-topmod );
        } else {
            drawShadowArrow( i, left+leftmod, top-topmod );
        }
        leftmod = ( leftmod + 25 ) % 75;
        if ( i % 3 == 0)
            topmod = ( topmod + 25 );
    }
    uint16_t buttons = rawinput >> 4;
    if ( buttons & CC_BUTTON_A ) {
        drawButton( 0, left, top + 25 );
        drawSolidRect( 2, 0, 25, boxHeight - 32, red );
    } else {
        drawShadowButton( 0, left, top + 25 );
    }
    if ( buttons & CC_BUTTON_B ) {
        drawButton( 1, left+25, top + 25 );
        drawSolidRect( 29, 0, 25, boxHeight - 32, red );
    } else {
        drawShadowButton( 1, left+25, top + 25 );
    }
    if ( buttons & CC_BUTTON_C ) {
        drawButton( 2, left+50, top + 25 );
        drawSolidRect( 56, 0, 25, boxHeight - 32, red );
    } else {
        drawShadowButton( 2, left+50, top + 25 );
    }
    if ( buttons & CC_BUTTON_D ) {
        drawButton( 3, left+75, top + 25 );
    } else {
        drawShadowButton( 3, left+75, top + 25 );
    }
}

int DllTrialManager::getMoveWidth( Move move, int x )
{
    int moveWidth = 0;
    int roffset = i3;
    for( Token token : move.text ) {
        if ( token.type == Button ) {
            moveWidth += 25;
        } else if ( token.type == Direction ) {
            moveWidth += 25;
        } else if ( token.type == String ) {
            moveWidth += 24*token.text.length();
            if ( token.text[token.text.size() - 1] == ')') {
                moveWidth -= 20;
            }
        } else if ( token.type == Symbol ) {
            if ( token.text[ 0 ] == 'd' ) {
                moveWidth += 35 + 11;
            } else if ( token.text[ 0 ] == 'j' ) {
                moveWidth += 30;
            } else if ( token.text[ 0 ] == 'A' ) {
                moveWidth += 20;
                moveWidth += 16;
                moveWidth += 14;
                moveWidth += 17;
            } else if ( token.text[ 0 ] == 't' ) {
                moveWidth += 15;
                moveWidth += 11;
                moveWidth += 15;
            }
        }
    }
    return moveWidth + roffset + 18;
}

int DllTrialManager::getMoveWidthScaled( Move move, int x, int buttonWidth )
{
    int moveWidth = 0;
    int roffset = i3;
    for( Token token : move.text ) {
        if ( token.type == Button ) {
            moveWidth += buttonWidth;
        } else if ( token.type == Direction ) {
            moveWidth += buttonWidth;
        } else if ( token.type == String ) {
            moveWidth += buttonWidth*token.text.length();
            if ( token.text[token.text.size() - 1] == ')') {
                moveWidth -= buttonWidth / 2;
            }
        } else if ( token.type == Symbol ) {
            if ( token.text[ 0 ] == 'd' ) {
                moveWidth += buttonWidth*3;
            } else if ( token.text[ 0 ] == 'j' ) {
                moveWidth += buttonWidth*2;
            } else if ( token.text[ 0 ] == 'A' ) {
                moveWidth += buttonWidth*4;
            } else if ( token.text[ 0 ] == 't' ) {
                moveWidth += buttonWidth*3;
            }
        }
    }
    return moveWidth + roffset;
}

int DllTrialManager::drawMove( Move move, MoveStatus color, int x, int y )
{
    int loffset = i1;
    if ( move.position != Start ) {
        loffset += 15;
    }
    int yoffset = i2;
    int ytextoffset = i2;
    int roffset = i3;
    int currX = x + loffset;
    int nextX = x + loffset;
    int moveWidth = 0;
    for( Token token : move.text ) {
        if ( token.type == Button ) {
            nextX += 25;
            /*
            if ( nextX > 620){
                y += 25;
                currX = x + loffset;
            }
            */
            int buttonId = token.text[0] - L'A';
            drawButton( buttonId, currX, y+yoffset );
            currX += 25;
            moveWidth += 25;
        } else if ( token.type == Direction ) {
            int buttonId = token.text[0] - L'0';
            drawArrow( buttonId, currX, y+yoffset );
            currX += 25;
            moveWidth += 25;
        } else if ( token.type == String ) {
            if ( token.text == "Airbackdash" ) {
                drawTextWithBorder( "A", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "i", currX, y+ytextoffset );
                currX += 24 - 10;
                drawTextWithBorder( "r", currX, y+ytextoffset );
                currX += 24 - 10;
                drawTextWithBorder( "b", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "a", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "c", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "k", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "d", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "a", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "s", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "h", currX, y+ytextoffset );
                currX += 24;
                moveWidth += 24*token.text.length() - 20;
            } else if ( token.text == "Airdash" ) {
                drawTextWithBorder( "A", currX, y+ytextoffset );
                currX += 24 - 8;
                drawTextWithBorder( "i", currX, y+ytextoffset );
                currX += 24 - 10;
                drawTextWithBorder( "r", currX, y+ytextoffset );
                currX += 24 - 11;
                drawTextWithBorder( "d", currX, y+ytextoffset );
                currX += 24 - 10;
                drawTextWithBorder( "a", currX, y+ytextoffset );
                currX += 24 - 8;
                drawTextWithBorder( "s", currX, y+ytextoffset );
                currX += 24 - 8;
                drawTextWithBorder( "h", currX, y+ytextoffset );
                currX += 24;
                moveWidth += 24*token.text.length() -8 -8 -10 -10 - 11 -8;
            } else if ( token.text == "Airdodge" ) {
                drawTextWithBorder( "A", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "i", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "r", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "d", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "o", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "d", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "g", currX, y+ytextoffset );
                currX += 24;
                drawTextWithBorder( "e", currX, y+ytextoffset );
                currX += 24;
                moveWidth += 24*token.text.length();
            } else {
                drawTextWithBorder( token.text, currX, y+ytextoffset, 24, 24, 0x2cc );
                currX += 24*token.text.length();
                moveWidth += 24*token.text.length();
                if ( token.text[token.text.size() - 1] == ')') {
                    currX -= 15;
                    moveWidth -= 15;
                }
            }
        } else if ( token.type == Symbol ) {
            if ( token.text[ 0 ] == 'd' ) {
                drawTextWithBorder( "d", currX, y+ytextoffset );
                drawTextWithBorder( "j", currX + 14, y+ytextoffset );
                drawTextWithBorder( ".", currX + 25, y+ytextoffset );
                currX += 35 + 11;
                moveWidth += 35 + 11;
            } else if ( token.text[ 0 ] == 'j' ) {
                drawTextWithBorder( "j", currX, y+ytextoffset );
                drawTextWithBorder( ".", currX + 10, y+ytextoffset );
                currX += 30;
                moveWidth += 30;
            } else if ( token.text[ 0 ] == 'A' ) {
                drawTextWithBorder( "A", currX, y+ytextoffset );
                currX += 20;
                moveWidth += 20;
                drawTextWithBorder( "d", currX, y+ytextoffset );
                currX += 16;
                moveWidth += 16;
                drawTextWithBorder( "d", currX, y+ytextoffset );
                currX += 14;
                moveWidth += 14;
                drawTextWithBorder( ".", currX, y+ytextoffset );
                currX += 17;
                moveWidth += 17;
            } else if ( token.text[ 0 ] == 't' ) {
                drawTextWithBorder( "t", currX, y+ytextoffset );
                currX += 15;
                moveWidth += 15;
                drawTextWithBorder( "k", currX, y+ytextoffset );
                currX += 11;
                moveWidth += 11;
                drawTextWithBorder( ".", currX, y+ytextoffset );
                currX += 15;
                moveWidth += 15;
            }
        }
    }
    x = drawComboBacking( move.position, color, x, y, moveWidth+roffset );
    return x;
}

int DllTrialManager::drawMoveScaled( Move move, MoveStatus color, int x, int y, int buttonWidth )
{
    int loffset = i1;
    if ( move.position != Start ) {
        loffset += 15;
    }
    int yoffset = i2;
    int ytextoffset = i2;
    int roffset = i3;
    int currX = x + loffset;
    int moveWidth = 0;

    for( Token token : move.text ) {
        if ( token.type == Button ) {
            int buttonId = token.text[0] - 'A';
            drawButton( buttonId, currX, y+yoffset, buttonWidth, buttonWidth );
            currX += buttonWidth;
            moveWidth += buttonWidth;
        } else if ( token.type == Direction ) {
            int buttonId = token.text[0] - '0';
            drawArrow( buttonId, currX, y+yoffset, buttonWidth, buttonWidth );
            currX += buttonWidth;
            moveWidth += buttonWidth;
        } else if ( token.type == String ) {
            drawTextWithBorder( token.text, currX, y+ytextoffset, buttonWidth, buttonWidth );
            currX += buttonWidth*token.text.length();
            moveWidth += buttonWidth*token.text.length();
            if ( token.text[token.text.size() - 1] == ')') {
                currX -= buttonWidth / 2;
                moveWidth -= buttonWidth / 2;
            }
        } else if ( token.type == Symbol ) {
            if ( token.text[ 0 ] == 'd' ) {
                drawTextWithBorder( "dj.", currX, y+ytextoffset, buttonWidth, buttonWidth );
                currX += buttonWidth*3;
                moveWidth += buttonWidth*3;
            } else if ( token.text[ 0 ] == 'j' ) {
                drawTextWithBorder( "j.", currX, y+ytextoffset, buttonWidth, buttonWidth );
                currX += buttonWidth*2;
                moveWidth += buttonWidth*2;
            } else if ( token.text[ 0 ] == 'A' ) {
                drawTextWithBorder( "Add.", currX, y+ytextoffset, buttonWidth, buttonWidth );
                currX += buttonWidth*4;
                moveWidth += buttonWidth*4;
            } else if ( token.text[ 0 ] == 't' ) {
                drawTextWithBorder( "tk.", currX, y+ytextoffset, buttonWidth, buttonWidth );
                currX += buttonWidth*3;
                moveWidth += buttonWidth*3;
            }
            currX -= buttonWidth / 2;
            moveWidth -= buttonWidth / 2;
        }
    }
    x = drawComboBacking( move.position, color, x, y, moveWidth+roffset, buttonWidth + 7 );
    return x;
}

void DllTrialManager::drawCombo()
{
    int x = i4;
    int newx = i4;
    int y = i5;
    y = 20;
    if ( TrialManager::inputGuideEnabled ) {
        y += 25;
    }
    int loffset = 0;
    int yoffset = 0;
    //int roffset = 10;
    int currentMove = 0;
    int currentFail = 3;
    MoveStatus color;

    vector<string> comboTrialText;
    Trial currentTrial;
    vector<Move> moveList;
    if ( !TrialManager::charaTrials.empty() ) {
        currentTrial = TrialManager::charaTrials[TrialManager::currentTrialIndex];
        comboTrialText = currentTrial.comboText;
        moveList = currentTrial.tokens;
        currentMove = TrialManager::comboTrialPosition;
        currentFail = TrialManager::comboDropPos;
    }

    if ( !TrialManager::inputGuideEnabled ) {
        drawText ( currentTrial.name, 30, y-16, 14, 16, 0x1f0 );
        drawTextBorder ( currentTrial.name, 30, y-16, 14, 16, 0x1f0 );
        drawSolidRect( 25, y-16, currentTrial.name.size() * 15, 17, bg, 0x2ef );
    }
    for( uint16_t i = 0; i < moveList.size(); ++i ) {
        if ( i < currentMove ) {
            color = Done;
        } else if ( i == currentMove ) {
            color = Current;
        } else if ( i == currentFail ) {
            color = Failed;
        } else {
            color = Next;
        }
        Move move = moveList[i];
        int moveWidth;
        if ( TrialManager::trialScale == 0 ) {
            moveWidth = getMoveWidth( move, x+loffset );
            if ( moveWidth + x > 630 ) {
                y += 30;
                x = i4;
            }
            newx = drawMove( move, color, x+loffset, y+yoffset );
            x = newx;
        } else if ( TrialManager::trialScale == 1 ) {
            moveWidth = getMoveWidthScaled( move, x+loffset, 20 );
            if ( moveWidth + x > 630 ) {
                y += 26;
                x = i4;
            }
            newx = drawMoveScaled( move, color, x+loffset, y+yoffset, 20 );
            x = newx;
        } else if ( TrialManager::trialScale == 2 ) {
            moveWidth = getMoveWidth( move, x+loffset );
            if ( moveWidth + x > 630 ) {
                y += 18;
                x = i4;
            }
            newx = drawMoveScaled( move, color, x+loffset, y+yoffset, 0xc );
            x = newx;
        }
    }
}

void DllTrialManager::drawSolidRect( int x, int y, int width, int height, ARGB color, int layer ){
    int colorValue = ( color.alpha << 24 ) + ( color.red << 16 ) +
        ( color.green << 8 ) + ( color.blue );
    CallDrawRect( x, y, width, height, colorValue, colorValue, colorValue, colorValue, layer );
}
void DllTrialManager::drawiidx(){
    if (tmp2 > boxHeight + 10 ) {
        tmp2=-20;
    }
    tmp2+=3;
    drawSolidRect( 0, boxHeight - 32, boxWidth, 5, red );
    drawSolidRect( 0, 0, boxWidth, boxHeight, ARGB{ 0xaa, 0x0, 0x0, 0x0 } );
    drawSolidRect( 0, boxHeight - 27, boxWidth, 2, white );
    drawSolidRect( 0, boxHeight, boxWidth, 2, white );
    drawSolidRect( 0, 0, 2, boxHeight, white );
    drawButton( 0, 2, boxHeight - 25 );
    drawSolidRect( 27, 0, 2, boxHeight, white );
    drawButton( 1, 29, boxHeight - 25 );
    drawSolidRect( 54, 0, 2, boxHeight, white );
    drawButton( 2, 56, boxHeight - 25 );
    drawSolidRect( 81, 0, 2, boxHeight, white );

    drawSolidRect( 2, tmp2, 25, 2, white, 0x2cc );
    drawSolidRect( 29, tmp2-10, 25, 2, white, 0x2cc );
    drawSolidRect( 56, tmp2-20, 25, 2, white, 0x2cc );
}

void DllTrialManager::drawInputGuideButtons( uint16_t input, uint16_t lastinput, int x ) {
    bool drawA, drawB, drawC, drawD, drawE;
    int y = 12;
    drawA = ( input & COMBINE_INPUT ( 0, CC_BUTTON_A ) ) && ( !( lastinput & COMBINE_INPUT ( 0, CC_BUTTON_A ) ) );
    drawB = ( input & COMBINE_INPUT ( 0, CC_BUTTON_B ) ) && ( !( lastinput & COMBINE_INPUT ( 0, CC_BUTTON_B ) ) );
    drawC = ( input & COMBINE_INPUT ( 0, CC_BUTTON_C ) ) && ( !( lastinput & COMBINE_INPUT ( 0, CC_BUTTON_C ) ) );
    drawD = ( input & COMBINE_INPUT ( 0, CC_BUTTON_D ) ) && ( !( lastinput & COMBINE_INPUT ( 0, CC_BUTTON_D ) ) );
    drawE = ( input & COMBINE_INPUT ( 0, CC_BUTTON_E ) ) && ( !( lastinput & COMBINE_INPUT ( 0, CC_BUTTON_E ) ) );
    int numButtons = drawA + drawB + drawC + drawD + drawE;
    int button1, button2, button3, button4;
    button1 = button2 = button3 = button4 = 0;
    if ( drawA ) {
        button1 = 0;
    }
    if ( drawB ) {
        if ( button1 ) {
            button2 = 1;
        } else {
            button1 = 1;
        }
    }
    if ( drawC ) {
        if ( button2 ) {
            button3 = 2;
        } else if ( button1 ) {
            button2 = 2;
        } else {
            button1 = 2;
        }
    }
    if ( drawD ) {
        if ( button3 ) {
            button4 = 3;
        } else if ( button2 ) {
            button3 = 3;
        } else if ( button1 ) {
            button2 = 3;
        } else {
            button1 = 3;
        }
    }
    if ( drawE ) {
        button1 = 4;
    }
    if ( numButtons == 0 ) {
        return;
    } else if ( numButtons == 1 ) {
        drawButton( button1, x, y );
    } else if ( numButtons == 2 ) {
        drawButton( button1, x, y );
        drawButton( button2, x, 22 + y );
    } else if ( numButtons == 3 ) {
        drawButton( button1, x, y );
        drawButton( button2, x-12, 22 + y );
        drawButton( button2, x+13, 22 + y );
    } else if ( numButtons == 4 ) {
        drawButton( button1, x, y );
        drawButton( button2, x, 22 + y);
        drawButton( button1, x+25, y );
        drawButton( button2, x+25, 22 + y );
    }
    drawSolidRect( x+10, 7, 4, 33, white, 0x2cb );
    int inputOffset = 18;
    if ( x == inputOffset && TrialManager::playAudioCue ) {
        LOG(TrialManager::audioCueName.c_str());
        if ( TrialManager::audioCueName.find ( SYSTEM_ALERT_PREFEX ) == 0 )
            PlaySound(TEXT(TrialManager::audioCueName.c_str()), 0, SND_ALIAS | SND_ASYNC );
        else
            PlaySound(TEXT(TrialManager::audioCueName.c_str()), 0, SND_FILENAME | SND_ASYNC | SND_NODEFAULT );
    }
    if ( (x == inputOffset || x == inputOffset + 3) && TrialManager::playScreenFlash ) {
        uint32_t rawcolor = TrialManager::screenFlashColor;
        LOG(TrialManager::audioCueName.c_str());
        ARGB color = ARGB{ (uint8_t) ( ( rawcolor >> 24 ) & 0xFF ),
                           (uint8_t) ( ( rawcolor >> 16 ) & 0xFF ),
                           (uint8_t) ( ( rawcolor >> 8 ) & 0xFF ),
                           (uint8_t) ( ( rawcolor ) & 0xFF ) };
        drawSolidRect( 0, 0, 640, 480, color, 0x300 );
    }
}

void DllTrialManager::drawInputGuide() {
    int x = TrialManager::inputPosition;
    Trial currentTrial = TrialManager::charaTrials[TrialManager::currentTrialIndex];
    drawSolidRect( 0, 21, 640, 3, white, 0x2ca );
    drawSolidRect( 30, 0, 3, 45, white, 0x2ca );
    drawSolidRect( 0, 0, 640, 46, darkgrey, 0x2ca );
    for ( uint16_t i = 1; i < currentTrial.demoInputs.size(); ++i ) {
        uint16_t input = currentTrial.demoInputs[i];
        uint16_t lastinput = currentTrial.demoInputs[i-1];
        drawInputGuideButtons( input, lastinput, x );
        x+=3;
    }
}

void DllTrialManager::drawAttackDisplay()
{
    string test = "MAX TEST";
    string testVal = "50";
    int leftX = 0x1a4;
    int rightX = leftX + 200;
    int y = 0xe8;
    //drawText(test, leftX, y, 0xa, 0xe );
    //drawText(test, leftX, y, 0xa, 0xe );
    //drawTextBorder(test, leftX, y, 0xa, 0xe );
    //drawText(testVal, rightX-0xa*2, y, 0xa, 0xe );

    int currentMeter = *CC_P1_METER_ADDR;
    if ( *CC_P2_SEQUENCE_ADDR != 0 ) {
        if ( currentMeter > lastSeenMeter ) {
            meterGained = currentMeter - lastSeenMeter;
            totalMeterGained += meterGained;
        }
        showTotalMeterGained = totalMeterGained;
        lastSeenMeter = currentMeter;
    } else {
        lastSeenMeter = currentMeter;
        totalMeterGained = 0;
    }
    string meter = "METER GAIN";
    string gained = "";
    y += 14;
    char buf[10];
    sprintf(buf, "%d.%02d", meterGained/100, meterGained%100 );
    gained = string(buf);
    drawTextWithBorder(meter, leftX, y, 0xa, 0xe );
    drawTextWithBorder(gained, rightX-0xa*gained.length(), y, 0xa, 0xe );
    y+=14;
    string total = "TOTAL METER GAIN";
    sprintf(buf, "%d.%02d", showTotalMeterGained/100, showTotalMeterGained%100 );
    string totalgained = string(buf);
    drawTextWithBorder(total, leftX, y, 0xa, 0xe );
    drawTextWithBorder(totalgained, rightX-0xa*totalgained.length(), y, 0xa, 0xe );
    y+=14;
    ifstream file( "trainingentries.txt");
    char buf2[40];
    if ( file ) {
        string label;
        string value;
        string type;
        getline( file, label );
        int numEntries = stoi( label );
        for ( auto i=0; i<numEntries; ++i ) {
            getline( file, label );
            getline( file, value );
            getline( file, type );
            int val = stoi(value, nullptr, 16);
            if ( type == "float" ) {
                sprintf(buf2, "%f", *(float*)val);
            } else if ( type == "int" ) {
                sprintf(buf2, "%d", *(int*)val);
            }
            //sprintf(buf2, "%d", *val);
            string formatValue = string(buf2);
            drawAttackDisplayRow( label, formatValue, y );
            y+= 14;
        }
    }
}

void DllTrialManager::drawAttackDisplayRow( string label, string value, int y )
{
    int leftX = 0x1a4;
    int rightX = leftX + 200;
    drawTextWithBorder(label, leftX, y, 0xa, 0xe );
    drawTextWithBorder(value, rightX-0xa*value.length(), y, 0xa, 0xe );
}

void DllTrialManager::render()
{
    if ( TrialManager::hideText )
        return;
    if ( *CC_SHOW_ATTACK_DISPLAY ) {
        drawAttackDisplay();
    }
    //drawInputs();
    if ( TrialManager::showCombo )
        drawCombo();
    //drawiidx();
    if ( TrialManager::inputGuideEnabled )
        drawInputGuide();
    if ( ProcessManager::isWine() )
        drawWineOverlay();
    if ( TrialManager::inputEditorEnabled )
        drawInputEditor();
}

void DllTrialManager::drawWineOverlay()
{
    int textHeight = 16;
    int textWidth = 7;
    array<string, 3> text = DllOverlayUi::getText();
    array<string, 2> selectorLine = DllOverlayUi::getSelectorLine();
    int height = DllOverlayUi::getHeight();
    int newHeight = DllOverlayUi::getNewHeight();
    array<RECT, 2> selector = DllOverlayUi::getSelector();
    array<bool, 2> shouldDrawSelector = DllOverlayUi::getShouldDrawSelector();
    int left;

    if ( ! TrialManager::dtext.empty() && !TrialManager::hideText ) {
        left = 640 - 13*TrialManager::dtext.size();
        drawText ( TrialManager::dtext, left, 0, 14, textHeight, 0x1f0 );
        drawTextBorder ( TrialManager::dtext, left, 0, 14, textHeight, 0x1f0 );
    }
    if ( DllOverlayUi::isDisabled() )
        return;

    // Only draw text if fully enabled or showing a message
    drawSolidRect( 0, 0, 640, height * 1.2 + 20, bg, 0x2fe );
    if ( !DllOverlayUi::isEnabled() )
        return;

    if ( ! ( text[0].empty() && text[1].empty() && text[2].empty() ) )
    {
        const int centerX = 640 / 2;
        int OVERLAY_TEXT_BORDER = 10;
        RECT rect;
        rect.left   = OVERLAY_TEXT_BORDER;
        rect.right  = 640 - OVERLAY_TEXT_BORDER;
        rect.top    = OVERLAY_TEXT_BORDER - 2;
        rect.bottom = rect.top + height + OVERLAY_TEXT_BORDER;
        if ( newHeight == height && height > 5 )
        {
            int right;
            if ( shouldDrawSelector[0] ) {
                right = selector[0].left + selectorLine[0].size() * (textWidth-1) + 5;
                drawSolidRect ( selector[0].left, selector[0].top, right - selector[0].left, selector[0].bottom - selector[0].top, red, 0x2ff );
            }

            if ( shouldDrawSelector[1] ) {
                int width = selectorLine[1].size()* (textWidth-1);
                left = 640 - width;
                drawSolidRect ( left - 10, selector[1].top, width + 5, selector[1].bottom - selector[1].top, right_selector_color, 0x2ff );
            }
            int top;
            if ( ! text[0].empty() ) {
                left = rect.left - 5;
                top = rect.top;
                for ( string line : split( text[0], "\n" ) ) {
                    drawText ( line, left, top, textWidth, textHeight, 0x500 );
                    top += textHeight - 2;
                }
            }

            if ( ! text[1].empty() ) {
                top = rect.top;
                for ( string line : split( text[1], "\n" ) ) {
                    left = centerX - textWidth * ( line.size() / 2 ) - 5;
                    drawText ( line, left, top, textWidth, textHeight, 0x500 );
                    top += textHeight - 2;
                }
            }

            if ( ! text[2].empty() ) {
                top = rect.top;
                for ( string line : split( text[2], "\n" ) ) {
                    left = 640 - line.size()* (textWidth-1) - 5;
                    drawText ( line, left, top, textWidth, textHeight, 0x500 );
                    top += textHeight - 2;
                }
            }
        }
    }
}

void DllTrialManager::drawGrid( int x, int y, int thickness, int xspacing, int yspacing, int numX, int numY, ARGB color )
{
    int width = ( thickness + xspacing ) * numX + thickness;
    int height = ( thickness + yspacing ) * numY + thickness;
    int left = x;
    int top = y;
    for ( int i = 0; i <= numX; ++i ) {
        drawSolidRect( left, y, thickness, height, color );
        left += thickness + xspacing;
    }
    for ( int i = 0; i <= numY; ++i ) {
        drawSolidRect( x, top, width, thickness, color );
        top += thickness + yspacing;
    }
}

void DllTrialManager::drawInputEditor()
{
    int inputSpacing = 27;
    int screenWidth = 640;
    int gridWidth = ( 9 * 25 + 45 + 11 * 2 );
    int gridLeftOffset = ( screenWidth - gridWidth ) / 2;
    int x = gridLeftOffset + 45 + 2;
    int inputLeft = x + 2;
    int y = 40;

    // bg
    drawSolidRect( 0, 34, 640, 480, ARGB{ 220, 0x0, 0x0, 0x0 }, 0x1c0 );
    // grids
    drawGrid( gridLeftOffset + 45 + 2, y-3, 2, 25, 25, 9, 15, white);
    drawGrid( gridLeftOffset, y-3, 2, 45, 25, 1, 15, white);

    // Infotext
    int infoBoxX = 35;
    int infoButtonX = 10;
    int infoBoxY = 150;

    char buf[200];
    sprintf(buf, "Current Scroll Speed: %d", TrialManager::inputEditorSpeed);
    string scrollSpeed = string( buf );
    drawTextWithBorder(scrollSpeed, 320-10*0xa, 480-25, 0xa, 0xe );
    drawTextWithBorder(":Toggle Input", infoBoxX, infoBoxY, 0xa, 0xe );
    drawButton( 0, infoButtonX, infoBoxY - 5 );
    infoBoxY += 30;
    drawTextWithBorder(":Exit", infoBoxX, infoBoxY, 0xa, 0xe );
    drawButton( 1, infoButtonX, infoBoxY - 5 );
    infoBoxY += 30;
    drawTextWithBorder(":Change Speed", infoBoxX, infoBoxY, 0xa, 0xe );
    drawButton( 2, infoButtonX, infoBoxY - 5 );
    infoBoxY += 30;
    drawTextWithBorder(":Insert Row", infoBoxX, infoBoxY, 0xa, 0xe );
    drawButton( 3, infoButtonX, infoBoxY - 5 );
    infoBoxY += 30;
    drawTextWithBorder(":Delete Row", infoBoxX, infoBoxY, 0xa, 0xe );
    drawButton( 4, infoButtonX, infoBoxY - 5 );
    infoBoxY += 30;
    drawTextWithBorder(":Switch Player", infoBoxX, infoBoxY, 0xa, 0xe );
    drawButton( 4, infoButtonX, infoBoxY - 5 );
    infoBoxY += 30;
    drawTextWithBorder(":Switch Player", infoBoxX, infoBoxY, 0xa, 0xe );
    drawButton( 4, infoButtonX, infoBoxY - 5 );

    // Selection Square
    drawSolidRect( x + inputSpacing * ( 0 + TrialManager::inputEditorX ), y - 3 + ( 1 + TrialManager::inputEditorY ) * ( inputSpacing ), 2, inputSpacing, ARGB{ 0xff, 0x0, 0xff, 0x0 }, 0x4c0 );
    drawSolidRect( x + inputSpacing * ( 0 + TrialManager::inputEditorX ), y - 3 + ( 1 + TrialManager::inputEditorY ) * ( inputSpacing ), inputSpacing, 2, ARGB{ 0xff, 0x0, 0xff, 0x0 }, 0x4c0 );
    drawSolidRect( x + inputSpacing * ( 1 + TrialManager::inputEditorX ), y - 3 + ( 1 + TrialManager::inputEditorY ) * ( inputSpacing ), 2, inputSpacing, ARGB{ 0xff, 0x0, 0xff, 0x0 }, 0x4c0 );
    drawSolidRect( x + inputSpacing * ( 0 + TrialManager::inputEditorX ), y - 3 + ( 2 + TrialManager::inputEditorY ) * inputSpacing, inputSpacing, 2, ARGB{ 0xff, 0x0, 0xff, 0x0 }, 0x4c0 );

    drawArrow( 4, inputLeft, 40, 25, 25, 0x2cc );
    inputLeft += inputSpacing;
    drawArrow( 2, inputLeft, 40, 25, 25, 0x2cc );
    inputLeft += inputSpacing;
    drawArrow( 6, inputLeft, 40, 25, 25, 0x2cc );
    inputLeft += inputSpacing;
    drawArrow( 8, inputLeft, 40, 25, 25, 0x2cc );
    inputLeft += inputSpacing;
    drawButton( 0, inputLeft, 40, 25, 25, 0x2cc );
    inputLeft += inputSpacing;
    drawButton( 1, inputLeft, 40, 25, 25, 0x2cc );
    inputLeft += inputSpacing;
    drawButton( 2, inputLeft, 40, 25, 25, 0x2cc );
    inputLeft += inputSpacing;
    drawButton( 3, inputLeft, 40, 25, 25, 0x3cc );
    inputLeft += inputSpacing;
    drawButton( 4, inputLeft, 40, 25, 25, 0x3cc );

    y += 27;
    int p = TrialManager::inputEditorPosition;
    for ( int i = p; i < p+14; ++i ) {
        drawInputEditorButtons( x + 2, y, i, TrialManager::inputEditorBuffer[i], 27 );
        y += 27;
    }
    /*
    drawSolidRect( 0, boxHeight - 32, boxWidth, 5, red );
    drawSolidRect( 0, 0, boxWidth, boxHeight, ARGB{ 0xaa, 0x0, 0x0, 0x0 } );
    drawSolidRect( 0, boxHeight - 27, boxWidth, 2, white );
    drawSolidRect( 0, boxHeight, boxWidth, 2, white );
    drawSolidRect( 0, 0, 2, boxHeight, white );
    drawSolidRect( 27, 0, 2, boxHeight, white );
    drawSolidRect( 54, 0, 2, boxHeight, white );
    drawSolidRect( 81, 0, 2, boxHeight, white );

    drawSolidRect( 2, tmp2, 25, 2, white, 0x2cd );
    drawSolidRect( 29, tmp2-10, 25, 2, white, 0x2ce );
    drawSolidRect( 56, tmp2-20, 25, 2, white, 0x2cf );
    */

}

void DllTrialManager::drawInputEditorButtons( int x, int y, int frame, uint16_t input, int spacing ) {
    drawTextWithBorder(to_string(frame), x-spacing-19, y+2, 0xa, 0xe );
    int directionFlags = input & 0xF;
    if ( directionFlags & 0b0001 ) {
        drawArrow( 4, x, y );
    }
    if ( directionFlags & 0b0010 ) {
        drawArrow( 2, x+spacing, y );
    }
    if ( directionFlags & 0b0100 ) {
        drawArrow( 6, x+spacing*2, y );
    }
    if ( directionFlags & 0b1000 ) {
        drawArrow( 8, x+spacing*3, y );
    }
    int buttonFlags = input >> 4;
    if ((buttonFlags & 0b00001) != 0) {
        drawButton( 0, x+spacing*4, y );
    }
    if ((buttonFlags & 0b00010) != 0) {
        drawButton( 1, x+spacing*5, y );
    }
    if ((buttonFlags & 0b00100) != 0) {
        drawButton( 2, x+spacing*6, y );
    }
    if ((buttonFlags & 0b01000) != 0) {
        drawButton( 3, x+spacing*7, y );
    }
    if ((buttonFlags & 0b10000) != 0) {
        drawButton( 4, x+spacing*8, y );
    }
}

void DllTrialManager::convertInputEditorButtons( int x, int y, int frame, uint16_t input, int spacing ) {
    string value = "400";
    value += to_string( frame );
    drawTextWithBorder(value, x-spacing, y, 0xa, 0xe );
    int direction = input & 0xF;
    if ( direction == 0 )
        direction = 5;
    int drawFlags = 0;
    switch ( direction ) {
    case 1:
        drawFlags ^= 0b1100;
        break;
    case 2:
        drawFlags ^= 0b0100;
        break;
    case 3:
        drawFlags ^= 0b0110;
        break;
    case 4:
        drawFlags ^= 0b1000;
        break;
    case 5:
        drawFlags ^= 0b0000;
        break;
    case 6:
        drawFlags ^= 0b0010;
        break;
    case 7:
        drawFlags ^= 0b1001;
        break;
    case 8:
        drawFlags ^= 0b0001;
        break;
    case 9:
        drawFlags ^= 0b0011;
        break;
    }
    if ( drawFlags & 0b1000 ) {
        drawArrow( 4, x, y );
    }
    if ( drawFlags & 0b0100 ) {
        drawArrow( 2, x+spacing, y );
    }
    if ( drawFlags & 0b0010 ) {
        drawArrow( 6, x+spacing*2, y );
    }
    if ( drawFlags & 0b0001 ) {
        drawArrow( 8, x+spacing*3, y );
    }
    if ((input & 0x4100) != 0) {
        drawButton( 0, x+spacing*4, y );
    }
    if ((input & 0x8200) != 0) {
        drawButton( 1, x+spacing*5, y );
    }
    if ((input & 0x0400) != 0) {
        drawButton( 2, x+spacing*6, y );
    }
    if ((input & 0x0080) != 0) {
        drawButton( 3, x+spacing*7, y );
    }
    if ((input & 0x0040) != 0) {
        drawButton( 4, x+spacing*8, y );
    }
    if ((input & 0x0800) != 0) {
        drawButton( 5, x+spacing*9, y );
    }
}

void DllTrialManager::clear()
{
    comboNames.clear();
    comboText.clear();
    comboSeq.clear();
    comboHit.clear();
    TrialManager::comboTrialText.clear();
    TrialManager::fullStrings.clear();
    TrialManager::comboTrialPosition = 0;
    currentHitcount = 0;
    comboDrop = false;
    comboStart = false;
    TrialManager::playDemo = false;
    TrialManager::demoPosition = 0;
    TrialManager::isRecording = false;
    TrialManager::charaTrials.clear();
    TrialManager::inputGuideEnabled = false;
    TrialManager::playInputs = false;
    TrialManager::comboDrop = false;
    TrialManager::comboStart = false;
    TrialManager::inputPosition = 0;
}
