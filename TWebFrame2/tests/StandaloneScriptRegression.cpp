#include "../src/DOM.h"
#include "../src/JavaScript.h"
#include "RegressionIO.h"
#include <ole2.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
using namespace TWebFrame::Internal;
using namespace RegressionIO;
#include "StandaloneScriptRegressions.h"
#include "StringTransformRegressions.h"
#include "EvalWorkerRegressions.h"
#include "SwitchDispatchRegressions.h"
#include "StringAppendRegressions.h"
#include "ByteCopyRegressions.h"
#include "FastPropertyRegressions.h"
#include "NativeLoopRegressions.h"
int wmain(){const int existing=RunStandaloneRegressions();const unsigned transform=RunStringTransformRegressions();const unsigned eval=RunEvalWorkerRegressions();const unsigned switches=RunSwitchDispatchRegressions();const unsigned append=RunStringAppendRegressions();const unsigned bytes=RunByteCopyRegressions();const unsigned properties=RunFastPropertyRegressions();const unsigned loops=RunNativeLoopRegressions();std::wcout<<L"String transform regressions: "<<transform<<L" failure(s)\nEval/Worker regressions: "<<eval<<L" failure(s)\nSwitch dispatch regressions: "<<switches<<L" failure(s)\nString append regressions: "<<append<<L" failure(s)\nByte copy regressions: "<<bytes<<L" failure(s)\nFast property regressions: "<<properties<<L" failure(s)\nNative loop regressions: "<<loops<<L" failure(s)\n";return existing||transform||eval||switches||append||bytes||properties||loops?1:0;}
