#pragma once
#include "pasm.h"

/* Local Macro Definitions */

#define MACRO_NAME_LEN      TOKEN_MAX_LEN
#define MACRO_MAX_ARGS      8
#define MACRO_MAX_LINES     128
#define MACRO_LINE_LENGTH   256
#define MACRO_MAX_LABELS    32

/* Macro Struct Record */
typedef struct _MACRO {
    struct _MACRO   *pPrev;         /* Previous in MACRO list */
    struct _MACRO   *pNext;         /* Next in MACRO list */
    char            Name[MACRO_NAME_LEN];
    int             InUse;          /* Macro is in use */
    int             Id;             /* Macro ID */
    int             Arguments;      /* Number of arguments */
    int             Required;       /* Number of required arguments */
    int             Labels;         /* Number of labels */
    int             Expands;        /* Number of label expansions */
    int             CodeLines;      /* Number of code lines */
    char            ArgName[MACRO_MAX_ARGS][TOKEN_MAX_LEN];
    char            ArgDefault[MACRO_MAX_ARGS][TOKEN_MAX_LEN];
    char            LableName[MACRO_MAX_LABELS][TOKEN_MAX_LEN];
    char            Code[MACRO_MAX_LINES][MACRO_LINE_LENGTH];
    int             LineNumbers[MACRO_MAX_LINES];
    char            SourceName[SOURCE_NAME];
    int             SourceIndex;
} MACRO;

