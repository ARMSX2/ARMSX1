#ifndef CDROM_H
#define CDROM_H

#include <stdint.h>

#include "frontend/diagnostics.h"

#include "queue.h"
#include "../../state.h"
#include "disc.h"
#include "../ic.h"

#define PSX_CDROM_BEGIN 0x1f801800
#define PSX_CDROM_END   0x1f801803
#define PSX_CDROM_SIZE  0x4

/*
    Command                Average   Min       Max
    GetStat (normal)       000c4e1h  0004a73h..003115bh
    GetStat (when stopped) 0005cf4h  000483bh..00093f2h
    Pause (single speed)   021181ch  020eaefh..0216e3ch ;\time equal to
    Pause (double speed)   010bd93h  010477Ah..011B302h ;/about 5 sectors
    Pause (when paused)    0001df2h  0001d25h..0001f22h
    Stop (single speed)    0d38acah  0c3bc41h..0da554dh
    Stop (double speed)    18a6076h  184476bh..192b306h
    Stop (when stopped)    0001d7bh  0001ce8h..0001eefh
    GetID                  0004a00h  0004922h..0004c2bh
    Init                   0013cceh  000f820h..00xxxxxh
*/

#define CD_DELAY_1MS 33869
#define CD_DELAY_FR 50401
#define CD_DELAY_INIT_FR 81102
#define CD_DELAY_PAUSE_SS 2168860
#define CD_DELAY_PAUSE_DS 1097107
#define CD_DELAY_STOP_SS 13863626
#define CD_DELAY_STOP_DS 25845878
#define CD_DELAY_READ_SS (33868800 / 75)
#define CD_DELAY_READ_DS (33868800 / (2*75))
#define CD_DELAY_START_READ (cdrom_get_read_delay(cdrom) + cdrom_get_seek_delay(cdrom, ts))
/* REVERTED 2026-08-01. I removed the +4ms here on the theory that the drive was capped at ~94
   sectors/s vs hardware 150. The theory was arithmetically right and the fix did NOT work: MGS's
   refill gaps went 3 -> 5, and Xenogears then looped its first FMV forever — exactly the failure
   this file already warns about ("games that stream XA audio or FMV expect data to arrive at the
   drive's real rate"). MGS's actual bug was the SPU never being cycle-ticked (see psx/psx.c).
   Any future attempt at real sector pacing must be tested against an FMV, not just a codec call. */
#define CD_DELAY_ONGOING_READ (cdrom_get_read_delay(cdrom) + (CD_DELAY_1MS * 4))

#define XA_STEREO_SAMPLES 2016 // Samples per sector
#define XA_MONO_SAMPLES 4032 // Samples per sector
#define XA_STEREO_RESAMPLE_SIZE 2352 // 2352
#define XA_MONO_RESAMPLE_SIZE 4704 // 4704
#define XA_STEREO_RESAMPLE_MAX_SIZE 4704 // 2352 * 2 (because of 18KHz mode)
#define XA_MONO_RESAMPLE_MAX_SIZE 9408 // 4704 * 2 (because of 18KHz mode)
#define XA_UPSAMPLE_SIZE 28224 // 4032 * 7
#define XA_RINGBUF_SIZE 32

/*
    7   Speed       (0=Normal speed, 1=Double speed)
    6   XA-ADPCM    (0=Off, 1=Send XA-ADPCM sectors to SPU Audio Input)
    5   Sector Size (0=800h=DataOnly, 1=924h=WholeSectorExceptSyncBytes)
    4   Ignore Bit  (0=Normal, 1=Ignore Sector Size and Setloc position)
    3   XA-Filter   (0=Off, 1=Process only XA-ADPCM sectors that match Setfilter)
    2   Report      (0=Off, 1=Enable Report-Interrupts for Audio Play)
    1   AutoPause   (0=Off, 1=Auto Pause upon End of Track) ;for Audio Play
    0   CDDA        (0=Off, 1=Allow to Read CD-DA Sectors; ignore missing EDC)
*/

#define MODE_CDDA           0x01
#define MODE_AUTOPAUSE      0x02
#define MODE_REPORT         0x04
#define MODE_XA_FILTER      0x08
#define MODE_IGNORE         0x10
#define MODE_SECTOR_SIZE    0x20
#define MODE_XA_ADPCM       0x40
#define MODE_SPEED          0x80

/*
    Command          Parameters      Response(s)
    00h -            -               INT5(11h,40h)  ;reportedly "Sync" uh?
    01h Getstat      -               INT3(stat)
    02h Setloc     E amm,ass,asect   INT3(stat)
    03h Play       E (track)         INT3(stat), optional INT1(report bytes)
    04h Forward    E -               INT3(stat), optional INT1(report bytes)
    05h Backward   E -               INT3(stat), optional INT1(report bytes)
    06h ReadN      E -               INT3(stat), INT1(stat), datablock
    07h MotorOn    E -               INT3(stat), INT2(stat)
    08h Stop       E -               INT3(stat), INT2(stat)
    09h Pause      E -               INT3(stat), INT2(stat)
    0Ah Init         -               INT3(late-stat), INT2(stat)
    0Bh Mute       E -               INT3(stat)
    0Ch Demute     E -               INT3(stat)
    0Dh Setfilter  E file,channel    INT3(stat)
    0Eh Setmode      mode            INT3(stat)
    0Fh Getparam     -               INT3(stat,mode,null,file,channel)
    10h GetlocL    E -               INT3(amm,ass,asect,mode,file,channel,sm,ci)
    11h GetlocP    E -               INT3(track,index,mm,ss,sect,amm,ass,asect)
    12h SetSession E session         INT3(stat), INT2(stat)
    13h GetTN      E -               INT3(stat,first,last)  ;BCD
    14h GetTD      E track (BCD)     INT3(stat,mm,ss)       ;BCD
    15h SeekL      E -               INT3(stat), INT2(stat)  ;\use prior Setloc
    16h SeekP      E -               INT3(stat), INT2(stat)  ;/to set target
    17h -            -               INT5(11h,40h)  ;reportedly "SetClock" uh?
    18h -            -               INT5(11h,40h)  ;reportedly "GetClock" uh?
    19h Test         sub_function    depends on sub_function (see below)
    1Ah GetID      E -               INT3(stat), INT2/5(stat,flg,typ,atip,"SCEx")
    1Bh ReadS      E?-               INT3(stat), INT1(stat), datablock
    1Ch Reset        -               INT3(stat), Delay
    1Dh GetQ       E adr,point       INT3(stat), INT2(10bytesSubQ,peak_lo) ;\not
    1Eh ReadTOC      -               INT3(late-stat), INT2(stat)           ;/vC0
    1Fh VideoCD      sub,a,b,c,d,e   INT3(stat,a,b,c,d,e)   ;<-- SCPH-5903 only
    1Fh..4Fh -       -               INT5(11h,40h)  ;-Unused/invalid
    50h Secret 1     -               INT5(11h,40h)  ;\
    51h Secret 2     "Licensed by"   INT5(11h,40h)  ;
    52h Secret 3     "Sony"          INT5(11h,40h)  ; Secret Unlock Commands
    53h Secret 4     "Computer"      INT5(11h,40h)  ; (not in version vC0, and,
    54h Secret 5     "Entertainment" INT5(11h,40h)  ; nonfunctional in japan)
    55h Secret 6     "<region>"      INT5(11h,40h)  ;
    56h Secret 7     -               INT5(11h,40h)  ;/
    57h SecretLock   -               INT5(11h,40h)  ;-Secret Lock Command
    58h..5Fh Crash   -               Crashes the HC05 (jumps into a data area)
    6Fh..FFh -       -               INT5(11h,40h)  ;-Unused/invalid
*/

#define CDL_GETSTAT    0x01
#define CDL_SETLOC     0x02
#define CDL_PLAY       0x03
#define CDL_FORWARD    0x04
#define CDL_BACKWARD   0x05
#define CDL_READN      0x06
#define CDL_MOTORON    0x07
#define CDL_STOP       0x08
#define CDL_PAUSE      0x09
#define CDL_INIT       0x0a
#define CDL_MUTE       0x0b
#define CDL_DEMUTE     0x0c
#define CDL_SETFILTER  0x0d
#define CDL_SETMODE    0x0e
#define CDL_GETPARAM   0x0f
#define CDL_GETLOCL    0x10
#define CDL_GETLOCP    0x11
#define CDL_SETSESSION 0x12
#define CDL_GETTN      0x13
#define CDL_GETTD      0x14
#define CDL_SEEKL      0x15
#define CDL_SEEKP      0x16
#define CDL_TEST       0x19
#define CDL_GETID      0x1a
#define CDL_READS      0x1b
#define CDL_RESET      0x1c
#define CDL_GETQ       0x1d
#define CDL_READTOC    0x1e
#define CDL_VIDEOCD    0x1f

/*
    ___These values appear in the FIRST response; with stat.bit0 set___
    10h - Invalid Sub_function (for command 19h), or invalid parameter value
    20h - Wrong number of parameters (most CD commands need an exact number of parameters)
    40h - Invalid command
    80h - Cannot respond yet (eg. required info was not yet read from disk yet)
            (namely, TOC not-yet-read or so)
            (also appears if no disk inserted at all)
    ___These values appear in the SECOND response; with stat.bit2 set___
    04h - Seek failed (when trying to use SeekL on Audio CDs)
    ___These values appear even if no command was sent; with stat.bit2 set___
    08h - Drive door became opened
*/

#define CD_ERR_SEEK_FAILED           0x04
#define CD_ERR_DOOR_OPENED           0x08
#define CD_ERR_INVALID_SUBFUNCTION   0x10
#define CD_ERR_WRONG_PARAMETER_COUNT 0x20
#define CD_ERR_INVALID_COMMAND       0x40
#define CD_ERR_NO_DISC               0x80

/*
    7  Play          Playing CD-DA         ;\only ONE of these bits can be set
    6  Seek          Seeking               ; at a time (ie. Read/Play won't get
    5  Read          Reading data sectors  ;/set until after Seek completion)
    4  ShellOpen     Once shell open (0=Closed, 1=Is/was Open)
    3  IdError       (0=Okay, 1=GetID denied) (also set when Setmode.Bit4=1)
    2  SeekError     (0=Okay, 1=Seek error)     (followed by Error Byte)
    1  Spindle Motor (0=Motor off, or in spin-up phase, 1=Motor on)
    0  Error         Invalid Command/parameters (followed by Error Byte)
*/

#define CD_STAT_PLAY        0x80
#define CD_STAT_SEEK        0x40
#define CD_STAT_READ        0x20
#define CD_STAT_SHELLOPEN   0x10
#define CD_STAT_IDERROR     0x08
#define CD_STAT_SEEKERROR   0x04
#define CD_STAT_SPINDLE     0x02
#define CD_STAT_ERROR       0x01

// #define SET_INT(n) cdrom->ifr = n;

enum {
    CD_STATE_IDLE,
    CD_STATE_TX_RESP1,
    CD_STATE_TX_RESP2,
    CD_STATE_READ,
    CD_STATE_PLAY
};

enum {
    QUERY_TRACK_COUNT,
    QUERY_TRACK_ADDR,
    QUERY_TRACK_TYPE
};

typedef struct {
    int mute;
    uint32_t bus_delay;
    uint32_t io_base, io_size;
    psx_disc_t* disc;
    psx_ic_t* ic;
    int disc_type;
    int version;
    int region;
    int index;
    int pending_speed_switch_delay;
    int seek_precision;
    int fake_getlocl_data;
    uint8_t ier;
    uint8_t ifr;
    uint8_t vol_pending[4];
    uint8_t vol[4];
    uint8_t mode;
    int data_req;
    queue_t* data;
    queue_t* response;
    queue_t* parameters;
    uint8_t pending_command;
    int busy;
    uint32_t xa_lba;
    int xa_playing;
    int xa_mute;
    int xa_channel;
    int xa_file;
    int xa_remaining_samples;
    int16_t xa_left_h[2];
    int16_t xa_right_h[2];
    int16_t xa_prev_left_sample;
    int16_t xa_prev_right_sample;
    int xa_sample_index;
    int state;
    int prev_state;
    int int1_pending;
    int int2_pending;
    int delay;
    uint32_t pending_lba;
    uint32_t lba;
    int16_t cdda_buf[CD_SECTOR_SIZE >> 1];
    int32_t cdda_remaining_samples;
    uint32_t cdda_sample_index;
    uint32_t cdda_sectors_played;
    int cdda_playing;
    int cdda_prev_track;
    int read_ongoing;
    uint8_t xa_buf[CD_SECTOR_SIZE];
    int16_t xa_left_buf[XA_STEREO_SAMPLES];
    int16_t xa_right_buf[XA_STEREO_SAMPLES];
    int16_t xa_mono_buf[XA_MONO_SAMPLES];
    int16_t xa_upsample_buf[XA_UPSAMPLE_SIZE];
    int16_t xa_left_resample_buf[XA_STEREO_RESAMPLE_MAX_SIZE];
    int16_t xa_right_resample_buf[XA_STEREO_RESAMPLE_MAX_SIZE];
    int16_t xa_mono_resample_buf[XA_MONO_RESAMPLE_MAX_SIZE];
    /* Path of the currently-open image. Save states never embed the disc itself; they record
       this plus a fingerprint so a state can be refused if it is loaded against a different
       game, and so the already-open disc can simply be re-seeked on load. */
    char disc_path[1024];
} psx_cdrom_t;

enum {
    CDR_VERSION_01,  // DTL-H2000                 (??-???-????)
    CDR_VERSION_C0A, // PSX (PU-7)                (19-Sep-1994)
    CDR_VERSION_C0B, // PSX (PU-7)                (18-Nov-1994)
    CDR_VERSION_C1A, // PSX (EARLY-PU-8)          (16-May-1995)
    CDR_VERSION_C1B, // PSX (LATE-PU-8)           (24-Jul-1995)
    CDR_VERSION_D1,  // PSX (LATE-PU-8, Debug)    (24-Jul-1995)
    CDR_VERSION_C2V, // PSX (PU-16, Video CD)     (15-Aug-1996)
    CDR_VERSION_C1Y, // PSX (LATE-PU-8, Yaroze)   (18-Aug-1996)
    CDR_VERSION_C2J, // PSX (PU-18) (J)           (12-Sep-1996)
    CDR_VERSION_C2A, // PSX (PU-18) (U/E)         (10-Jan-1997)
    CDR_VERSION_C2B, // PSX (PU-20)               (14-Aug-1997)
    CDR_VERSION_C3A, // PSX (PU-22)               (10-Jul-1998)
    CDR_VERSION_C3B, // PSX/PSone (PU-23, PM-41)  (01-Feb-1999)
    CDR_VERSION_C3C  // PSone/late (PM-41(2))     (06-Jun-2001)
};

enum {
    CDR_REGION_JAPAN,
    CDR_REGION_EUROPE,
    CDR_REGION_AMERICA
};

uint8_t cdrom_get_stat(psx_cdrom_t* cdrom);
void cdrom_error(psx_cdrom_t* cdrom, uint8_t stat, uint8_t err);
int cdrom_get_seek_delay(psx_cdrom_t* cdrom, int ts);
int cdrom_get_pause_delay(psx_cdrom_t* cdrom);
int cdrom_get_read_delay(psx_cdrom_t* cdrom);
void cdrom_set_int(psx_cdrom_t* cdrom, int n);
void cdrom_process_setloc(psx_cdrom_t* cdrom);

psx_cdrom_t* psx_cdrom_create(void);
void psx_cdrom_init(psx_cdrom_t* cdrom, psx_ic_t* ic);
void psx_cdrom_reset(psx_cdrom_t* cdrom);
void psx_cdrom_set_version(psx_cdrom_t* cdrom, int version);
void psx_cdrom_set_region(psx_cdrom_t* cdrom, int region);
int psx_cdrom_open(psx_cdrom_t* cdrom, const char* path);
void psx_cdrom_close(psx_cdrom_t* cdrom);
uint32_t psx_cdrom_read32(psx_cdrom_t* cdrom, uint32_t addr);
uint32_t psx_cdrom_read16(psx_cdrom_t* cdrom, uint32_t addr);
uint32_t psx_cdrom_read8(psx_cdrom_t* cdrom, uint32_t addr);
void psx_cdrom_write32(psx_cdrom_t* cdrom, uint32_t addr, uint32_t value);
void psx_cdrom_write16(psx_cdrom_t* cdrom, uint32_t addr, uint32_t value);
void psx_cdrom_write8(psx_cdrom_t* cdrom, uint32_t addr, uint32_t value);
void psx_cdrom_update(psx_cdrom_t* cdrom, int cycles);
void psx_cdrom_get_audio_samples(psx_cdrom_t* cdrom, void* buf, size_t size);
/* CdlGetstat, CdlSetmode, ... for the `audio_diag` command log. Never NULL. */
const char* psx_cdrom_command_name(uint8_t cmd);
void psx_cdrom_destroy(psx_cdrom_t* cdrom);

/* --- save-state support (see psx/state.h) -------------------------------------------------
   The disc is a HOST resource: it is never serialised. Instead a state records the disc's
   identity (path + fingerprint + track count) and only the controller's logical state, so
   loading re-seeks the disc that is already open. */
const char* psx_cdrom_get_disc_path(psx_cdrom_t* cdrom);
int psx_cdrom_get_disc_track_count(psx_cdrom_t* cdrom);
uint64_t psx_cdrom_get_disc_fingerprint(psx_cdrom_t* cdrom);
void psx_cdrom_save_state(psx_cdrom_t*, psx_state_writer_t*);
int psx_cdrom_load_state(psx_cdrom_t*, psx_state_reader_t*);

/* CD-ROM speedup. Divisors on the emulated drive's read and seek delays; 1 = authentic timing.
   Read speedup is the one that can break streamed XA audio and FMV — see cdrom.c. */
void psx_cdrom_set_read_speedup(unsigned factor);
void psx_cdrom_set_seek_speedup(unsigned factor);
unsigned psx_cdrom_get_read_speedup(void);
unsigned psx_cdrom_get_seek_speedup(void);

#endif
