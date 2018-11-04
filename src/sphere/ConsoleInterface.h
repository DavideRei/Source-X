#ifndef _INC_CONSOLEINTERFACE_H
#define _INC_CONSOLEINTERFACE_H

#include "../common/sphere_library/CSString.h"
#include "../common/sphere_library/smutex.h"
#include <queue>

class ConsoleOutput
{
    CSString _sContextString;
    COLORREF _iHourColor;
    CSString _sHourString;
    COLORREF _iLogTypeColor;
    CSString _sLogTypeString;
    COLORREF _iTextColor;
    CSString _sTextString;
public:
    friend class CNTWindow;
    ConsoleOutput(COLORREF iLogColor, CSString sLogString);
    ConsoleOutput(CSString sLogString);
    ~ConsoleOutput();
    COLORREF GetHourColor();
    CSString GetHourString();
    COLORREF GetLogTypeColor();
    CSString GetLogTypeString();
    COLORREF GetTextColor();
    CSString GetTextString();
};

class ConsoleInterface
{
    std::queue<ConsoleOutput*> *_qStorage1;
    std::queue<ConsoleOutput*> *_qStorage2;
    SimpleMutex _inMutex;

protected:
    ConsoleInterface();
    ~ConsoleInterface();

    std::queue<ConsoleOutput*> **_qOutput;
    SimpleMutex _outMutex;

    void SwitchQueues();
public:
    void AddConsoleOutput(ConsoleOutput *output);
};
#endif //_INC_CONSOLEINTERFACE_H