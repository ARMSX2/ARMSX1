#ifndef XA_H
#define XA_H

/*
  000h 0Ch  Sync
  00Ch 4    Header (Minute,Second,Sector,Mode=02h)
  010h 4    Sub-Header (File, Channel, Submode with bit5=1, Codinginfo)
  014h 4    Copy of Sub-Header
  018h 914h Data (2324 bytes)
  92Ch 4    EDC (checksum accross [010h..92Bh]) (or 00000000h if no EDC)
*/

enum {
    XA_HDR_MINUTE = 0x0c,
    XA_HDR_SECOND,
    XA_HDR_SECTOR,
    XA_HDR_MODE,
    XA_SHDR_FILE,
    XA_SHDR_CHANNEL,
    XA_SHDR_SUBMODE,
    XA_SHDR_CODINGINFO
};

/*
  0   End of Record (EOR) (all Volume Descriptors, and all sectors with EOF)
  1   Video     ;\Sector Type (usually ONE of these bits should be set)
  2   Audio     ; Note: PSX .STR files are declared as Data (not as Video)
  3   Data      ;/
  4   Trigger           (for application use)
  5   Form2             (0=Form1/800h-byte data, 1=Form2, 914h-byte data)
  6   Real Time (RT)
  7   End of File (EOF) (or end of Directory/PathTable/VolumeTerminator)
*/

enum {
    XA_SM_EOR   = 0x01,
    XA_SM_VIDEO = 0x02,
    XA_SM_AUDIO = 0x04,
    XA_SM_DATA  = 0x08,
    XA_SM_TRIG  = 0x10,
    XA_SM_FORM2 = 0x20,
    XA_SM_RT    = 0x40,
    XA_SM_EOF   = 0x80
};

/*
  0-1 Mono/Stereo     (0=Mono, 1=Stereo, 2-3=Reserved)
  2-2 Sample Rate     (0=37800Hz, 1=18900Hz, 2-3=Reserved)
  4-5 Bits per Sample (0=Normal/4bit, 1=8bit, 2-3=Reserved)
  6   Emphasis        (0=Normal/Off, 1=Emphasis)
  7   Reserved        (0)
*/

enum {
    XA_CI_MODE       = 0x03,
    XA_CI_SAMPLERATE = 0x0c,
    XA_CI_BPS        = 0x30,
    XA_CI_EMPHASIS   = 0x40,
    XA_CI_MONO       = 0x00,
    XA_CI_STEREO     = 0x01,
    XA_CI_37800HZ    = 0x00,
    XA_CI_18900HZ    = 0x04,
    XA_CI_4BIT       = 0x00,
    XA_CI_8BIT       = 0x10
};

void xa_decode_sector(void*, void*);

#endif