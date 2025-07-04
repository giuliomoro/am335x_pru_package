/*
 * pasm.c
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com/
 *
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *    Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *    Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
*/

/*===========================================================================
 * Copyright (c) Texas Instruments Inc 2010-12
 *
 * Use of this software is controlled by the terms and conditions found in the
 * license agreement under which this software has been supplied or provided.
 * ============================================================================
 */

/*===========================================================================
// PASM - PRU Assembler
//---------------------------------------------------------------------------
//
// File     : pasm.c
//
// Description:
//     Main assembler control program.
//         - Processes command line and flags
//         - Runs the main assembler engine (dual pass)
//         - Handles error reporting
//         - Handles label creation and matching
//         - Handle output file generation
//
//---------------------------------------------------------------------------
// Revision:
//     21-Jun-13: 0.84 - Open source version
//     03-Mar-15: 0.85 - Modified to build using Visual Studio 2008
//     07-Jul-14: 0.86 - Fixed -L listing generation and improved listing speed
//     21-May-16: 0.87 - Added -f option for 'FreeBasic array' binary output
============================================================================*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#if !defined(__APPLE__) && !defined(__FreeBSD__)
#include <malloc.h>
#else
#include <stdlib.h>
#endif
#include <ctype.h>
#include "pasm.h"
#include "pasmmacro.h"
#include "pasmdbg.h"
#include "path_utils.h"

/*
// Multiple Core Revision Support
// ------------------------------
//
//  -V0  PRU Legacy        (same as -x)
//  -V1  PRU Generation 1  (default)
//       Adds [LMBD,SCAN,HALT,ZERO(1),MVI(1),SLP(1)]  Removes [LFC,STC]
//  -V2  PRU Generation 2  (same as -X)
//       Adds [ZERO(2),FILL,XIN,XOUT,XCHG,MVI(2)] Removes [SCAN]
//  -V3  PRU Generation 3
//       Adds [SLP(2),LOOP,ILOOP,SXIN,SXOUT,SXCHG,NOPx]
//
// ZERO(1) : Zero is multi-cycle pseudo op encoded via moves
// ZERO(2) : Zero is single-cycle pseudo op encoded via XFR
//
// MVI(1)  : Pseudo op forms of MVI only
// MVI(2)  : Pseudo op forms of MVI only
//
// SLP(1)  : SLP with trailing NOP
// SLP(2)  : SLP without trailing NOP
*/


/* ---------- Local Macro Definitions ----------- */

#define PROCESSOR_NAME_STRING ("PRU")
#define VERSION_STRING        ("0.87")

#define MAXFILE               (256)     /* Max file length for output files */
#define MAX_PROGRAM           (16384)   /* Max instruction count */
#define MAX_CMD_EQUATE        (8)       /* Max equates that can be put on command line */

#define RET_ERROR             (1)
#define RET_SUCCESS           (0)

/* Big/Little Endian Conversions */
#define HNC16(a) ((((a)>>8)&0xff)+(((a)<<8)&0xff00))
#define HNC32(a) ((((a)>>24)&0xff)+(((a)>>8)&0xff00)+(((a)<<8)&0xff0000)+(((a)<<24)&0xff000000))

/* User Options */
unsigned int Options = 0;
unsigned int Core    = CORE_NONE;
FILE *ListingFile = 0;

/* Assembler Engine */
int  Pass;                  /* Pass 1 or 2 of parser */
int  HaveEntry;             /* Entrypont flag (init to 0) */
int  EntryPoint;            /* Entrypont (init to -1) */
int  CodeOffset;            /* Current instruction "word" offset (zero based) */
int  Errors;                /* Total number or errors */
int  FatalError;            /* Set on fatal error */
int  Warnings;              /* Total number of warnings */
uint RetRegValue;           /* Return register index */
uint RetRegField;           /* Return register field */

LABEL   *pLabelList=0;       /* List of installed labels */
int     LabelCount=0;

CODEGEN ProgramImage[MAX_PROGRAM];

SOURCEFILE cmdLine = { 0, 0, 0, 0, 0, 0, 0, 0, "[CommandLine]", "" };
char cmdLineName[MAX_CMD_EQUATE][EQUATE_NAME_LEN];
char cmdLineData[MAX_CMD_EQUATE][EQUATE_DATA_LEN];
int cmdLineEquates = 0;

char nameCArray[EQUATE_DATA_LEN];
int  nameCArraySet = 0;

/* Local Support Funtions */
static int ValidateOffset( SOURCEFILE *ps );
static int PrintLine( FILE *pfOut, SOURCEFILE *ps );
static int GetInfoFromAddr( uint address, uint *pIndex, uint *pLineNo, MACRODATA *pMacroData, uint *pCodeWord );
static int ListFile( FILE *pfOut, SOURCEFILE *ps );

/*
// Main Assembler Entry Point
//
*/
int main(int argc, char *argv[])
{
    int i,j;
    int CodeOffsetPass1 = 0;
    char *infile, *outfile, *flags;
    SOURCEFILE *mainsource;
    char outbase[MAXFILE],outfilename[MAXFILE];

    printf("\n\n%s Assembler Version %s\n",PROCESSOR_NAME_STRING, VERSION_STRING);
    printf("Copyright (C) 2005-2013 by Texas Instruments Inc.\n\n");

    /* Scan argv[0] to the final '/' in program name */
    i=0;
    j=-1;
    while( argv[0][i] )
    {
        if( argv[0][i] == '/' || argv[0][i] == '\\')
            j=i;
        i++;
    }
    argv[0]+=(j+1);

    /*
    // Process command line
    */
    infile=0;
    flags=0;
    outfile=0;

    if( argc<2 )
    {
USAGE:
        fprintf(stderr,"Usage: %s [-V#EBbcmLldfz] [-Idir] [-Dname=value] [-Cname] InFile [OutFileBase]\n\n",argv[0]);
        fprintf(stderr,"    V# - Specify core version (V0,V1,V2,V3). (Default is V1)\n");
        fprintf(stderr,"    E  - Assemble for big endian core\n");
        fprintf(stderr,"    B  - Create big endian binary output (*.bib)\n");
        fprintf(stderr,"    b  - Create little endian binary output (*.bin)\n");
        fprintf(stderr,"    c  - Create 'C array' binary output (*_bin.h)\n");
        fprintf(stderr,"    m  - Create 'image' binary output (*.img)\n");
        fprintf(stderr,"    L  - Create annotated source file style listing (*.txt)\n");
        fprintf(stderr,"    n  - Do not show macro content in annotated file listing\n");
        fprintf(stderr,"    N  - Use original macro content for annotated file listing (slower)\n");
        fprintf(stderr,"    l  - Create raw listing file (*.lst)\n");
        fprintf(stderr,"    d  - Create pView debug file (*.dbg)\n");
        fprintf(stderr,"    f  - Create 'FreeBasic array' binary output (*.bi)\n");
        fprintf(stderr,"    z  - Enable debug messages\n");
        fprintf(stderr,"    I  - Add the directory dir to search path for \n"
               "         #include <filename> type of directives (where \n"
               "         angled brackets are used instead of quotes).\n");
        fprintf(stderr,"\n    D  - Set equate 'name' to 1 using '-Dname', or to any\n");
        fprintf(stderr,"         value using '-Dname=value'\n");
        fprintf(stderr,"    C  - Name the C array in 'C array' binary output\n");
        fprintf(stderr,"         to 'name' using '-Cname'\n");
        fprintf(stderr,"\n");
        return(RET_ERROR);
    }

    /* Get all non-flag arguments */
    for( i=1; i<argc; i++ )
    {
        if( argv[i][0] != '-' )
        {
            if( !infile )
                infile = argv[i];
            else if( !outfile )
                outfile = argv[i];
            else
                goto USAGE;
        }
    }

    /* Get all flag arguments */
    for( i=1; i<argc; i++ )
    {
        if( argv[i][0] == '-' )
        {
            flags = argv[i];
            flags++;
            while( *flags )
            {
                if( *flags == 'I' )
                {
                    add_include_dir(++flags);
                    break;
                }
                else if( *flags == 'D' )
                {
                    flags++;
                    if( cmdLineEquates==MAX_CMD_EQUATE )
                    {
                        fprintf(stderr,"\nToo many command line equates\n\n");
                        goto USAGE;
                    }
                    j=0;
                    while( j<EQUATE_NAME_LEN && *flags && *flags!='=' )
                        cmdLineName[cmdLineEquates][j++]=*flags++;
                    if( j==EQUATE_NAME_LEN )
                    {
                        fprintf(stderr,"\nCommand line equate name too long\n\n");
                        goto USAGE;
                    }
                    strcpy( cmdLineData[cmdLineEquates], "1" );
                    if( *flags=='=' )
                    {
                        flags++;
                        j=0;
                        while( j<EQUATE_DATA_LEN && *flags )
                            cmdLineData[cmdLineEquates][j++]=*flags++;
                        if( j==EQUATE_DATA_LEN )
                        {
                            fprintf(stderr,"\nCommand line equate data too long\n\n");
                            goto USAGE;
                        }
                    }
                    cmdLineEquates++;
                    break;
                }
                else if( *flags == 'C' )
                {
                    flags++;
                    j = 0;
                    while( j<EQUATE_DATA_LEN && *flags )
                    {
                        nameCArray[j++]=*flags++;
                    }
                    if( j==EQUATE_DATA_LEN )
                    {
                        fprintf(stderr,"\nCArray name too long\n\n");
                        goto USAGE;
                    }
                    nameCArraySet = 1;
                    break;
                }
                else if( *flags == 'V' )
                {
                    flags++;
                    if( *flags<'0' || *flags>'3' )
                    {
                        fprintf(stderr,"\nExpected a number (0-3) after option 'V'\n\n");
                        goto USAGE;
                    }
                    if( Core != CORE_NONE )
                    {
                        fprintf(stderr,"\nDo not specify more than one core version or use -V with -X or -x\n\n");
                        goto USAGE;
                    }
                    Core = CORE_V0 + *flags - '0';
                }
                else if( *flags == 'x' )
                {
                    if( Core != CORE_NONE )
                    {
                        fprintf(stderr,"\nDo not use -x with -X or -V\n\n");
                        goto USAGE;
                    }
                    Core = CORE_V0;
                }
                else if( *flags == 'X' )
                {
                    if( Core != CORE_NONE )
                    {
                        fprintf(stderr,"\nDo not use -X with -x or -V\n\n");
                        goto USAGE;
                    }
                    Core = CORE_V2;
                }
                else if( *flags == 'E' )
                    Options |= OPTION_BIGENDIAN;
                else if( *flags == 'b' )
                    Options |= OPTION_BINARY;
                else if( *flags == 'B' )
                    Options |= OPTION_BINARYBIG;
                else if( *flags == 'c' )
                    Options |= OPTION_CARRAY;
                else if( *flags == 'm' )
                    Options |= OPTION_IMGFILE;
                else if( *flags == 'l' )
                    Options |= OPTION_LISTING;
                else if( *flags == 'L' )
                    Options |= OPTION_SOURCELISTING;
                else if( *flags == 'N' )
                    Options |= OPTION_SOURCELISTING_ORIGINAL_MACROS;
                else if( *flags == 'n' )
                    Options |= OPTION_SOURCELISTING_NO_MACROS;
                else if( *flags == 'd' )
                    Options |= OPTION_DBGFILE;
                else if( *flags == 'f' )
                    Options |= OPTION_FBARRAY;
                else if( *flags == 'z' )
                    Options |= OPTION_DEBUG;
                else
                {
                    fprintf(stderr,"\nUnknown flag '%c'\n\n",*flags);
                    goto USAGE;
                }
                flags++;
            }
        }
    }

    if( Core==CORE_NONE )
        Core = CORE_V1;

    /* Check input file */
    if( !infile )
        goto USAGE;

    /* Check output file base - make sure no '.' */
    if( outfile )
    {
        if( strlen(outfile) > (MAXFILE-5) )
            { Report(0,REP_ERROR,"Outfile name too long"); return(RET_ERROR); }
        i=0;
        while( outfile[i] )
        {
            if( outfile[i]=='.' )
            {
                if( outfile[i+1]=='.' )
                    i++;
                else
                    { Report(0,REP_ERROR,"Outfile should be basename only - no '.'"); return(RET_ERROR); }
            }
            i++;
        }
        strcpy( outbase, outfile );
    }

    /* Test opening the main source file */
    if( !(mainsource=InitSourceFile(0,infile,0)) )
        return(RET_ERROR);

    /* Setup outfile base */
    if( !outfile )
    {
        for(i=0; mainsource->SourceName[i] && mainsource->SourceName[i]!='.'; i++ )
            outbase[i]=mainsource->SourceName[i];
        outbase[i] = 0;
    }
    if( Options & OPTION_DEBUG )
        printf("Output base filename: '%s'\n",outbase);

    /* Close the source file for now */
    CloseSourceFile( mainsource );

    /* If no output specified, default to 'C' array */
    if( !(Options & (OPTION_BINARY|OPTION_CARRAY|OPTION_BINARYBIG|OPTION_IMGFILE|OPTION_DBGFILE|OPTION_FBARRAY)) )
    {
        printf("Note: Using default output '-c' (C array *_bin.h)\n\n");
        Options |= OPTION_CARRAY;
    }

    /* Open listing file */
    if( Options & OPTION_LISTING )
    {
        strcpy( outfilename, outbase );
        strcat( outfilename, ".lst" );
        if (!(ListingFile = fopen(outfilename,"wb")))
            { Report(0,REP_ERROR,"Unable to open output file: %s",outfilename); return(RET_ERROR); }
    }

    /* Clear the binary image */
    memset( ProgramImage, 0, sizeof(ProgramImage) );

    /* Make 2 assembler passes */
    Pass        = 0;
    Errors      = 0;
    Warnings    = 0;
    FatalError  = 0;
    RetRegValue = DEFAULT_RETREGVAL;
    RetRegField = DEFAULT_RETREGFLD;
    while( !Errors && Pass<2 )
    {
        Pass++;
        CodeOffset = -1;
        HaveEntry = 0;
        EntryPoint = -1;

        /* Initialize the PP and DOT modules */
        for(i=0; i<cmdLineEquates; i++ )
            EquateCreate( &cmdLine, cmdLineName[i], cmdLineData[i] );
        DotInitialize(Pass);

        /* Process the main source file */
        if( !(mainsource=InitSourceFile(0,infile,0)) )
            break;
        ProcessSourceFile( mainsource );
        CloseSourceFile( mainsource );

        /* Cleanup the PP and DOT modules */
        if (Pass==1)
        {
            // postpone cleanup until after we've used macros in the listing
            ppCleanup(Pass);
            DotCleanup(Pass);
        }

        if( Pass==1 )
        {
            CodeOffsetPass1 = CodeOffset;
        }
    }

    /* Close the listing file */
    if( ListingFile )
        fclose( ListingFile );

    /* Make sure user didn't do something silly */
    if( CodeOffsetPass1!=CodeOffset )
    {
        fprintf(stderr,"Error: Offset changed between pass 1 and pass 2\n");
        Errors++;
    }

    /* Process the results */
    printf("\nPass %d : %d Error(s), %d Warning(s)\n\n",Pass,Errors,Warnings);
    if( Errors || CodeOffset<=0 )
        Options = 0;
    else
        printf("Writing Code Image of %d word(s)\n\n",CodeOffset);

    /* Create the output files */
    if( Options & OPTION_CARRAY )
    {
        FILE *Outfile;

        strcpy( outfilename, outbase );
        strcat( outfilename, "_bin.h" );
        if (!(Outfile = fopen(outfilename,"wb")))
            Report(0,REP_ERROR,"Unable to open output file: %s",outfilename);
        else
        {
            fprintf( Outfile, "\n\n"
                    "/* This file contains the %s instructions in a C array which are to  */\n"
                    "/* be downloaded from the host CPU to the %s instruction memory.     */\n"
                    "/* This file is generated by the %s assembler.                       */\n",
                    PROCESSOR_NAME_STRING, PROCESSOR_NAME_STRING, PROCESSOR_NAME_STRING);
            if( !nameCArraySet )
                fprintf(Outfile,"\nconst unsigned int %scode[] =  {\n",PROCESSOR_NAME_STRING);
            else
                fprintf(Outfile,"\nconst unsigned int %s[] =  {\n",nameCArray);
            for(i=0;i<(CodeOffset-1);i++)
                fprintf(Outfile,"     0x%08x,\n",ProgramImage[i].CodeWord);
            fprintf(Outfile,"     0x%08x };\n\n",ProgramImage[CodeOffset-1].CodeWord);
            fclose( Outfile );
        }
    }
    if( Options & OPTION_FBARRAY )
    {
        FILE *Outfile;

        strcpy( outfilename, outbase );
        strcat( outfilename, ".bi" );
        if (!(Outfile = fopen(outfilename,"wb")))
            Report(0,REP_ERROR,"Unable to open output file: %s",outfilename);
        else
        {
            fprintf( Outfile, "\n\n"
                    "\' This file contains the %s instructions in a FB array which are  \n"
                    "\' to be downloaded from the host CPU to the %s instruction memory.\n"
                    "\' This file is generated by the %s assembler.                     \n",
                    PROCESSOR_NAME_STRING, PROCESSOR_NAME_STRING, PROCESSOR_NAME_STRING);
            if( !nameCArraySet )
                fprintf(Outfile,"\nSTATIC SHARED AS UINTEGER %scode(...) = { _\n",PROCESSOR_NAME_STRING);
            else
                fprintf(Outfile,"\nSTATIC SHARED AS UINTEGER %s(...) = { _\n",nameCArray);
            for(i=0;i<(CodeOffset-1);i++)
                fprintf(Outfile,"  &H%08x, _\n",ProgramImage[i].CodeWord);
            fprintf(Outfile,"  &H%08x }\n\n",ProgramImage[CodeOffset-1].CodeWord);
            fclose( Outfile );
        }
    }
    if( Options & OPTION_IMGFILE )
    {
        FILE *Outfile;

        strcpy( outfilename, outbase );
        strcat( outfilename, ".img" );
        if (!(Outfile = fopen(outfilename,"wb")))
            Report(0,REP_ERROR,"Unable to open output file: %s",outfilename);
        else
        {
            for(i=0;i<CodeOffset;i++)
                fprintf(Outfile,"%08x\n",ProgramImage[i].CodeWord);
            fclose( Outfile );
        }
    }
    if( Options & OPTION_DBGFILE )
    {
        FILE *Outfile;
        uint BigEndian;

        BigEndian = 0;
        *(unsigned char *)&BigEndian = 1;
        if( BigEndian != 1 )
            BigEndian = 1;
        else
            BigEndian = 0;

        strcpy( outfilename, outbase );
        strcat( outfilename, ".dbg" );
        if (!(Outfile = fopen(outfilename,"wb")))
            Report(0,REP_ERROR,"Unable to open output file: %s",outfilename);
        else
        {
            DBGFILE_HEADER hdr;
            DBGFILE_HEADER hdr_write;
            DBGFILE_LABEL  lbl;
            DBGFILE_FILE   file;
            DBGFILE_CODE   code;
            LABEL          *pLabel;
            unsigned int file_offset;
            int i;

            memset( &hdr, 0, sizeof(DBGFILE_HEADER) );
            hdr.FileID      = DBGFILE_FILEID_VER3;
            file_offset     = sizeof(DBGFILE_HEADER);
            hdr.LabelCount  = LabelCount;
            hdr.LabelOffset = file_offset;
            file_offset += hdr.LabelCount * sizeof(DBGFILE_LABEL);
            hdr.FileCount   = sfIndex;
            hdr.FileOffset  = file_offset;
            file_offset += hdr.FileCount * sizeof(DBGFILE_FILE);
            hdr.CodeCount   = CodeOffset;
            hdr.CodeOffset  = file_offset;
            hdr.EntryPoint  = EntryPoint;

            if( Options & OPTION_BIGENDIAN )
                hdr.Flags |= DBGHDR_FLAGS_BIGENDIAN;

            if(!BigEndian)
                hdr_write = hdr;
            else
            {
                hdr_write.FileID      = HNC32(hdr.FileID);
                hdr_write.LabelCount  = HNC32(hdr.LabelCount);
                hdr_write.LabelOffset = HNC32(hdr.LabelOffset);
                hdr_write.FileCount   = HNC32(hdr.FileCount);
                hdr_write.FileOffset  = HNC32(hdr.FileOffset);
                hdr_write.CodeCount   = HNC32(hdr.CodeCount);
                hdr_write.CodeOffset  = HNC32(hdr.CodeOffset);
                hdr_write.EntryPoint  = HNC32(hdr.EntryPoint);
                hdr_write.Flags       = HNC32(hdr.Flags);
            }
            if( fwrite(&hdr_write,1,sizeof(DBGFILE_HEADER),Outfile) != sizeof(DBGFILE_HEADER) )
                Report(0,REP_ERROR,"File write error");

            pLabel = pLabelList;
            for( i=0; i<(int)hdr.LabelCount; i++ )
            {
                memset( &lbl, 0, sizeof(DBGFILE_LABEL) );
                if( !pLabel )
                    Report(0,REP_ERROR,"Fatal label tracking error");
                else
                {
                    lbl.AddrOffset = pLabel->Offset;
                    strcpy(lbl.Name,pLabel->Name);
                    if(BigEndian)
                        lbl.AddrOffset = HNC32(lbl.AddrOffset);
                    if( fwrite(&lbl,1,sizeof(DBGFILE_LABEL),Outfile) != sizeof(DBGFILE_LABEL) )
                        Report(0,REP_ERROR,"File write error");
                    pLabel = pLabel->pNext;
                }
            }

            for(i=0; i<(int)hdr.FileCount; i++)
            {
                memset( &file, 0, sizeof(DBGFILE_FILE) );
                if( !strcmp( sfArray[i].SourceBaseDir,".") ||
		    !strcmp( sfArray[i].SourceBaseDir,"./.") ||
                    ((strlen(sfArray[i].SourceName)+strlen(sfArray[i].SourceBaseDir)) >= DBGFILE_NAMELEN_SHORT) )
                    strcpy(file.SourceName,sfArray[i].SourceName);
                else
                {
                    strcpy(file.SourceName,sfArray[i].SourceBaseDir);
                    strcat(file.SourceName,"/");
                    strcat(file.SourceName,sfArray[i].SourceName);
                }
                if( fwrite(&file,1,sizeof(DBGFILE_FILE),Outfile) != sizeof(DBGFILE_FILE) )
                    Report(0,REP_ERROR,"File write error");
            }

            for(i=0; i<(int)hdr.CodeCount; i++)
            {
                memset( &code, 0, sizeof(DBGFILE_CODE) );
                code.Flags      = ProgramImage[i].Flags;
                code.Resv8      = ProgramImage[i].Resv8;
                code.FileIndex  = ProgramImage[i].FileIndex;
                code.Line       = ProgramImage[i].Line;
                code.AddrOffset = ProgramImage[i].AddrOffset;
                code.CodeWord   = ProgramImage[i].CodeWord;
                if(BigEndian)
                {
                    code.FileIndex  = HNC16(code.FileIndex);
                    code.Line       = HNC32(code.Line);
                    code.AddrOffset = HNC32(code.AddrOffset);
                    code.CodeWord   = HNC32(code.CodeWord);
                }
                if( fwrite(&code,1,sizeof(DBGFILE_CODE),Outfile) != sizeof(DBGFILE_CODE) )
                    Report(0,REP_ERROR,"File write error");
            }
            fclose( Outfile );
        }
    }
    if( Options & OPTION_SOURCELISTING )
    {
        FILE *Outfile;

        strcpy( outfilename, outbase );
        strcat( outfilename, ".txt" );
        if (!(Outfile = fopen(outfilename,"wb")))
            Report(0,REP_ERROR,"Unable to open output file: %s",outfilename);
        else
        {
            char FullPath[SOURCE_BASE_DIR+SOURCE_NAME];

            for( i=0; i<(int)sfIndex; i++ )
            {
                fprintf(Outfile, "Source File %d : '%s' ", i+1, sfArray[i].SourceName);
                strcpy(FullPath,sfArray[i].SourceBaseDir);
                strcat(FullPath, "/");
                strcat(FullPath,sfArray[i].SourceName);
                sfArray[i].FilePtr=fopen(FullPath,"rb");
                if( sfArray[i].FilePtr!=0 )
                {
                    sfArray[i].CurrentLine   = 1;
                    sfArray[i].CurrentColumn = 1;
                    sfArray[i].LastChar      = 0;
                    ListFile(Outfile,&sfArray[i]);
                    fclose(sfArray[i].FilePtr);
                    fprintf(Outfile, "\n\n");
                }
                else
                    fprintf(Outfile, "(File Not Found '%s')\n\n",FullPath);
            }

            fclose(Outfile);
        }
    }
    if( Options & OPTION_BINARY )
    {
        FILE *Outfile;

        strcpy( outfilename, outbase );
        strcat( outfilename, ".bin" );
        if (!(Outfile = fopen(outfilename,"wb")))
            Report(0,REP_ERROR,"Unable to open output file: %s",outfilename);
        else
        {
            unsigned char tmp;

            /* Write out as Little Endian */
            for(i=0;i<CodeOffset;i++)
            {
                tmp = (unsigned char)ProgramImage[i].CodeWord;
                fwrite(&tmp,1,1,Outfile);
                tmp = (unsigned char)(ProgramImage[i].CodeWord>>8);
                fwrite(&tmp,1,1,Outfile);
                tmp = (unsigned char)(ProgramImage[i].CodeWord>>16);
                fwrite(&tmp,1,1,Outfile);
                tmp = (unsigned char)(ProgramImage[i].CodeWord>>24);
                fwrite(&tmp,1,1,Outfile);
            }
            fclose( Outfile );
        }
    }
    if( Options & OPTION_BINARYBIG )
    {
        FILE *Outfile;

        strcpy( outfilename, outbase );
        strcat( outfilename, ".bib" );
        if (!(Outfile = fopen(outfilename,"wb")))
            Report(0,REP_ERROR,"Unable to open output file: %s",outfilename);
        else
        {
            unsigned char tmp;

            /* Write out as Big Endian */
            for(i=0;i<CodeOffset;i++)
            {
                tmp = (unsigned char)(ProgramImage[i].CodeWord>>24);
                fwrite(&tmp,1,1,Outfile);
                tmp = (unsigned char)(ProgramImage[i].CodeWord>>16);
                fwrite(&tmp,1,1,Outfile);
                tmp = (unsigned char)(ProgramImage[i].CodeWord>>8);
                fwrite(&tmp,1,1,Outfile);
                tmp = (unsigned char)ProgramImage[i].CodeWord;
                fwrite(&tmp,1,1,Outfile);
            }
            fclose( Outfile );
        }
    }

    /* postponed cleanup from second pass while we were using the macros in OPTION_SOURCELISTING */
    ppCleanup(Pass);
    DotCleanup(Pass);
    /* Assember label cleanup */
    while( pLabelList )
        LabelDestroy( pLabelList );

    if( Errors || CodeOffset<=0 )
        return(RET_ERROR);
    return(RET_SUCCESS);
}


/*
// ProcessSourceFile
//
// New source file to assemble.
//
// Returns 1 on success, 0 on error
*/
#define MAX_SOURCE_LINE 8192
int ProcessSourceFile( SOURCEFILE *ps )
{
    int     i;
    char*   src = (char*)malloc(MAX_SOURCE_LINE);

    for(;;)
    {
        /* Abort on a total disaster */
        if( FatalError || Errors >= 25 )
            { fprintf(stderr,"Aborting...\n"); return(0); }

        /* Get a line of source code */
        i = GetSourceLine( ps, src, MAX_SOURCE_LINE );
        if( !i )
        {
            free(src);
            return(1);
        }
        if( i<0 )
            continue;

        if( !ProcessSourceLine(ps, i, src, MAX_SOURCE_LINE) && Pass==2 )
        {
            free(src);
            return(0);
        }
    }
}


/*
// ProcessSourceLine
//
// New source line to assemble.
//
// Returns 1 on success, 0 on error
*/
int ProcessSourceLine( SOURCEFILE *ps, int length, char *src, int MaxLen )
{
    char    *pParams[MAX_TOKENS];
    SRCLINE sl;
    int     i,rc;

REPEAT:
    if( !ParseSourceLine(ps,length,src,&sl) )
        return(0);

    /* Process Label */
    if( sl.Flags & SRC_FLG_LABEL )
    {
        /* Make sure offset is ready */
        if( !ValidateOffset(ps) )
            return(0);

        /* Create Label */
        if( Pass==1 )
        {
            LabelCreate(ps, sl.Label, CodeOffset);
        }

        /* Note it in listing file */
        if( Pass==2 && (Options & OPTION_LISTING) )
        {
            fprintf(ListingFile,"%s(%5d) : 0x%04x = Label      : %s:\n",
                    ps->SourceName,ps->CurrentLine,CodeOffset,sl.Label);
        }
    }

    /* Process Command/Opcode */
    if( sl.Terms )
    {
        /* Get the parameters into a collection of string pointers */
        for(i=0; i<(int)sl.Terms; i++)
            pParams[i]=sl.Term[i];
        for( ; i<MAX_TOKENS; i++ )
            pParams[i]=0;

        /* Perform structure processing */
        if (!CheckMacro(pParams[0]))
            for(i=0; i<(int)sl.Terms; i++)
                if( StructParamProcess(ps, i, pParams[i])<0 )
                    { Report(ps,REP_ERROR,"Error in struct parsing parameter %d",i); return(0); }

        /* Process a dot command */
        if( sl.Flags & SRC_FLG_DOTCMD1 )
        {
            src[0] = 0;

            rc = DotCommand(ps,sl.Terms,pParams,src,MaxLen);
            if( rc<0 )
                return(0);
            if( !rc )
                return(1);
            /*
            // The dot command generated new code, process it now
            */
            goto REPEAT;
        }
        else
        {
            /* Process the macro or opcode */
            if (CheckMacro(pParams[0]))
            {
                // Process Macros
                if ( !ProcessMacro(ps, sl.Terms, pParams) )
                    return (0);
            }
            else
            {
                // Process Opcodes
                if( !ProcessOp(ps, sl.Terms, pParams) )
                {
                    GenOp( ps, sl.Terms, pParams, 0xFFFFFFFF );
                    return (0);
                }
            }
        }
    }
    return(1);
}


/*
// ParseSourceLine
//
// New source line to parse.
//
// Returns 1 on success, 0 on error
*/
int ParseSourceLine( SOURCEFILE *ps, int length, char *src, SRCLINE *pa )
{
    char    c;
    int     srcIdx,wordIdx;
    int     parmCnt;

    srcIdx = 0;
    pa->Flags = 0;
    pa->Terms = 0;

PROCESS_LINE:
    /* Make sure character 1 is legal */
    c = src[srcIdx++];
    if( !LabelChar(c,1) && c!='.' )
    {
        Report(ps,REP_ERROR,"Syntax error in Cmd/Opcode");
        return(0);
    }

    /* Get the Opcode or Command */
    wordIdx = 0;
    while( LabelChar(c,0) || c=='.' )
    {
        if( wordIdx>=(TOKEN_MAX_LEN-1) )
            { Report(ps,REP_ERROR,"Cmd/Opcode too long"); return(0); }
        pa->Term[0][wordIdx++] = c;
        c = src[srcIdx++];
    }
    pa->Term[0][wordIdx]=0;

    /* See if it is a label */
    if( c==':' )
    {
        if( pa->Flags & SRC_FLG_LABEL )
            { Report(ps,REP_ERROR,"Two labels found on the same line"); return(0); }
        pa->Flags |= SRC_FLG_LABEL;
        strcpy(pa->Label,pa->Term[0]);

        /* Process any assembly after the label */
        c = src[srcIdx];
        if( c!=0 )
        {
            while( c==' ' || c==0x9 )
                c = src[++srcIdx];
            goto PROCESS_LINE;
        }
        return(1);
    }

    if( c!=' ' && c!=0 && c!=0x9 )
    {
        Report(ps,REP_ERROR,"Syntax error in Cmd/Opcode");
        return(0);
    }

    /* Get up to "MAX_TOKENS-1" parameters (comma delimited) */
    parmCnt=0;
    while(c)
    {
        wordIdx=0;
        parmCnt++;
        if( parmCnt==MAX_TOKENS )
            { Report(ps,REP_ERROR,"Too many parameters on line"); return(0); }

        /* Trim off leading white space */
        while( c==' ' || c==0x9 )
            c = src[srcIdx++];

        if( !LabelChar(c,0) &&
                    c!='.' && c!='#' && c!='-' && c!='(' && c!='"' && c!='&' && c!='*' )
            { Report(ps,REP_ERROR,"Syntax error in parameter %d",parmCnt); return(0); }

        if( parmCnt==1 && c=='.' )
        {
            while( c!=0 && c!=',' && c!=' ' && c!=0x9 )
            {
                if( wordIdx>=(TOKEN_MAX_LEN-1) )
                    { Report(ps,REP_ERROR,"Parameter %d too long",parmCnt); return(0); }
                pa->Term[parmCnt][wordIdx++] = c;
                c = src[srcIdx++];

            }
            if(c==' ' || c==0x9)
                c=',';

            pa->Flags |= SRC_FLG_DOTCMD2;
        }
        else
        {
            while( c!=0 && c!=',' )
            {
                if( wordIdx>=(TOKEN_MAX_LEN-1) )
                    { Report(ps,REP_ERROR,"Parameter %d too long",parmCnt); return(0); }
                pa->Term[parmCnt][wordIdx++] = c;
                c = src[srcIdx++];
            }
        }
        pa->Term[parmCnt][wordIdx] = 0;

        /* Trim off trailing white space */
        while( wordIdx && (pa->Term[parmCnt][wordIdx-1]==0x9 || pa->Term[parmCnt][wordIdx-1]==' ') )
            pa->Term[parmCnt][--wordIdx]=0;

        /* This character must be a comma or NULL */
        if( c==',' )
            c = src[srcIdx++];
        else if( c )
            { Report(ps,REP_ERROR,"Syntax error in parameter %d",parmCnt); return(0); }
    }

    parmCnt++;
    pa->Terms = parmCnt;

    /* If its a dot command, mark it */
    if( pa->Term[0][0]=='.' )
        pa->Flags |= SRC_FLG_DOTCMD1;

    return(1);
}


/*
// GenOp
//
// Generate an opcode to the ouput file
//
// ps      - Pointer to source file record
// TermCnt - Number of terms (including the command)
// pTerms  - Pointer to the terms
// opcode  - Generated Opcode
*/
void GenOp( SOURCEFILE *ps, int TermCnt, char **pTerms, uint opcode )
{
    int i;

    if( !ValidateOffset(ps) )
        return;

    if( (Options & OPTION_LISTING) && Pass==2 )
    {
        fprintf(ListingFile,"%s(%5d) : 0x%04x = 0x%08x :     ",
               ps->SourceName,ps->CurrentLine,CodeOffset,opcode);
        fprintf(ListingFile,"%-8s ",pTerms[0]);
        for(i=1; i<TermCnt; i++)
        {
            if( i>1 )
                fprintf(ListingFile,", %s",pTerms[i]);
            else
                fprintf(ListingFile,"%s",pTerms[i]);
        }
        if( opcode==0xFFFFFFFF )
            fprintf(ListingFile,"  // *** ERROR ***");

        fprintf(ListingFile,"\n");
    }

    ProgramImage[CodeOffset].Flags      = CODEGEN_FLG_FILEINFO|CODEGEN_FLG_CANMAP;
    ProgramImage[CodeOffset].FileIndex  = ps->FileIndex;
    ProgramImage[CodeOffset].Line       = ps->CurrentLine;
    ProgramImage[CodeOffset].AddrOffset = CodeOffset;
    if(ps->MacroData) {
        ProgramImage[CodeOffset].MacroData = *ps->MacroData;
    }
     else
        ProgramImage[CodeOffset].MacroData.IsMacro = 0;
    ProgramImage[CodeOffset++].CodeWord = opcode;
}


/*
// Report
//
// Report an abnormal condition
*/
void Report( SOURCEFILE *ps, int Level, char *fmt, ... )
{
    va_list arg_ptr;

    if( Pass==1 && Level==REP_WARN2 )
        return;
    if( Pass==2 && (Level==REP_INFO || Level==REP_WARN1) )
        return;

    FILE* file;
    if( ( Level == REP_FATAL ) || ( Level == REP_ERROR ) || ( Level==REP_WARN1 || Level==REP_WARN2 ))
	    file = stderr;
    else
	    file = stdout;

    /* Log to stdout or stderr accordingly.
     * We adhere here to the exact same output format that compilers (gcc,
     * clang) or other source code processing tool is reporting messages:
     * <filename>:<line-number>: <message>
     * This standard format is parsed by default by IDEs and editors
     * such as emacs or vi than then can jump to error messages easily. */
    if( ps )
		fprintf(file, "%s:%d: ",ps->SourceName,ps->CurrentLine);

    if( Level == REP_FATAL )
    {
        fprintf(file,"Fatal Error: ");
        FatalError=1;
        Errors++;
    }
    else if( Level == REP_ERROR )
    {
        fprintf(file,"Error: ");
        Errors++;
    }
    else if( Level==REP_WARN1 || Level==REP_WARN2 )
    {
        fprintf(file,"Warning: ");
        Warnings++;
    }
    else
        fprintf(file, "Note: ");

    va_start( arg_ptr, fmt );
    vfprintf(file, fmt, arg_ptr );
    va_end( arg_ptr );

    if( !ps )
		fprintf(file,"\n");
    fprintf(file,"\n");
}


/*
// LabelChar
//
// Return whether the character is legal for a label.
// Numbers are not allowed when FlagFirstChar is set.
//
// Returns 1 on success, 0 on error
*/
int LabelChar( char c, int FlagFirstChar )
{
    if( FlagFirstChar )
    {
        if( (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c=='_') )
            return(1);
        else
            return(0);
    }
    if( (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||(c=='_'))
        return(1);
    else
        return(0);
}


/*
// LabelCreate
//
// Create a label with the supplied offset value
//
// Returns 1 on success, 0 on error
*/
int LabelCreate( SOURCEFILE *ps, char *label, int value )
{
    LABEL *pl;

    if( strlen(label) >= LABEL_NAME_LEN )
        { Report(ps,REP_ERROR,"Label too long"); return(0); }

    /* Make sure this name is OK to use */
    if( !CheckName(ps,label) )
        return(0);

    /* Allocate a new record */
    pl = malloc(sizeof(LABEL));
    if( !pl )
        { Report(ps,REP_FATAL,"Memory allocation failed"); return(0); }

    strcpy( pl->Name, label );
    pl->Offset = value;

    /* Put this label in the master list */
    pl->pPrev  = 0;
    pl->pNext  = pLabelList;
    if( pLabelList )
        pLabelList->pPrev = pl;
    pLabelList = pl;
    LabelCount++;

    if( (Options & OPTION_DEBUG) )
        printf("%s(%5d) : LABEL  : '%s' = %05d\n", ps->SourceName,ps->CurrentLine,label,value);

    return(1);
}


/*
// LabelFind
//
// Searches for an equate by name. If found, returns the record pointer.
//
// Returns LABEL * on success, 0 on error
*/
LABEL *LabelFind( char *name )
{
    LABEL *pl;

    pl = pLabelList;
    while( pl )
    {
        if( !strcmp( name, pl->Name ) )
            break;
        pl = pl->pNext;
    }
    return(pl);
}


/*
// LabelDestroy
//
// Frees and label record.
//
// void
*/
void LabelDestroy( LABEL *pl )
{
    if( !pl->pPrev )
        pLabelList = pl->pNext;
    else
        pl->pPrev->pNext = pl->pNext;

    if( pl->pNext )
        pl->pNext->pPrev = pl->pPrev;

    LabelCount--;

    free(pl);
}


/*
// Check Name
//
// Returns 1 if the name is free, or 0 if it is in use
*/
int CheckName( SOURCEFILE *ps, char *name )
{
    /* Make sure its not a reserved word */
    if( CheckTokenType(name)!=TOKENTYPE_UNRESERVED )
        { Report(ps,REP_ERROR,"Illegal use of reserved word '%s'",name); return(0); }
    if( LabelFind(name) )
        { Report(ps,REP_ERROR,"'%s' is already a label",name); return(0); }
    if( CheckEquate(name) )
        { Report(ps,REP_ERROR,"'%s' is already an equate",name); return(0); }
    if( CheckStruct(name) )
        { Report(ps,REP_ERROR,"'%s' is already a structure or scope",name); return(0); }
    if( CheckMacro(name) )
        { Report(ps,REP_ERROR,"'%s' is already a macro",name); return(0); }
    return(1);
}


/*===================================================================
//
// Private Functions
//
====================================================================*/

/*
// ValidateOffset
//
// Validates that the current offset is ready to be used
//
// Returns 1 on success, 0 on error
*/
static int ValidateOffset( SOURCEFILE *ps )
{
    uint opcode;

    if( CodeOffset==-1 )
    {
        CodeOffset = 8;
        if( EntryPoint<0 )
            EntryPoint = 8;
        if( Core != CORE_V0 )
            Report(ps,REP_WARN1,"Using default code origin of 8");
        else
        {
            opcode = 0x21000900;

            /* Note it in listing file */
            if( Pass==2 && (Options & OPTION_LISTING) )
            {
                fprintf(ListingFile,
                        "%s(%5d) : 0x%04x = 0x%08x :     JMP      #0x9 // Legacy Mode\n",
                        ps->SourceName,ps->CurrentLine,CodeOffset,opcode);
            }

            ProgramImage[CodeOffset].Flags      = CODEGEN_FLG_FILEINFO;
            ProgramImage[CodeOffset].FileIndex  = ps->FileIndex;
            ProgramImage[CodeOffset].Line       = ps->CurrentLine;
            ProgramImage[CodeOffset].AddrOffset = CodeOffset;
            ProgramImage[CodeOffset++].CodeWord = opcode;
        }
    }

    if( CodeOffset >= MAX_PROGRAM )
        { Report(ps,REP_FATAL,"Max program size exceeded"); return(0); }

    return(1);
}

static int PrintLineFromSource( FILE *pfOut, unsigned int i, unsigned int line )
{
    if (!pfOut)
        return 0;
    if (i >= sfIndex)
        return 0;
    char FullPath[SOURCE_BASE_DIR+SOURCE_NAME];
    snprintf(FullPath,sizeof(FullPath),"%s/%s",sfArray[i].SourceBaseDir,sfArray[i].SourceName);
    FILE* file = fopen(FullPath, "r");
    unsigned int currentLine = 1; // start from line 1
    unsigned int printed = 0;
    for(;file && !printed;)
    {
        char Chunk[1024];
        size_t ret = fread(&Chunk, 1, sizeof(Chunk), file);
        if (!ret)
            break;
        for(unsigned int n = 0; n < ret; ++n)
        {
            char c = Chunk[n];
            if(currentLine == line)
            {
                fprintf(pfOut, "%c", c);
            }
            if('\n' == c)
            {
                if( currentLine == line )
                {
                    printed = 1;
                    break; // done
                }
                currentLine++; // keep looking for line
            }
        }
    }
    if( !printed )
        fprintf(pfOut, "\n"); // in case anything goes wrong, still create a new line
    fclose(file);
    return 0;
}
/*
// PrintLine
//
// Prints out a line of the source file for source listings
//
// Returns 1 on success, 0 on EOF
*/
static int PrintLine( FILE *pfOut, SOURCEFILE *ps )
{
    int i;
    char c;

AGAIN:
    i = fread( &c, 1, 1, ps->FilePtr );
    if( i != 1 )
        return(0);
    if( c == 0xd )
        goto AGAIN;
    if( c == 0xa )
    {
        ps->CurrentLine++;
        fprintf(pfOut,"\n");
        return(1);
    }
    fprintf(pfOut,"%c",c);
    goto AGAIN;
}

/*
// GetInfoFromAddr
//
// Returns the SourceFileIndex, Line Number, and CodeWord for a given address offset
//
// Returns 0 on success, -1 on error
*/
static int GetInfoFromAddr( uint address, uint *pIndex, uint *pLineNo, MACRODATA *pMacroData, uint *pCodeWord )
{
    // Return data if within written portion of array and entry has been set
    if (address < CodeOffset && ProgramImage[address].AddrOffset == address) {
       *pIndex = ProgramImage[address].FileIndex;
       *pLineNo = ProgramImage[address].Line;
       *pMacroData = ProgramImage[address].MacroData;
       *pCodeWord = ProgramImage[address].CodeWord;
       return 0;
    }
    return -1;
}

/*
// ListFile
//
// Prints out an object code annotated listing of an original source file
//
// Returns 1 on success
*/
static int ListFile( FILE *pfOut, SOURCEFILE *ps )
{
    uint addr, index, line, code, count, output, cline;
    MACRODATA md;

    count = 0;
    for( addr=0; addr<(uint)CodeOffset; addr++ )
    {
        if( GetInfoFromAddr( addr, &index, &line, &md, &code ) >= 0 )
        {
            if( index == ps->FileIndex )
                count++;
        }
    }

    if( !count )
    {
        // No code section
        fprintf(pfOut,"(No Output Generated)\n\n");

        for(;;)
        {
            fprintf(pfOut,"%5d :                   : ",ps->CurrentLine );
            if( !PrintLine(pfOut,ps) )
                return(1);
        }
    }
    else
    {
        fprintf(pfOut,"(%d Instructions Generated)\n\n",count);

        for(;;)
        {
            output = 0;
            cline = ps->CurrentLine;

            for( addr=0; addr<(uint)CodeOffset; addr++ )
            {
                MACRODATA md;
                if( (GetInfoFromAddr( addr, &index, &line, &md, &code ) < 0) || index!=ps->FileIndex || line<cline )
                    continue;

                if( line == cline )
                {
                    int printMacroNow = md.IsMacro && !( Options & OPTION_SOURCELISTING_NO_MACROS );
                    if( !output )
                    {
                        fprintf(pfOut,"%5d : ",line);
                        if ( printMacroNow ) // leave addr/code blank, will be printed in macro below
                            fprintf(pfOut,"%18s: ", "");
                        else
                            fprintf(pfOut,"0x%04x 0x%08x : ",addr,code );
                        if( !PrintLine(pfOut,ps) )
                            return(1);
                        output = 1;
                    }
                    else if( !printMacroNow )
                    {
                        fprintf(pfOut,"      : 0x%04x 0x%08x :\n",addr,code );
                    }
                    if( printMacroNow )
                    {
                        unsigned int lineInFile = md.Macro->LineNumbers[md.LineInMacro];
                        fprintf(pfOut,"%5d : %20s: %d : 0x%04x 0x%08x : ",line,md.Macro->Name,lineInFile,addr,code);
                        if ( md.IsMacro && ( Options & OPTION_SOURCELISTING_ORIGINAL_MACROS ) )
                        {
                            PrintLineFromSource( pfOut, md.Macro->SourceIndex, lineInFile );
                        } else {
                            fprintf(pfOut, "%s\n", md.Macro->Code[md.LineInMacro]);
                        }
                    }
                }
            }

            if( !output )
            {
                fprintf(pfOut,"%5d :                   : ",ps->CurrentLine );
                if( !PrintLine(pfOut,ps) )
                    return(1);
            }
        }
    }
    return(1);
}

void PrintSourceFile(SOURCEFILE* s)
{
    printf("pParent: %p\n", s->pParent);
    printf("FilePtr: %p\n", s->FilePtr);
    printf("InUse: %d\n", s->InUse);
    printf("FileIndex: %d\n", s->FileIndex);
    printf("CurrentLine: %d\n", s->CurrentLine);
    printf("CurrentColumn: %d\n", s->CurrentColumn);
    printf("ccDepthIn: %d\n", s->ccDepthIn);
    printf("LastChar: %c\n", s->LastChar);
    printf("SourceName: %s\n", s->SourceName);
    printf("SourceBaseDir: %s\n", s->SourceBaseDir);
}
