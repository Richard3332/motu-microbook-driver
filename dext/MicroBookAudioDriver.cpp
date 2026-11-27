// Audio flavour: compile the shared implementation (MicroBookInterface.cpp) under the class name
// MicroBookAudioDriver with the Core Audio code enabled.
#define MB_AUDIO 1
#define MicroBookInterface MicroBookAudioDriver
#define MicroBookInterface_IVars MicroBookAudioDriver_IVars
#include "MicroBookInterface.cpp"
