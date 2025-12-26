#ifndef VERSION_H
#define VERSION_H

// ==========================================================
// Projektname und Version mit automatischer Build-Date/Time
// ===========================================================

const char PROJECT_NAME[] = "Gas-O-Meter2";
const char PROJECT_VERSION[] = "0.1";
const char SKETCHCOMPILE[] =
{
   // YYYY- year
   __DATE__[7], __DATE__[8],__DATE__[9], __DATE__[10],'-',
   // First month letter, Oct Nov Dec = '1' otherwise '0'
   (__DATE__[0] == 'O' || __DATE__[0] == 'N' || __DATE__[0] == 'D') ? '1' : '0',
   // Second month letter
   (__DATE__[0] == 'J') ? ( (__DATE__[1] == 'a') ? '1' :       // Jan, Jun or Jul
                            ((__DATE__[2] == 'n') ? '6' : '7') ) :
   (__DATE__[0] == 'F') ? '2' :                                // Feb 
   (__DATE__[0] == 'M') ? (__DATE__[2] == 'r') ? '3' : '5' :   // Mar or May
   (__DATE__[0] == 'A') ? (__DATE__[1] == 'p') ? '4' : '8' :   // Apr or Aug
   (__DATE__[0] == 'S') ? '9' :                                // Sep
   (__DATE__[0] == 'O') ? '0' :                                // Oct
   (__DATE__[0] == 'N') ? '1' :                                // Nov
   (__DATE__[0] == 'D') ? '2' :                                // Dec
   0,'-',
   // First day letter, replace space with digit
   __DATE__[4]==' ' ? '0' : __DATE__[4],
   // Second day letter
   __DATE__[5],
   // Separator DATE / TIME
  ' ', '-' , ' ' ,
   // Time
   __TIME__[0] ,__TIME__[1] ,__TIME__[2] ,
   __TIME__[3] ,__TIME__[4] ,__TIME__[5] ,
   __TIME__[6] ,__TIME__[7] ,__TIME__[8] ,
  '\0'
};

// ==========================================================
// Build-Timestamp (UNIX-EPOCH) - automatisch vom Build-System gesetzt
// ===========================================================

#ifndef BUILD_TIMESTAMP
#define BUILD_TIMESTAMP 0  // Fallback, falls nicht gesetzt
#endif

// Build-Timestamp als uint32_t (passt in NVS)
// Wird als Magic-Key für NVS-Ring-Speicher-Initialisierung verwendet
#define RING_BUFFER_VERSION ((uint32_t)BUILD_TIMESTAMP)

#endif // VERSION_H