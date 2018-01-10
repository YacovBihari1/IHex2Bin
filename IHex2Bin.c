///////////////////////////////////////////////////////////////////////////////////////////
#define DEBUG

///////////////////////////////////////////
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////////////////
#define MAX_RECORD_SIZE 270
#define MAX_PAGE_NUM    255 // 0xFF

#define FIRMW_VERSION   "1234"

///////////////////////////////////////////////////////////////////////////////////////////
// Record types
#define DATA_REC        (0) // Data Record
#define EOF_REC         (1) // The End of the Hex File
#define LINEAR_ADDR     (2) // Extended Segment Address - Not used in PIC?
#define ENTRY_POINT     (3) // Start Segment Address - Not used in PIC
#define EXTND_ADDR      (4) // Extended Linear Address
#define LINEAR_ENTRY_P  (5) // Start Linear Address - Not used in PIC

///////////////////////////////////////////////////////////////////////////////////////////
#define FOREVER         for(;;)
#define START_MARKER    ':'
#define SUCCESS         (0)
#define FAILURE         (-1)

#define FPTMP_MODE      "w+b"
#define FPOUT_MODE      "w+b"
#define FPIN_MODE       "r"

#define FPTMP_FILE      "TmpFile.bin"

#ifdef  DEBUG
    #define FDBG_MODE       "w+b"
    #define DEBUG_FILE      "DebugFile.txt"
#endif  //DEBUG

///////////////////////////////////////////////////////////////////////////////////////////
#define WORD_CHKSUM(w)  ((w>>8)+(w&0xFF))

#define ADD_TO_CURR_REC(c)  {CurrRecord[RecOffset++]=c;CurrRecord[RecOffset]='\0';}
#define PRINT_CURR_RECORD   fprintf(stdout,"\nRecord %06d: %s\n",RecordNum,CurrRecord)

///////////////////////////////////////////////////////////////////////////////////////////
static FILE     *fpin,*fpout,*fptmp;

#ifdef  DEBUG
static FILE     *fdbg;
#endif  //DEBUG

static uint8_t  CurrPage ;
static uint16_t BlockNum ;
static uint16_t RecordNum ;
static uint16_t RecOffset ;
static uint16_t DataRec ;

static uint8_t  InpChckSum ;
static uint8_t  CurrRecord[MAX_RECORD_SIZE +1];

static uint16_t UsedPage[MAX_PAGE_NUM]; // Assume initialized to '0'
static uint32_t TotData ;

typedef struct {
    uint8_t StartRec;  // ':'
    uint8_t DataLen[2];
    uint8_t Address[4];
    uint8_t RecType[2];
} HexRecHead_t;
//  Data[]
//  InpChckSum

///////////////////////////////////////////////////////////////////////////////////////////
typedef struct {
    uint16_t    BlockNum;       // 1,2,3....
    uint16_t    FirmwVers[2];   // Not sure how it's managed
    uint8_t     InstrNum;       // Normally it's DataLen/3. MAX = MAX_INSTR_BLOCK
    uint8_t     Page ;          // x00 - xFF
    uint16_t    W_Address ;     // 0x0000 - 0xFFFF
    uint16_t    Rsrvd;          // Keep it 0
    uint16_t    DataLen ;       // Number of Data bytes
} BinRecHead_t;
//  Data[]
//  OutpChckSum     // For now it's a simple one

///////////////////////////////////////////////////////////////////////////////////////////
static void
OpenFiles(int argc, char *argv[])
{
 char   *TmpP;
 char   *TmpBuff =NULL;

    if ((argc !=2) && (argc !=3)) {
        fprintf (stderr,"Illegal number of parameter\n");
        fprintf (stderr,"Usage: %s Hex-file [Bin-file]\n",argv[0]);
        exit(-1);
    }

    TmpP =strchr(argv[1],'.');

    if (NULL != TmpP ) {    // Input file have extension
        fpin=fopen(argv[1],FPIN_MODE);
        *TmpP='\0';
    }
    else {  // No extension. Let's make it ".hex"
        TmpBuff =(char *)malloc (strlen(argv[1])+strlen(".hex")+1);
        
        if (NULL ==TmpBuff ){
            fprintf(stderr,"Memory allocation error!\n");
            exit(-2);
        }

        strcpy(TmpBuff,argv[1]);
        strcat(TmpBuff,".hex");
        fpin=fopen(TmpBuff,FPIN_MODE);
        free (TmpBuff );
    }

    if (NULL ==fpin ) {
        fprintf(stderr,"Couldn't open input file %s\n",TmpBuff );
        exit(-3);
    }

    // At this point argv[1] has no extension!
    //////////////////////////////////////////
    if (argc ==3) {
        TmpP =strchr(argv[2],'.');
        
        if (NULL !=TmpP ) {
            fpout =fopen(argv[2],FPOUT_MODE);
        }
        else {
            TmpBuff =(char *)malloc (strlen(argv[2])+strlen(".bin")+1);

            if (NULL ==TmpBuff ){
                fprintf(stderr,"Memory allocation error!");
                exit(-2);
            }

            strcpy(TmpBuff,argv[2]);
            strcat(TmpBuff,".bin");
            fpout=fopen(TmpBuff,FPOUT_MODE);
            free (TmpBuff );
        }
    }
    else {  // argv[1] has no extension!
        TmpBuff =(char *)malloc (strlen(argv[1])+strlen(".bin")+1);

        if (NULL ==TmpBuff ){
            fprintf(stderr,"Memory allocation error!");
            exit(-2);
        }

        strcpy(TmpBuff,argv[1]);
        strcat(TmpBuff,".bin");

        fpout = fopen(TmpBuff,FPOUT_MODE);
        free (TmpBuff );
    }

    if (NULL ==fpout ) {
        fclose (fpin );
        fprintf(stderr,"Couldn't open output file %s\n",TmpBuff );
        exit(-3);
    }

    fptmp =fopen(FPTMP_FILE ,FPTMP_MODE);

    if (NULL ==fptmp ) {
        fclose (fpin);
        fclose (fpout);
        fprintf(stderr,"Couldn't create temporary file '%s'\n",FPTMP_FILE);
        exit(-3);
    }

#ifdef  DEBUG
    fdbg =fopen(DEBUG_FILE ,FDBG_MODE);

    if (NULL ==fdbg ) {
        fclose (fpin);
        fclose (fpout);
        fclose (fptmp);
        fprintf(stderr,"Couldn't create debug file '%s'\n",DEBUG_FILE);
        exit(-3);
    }
#endif  //DEBUG
}

static uint8_t
NextCharVal(void )
{
 int        HexChar =fgetc(fpin );
 uint8_t    RetChar =HexChar | 0x20;

    if (EOF ==HexChar ) {
        PRINT_CURR_RECORD;
        fprintf (stderr ,"Unexpected EOF!\n");
        exit (-1);
    }

    ADD_TO_CURR_REC (HexChar );

    if ((RetChar >='0') && (RetChar <='9')) RetChar &= 0xF;
    else if ((RetChar >= 'a') && (RetChar <= 'f')) RetChar -= 87;
    else {
        PRINT_CURR_RECORD;
        fprintf (stderr ,"Illegal hex character in Record %06d: %c\n", RecordNum, HexChar);
        exit(-4);
    }

    return RetChar ;
}

static uint8_t  // Reads two ASCII characters and updates 'InpChckSum'
ReadByteVal (void)
{
 uint8_t ByteVal =NextCharVal() <<4;

    ByteVal +=NextCharVal() ;
    InpChckSum +=ByteVal ;
    return ByteVal ;
}

static uint16_t // Reads four ASCII characters and updates 'InpChckSum'
ReadWordVal (void)
{
 uint16_t WordVal = ReadByteVal() <<8;

    return WordVal +ReadByteVal();
}

static void
HandleRecord (void)
{
 uint8_t    DataLen;
 uint8_t    RecType;
 uint16_t   Address;

    ++RecordNum ;
    RecOffset =0;
    InpChckSum =0;
    DataLen =ReadByteVal(); // Two characters
    Address =ReadWordVal(); // Four characters
    RecType =ReadByteVal(); // Two characters
#ifdef  DEBUG
    //fprintf(stdout,"\nDataLen =%d,Address =%d,RecType =%d",DataLen,Address,RecType);
#endif  //DEBUG

    if ((Address & 1) || (DataLen & 1)){  // Odd address? Odd record length?
        PRINT_CURR_RECORD;
        fprintf (
            stderr,
            "Record %06d has illegal (Odd) address or illegal Data length!",
            RecordNum
        );
        exit(-8);
    }

    switch (RecType) {
        case EXTND_ADDR:        // Extended Linear (DataLen is '2' and Address is '0'
            if ((2==DataLen ) && (0==Address )) {
             uint16_t HighAddress =ReadWordVal();
                
                if (HighAddress >MAX_PAGE_NUM ) {
                    PRINT_CURR_RECORD;
                    fprintf(
                        stderr,
                        "Illegal Address/Page (out of range): %04x in Record %06d\n",
                        HighAddress,
                        RecordNum
                    );
                    exit (-7);
                }

                CurrPage =HighAddress & 0xFF ;
                break;
            }

            PRINT_CURR_RECORD;
            fprintf (stderr ,"Record %06d is illegal!", RecordNum);
            exit(-4);
        case DATA_REC:          // Data Record
            if (DataLen % 4) {
                PRINT_CURR_RECORD;
                fprintf (
                    stderr,
                    "Record %06d has illegal Data length(not mod 4)!",
                    RecordNum
                );
                exit(-8);
            }

            if (DataLen ) {
/**************************
typedef struct {
    uint16_t    BlockNum;       // 1,2,3....
    uint16_t    FirmwVers[2];   // Not sure how it's managed
    uint8_t     InstrNum;       // Normally it's DataLen/3. MAX = MAX_INSTR_BLOCK
    uint8_t     Page ;          // x00 - xFF
    uint16_t    W_Address ;     // 0x0000 - 0xFFFF
    uint16_t    Rsrvd;          // Keep it 0
    uint16_t    DataLen ;       // Number of Data bytes
} BinRecHead_t;
//  Data[]
//  OutpChckSum     // Simple one (for now)
***************************/
             BinRecHead_t   Header ;
             uint16_t   W_Address =Address >>1 ;    // Divide by 2
             uint8_t    OutpChckSum ;
             uint8_t    DataB;

                ++DataRec ;
                ++UsedPage[CurrPage ];

                Header.BlockNum =++BlockNum ;
                memcpy(Header.FirmwVers ,FIRMW_VERSION ,strlen(FIRMW_VERSION ));
                Header.InstrNum =DataLen /4;
                Header.Page =CurrPage ;
                Header.W_Address =W_Address ;
                Header.Rsrvd =0;
                Header.DataLen =3*Header.InstrNum ;

                OutpChckSum =WORD_CHKSUM(Header.BlockNum) ;
                OutpChckSum +=WORD_CHKSUM(Header.FirmwVers[0]);
                OutpChckSum +=WORD_CHKSUM(Header.FirmwVers[1]);
                OutpChckSum +=Header.InstrNum ;
                OutpChckSum +=Header.Page;
                OutpChckSum +=WORD_CHKSUM(Header.W_Address);
                OutpChckSum +=WORD_CHKSUM(Header.DataLen); 
#ifdef  DEBUG
                fprintf(
                    stdout,
                    "\n %6d %2d %02X %04X %2d  ",
                    BlockNum,
                    Header.InstrNum,
                    CurrPage,
                    W_Address,
                    Header.DataLen
                );
                fprintf(
                    fdbg /*stdout*/,
                    "\n%6d %2d %02X %04X %2d",
                    Header.BlockNum,
                    Header.InstrNum,
                    Header.Page,
                    Header.W_Address,
                    Header.DataLen
                );
#endif  //DEBUG

                fwrite(&Header ,sizeof(BinRecHead_t),1,fptmp );
                TotData +=Header.DataLen ;

                while (DataLen--) {
                    DataB =ReadByteVal(); // Two characters

                    if (DataLen %4) {
                        OutpChckSum +=DataB ;
                        fwrite(&DataB ,1,1,fptmp );
#ifdef  DEBUG
                        fprintf(stdout,"%02X",DataB);
#endif  //DEBUG
                    }
#ifdef  DEBUG
                    else fprintf(stdout," ");
#endif  //DEBUG
                }

                fwrite(&OutpChckSum ,1,1,fptmp ); // OutpChckSum
#ifdef  DEBUG
                fprintf(stdout," %02X",OutpChckSum);
#endif  //DEBUG
            }

            break;
        case EOF_REC:           // The End of the Hex File (DataLen and Address are both '0')
            if ((0==DataLen ) && (0==Address )) {
                break;
            }

            PRINT_CURR_RECORD;
            fprintf (stderr ,"Record %06d is illegal!", RecordNum);
            exit(-4);
        /////////////////////////////////////////////////////////////////////
        case LINEAR_ADDR:       // Extended Segment Address - Not used in PIC
        case ENTRY_POINT:       // Start Segment Address - Not used in PIC
        case LINEAR_ENTRY_P:    // Start Linear Address - Not used in PIC
        default:
            fprintf(stderr,"\nIllegal Record Type: %02d in Record %06d\n",RecType, RecordNum );
            exit (-5);
    }

    ReadByteVal();  // Read the InpChckSum

    if (0 !=InpChckSum ) {
        PRINT_CURR_RECORD;
        fprintf (stderr,"Record ChkSum error!");
        exit(-6);
    }

#ifdef  DEBUG
    //PRINT_CURR_RECORD;
#endif  //DEBUG
}

static int
ReadRecords (void)
{
    RecordNum =0;
    DataRec =0;
    BlockNum =0;
    CurrPage =0;    // The default
    TotData =0;

    //fprintf(fdbg /*stdout*/," Block# Page# Address DataLen\n");

    FOREVER {
     int HexChar ;

        memset (CurrRecord ,'\0',MAX_RECORD_SIZE +1);

        do { // Find next START_MARKER
            HexChar =fgetc (fpin);

            if (EOF ==HexChar ) {
                if (RecordNum ) return SUCCESS;

                return EOF ;
            }
        } while (START_MARKER != HexChar);

        ADD_TO_CURR_REC(START_MARKER );
        HandleRecord ();
    }
}

int
main(int argc, char *argv[])
{
 int    RetVal ;
 
    OpenFiles(argc, argv);
    RetVal =ReadRecords();
    
    if (SUCCESS ==RetVal ) {
#ifdef  DEBUG
     uint16_t   Page =0;

        while (Page <MAX_PAGE_NUM ) {
            if (UsedPage [Page ] ) {
                fprintf (stdout ,"\nPage %d - %d records",Page,UsedPage [Page ]);
                fprintf (fdbg ,"\nPage %d - %d records",Page,UsedPage [Page ]);
            }

            ++Page;
        }

        fprintf (fdbg ,"\nTotal Data: %X (%d) bytes",TotData,TotData );
#endif  //DEBUG
        fprintf (stdout ,"\nSuccessful end. Total of %06d Records, %06d Data-records", RecordNum ,DataRec );
        fprintf (stdout ,"\nTotal Data: %X (%d) bytes",TotData,TotData );
    }
    else if (EOF ==RetVal ) {
        fprintf (stderr ,"\nInput file ended unexpectedly after %d Records!", RecordNum );
    }
    else fprintf (stderr ,"\nOperation stopped for unknown reason after %d Records!", RecordNum );

    fclose (fpin );
    fclose (fpout );
    fclose (fptmp);
#ifdef  DEBUG
    fclose (fdbg );
#endif  //DEBUG
}
