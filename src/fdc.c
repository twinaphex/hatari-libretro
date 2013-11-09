/*
  Hatari - fdc.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Floppy Disk Controller(FDC) emulation.
  All commands are emulated with good timings estimation, as many programs
  (demo or cracked games) rely on accurate FDC timings and DMA transfer by blocks
  of 16 bytes.
  The behaviour of all FDC's registers matches the official docs and should not
  cause programs to fail when accessing the FDC (especially for Status Register).
  As Hatari only handles ST/MSA disk images that only support 512 bytes sectors as
  well as a fixed number of sectors per track, a few parts of the FDC emulation are
  simplified and would need to be changed to handle more complex disk images (Pasti).
*/

const char FDC_fileid[] = "Hatari fdc.c : " __DATE__ " " __TIME__;

#include "main.h"
#include "configuration.h"
#include "fdc.h"
#include "hdc.h"
#include "floppy.h"
#include "ioMem.h"
#include "log.h"
#include "m68000.h"
#include "memorySnapShot.h"
#include "mfp.h"
#include "psg.h"
#include "stMemory.h"
#include "screen.h"
#include "video.h"
#include "clocks_timings.h"
#include "utils.h"
#include "statusbar.h"


/*
  Floppy Disk Controller

Programmable Sound Generator (YM-2149)

  0xff8800(even byte)  - PSG Register Data (Read, used for parallel port)
            - PSG Register Select (Write)

  Write to bits 0-3 to select PSG register to use(then write data to 0xfff8802)
    Value    Register

    0000    Channel A Fine Tune
    0001    Channel A Coarse Tune
    0010    Channel B Fine Tune
    0011    Channel B Coarse Tune
    0100    Channel C Fine Tune
    0101    Channel C Coarse Tune
    0110    Noise Generator Control
    0111    Mixer Control - I/O enable
    1000    Channel A Amplitude
    1001    Channel B Amplitude
    1010    Channel C Amplitude
    1011    Envelope Period Fine Tune
    1100    Envelope Peroid Coarse Tune
    1101    Envelope Shape
    1110    I/O Port A Select (Write only)
    1111    I/O Port B Select

  0xfff8802(even byte)  - Bits according to 0xff8800 Register select

  1110(Register 14) - I/O Port A
    Bit 0 - Floppy side 0/1
    Bit 1 - Floppy drive 0 select
    Bit 2 - Floppy drive 1 select
    Bit 3 - RS232 Ready to send (RTS)
    Bit 4 - RS232 Data Terminal Ready (DTR)
    Bit 5 - Centronics Strobe
    Bit 6 - General Purpose Output
    Bit 7 - Reserved

ACSI DMA and Floppy Disk Controller(FDC)
  0xff8604 - information from file '1772.info.txt, by David Gahris' (register r0)
    Word access only, but only lower byte (ff8605) is used
  (write) - Disk controller
    Set DMA sector count if ff8606 bit 4 == 1
    Set FDC's internal registers depending on bit 1/2 of ff8606 if bit 4 == 0
  (read) - Disk controller status
    Bit 0 - Busy.  This bit is 1 when the 177x is busy.  This bit is 0 when the 177x is free for CPU commands.
    Bit 1 - Index / Data Request.  On Type I commands, this bit is high during the index pulse that occurs once
      per disk rotation.  This bit is low at all times other than the index pulse.  For Type II and III commands,
      Bit 1 high signals the CPU to handle the data register in order to maintain a continuous flow of data.
      Bit 1 is high when the data register is full during a read or when the data register is empty during a write.
      "Worst case service time" for Data Request is 23.5 cycles.
    Bit 2 - Track Zero / Lost Data.  After Type I commands, this bit is 0 if the mechanism is at track zero.
      This bit is 1 if the head is not at track zero.  After Type II or III commands, this bit is 1 if the
      CPU did not respond to Data Request (Status bit 1) in time for the 177x to maintain a continuous data flow.
      This bit is 0 if the CPU responded promptly to Data Request.
      NOTE : on ST, Lost Data is never set because the DMA always handles the data request signal.
    Bit 3 - CRC Error.  This bit is high if a sector CRC on disk does not match the CRC which the 177x
      computed from the data.  The CRC polynomial is x^16+x^12+x^5+1.  If the stored CRC matches the newly
      calculated CRC, the CRC Error bit is low.  If this bit and the Record Not Found bit are set, the error
      was in an ID field.  If this bit is set but Record Not Found is clear, the error was in a data field.
    Bit 4 - Record Not Found.  This bit is set if the 177x cannot find the track, sector, or side which
      the CPU requested.  Otherwise, this bit is clear.
    Bit 5 - Spin-up / Record Type.  For Type I commands, this bit is low during the 6-revolution motor
      spin-up time.  This bit is high after spin-up.  For Type II and Type III commands, Bit 5 low
      indicates a normal data mark.  Bit 5 high indicates a deleted data mark.
    Bit 6 - Write Protect.  This bit is not used during reads.  During writes, this bit is high when the disk is write protected.
      After a type I command, this bit is constantly updated an give the current value of the WPT signal.
    Bit 7 - Motor On.  This bit is high when the drive motor is on, and low when the motor is off.

  0xff8606 - DMA Status(read), DMA Mode Control(write) - NOTE bits 0,9-15 are not used
    Bit 1 - FDC Pin A0 (See below)
    Bit 2 - FDC Pin A1
    Bit 3 - FDC/HDC Register Select
    Bit 4 - FDC/Sector count select
    Bit 5 - Reserved
    Bit 6 - Enable/Disable DMA
    Bit 7 - HDC/FDC
    Bit 8 - Read/Write

    A1  A0    Read        Write(bit 8==1)
    0  0    Status        Command
    0  1    Track Register    Track Register
    1  0    Sector Register    Sector Register
    1  1    Data Register    Data Register


  According to the documentation INTRQ is generated at the completion of each
  command (causes an interrupt in the MFP). INTRQ is reset by reading the status
  register OR by loading a new command. So, does this mean the GPIP? Or does it
  actually CANCEL the interrupt? Can this be done?

  NOTE [NP] : The DMA is connected to the FDC and its Data Register, each time a DRQ
  is made by the FDC, it's handled by the DMA through its internal 16 bytes buffer.
  This means that in the case of the Atari ST the LOST_DATA bit will never be set
  in the Status Register (but data can be lost if FDC_DMA.SectorCount=0 as there
  will be no transfer between DMA and RAM)


  Detecting disk changes :
  ------------------------
  3'1/2 floppy drives include a 'DSKCHG' signal on pin 34 to detect when a disk was changed.
  Unfortunatelly on ST, this signal is not connected. Nevertheless, it's possible to detect
  a disk was inserted or ejected by looking at the 'WPT' signal which tells if a disk is write
  protected or not.
  At the drive level, a light is emitted above the top left corner of the floppy :
   - if the write protection hole on the floppy is opened, the light goes through and the disk
     is considered to be write protected.
   - if the write protection hole on the floppy is closed, the light can't go through and the
     disk is write enabled.
  The point is that when any "solid" part of the floppy obstructs the light signal, the WPT
  signal will change immediately : it will be considered as if a write enabled disk was present.
  So, when a floppy is ejected or inserted, the body of the floppy will briefly obstruct the light,
  whatever the state of the protection hole could be.
  Similarly, when there's no floppy inside the drive, the light signal can pass through, so it will
  be considered as if a write protected disk was present.
  So, let's call 'C' the state when protection hole is Closed (ie WPT = 0) and 'O' the state
  when protection hole is Opened (ie WPT = 1). We have the following cases :
    - floppy in drive : state can be C or O depending on the protection tab. Let's call it 'X'
    - no floppy in drive : state is equivalent to O (because the light signal is not obstructed)
    - ejecting a floppy : states will go from X to C and finally to O
    - inserting a floppy : states will go from O to C and finally to X

  The TOS monitors the changes on the WPT signal to determine if a floppy was ejected or inserted.
  On TOS 1.02fr, the code is located between $fc1bc4 and $fc1ebc. Every 8 VBL, one floppy drive is checked
  to see if the WPT signal changed. When 1 drive is connected, this means a floppy change should keep the
  WPT signal during at least 8 VBLs. When 2 drive are connected, each drive is checked every 16 VBLs, so
  the WPT signal should be kept for at least 16 VBLs.

  During these transition phases between "ejected" and "inserted", we force the WPT signal to either 0 or 1,
  depending on which transition we're emulating (see Floppy_DriveTransitionUpdateState()) :
    - Ejecting : WPT will be X, then 0, then 1
    - Inserting : WPT will be 1, then 0, then X

*/

/*-----------------------------------------------------------------------*/

#define	FDC_STR_BIT_BUSY			0x01
#define	FDC_STR_BIT_INDEX			0x02		/* type I */
#define	FDC_STR_BIT_DRQ				0x02		/* type II and III */
#define	FDC_STR_BIT_TR00			0x04		/* type I */
#define	FDC_STR_BIT_LOST_DATA			0x04		/* type II and III */
#define	FDC_STR_BIT_CRC_ERROR			0x08
#define	FDC_STR_BIT_RNF				0x10
#define	FDC_STR_BIT_SPIN_UP			0x20		/* type I */
#define	FDC_STR_BIT_RECORD_TYPE			0x20		/* type II and III */
#define	FDC_STR_BIT_WPRT			0x40
#define	FDC_STR_BIT_MOTOR_ON			0x80


#define	FDC_COMMAND_BIT_VERIFY			(1<<2)		/* 0=no verify after type I, 1=verify after type I */
#define	FDC_COMMAND_BIT_HEAD_LOAD		(1<<2)		/* for type II/III 0=no extra delay, 1=add 30 ms delay to set the head */
#define	FDC_COMMAND_BIT_SPIN_UP			(1<<3)		/* 0=enable motor's spin up, 1=disable motor's spin up */
#define	FDC_COMMAND_BIT_UPDATE_TRACK		(1<<4)		/* 0=don't update TR after type I, 1=update TR after type I */
#define	FDC_COMMAND_BIT_MULTIPLE_SECTOR		(1<<4)		/* 0=read/write only 1 sector, 1=read/write many sectors */



/* FDC Emulation commands used in FDC.Command */
enum
{
	FDCEMU_CMD_NULL = 0,
	/* Type I */
	FDCEMU_CMD_RESTORE,
	FDCEMU_CMD_SEEK,
	FDCEMU_CMD_STEP,					/* Also used for STEP IN and STEP OUT */
	/* Type II */
	FDCEMU_CMD_READSECTORS,
	FDCEMU_CMD_WRITESECTORS,
	/* Type III */
	FDCEMU_CMD_READADDRESS,
	FDCEMU_CMD_READTRACK,
	FDCEMU_CMD_WRITETRACK,
	/* Type IV */
	FDCEMU_CMD_FORCEINT,

	/* Other fake commands used internally */
	FDCEMU_CMD_MOTOR_STOP
};


/* FDC Emulation commands' sub-states used in FDC.CommandState */
enum
{
	FDCEMU_RUN_NULL = 0,

	/* Restore */
	FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO,
	FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO_SPIN_UP,
	FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO_MOTOR_ON,
	FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO_LOOP,
	FDCEMU_RUN_RESTORE_VERIFY,
	FDCEMU_RUN_RESTORE_VERIFY_HEAD_OK,
	FDCEMU_RUN_RESTORE_VERIFY_LOOP,
	FDCEMU_RUN_RESTORE_COMPLETE,
	/* Seek */
	FDCEMU_RUN_SEEK_TOTRACK,
	FDCEMU_RUN_SEEK_TOTRACK_SPIN_UP,
	FDCEMU_RUN_SEEK_TOTRACK_MOTOR_ON,
	FDCEMU_RUN_SEEK_VERIFY,
	FDCEMU_RUN_SEEK_VERIFY_HEAD_OK,
	FDCEMU_RUN_SEEK_VERIFY_LOOP,
	FDCEMU_RUN_SEEK_COMPLETE,
	/* Step / Step In / Step Out */
	FDCEMU_RUN_STEP_ONCE,
	FDCEMU_RUN_STEP_ONCE_SPIN_UP,
	FDCEMU_RUN_STEP_ONCE_MOTOR_ON,
	FDCEMU_RUN_STEP_VERIFY,
	FDCEMU_RUN_STEP_VERIFY_HEAD_OK,
	FDCEMU_RUN_STEP_VERIFY_LOOP,
	FDCEMU_RUN_STEP_COMPLETE,
	/* Read Sector */
	FDCEMU_RUN_READSECTORS_READDATA,
	FDCEMU_RUN_READSECTORS_READDATA_SPIN_UP,
	FDCEMU_RUN_READSECTORS_READDATA_HEAD_LOAD,
	FDCEMU_RUN_READSECTORS_READDATA_MOTOR_ON,
	FDCEMU_RUN_READSECTORS_READDATA_CHECK_SECTOR_HEADER,
	FDCEMU_RUN_READSECTORS_READDATA_TRANSFER_START,
	FDCEMU_RUN_READSECTORS_READDATA_TRANSFER_LOOP,
	FDCEMU_RUN_READSECTORS_CRC,
	FDCEMU_RUN_READSECTORS_RNF,
	FDCEMU_RUN_READSECTORS_COMPLETE,
	/* Write Sector */
	FDCEMU_RUN_WRITESECTORS_WRITEDATA,
	FDCEMU_RUN_WRITESECTORS_WRITEDATA_SPIN_UP,
	FDCEMU_RUN_WRITESECTORS_WRITEDATA_HEAD_LOAD,
	FDCEMU_RUN_WRITESECTORS_WRITEDATA_MOTOR_ON,
	FDCEMU_RUN_WRITESECTORS_WRITEDATA_CHECK_SECTOR_HEADER,
	FDCEMU_RUN_WRITESECTORS_WRITEDATA_TRANSFER_START,
	FDCEMU_RUN_WRITESECTORS_WRITEDATA_TRANSFER_LOOP,
	FDCEMU_RUN_WRITESECTORS_CRC,
	FDCEMU_RUN_WRITESECTORS_RNF,
	FDCEMU_RUN_WRITESECTORS_COMPLETE,
	/* Read Address */
	FDCEMU_RUN_READADDRESS,
	FDCEMU_RUN_READADDRESS_SPIN_UP,
	FDCEMU_RUN_READADDRESS_HEAD_LOAD,
	FDCEMU_RUN_READADDRESS_MOTOR_ON,
	FDCEMU_RUN_READADDRESS_DMA,
	FDCEMU_RUN_READADDRESS_COMPLETE,
	/* Read Track */
	FDCEMU_RUN_READTRACK,
	FDCEMU_RUN_READTRACK_SPIN_UP,
	FDCEMU_RUN_READTRACK_HEAD_LOAD,
	FDCEMU_RUN_READTRACK_MOTOR_ON,
	FDCEMU_RUN_READTRACK_INDEX,
	FDCEMU_RUN_READTRACK_DMA,
	FDCEMU_RUN_READTRACK_COMPLETE,
	/* Write Track */
	FDCEMU_RUN_WRITETRACK,
	FDCEMU_RUN_WRITETRACK_SPIN_UP,
	FDCEMU_RUN_WRITETRACK_HEAD_LOAD,
	FDCEMU_RUN_WRITETRACK_MOTOR_ON,
	FDCEMU_RUN_WRITETRACK_INDEX,
	FDCEMU_RUN_WRITETRACK_DMA,
	FDCEMU_RUN_WRITETRACK_COMPLETE,
	/* Force Int */
	FDCEMU_RUN_FORCEINT,
	FDCEMU_RUN_FORCEINT_LOOP,
	FDCEMU_RUN_FORCEINT_COMPLETE,

	/*  Motor Stop */
	FDCEMU_RUN_MOTOR_STOP,
	FDCEMU_RUN_MOTOR_STOP_WAIT,
	FDCEMU_RUN_MOTOR_STOP_COMPLETE
};



/*
 * Standard hardware values for the FDC. This should allow to get very good timings' emulation
 * when dealing with non protected disks that still require a correct speed (MSA or ST images)
 *
 * - WD1772's datasheet is based on a reference clock of 8 MHz, so delays expressed in milli-seconds
 *   will be slightly different for the Atari ST, whose FDC's clock is around 8.021247 MHz (but this is
 *   not really noticeable in practice, less than 0.3 %)
 * - DD MFM encoding defines a standard signal of 4 micro sec per bit (a possible variation of +/- 10 %
 *   should still be possible). This means the WD1772 will read/write at 250 kbits/sec.
 *   Taking 4 us per bit means 32 us for a full byte, and with a 8 MHz clock, 256 cycles per byte.
 * - The floppy drives used in the Atari ST are spinning at 300 RPM. Variations are possible, as long
 *   as it keeps the duration of an MFM bit in the required 4 us +/- 10 % (in practice, ST drives are often
 *   at 299-301 RPM)
 * - When FDC runs at 8 MHz, the 250 kbits/s and 300 RPM give 6250 bytes for a standard track
 * - When FDC runs at 8.021247 MHz (Atari ST), the 250.664 kbit/s and 300 RPM give 6267 bytes per track
 */


#define	FDC_CLOCK_STANDARD			(8000000.L)	/* In the WD1772's datasheet, all timings are related to a reference clock of 8 MHz */
#define FDC_DELAY_CYCLE_MFM_BYTE		( 4 * 8 * 8 )	/* 4 us per bit, 8 bits per byte, 8 MHz clock -> 256 cycles */
#define	FDC_BITRATE_STANDARD			250000		/* read/write speed of the WD1772 in bits per sec */
#define	FDC_RPM_STANDARD			300		/* 300 RPM or 5 spins per sec */
//#define	FDC_TRACK_BYTES_STANDARD		( ( FDC_BITRATE_STANDARD / 8 ) / ( FDC_RPM_STANDARD / 60 ) )	/* 6250 bytes */
#define FDC_TRACK_BYTES_STANDARD	6268
//#define FDC_TRACK_BYTES_STANDARD	6272

#define FDC_TRANSFER_BYTES_US( n )		(  ( n ) * 8 * 1000000.L / FDC_BITRATE_STANDARD )	/* micro sec to read/write 'n' bytes in the WD1772 */

#define	FDC_DELAY_IP_SPIN_UP			6		/* 6 index pulses to reach correct speed during spin up */
#define	FDC_DELAY_IP_MOTOR_OFF			9		/* Turn off motor 9 index pulses after the last command */


/* Delays are in micro sec */
#define	FDC_DELAY_US_MOTOR_ON			( 1000000.L * 6 / ( FDC_RPM_STANDARD / 60 ) )	/* 6 spins to reach correct speed */
#define	FDC_DELAY_US_MOTOR_OFF			( 1000000.L * 9 / ( FDC_RPM_STANDARD / 60 ) )	/* Turn off motor 9 spins after the last command */

#define	FDC_DELAY_US_HEAD_LOAD			( 15 * 1000 )	/* Additionnal 15 ms delay to load the head in type II/III */

#define	FDC_DELAY_US_RNF			( 1000000.L * 5 / ( FDC_RPM_STANDARD / 60 ) )	/* 5 spins to set RNF */

#define	FDC_DELAY_US_INDEX_PULSE_LENGTH		( 1.5 * 1000 )	/* Index pulse signal remain high during 1.5 ms on each rotation */


/* Internal delays to process commands are in fdc cycles for a 8 MHz clock */
#define	FDC_DELAY_CYCLE_TYPE_I_PREPARE		(90*8)		/* Types I commands take at least 0.09 ms to execute */
								/* (~740 cpu cycles @ 8 Mhz). [NP] : this was measured on a 520 STF */
								/* and avoid returning immediately when command has no effect */
#define	FDC_DELAY_CYCLE_TYPE_II_PREPARE		(1*8) // 65	/* Start Type II commands immediately */
#define	FDC_DELAY_CYCLE_TYPE_III_PREPARE	(1*8)		/* Start Type III commands immediately */
#define	FDC_DELAY_CYCLE_TYPE_IV_PREPARE		(100*8)		/* FIXME [NP] : this was not measured */
#define	FDC_DELAY_CYCLE_COMMAND_COMPLETE	(1*8)		/* Number of cycles before going to the _COMPLETE state (~8 cpu cycles) */
#define	FDC_DELAY_CYCLE_COMMAND_IMMEDIATE	(1*8)		/* Number of cycles to go immediately to another state */

/* When the drive is switched off or if there's no floppy, some commands will wait forever */
/* as they can't find the next index pulse. Instead of continuously testing if a valid drive */
/* or floppy becomes available (which would slow down emulation), we only test every 50000 FDC cycles, */
/* which shouldn't give any noticeable emulation error */
#define	FDC_DELAY_CYCLE_WAIT_NO_DRIVE_FLOPPY	50000

#define	FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE	500

#define	FDC_DELAY_TRANSFER_DMA_16		FDC_TRANSFER_BYTES_US( DMA_DISK_TRANSFER_SIZE )


#define	DMA_DISK_SECTOR_SIZE			512		/* Sector count at $ff8606 is for 512 bytes blocks */
#define	DMA_DISK_TRANSFER_SIZE			16		/* DMA transfers blocks of 16 bytes at a time */

#define	FDC_PHYSICAL_MAX_TRACK			90		/* Head can't go beyond 90 tracks */


#define FDC_SIDE				( FDC.SideSignal )	/* Side 0 or 1 */
#define	FDC_DRIVE				FDC_FindFloppyDrive()

#define	FDC_STEP_RATE				( FDC.CR & 0x03 )	/* Bits 0 and 1 of the current type I command */

static int FDC_StepRate_ms[] = { 6 , 12 , 2 , 3 };		/* Controlled by bits 1 and 0 (r1/r0) in type I commands */


#define	FDC_SECTOR_SIZE_128			0		/* Sector size used in the ID fields */
#define	FDC_SECTOR_SIZE_256			1
#define	FDC_SECTOR_SIZE_512			2
#define	FDC_SECTOR_SIZE_1024			3


/* These are some standard GAP values to format a track with 9 or 10 sectors */
/* When handling ST/MSA disk images, those values are required to get accurate */
/* timings when emulating disk's spin and index's position. */

#define	FDC_TRACK_LAYOUT_STANDARD_GAP1		60		/* Track Pre GAP : 0x4e */
#define	FDC_TRACK_LAYOUT_STANDARD_GAP2		12		/* Sector ID Pre GAP : 0x00 */
#define	FDC_TRACK_LAYOUT_STANDARD_GAP3a		22		/* Sector ID Post GAP : 0x4e */
#define	FDC_TRACK_LAYOUT_STANDARD_GAP3b		12		/* Sector DATA Pre GAP : 0x00 */
#define	FDC_TRACK_LAYOUT_STANDARD_GAP4		40		/* Sector DATA Pre GAP : 0x4e */
#define	FDC_TRACK_LAYOUT_STANDARD_GAP5		0		/* Track Post GAP : 0x4e (to fill the rest of the track, value is variable) */
								/* GAP5 is 664 bytes for 9 sectors or 50 bytes for 10 sectors */

/* Size of a raw standard 512 byte sector in a track, including ID field and all GAPs : 614 bytes */
/* (this must be the same as the data returned in FDC_UpdateReadTrackCmd() ) */
#define	FDC_TRACK_LAYOUT_STANDARD_RAW_SECTOR_512	( FDC_TRACK_LAYOUT_STANDARD_GAP2 \
				+ 3 + 1 + 6 + FDC_TRACK_LAYOUT_STANDARD_GAP3a + FDC_TRACK_LAYOUT_STANDARD_GAP3b \
				+ 3 + 1 + 512 + 2 + FDC_TRACK_LAYOUT_STANDARD_GAP4 )


#define	FDC_FAST_FDC_FACTOR			10		/* Divide all delays by this value when --fastfdc is used */

#define	FDC_DENSITY_FACTOR_DD			1
#define	FDC_DENSITY_FACTOR_HD			2		/* For a HD disk, we get x2 bytes than DD */
#define	FDC_DENSITY_FACTOR_ED			4		/* For a ED disk, we get x4 bytes than DD */


typedef struct {
	/* WD1772 internal registers */
	Uint8		DR;					/* Data Register */
	Uint8		TR;					/* Track Register */
	Uint8		SR;					/* Sector Register */
	Uint8		CR;					/* Command Register */
	Uint8		STR;					/* Status Register */
	int		StepDirection;				/* +1 (Step In) or -1 (Step Out) */

//	Uint8		MotorSignal;				/* 0=OFF 1=ON */
	Uint8		SideSignal;				/* Side 0 or 1 */
	int		DriveSelSignal;				/* 0 or 1 for drive A or B ; or -1 if no drive selected */

	
	/* Other variables */
	int		Command;				/* FDC emulation command currently being exceuted */
	int		CommandState;				/* Current state for the running command */
	Uint8		CommandType;				/* Type of latest FDC command (1,2,3 or 4) */
	bool		ReplaceCommandPossible;			/* true if the current command can be replaced by another one */
								/* ([NP] FIXME : only possible during prepare+spinup phases ?) */

	bool		StatusTypeI;				/* When true, STR will report the status of a type I command */
	int		IndexPulse_Counter;			/* To count the number of rotations when motor is ON */
	bool		UpdateIndexPulse;			/* true if motor was stopped and we're starting a spin up sequence */
	Uint64		IndexPulse_Time;			/* Clock value last time we had an index pulse with motor ON */
	Uint64		CommandExpire_Time;			/* Clock value to abort a command if it didn't complete before */
	Uint8		NextSector_ID_Field_SR;			/* Sector Register from the ID Field after a call to FDC_NextSectorID_NbBytes() */
} FDC_STRUCT;


typedef struct {
	/* DMA internal registers */
	Uint16		Status;
	Uint16		Mode;
	Uint16		SectorCount;
	Uint16		BytesInSector;

	/* Variables to handle our DMA buffer */
	int		PosInBuffer;
	int		PosInBufferTransfer;
	int		BytesToTransfer;
} FDC_DMA_STRUCT;


typedef struct {
	bool		Enabled;
	bool		DiskInserted;
	int		RPM;					/* Rotation Per Minutes * 1000 */
	int		Density;				/* 1 for DD (720 kB), 2 for HD (1.4 MB), 4 for ED (2.8 MB) */
	Uint8		HeadTrack;				/* Current position of the head */
//	Uint8		Motor;					/* State of the drive's motor : 0=OFF 1=ON */

	Uint64		IndexPulse_Time;			/* Clock value last time we had an index pulse with motor ON */
} FDC_DRIVE_STRUCT;


static FDC_STRUCT	FDC;					/* All variables related to the WD1772 emulation */
static FDC_DMA_STRUCT	FDC_DMA;				/* All variables related to the DMA transfer */
static FDC_DRIVE_STRUCT	FDC_DRIVES[ MAX_FLOPPYDRIVES ];		/* A: and B: */

static Uint8 DMADiskWorkSpace[ FDC_TRACK_BYTES_STANDARD*4+1000 ];/* Workspace used to transfer bytes between floppy and DMA */
								/* It should be large enough to contain a whole track */
								/* We use a x4 factor when we need to simulate HD and ED too */


/*--------------------------------------------------------------*/
/* Local functions prototypes					*/
/*--------------------------------------------------------------*/

static void	FDC_SetDriveLedBusy ( void );

static int	FDC_DelayToFdcCycles ( int Delay_micro );
static int	FDC_FdcCyclesToCpuCycles ( int FdcCycles );
static int	FDC_CpuCyclesToFdcCycles ( int CpuCycles );
static void	FDC_StartTimer_FdcCycles ( int FdcCycles , int InternalCycleOffset );
static int	FDC_DelayToCpuCycles ( int Delay_micro );
static int	FDC_TransferByte_FdcCycles ( int NbBytes );
static void	FDC_CRC16 ( Uint8 *buf , int nb , Uint16 *pCRC );

static void	FDC_ResetDMA ( void );
static void	FDC_DMA_InitTransfer ( void );
static bool	FDC_DMA_ReadFromFloppy ( void );
static bool	FDC_DMA_WriteToFloppy ( void );

static void	FDC_UpdateAll ( void );
static bool	FDC_ValidFloppyDrive ( void );
static int	FDC_FindFloppyDrive ( void );
static int	FDC_GetSectorsPerTrack ( int Track , int Side );
static int	FDC_GetSidesPerDisk ( int Track );
static int	FDC_GetDensity ( int Drive );
static int	FDC_GetBytesPerTrack ( void );

static void	FDC_IndexPulse_Update ( void );
static void	FDC_IndexPulse_Init ( int Drive );
static void	FDC_IndexPulse_Init_old ( void );
static int	FDC_IndexPulse_GetCurrentPos ( void );
static int	FDC_IndexPulse_GetCurrentPos_FdcCycles ( int *pFdcCyclesPerRev );
static int	FDC_IndexPulse_GetCurrentPos_NbBytes ( void );
static int	FDC_IndexPulse_GetState ( void );
static int	FDC_NextIndexPulse_FdcCycles ( void );
static int	FDC_NextIndexPulse_NbBytes ( void );
static int	FDC_NextSectorID_NbBytes ( void );

static void	FDC_Update_STR ( Uint8 DisableBits , Uint8 EnableBits );
static int	FDC_CmdCompleteCommon ( bool DoInt );
static void	FDC_VerifyTrack ( void );
static int	FDC_UpdateMotorStop_old ( void );
static int	FDC_UpdateMotorStop ( void );
static int	FDC_UpdateRestoreCmd ( void );
static int	FDC_UpdateSeekCmd ( void );
static int	FDC_UpdateStepCmd ( void );
static int	FDC_UpdateReadSectorsCmd ( void );
static int	FDC_UpdateWriteSectorsCmd ( void );
static int	FDC_UpdateReadAddressCmd ( void );
static int	FDC_UpdateReadTrackCmd ( void );
static int	FDC_UpdateForceIntCmd ( void );

static bool	FDC_Set_MotorON ( Uint8 FDC_CR );
static int	FDC_Check_MotorON ( Uint8 FDC_CR );
static int	FDC_TypeI_Restore ( void );
static int	FDC_TypeI_Seek ( void );
static int	FDC_TypeI_Step ( void );
static int	FDC_TypeI_StepIn ( void );
static int	FDC_TypeI_StepOut ( void );
static int	FDC_TypeII_ReadSector ( void );
static int	FDC_TypeII_WriteSector(void);
static int	FDC_TypeIII_ReadAddress ( void );
static int	FDC_TypeIII_ReadTrack ( void );
static int	FDC_TypeIII_WriteTrack ( void );
static int	FDC_TypeIV_ForceInterrupt ( bool bCauseCPUInterrupt );

static int	FDC_ExecuteTypeICommands ( void );
static int	FDC_ExecuteTypeIICommands ( void );
static int	FDC_ExecuteTypeIIICommands ( void );
static int	FDC_ExecuteTypeIVCommands ( void );
static void	FDC_ExecuteCommand ( void );

static void	FDC_WriteSectorCountRegister ( void );
static void	FDC_WriteCommandRegister ( void );
static void	FDC_WriteTrackRegister ( void );
static void	FDC_WriteSectorRegister ( void );
static void	FDC_WriteDataRegister ( void );

static bool	FDC_ReadSectorFromFloppy ( Uint8 *buf , Uint8 Sector , int *pSectorSize );
static bool	FDC_WriteSectorToFloppy ( int DMASectorsCount , Uint8 Sector , int *pSectorSize );


/*-----------------------------------------------------------------------*/
/**
 * Save/Restore snapshot of local variables('MemorySnapShot_Store' handles type)
 */
void FDC_MemorySnapShot_Capture(bool bSave)
{
	MemorySnapShot_Store(&FDC, sizeof(FDC));
	MemorySnapShot_Store(&FDC_DMA, sizeof(FDC_DMA));
	MemorySnapShot_Store(&FDC_DRIVES, sizeof(FDC_DRIVE_STRUCT));

	MemorySnapShot_Store(DMADiskWorkSpace, sizeof(DMADiskWorkSpace));
}


/*-----------------------------------------------------------------------*/
/**
 * Change the color of the drive's led color in the statusbar, depending
 * on the state of the busy bit in SR
 */
static void	FDC_SetDriveLedBusy ( void )
{
	if ( FDC.DriveSelSignal < 0 )
		return;						/* no drive selected */

	if ( FDC.SR & FDC_STR_BIT_BUSY )
		Statusbar_SetFloppyLed ( FDC.DriveSelSignal , LED_STATE_ON_BUSY );
	else
		Statusbar_SetFloppyLed ( FDC.DriveSelSignal , LED_STATE_ON );
}


//*-----------------------------------------------------------------------*/
/**
 * Convert a delay in micro seconds to its equivalent of fdc cycles
 * (delays in the WD1772 specs are relative to a 8 MHz reference clock)
 */
static int	FDC_DelayToFdcCycles ( int Delay_micro )
{
	int	FdcCycles;

	FdcCycles = (int) ( ( (Uint64) FDC_CLOCK_STANDARD * Delay_micro ) / 1000000 );

//fprintf ( stderr , "fdc state %d delay %d us %d fdc cycles\n" , FDC.Command , Delay_micro , FdcCycles );
	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Convert a number of fdc cycles at freq MachineClocks.FDC_Freq to a number
 * of cpu cycles at freq MachineClocks.CPU_Freq
 * TODO : we use a fixed 8 MHz clock and nCpuFreqShift to convert cycles for our
 * internal timers in cycInt.c. This should be replaced some days by using
 * MachineClocks.CPU_Freq and not using nCpuFreqShift anymore.
 * (for Falcon, we multiply cycles by 2 to simulate a freq in the 8 MHz range)
 */
static int	FDC_FdcCyclesToCpuCycles ( int FdcCycles )
{
	int	CpuCycles;

	/* Our conversion expects FDC_Freq to be nearly the same as CPU_Freq (8 Mhz) */
	/* but the Falcon uses a 16 MHz clock for the Ajax FDC */
	/* FIXME : as stated above, this should be handled better, without involving 8 MHz CPU_Freq */
	if ( ConfigureParams.System.nMachineType == MACHINE_FALCON )
		FdcCycles *= 2;					/* correct delays for a 8 MHz FDC_Freq clock instead of 16 */

//	CpuCycles = rint ( ( (Uint64)FdcCycles * MachineClocks.CPU_Freq ) / MachineClocks.FDC_Freq );
	CpuCycles = rint ( ( (Uint64)FdcCycles * 8021247.L ) / MachineClocks.FDC_Freq );
	CpuCycles &= -4;					/* Multiple of 4 */
	CpuCycles <<= nCpuFreqShift;				/* Compensate for x2 or x4 cpu speed */

//fprintf ( stderr , "fdc state %d delay %d fdc cycles %d cpu cycles\n" , FDC.Command , FdcCycles , CpuCycles );
//if ( Delay==4104) Delay=4166;		// 4166 : decade demo
	return CpuCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Convert a number of cpu cycles at freq MachineClocks.CPU_Freq to a number
 * of fdc cycles at freq MachineClocks.FDC_Freq (this is the opposite
 * of FDC_FdcCyclesToCpuCycles)
 * TODO : we use a fixed 8 MHz clock and nCpuFreqShift to convert cycles for our
 * internal timers in cycInt.c. This should be replaced some days by using
 * MachineClocks.CPU_Freq and not using nCpuFreqShift anymore.
 */
static int	FDC_CpuCyclesToFdcCycles ( int CpuCycles )
{
	int	FdcCycles;


	CpuCycles >>= nCpuFreqShift;				/* Compensate for x2 or x4 cpu speed */

//	FdcCycles = rint ( ( (Uint64)CpuCycles * MachineClocks.FDC_Freq ) / MachineClocks.CPU_Freq );
	FdcCycles = rint ( ( (Uint64)CpuCycles * MachineClocks.FDC_Freq ) / 8021247.L );

	/* Our conversion expects FDC_Freq to be nearly the same as CPU_Freq (8 Mhz) */
	/* but the Falcon uses a 16 MHz clock for the Ajax FDC */
	/* FIXME : as stated above, this should be handled better, without involving 8 MHz CPU_Freq */
	if ( ConfigureParams.System.nMachineType == MACHINE_FALCON )
		FdcCycles /= 2;					/* correct delays for a 8 MHz FDC_Freq clock instead of 16 */

//fprintf ( stderr , "fdc state %d delay %d cpu cycles %d fdc cycles\n" , FDC.Command , CpuCycles , FdcCycles );
	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Start an internal timer to handle the FDC's events.
 * If "fast floppy" mode is used, we speed up the timer by dividing
 * the number of cycles by a fixed number.
 */
static void	FDC_StartTimer_FdcCycles ( int FdcCycles , int InternalCycleOffset )
{
//fprintf ( stderr , "fdc start timer %d cycles\n" , FdcCycles );

	if ( ( ConfigureParams.DiskImage.FastFloppy ) && ( FdcCycles > FDC_FAST_FDC_FACTOR ) )
		FdcCycles /= FDC_FAST_FDC_FACTOR;

	CycInt_AddRelativeInterruptWithOffset ( FDC_FdcCyclesToCpuCycles ( FdcCycles ) , INT_CPU_CYCLE , INTERRUPT_FDC , InternalCycleOffset );
}


/*-----------------------------------------------------------------------*/
/**
 * Convert a delay in micro seconds to its equivalent of cpu cycles
 * (FIXME [NP] : for now we use a fixed 8 MHz clock, because cycInt.c requires it)
 */
static int	FDC_DelayToCpuCycles ( int Delay_micro )
{
	int	Delay;

	Delay = (int) ( ( (Uint64)MachineClocks.FDC_Freq * Delay_micro ) / 1000000 ) & -4;
Delay = Delay_micro*8;
//if ( Delay_micro==32 ) Delay=255;

	/* Our conversion expect FDC_Freq to be the same as CPU_Freq (8 Mhz) */
	/* but the Falcon uses a 16 MHz clock for the Ajax FDC */
	/* FIXME : as stated above, this should be handled better, without involving 8 MHz CPU_Freq */
	if ( ConfigureParams.System.nMachineType == MACHINE_FALCON )
		Delay /= 2;					/* correct delays for a 8 MHz clock instead of 16 */

//fprintf ( stderr , "fdc state %d delay %d us %d cycles\n" , FDC.Command , Delay_micro , Delay );
//if ( Delay==4104) Delay=4166;		// 4166 : decade demo
	return Delay;
}


/*-----------------------------------------------------------------------*/
/**
 * Return the number of FDC cycles required to read/write 'nb' bytes
 */
static int	FDC_TransferByte_FdcCycles ( int NbBytes )
{
//fprintf ( stderr , "fdc state %d transfer %d bytes\n" , FDC.Command , NbBytes );
	return NbBytes * FDC_DELAY_CYCLE_MFM_BYTE;
}


/*-----------------------------------------------------------------------*/
/**
 * Compute the CRC16 of 'nb' bytes stored in 'buf'.
 */
static void FDC_CRC16 ( Uint8 *buf , int nb , Uint16 *pCRC )
{
	int	i;

	crc16_reset ( pCRC );
	for ( i=0 ; i<nb ; i++ )
	{
//		fprintf ( stderr , "fdc crc16 %d 0x%x\n" , i , buf[ i ] );
		crc16_add_byte ( pCRC , buf[ i ] );
	}
//	fprintf ( stderr , "fdc crc16 0x%x 0x%x\n" , *pCRC>>8 , *pCRC & 0xff );
}


/*-----------------------------------------------------------------------*/
/**
 * Init variables used in FDC and DMA emulation
 */
void FDC_Init ( void )
{
	int	i;

        LOG_TRACE ( TRACE_FDC , "fdc init\n" );

	for ( i=0 ; i<MAX_FLOPPYDRIVES ; i++ )
	{
		FDC_DRIVES[ i ].Enabled = true;
		FDC_DRIVES[ i ].DiskInserted = false;
		FDC_DRIVES[ i ].RPM = FDC_RPM_STANDARD * 1000;
		FDC_DRIVES[ i ].Density = FDC_DENSITY_FACTOR_DD;
		FDC_DRIVES[ i ].HeadTrack = 0;			/* Set all drives to track 0 */
		FDC_DRIVES[ i ].IndexPulse_Time = 0;
	}

FDC_IndexPulse_GetCurrentPos();		// REMOVE : avoid gcc warning on unused function
FDC_NextIndexPulse_NbBytes();		// REMOVE : avoid gcc warning on unused function
}


/*-----------------------------------------------------------------------*/
/**
 * Reset variables used in FDC and DMA emulation
 */

/* This function is called after a hardware reset of the FDC.
 * Cold reset is when the computer is turned off/on.
 * Warm reset is when the reset button is pressed or the 68000
 * RESET instruction is used.
 * On warm reset, TR and DR should not be reset.
 * STR is set to 0 and SR is set to 1 (verified on a real STF)
 */
void FDC_Reset ( bool bCold )
{
        LOG_TRACE ( TRACE_FDC , "fdc reset mode=%s\n" , bCold?"cold":"warm" );

	/* Clear out FDC registers */
	FDC.CR = 0;
	FDC.STR = 0;
	FDC.SR = 1;
	FDC.StatusTypeI = false;

	/* On cold reset, TR and DR should be reset */
	/* On warm reset, TR and DR value should be kept */
	if ( bCold )
	{
		FDC.TR = 0;
		FDC.DR = 0;
	}
	FDC.StepDirection = 1;

	FDC.Command = FDCEMU_CMD_NULL;			/* FDC emulation command currently being executed */
	FDC.CommandState = FDCEMU_RUN_NULL;
	FDC.CommandType = 0;

	FDC.IndexPulse_Counter = 0;

	FDC_DMA.Status = 1;				/* no DMA error and SectorCount=0 */
	FDC_DMA.Mode = 0;
	FDC_DMA.SectorCount = 0;
	FDC_ResetDMA();
}


/*-----------------------------------------------------------------------*/
/**
 * Reset DMA (clear internal 16 bytes buffer)
 *
 * This is done by 'toggling' bit 8 of the DMA Mode Control register
 */
static void FDC_ResetDMA ( void )
{
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );
	LOG_TRACE(TRACE_FDC, "fdc reset dma VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* Reset bytes count for current DMA sector */
	FDC_DMA.BytesInSector = DMA_DISK_SECTOR_SIZE;

	/* Reset variables used to handle DMA transfer */
	FDC_DMA.PosInBuffer = 0;
	FDC_DMA.PosInBufferTransfer = 0;
	FDC_DMA.BytesToTransfer = 0;

	/* Reset HDC command status */
	HDC_ResetCommandStatus();
}


/*-----------------------------------------------------------------------*/
/**
 * Set DMA Status at $ff8606
 *
 * Bit 0 - _Error Status (0=Error 1=No erroe)
 * Bit 1 - _Sector Count Zero Status (0=Sector Count Zero)
 * Bit 2 - _Data Request Inactive Status
 *
 * FIXME [NP] : is bit 0 really used on ST ? It seems it's always 1 (no DMA error)
 */
void FDC_SetDMAStatus ( bool bError )
{
	/* Set error bit */
	if (!bError)
		FDC_DMA.Status |= 0x1;					/* No Error, set bit 0 */
	else
		FDC_DMA.Status &= ~0x1;					/* Error, clear bit 0 */
}


/*-----------------------------------------------------------------------*/
/**
 * Init some variables before starting a new DMA transfer.
 * We must store new data just after the most recent bytes that
 * were not yet transferred by the DMA (16 bytes buffer).
 * To avoid writing above the limit of DMADiskWorkSpace, we move
 * the current 16 bytes buffer at the start of DMADiskWorkSpace
 * if some bytes remain to be transferred, this way we never use
 * more than FDC_TRACK_BYTES_STANDARD in DMADiskWorkSpace.
 */
static void FDC_DMA_InitTransfer ( void )
{
	int	i;

	/* How many bytes remain in the current 16 bytes DMA buffer ? */
	if ( ( FDC_DMA.BytesToTransfer == 0 )				/* DMA buffer is empty */
	  || ( FDC_DMA.BytesToTransfer > DMA_DISK_TRANSFER_SIZE ) )	/* Previous DMA transfer did not finish (FDC errror or Force Int command) */
	{
		FDC_DMA.PosInBuffer = 0;				/* Add new data at the start of DMADiskWorkSpace */
		FDC_DMA.PosInBufferTransfer = 0;
		FDC_DMA.BytesToTransfer = 0;				/* No more data to transfer from the previous DMA buffer */
	}
	else								/* 16 bytes buffer partially filled */
	{
		for ( i=0 ; i<FDC_DMA.BytesToTransfer ; i++ )		/* Move these bytes at the start of the buffer */
			DMADiskWorkSpace[ i ] = DMADiskWorkSpace[ FDC_DMA.PosInBufferTransfer + i ];

		FDC_DMA.PosInBuffer = FDC_DMA.BytesToTransfer;		/* Add new data after the latest bytes stored in the 16 bytes buffer */
		FDC_DMA.PosInBufferTransfer = 0;
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Transfer 16 bytes from the DMA workspace to the RAM.
 * Instead of handling a real 16 bytes buffer, this implementation moves
 * a 16 bytes window in DMADiskWorkSpace. The current position of this window
 * is stored in FDC_DMA.PosInBufferTransfer and contains the equivalent of the
 * DMA's internal 16 bytes buffer.
 *
 * Return true if there're no more bytes to transfer or false if some
 * bytes can still be tranfered by the DMA.
 *
 * NOTE [NP] : The DMA is connected to the FDC, each time a DRQ is made by the FDC,
 * it's handled by the DMA and stored in the DMA 16 bytes buffer. This means
 * FDC_STR_BIT_LOST_DATA will never be set (but data can be lost if FDC_DMA.SectorCount==0)
 */
static bool FDC_DMA_ReadFromFloppy ( void )
{
	Uint32	Address;
//fprintf ( stderr , "dma transfer read count=%d bytes=%d pos=%d\n" , FDC_DMA.SectorCount, FDC_DMA.BytesToTransfer, FDC_DMA.PosInBufferTransfer );

	if ( FDC_DMA.BytesToTransfer < DMA_DISK_TRANSFER_SIZE )
		return true;						/* There should be at least 16 bytes to start a DMA transfer */

	if ( FDC_DMA.SectorCount == 0 )
	{
		//FDC_Update_STR ( 0 , FDC_STR_BIT_LOST_DATA );		/* If DMA is OFF, data are lost -> Not on the ST */
		FDC_DMA.PosInBufferTransfer += DMA_DISK_TRANSFER_SIZE;
		FDC_DMA.BytesToTransfer -= DMA_DISK_TRANSFER_SIZE;
		if ( FDC_DMA.BytesToTransfer < DMA_DISK_TRANSFER_SIZE )
			return true;					/* There should be at least 16 bytes to start a new DMA transfer */
		else
			return false;					/* FDC DMA is off but we still need to read all bytes from the floppy */
	}

	/* Transfer data and update DMA address */
	Address = FDC_GetDMAAddress();
	STMemory_SafeCopy ( Address , DMADiskWorkSpace + FDC_DMA.PosInBufferTransfer , DMA_DISK_TRANSFER_SIZE , "FDC DMA data read" );
	FDC_DMA.PosInBufferTransfer += DMA_DISK_TRANSFER_SIZE;
	FDC_DMA.BytesToTransfer -= DMA_DISK_TRANSFER_SIZE;
	FDC_WriteDMAAddress ( Address + DMA_DISK_TRANSFER_SIZE );

	/* Update Sector Count */
	FDC_DMA.BytesInSector -= DMA_DISK_TRANSFER_SIZE;
	if ( FDC_DMA.BytesInSector <= 0 )
	{
		FDC_DMA.SectorCount--;
		FDC_DMA.BytesInSector = DMA_DISK_SECTOR_SIZE;
	}

	if ( FDC_DMA.BytesToTransfer < DMA_DISK_TRANSFER_SIZE )
		return true;						/* There should be at least 16 bytes to start a new DMA transfer */
	else
		return false;						/* Transfer is not complete */
}


/*-----------------------------------------------------------------------*/
/**
 * Transfer 16 bytes from the RAM to disk using DMA.
 * This is used to write data to the disk with correct timings
 * by writing blocks of 16 bytes at a time.
 *
 * Return true if there're no more bytes to transfer or false if some
 * bytes can still be tranfered by the DMA.
 *
 * NOTE [NP] : in the case of the emulation in Hatari, the sector is first written
 * to the disk image and this function is just used to increment
 * DMA address at the correct pace to simulate that bytes are written from
 * blocks of 16 bytes handled by the DMA.
 */
static bool FDC_DMA_WriteToFloppy ( void )
{
	Uint32	Address;
//fprintf ( stderr , "dma transfer write count=%d bytes=%d pos=%d\n" , FDC_DMA.SectorCount, FDC_DMA.BytesToTransfer, FDC_DMA.PosInBufferTransfer );

	if ( FDC_DMA.BytesToTransfer < DMA_DISK_TRANSFER_SIZE )
		return true;						/* There should be at least 16 bytes to start a DMA transfer */

	if ( FDC_DMA.SectorCount == 0 )
	{
		//FDC_Update_STR ( 0 , FDC_STR_BIT_LOST_DATA );		/* If DMA is OFF, data are lost -> Not on the ST */
		FDC_DMA.PosInBufferTransfer += DMA_DISK_TRANSFER_SIZE;
		FDC_DMA.BytesToTransfer -= DMA_DISK_TRANSFER_SIZE;
		if ( FDC_DMA.BytesToTransfer < DMA_DISK_TRANSFER_SIZE )
			return true;					/* There should be at least 16 bytes to start a new DMA transfer */
		else
			return false;					/* FDC DMA is off but we still need to read all bytes from the floppy */
	}

	/* Transfer data and update DMA address */
	Address = FDC_GetDMAAddress();
	//STMemory_SafeCopy ( Address , DMADiskWorkSpace + FDC_DMA.PosInBufferTransfer , DMA_DISK_TRANSFER_SIZE , "FDC DMA data read" );
	FDC_DMA.PosInBufferTransfer += DMA_DISK_TRANSFER_SIZE;
	FDC_DMA.BytesToTransfer -= DMA_DISK_TRANSFER_SIZE;
	FDC_WriteDMAAddress ( Address + DMA_DISK_TRANSFER_SIZE );

	/* Update Sector Count */
	FDC_DMA.BytesInSector -= DMA_DISK_TRANSFER_SIZE;
	if ( FDC_DMA.BytesInSector <= 0 )
	{
		FDC_DMA.SectorCount--;
		FDC_DMA.BytesInSector = DMA_DISK_SECTOR_SIZE;
	}

	if ( FDC_DMA.BytesToTransfer < DMA_DISK_TRANSFER_SIZE )
		return true;						/* There should be at least 16 bytes to start a new DMA transfer */
	else
		return false;						/* Transfer is not complete */
}


/*-----------------------------------------------------------------------*/
/**
 * Update the FDC's internal variables on a regular basis.
 * To get correct accuracy, this should be called every 200-500 FDC cycles
 * So far, we only need to update the index position for the valid
 * drive/floppy ; updating every 500 cycles is enough for this case.
 */
void	FDC_UpdateAll ( void )
{
	FDC_IndexPulse_Update ();
}


/*-----------------------------------------------------------------------*/
/**
 * This function is used to enable/disable a drive when
 * using the UI or command line parameters
 */
void	FDC_EnableDrive ( int Drive , bool value )
{
	LOG_TRACE ( TRACE_FDC , "fdc enable drive=%d %s\n" , Drive , value?"on":"off" );

	if ( ( Drive >= 0 ) && ( Drive < MAX_FLOPPYDRIVES ) )
		FDC_DRIVES[ Drive ].Enabled = value;
}


/*-----------------------------------------------------------------------*/
/**
 * This function is called when a floppy is inserted in a drive
 * using the UI or command line parameters
 */
void	FDC_InsertFloppy ( int Drive )
{
	LOG_TRACE ( TRACE_FDC , "fdc insert drive=%d\n" , Drive );

	if ( ( Drive >= 0 ) && ( Drive < MAX_FLOPPYDRIVES ) )
	{
		FDC_DRIVES[ Drive ].DiskInserted = true;
		if ( ( FDC.STR & FDC_STR_BIT_MOTOR_ON ) != 0 )		/* If we insert a floppy while motor is already on, we must */
			FDC_IndexPulse_Init ( Drive );			/* init the index pulse's position */
		else
			FDC_DRIVES[ Drive ].IndexPulse_Time = 0;	/* Index pulse's position not known yet */
		FDC_DRIVES[ Drive ].Density = FDC_GetDensity ( Drive );
	}
}


/*-----------------------------------------------------------------------*/
/**
 * This function is called when a floppy is ejected from a drive
 * using the UI or command line parameters
 */
void	FDC_EjectFloppy ( int Drive )
{
	LOG_TRACE ( TRACE_FDC , "fdc eject drive=%d\n" , Drive );

	if ( ( Drive >= 0 ) && ( Drive < MAX_FLOPPYDRIVES ) )
	{
		FDC_DRIVES[ Drive ].DiskInserted = false;
		FDC_DRIVES[ Drive ].IndexPulse_Time = 0;		/* Stop counting index pulses on an empty drive */
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Handle a write in the IO_PORTA register $E through $ff8802. Only bits
 * 0-2 are available here, others are masked to 0.
 * bit 0 : side select
 * bit 1-2 : drive select
 *
 * If both drives are selected, we keep only drive 0
 */
void	FDC_SetDriveSide ( Uint8 io_porta_old , Uint8 io_porta_new )
{
	int	Side;
	int	Drive;

	if ( io_porta_old == io_porta_new )
		return;							/* No change */

	Side = ( (~io_porta_new) & 0x01 );				/* Side 0 or 1 */

	Drive = -1;							/* By default, don't select any drive */

	/* Check drive 1 first */
	if ( ( io_porta_new & 0x04 ) == 0 )
		Drive = 1;						/* Select drive 1 */

	/* If both drive 0 and drive 1 are enabled, we keep only drive 0 as newdrive */
	if ( ( io_porta_new & 0x02 ) == 0 )
		Drive = 0;						/* Select drive 0 (and un-select drive 1 if set above) */

	LOG_TRACE(TRACE_FDC, "fdc change drive/side io_porta_old=0x%x io_porta_new=0x%x side %d->%d drive %d->%d VBL=%d HBL=%d\n" ,
		  io_porta_old , io_porta_new , FDC.SideSignal , Side , FDC.DriveSelSignal , Drive , nVBLs , nHBL );

	if ( FDC.DriveSelSignal != Drive )
	{
		FDC_DRIVES[ FDC.DriveSelSignal ].IndexPulse_Time = 0;	/* Stop counting index pulse on the previous drive */
		if ( ( FDC.STR & FDC_STR_BIT_MOTOR_ON ) != 0 )		/* If we change drive while motor is already on, we must */
			FDC_IndexPulse_Init ( Drive );			/* init the index pulse's position on the new drive */
		else
			FDC_DRIVES[ Drive ].IndexPulse_Time = 0;	/* Index pulse's position not known yet */
	}
	
	FDC.SideSignal = Side;
	FDC.DriveSelSignal = Drive;
}


/*-----------------------------------------------------------------------*/
/**
 * Check if a floppy drive is selected
 * If not, we should ignore the corresponding FDC commands
 */
static bool FDC_ValidFloppyDrive ( void )
{
	if ( FDC.DriveSelSignal >= 0 )
		return true;
	else
		return false;						/* neither A: not B: are selected */
}


/*-----------------------------------------------------------------------*/
/**
 * Return device for FDC, check PORTA bits 1,2 (0=on,1=off)
 */
static int FDC_FindFloppyDrive ( void )
{
	/* Check Drive A first */
	if ((PSGRegisters[PSG_REG_IO_PORTA]&0x2)==0)
		return 0;						/* Device 0 (A:) */
	/* If off, check Drive B */
	if ((PSGRegisters[PSG_REG_IO_PORTA]&0x4)==0)
		return 1;						/* Device 1 (B:) */

	/* None appear to be selected so default to Drive A */
	/* [NP] 2012/03/04 : this is certainly wrong, we should ignore commands, not default to A: (see FDC_ValidFloppyDrive()) */
	return 0;							/* Device 0 (A:) */
}


/*-----------------------------------------------------------------------*/
/**
 * Return number of sectors for track/side of current drive
 * TODO [NP] : this function calls Floppy_FindDiskDetails which handles only ST/MSA
 * disk image so far, so this implies all tracks have in fact the same number
 * of sectors (we don't use Track and Side for now)
 */
static int FDC_GetSectorsPerTrack ( int Track , int Side )
{
	Uint16	SectorsPerTrack;

	if (EmulationDrives[ FDC_DRIVE ].bDiskInserted)
	{
		Floppy_FindDiskDetails ( EmulationDrives[ FDC_DRIVE ].pBuffer , EmulationDrives[ FDC_DRIVE ].nImageBytes , &SectorsPerTrack , NULL );
		return SectorsPerTrack;
	}
	else
		return 0;
}


static int FDC_GetSidesPerDisk ( int Track )
{
	Uint16	SidesPerDisk;

	if (EmulationDrives[ FDC_DRIVE ].bDiskInserted)
	{
		Floppy_FindDiskDetails ( EmulationDrives[ FDC_DRIVE ].pBuffer , EmulationDrives[ FDC_DRIVE ].nImageBytes , NULL , &SidesPerDisk );
		return SidesPerDisk;					/* 1 or 2 */
	}
	else
		return 0;
}


/*
 * A DD track is usually 9 or 10 sectors, but to handle HD or ED
 * ST/MSA disk images, we check if we have more than 18 or 36 sectors.
 * In that case, we use a x2 or x4 factor for theses disks.
 */
static int FDC_GetDensity ( int Drive )
{
	Uint16	SectorsPerTrack;

	if ( EmulationDrives[ Drive ].bDiskInserted )
	{
		Floppy_FindDiskDetails ( EmulationDrives[ Drive ].pBuffer , EmulationDrives[ Drive ].nImageBytes , &SectorsPerTrack , NULL );
		if ( SectorsPerTrack >= 36 )
			return FDC_DENSITY_FACTOR_ED;			/* Simulate a ED disk, 36 sectors or more */
		else if ( SectorsPerTrack >= 18 )
			return FDC_DENSITY_FACTOR_HD;			/* Simulate a HD disk, between 18 and 36 sectors */
		else
			return FDC_DENSITY_FACTOR_DD;			/* Normal DD disk */
	}
	else
		return FDC_DENSITY_FACTOR_DD;				/* No disk, default to Double Density */
}


/*-----------------------------------------------------------------------*/
/**
 * Return the number of bytes in a track when using the read/write track
 * type III command.
 * A DD track is usually FDC_TRACK_BYTES_STANDARD, but to handle HD or ED
 * ST/MSA disk images, we simulate a bigger track size if we have more
 * than 18 or 36 sectors.
 */
static int	FDC_GetBytesPerTrack ( void )
{
	int	TrackSize;
	int	MaxSector;

	TrackSize = FDC_TRACK_BYTES_STANDARD;				/* For a standard DD disk */

	MaxSector = FDC_GetSectorsPerTrack ( FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_SIDE );

	if ( MaxSector >= 36 )
		TrackSize *= 4;						/* Simulate a ED disk, 36 sectors or more */
	else if ( MaxSector >= 18 )
		TrackSize *= 2;						/* Simulate a HD disk, between 18 and 36 sectors */

	return TrackSize;
}


/*-----------------------------------------------------------------------*/
/**
 * If some valid drive/floppy are available and the motor signal is on,
 * update the current angular position for the drive and check if
 * a new index pulse was reached. Increase Index Pulse counter in that case.
 *
 * This function should be called at least every 500 FDC cycles when motor
 * is ON to get good accuracy.
 *
 * [NP] TODO : should we have 2 different Index Pulses for each side or do they
 * happen at the same time ?
 */
void	FDC_IndexPulse_Update ( void )
{
	Uint32	CpuCyclesPerRev;

//fprintf ( stderr , "update index drive=%d side=%d counter=%d VBL=%d HBL=%d\n" , FDC.DriveSelSignal , FDC.SideSignal , FDC.IndexPulse_Counter , nVBLs , nHBL );

	if ( ( FDC.STR & FDC_STR_BIT_MOTOR_ON ) == 0 )
		return;							/* Motor is OFF, nothing to update */

	if ( ( FDC.DriveSelSignal < 0 ) || ( !FDC_DRIVES[ FDC.DriveSelSignal ].Enabled )
		|| ( !FDC_DRIVES[ FDC.DriveSelSignal ].DiskInserted ) )
		return;							/* No valid drive/floppy, nothing to update */

	if ( FDC_DRIVES[ FDC.DriveSelSignal ].IndexPulse_Time == 0 )	/* No reference Index Pulse for this drive */
		FDC_IndexPulse_Init ( FDC.DriveSelSignal );

	CpuCyclesPerRev = ( (Uint64)(8021247.L*1000) / ( FDC_DRIVES[ FDC.DriveSelSignal ].RPM / 60 ) );

	if ( CyclesGlobalClockCounter - FDC_DRIVES[ FDC.DriveSelSignal ].IndexPulse_Time >= CpuCyclesPerRev )
	{
		  FDC_DRIVES[ FDC.DriveSelSignal ].IndexPulse_Time += CpuCyclesPerRev;	/* Position of the most recent Index Pulse */
		  FDC.IndexPulse_Counter++;
		  LOG_TRACE(TRACE_FDC, "fdc update index drive=%d side=%d counter=%d VBL=%d HBL=%d\n" ,
			  FDC.DriveSelSignal , FDC.SideSignal , FDC.IndexPulse_Counter , nVBLs , nHBL );
	}
}


/*-----------------------------------------------------------------------*/
/**
 * When motor is started, the position of the next index pulse will be random,
 * as we don't know how much the floppy rotated when the motor was stopped or
 * the floppy was inserted.
 * We compute a random position in the "past" (less than one revolution)
 * and use it as a reference to detect the next index pulse.
 *
 */
static void	FDC_IndexPulse_Init ( int Drive )
{
	int	CpuCyclesPerRev;
	Uint64	IndexPulse_Time;

	CpuCyclesPerRev = ( (Uint64)(8021247.L*1000) / ( FDC_DRIVES[ FDC_DRIVE ].RPM / 60 ) );
	IndexPulse_Time = CyclesGlobalClockCounter - rand () % CpuCyclesPerRev;
	if ( IndexPulse_Time <= 0 )					/* Should not happen (only if FDC_IndexPulse_Init is */
		IndexPulse_Time = 1;					/* called just after emulation starts) */
	FDC_DRIVES[ Drive ].IndexPulse_Time = IndexPulse_Time;

//fprintf ( stderr , "fdc index pulse init %lld\n" ,  FDC.IndexPulse_Time );
}


static void	FDC_IndexPulse_Init_old ( void )
{
	FDC.IndexPulse_Time = CyclesGlobalClockCounter;
//fprintf ( stderr , "fdc index pulse init %lld\n" ,  FDC.IndexPulse_Time );
}


/*-----------------------------------------------------------------------*/
/**
 * Return the current position in the track relative to the index pulse.
 * For standard floppy, this is a number of bytes in the range [0,6250[
 */
static int	FDC_IndexPulse_GetCurrentPos ()
{
	Uint64	BytesSinceIndex;

	/* Transform the current number of cycles since the reference index into a number of bytes */
	BytesSinceIndex = ( CyclesGlobalClockCounter - FDC.IndexPulse_Time ) / FDC_FdcCyclesToCpuCycles ( FDC_TransferByte_FdcCycles ( 1 ) );

//fprintf ( stderr , "fdc index pulse pos cur=%lld ref=%lld bytes=%lld pos=%d\n" ,  CyclesGlobalClockCounter , FDC.IndexPulse_Time , BytesSinceIndex , (int)(BytesSinceIndex % FDC_GetBytesPerTrack () ) );
	/* Ignore the total number of spins, only keep the position relative to the index pulse */
	return ( BytesSinceIndex % FDC_GetBytesPerTrack ()  );
}


static int	FDC_IndexPulse_GetCurrentPos_FdcCycles ( int *pFdcCyclesPerRev )
{
	int	CpuCyclesPerRev;
	int	CpuCyclesSinceIndex;

	/* Get the number of CPU cycles for one revolution of the floppy */
	/* RPM is already multiplied by 1000 to simulate non-integer values */
	CpuCyclesPerRev = ( (Uint64)(8021247.L*1000) / ( FDC_DRIVES[ FDC_DRIVE ].RPM / 60 ) );
	CpuCyclesSinceIndex = ( CyclesGlobalClockCounter - FDC.IndexPulse_Time ) % CpuCyclesPerRev;

	if ( pFdcCyclesPerRev )
		*pFdcCyclesPerRev = FDC_CpuCyclesToFdcCycles ( CpuCyclesPerRev );

	return FDC_CpuCyclesToFdcCycles ( CpuCyclesSinceIndex );
}


static int	FDC_IndexPulse_GetCurrentPos_NbBytes ( void )
{
	int	FdcCyclesSinceIndex;

	FdcCyclesSinceIndex = FDC_IndexPulse_GetCurrentPos_FdcCycles ( NULL );
//fprintf ( stderr , "fdc index current pos old=%d new=%d\n" , FDC_IndexPulse_GetCurrentPos() , FdcCyclesSinceIndex / FDC_DELAY_CYCLE_MFM_BYTE );

	return FdcCyclesSinceIndex / FDC_DELAY_CYCLE_MFM_BYTE;
}



/*-----------------------------------------------------------------------*/
/**
 * Return the current state of the index pulse signal.
 * The signal goes to 1 when reaching the index pulse location and remain
 * to 1 during 1.5 ms (approx 46 bytes).
 * During the rest of the track, the signal will be 0.
 */
static int	FDC_IndexPulse_GetState ( void )
{
#ifdef old_index
	int	CurrentPos;
	int	state;

	CurrentPos = FDC_IndexPulse_GetCurrentPos ();

	state = 0;
	if ( CurrentPos <  FDC_DelayToFdcCycles ( FDC_DELAY_US_INDEX_PULSE_LENGTH ) / FDC_TransferByte_FdcCycles ( 1 ) )
		state = 1;

//fprintf ( stderr , "fdc index state 1 pos pos=%d state=%d\n" , CurrentPos , state );
	return state;

#else
	int	state;
	int	FdcCyclesSinceIndex;

	FdcCyclesSinceIndex = FDC_IndexPulse_GetCurrentPos_FdcCycles ( NULL );

	state = 0;
	if ( FdcCyclesSinceIndex < FDC_DelayToFdcCycles ( FDC_DELAY_US_INDEX_PULSE_LENGTH ) )
		state = 1;

//fprintf ( stderr , "fdc index state 2 pos pos=%d state=%d\n" , FdcCyclesSinceIndex , state );
	
	return state;
#endif
}


/*-----------------------------------------------------------------------*/
/**
 * Return the number of bytes to read from the track before reaching the
 * next index pulse signal.
 */


static int	FDC_NextIndexPulse_FdcCycles ( void )
{
	int	FdcCyclesPerRev;
	int	FdcCyclesSinceIndex;
	int	res;

	FdcCyclesSinceIndex = FDC_IndexPulse_GetCurrentPos_FdcCycles ( &FdcCyclesPerRev );

	res = FdcCyclesPerRev - FdcCyclesSinceIndex;

	/* If the next IP is in 0 or 1 cycle, we consider this is a rounding error */
	/* and we wait for one full revolution (this can happen in Force Int on Index Pulse */
	/* when we call FDC_NextIndexPulse_FdcCycles in a loop) */
	if ( res <= 1 )
		res = FdcCyclesPerRev;
	
//fprintf ( stderr , "fdc next index current pos old=%d new=%d\n" , FDC_NextIndexPulse_NbBytes() * FDC_DELAY_CYCLE_MFM_BYTE , res );

	return res;
}


static int	FDC_NextIndexPulse_NbBytes ( void )
{
#ifdef old_index
	return FDC_GetBytesPerTrack () - FDC_IndexPulse_GetCurrentPos ();
#else
	return FDC_GetBytesPerTrack () - FDC_IndexPulse_GetCurrentPos_NbBytes ();
#endif
}


/*-----------------------------------------------------------------------*/
/**
 * Return the number of bytes to read from the track before reaching the
 * next sector's ID Field ($A1 $A1 $A1 $FE TR SIDE SR LEN CRC1 CRC2)
 * If no ID Field is found before the end of the track, we use the 1st
 * ID Field of the track (which simulates a full spin of the floppy).
 * We also store the next sector's number into NextSector_ID_Field_SR.
 * This function assumes some 512 byte sectors stored in ascending
 * order (for ST/MSA)
 */
static int	FDC_NextSectorID_NbBytes ( void )
{
	int	CurrentPos;
	int	MaxSector;
	int	TrackPos;
	int	i;
	int	NextSector;
	int	NbBytes;

#ifdef old_index
	CurrentPos = FDC_IndexPulse_GetCurrentPos ();
#else
	CurrentPos = FDC_IndexPulse_GetCurrentPos_NbBytes ();
#endif

	MaxSector = FDC_GetSectorsPerTrack ( FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_SIDE );
	TrackPos = FDC_TRACK_LAYOUT_STANDARD_GAP1;			/* Position of 1st raw sector */
	TrackPos += FDC_TRACK_LAYOUT_STANDARD_GAP2;			/* Position of ID Field in 1st raw sector */

	/* Compare CurrentPos with each sector's position in ascending order */
	for ( i=0 ; i<MaxSector ; i++ )
	{
		if ( CurrentPos < TrackPos )
			break;						/* We found the next sector */
		else
			TrackPos += FDC_TRACK_LAYOUT_STANDARD_RAW_SECTOR_512;
	}

	if ( i == MaxSector )						/* CurrentPos is after the last ID Field of this track */
	{
		/* Reach end of track (new index pulse), then go to sector 1 */
		NbBytes = FDC_GetBytesPerTrack () - CurrentPos + FDC_TRACK_LAYOUT_STANDARD_GAP1 + FDC_TRACK_LAYOUT_STANDARD_GAP2;
		NextSector = 1;
	}
	else								/* There's an ID Field before end of track */
	{
		NbBytes = TrackPos - CurrentPos;
		NextSector = i+1;
	}

//fprintf ( stderr , "fdc bytes next sector pos=%d trpos=%d nbbytes=%d maxsr=%d nextsr=%d\n" , CurrentPos, TrackPos, NbBytes, MaxSector, NextSector );
	FDC.NextSector_ID_Field_SR = NextSector;
	return NbBytes;
}



/*-----------------------------------------------------------------------*/
/**
 * Acknowledge FDC interrupt
 */
void FDC_AcknowledgeInterrupt ( void )
{
	/* Acknowledge in MFP circuit, pass bit, enable, pending */
	MFP_InputOnChannel ( MFP_INT_FDCHDC , 0 );
	MFP_GPIP &= ~0x20;
}


/*-----------------------------------------------------------------------*/
/**
 * Handle the current FDC command.
 * We use a timer to go from one state to another to emulate the different
 * phases of an FDC command.
 * When the command completes (success or failure), FDC.Command will be
 * set to FDCEMU_CMD_NULL. Until then, this function will be called to
 * handle each state of the command and the corresponding delay in micro
 * seconds.
 * This handler is called after a first delay corresponding to the prepare
 * delay and the eventual motor on delay.
 * Once we reach this point, the current command can not be replaced by
 * another command (except 'Force Interrupt')
 */
void FDC_InterruptHandler_Update ( void )
{
	int	FdcCycles = 0;
	int	PendingCyclesOver;

	/* Number of internal cycles we went over for this timer ( <= 0 ) */
	/* Used to restart the next timer and keep a constant rate (important for DMA transfers) */
	PendingCyclesOver = -PendingInterruptCount;			/* >= 0 */

//fprintf ( stderr , "fdc int handler %lld delay %d\n" , CyclesGlobalClockCounter, PendingCyclesOver );

	CycInt_AcknowledgeInterrupt();

	/* Update FDC's internal variables */
	FDC_UpdateAll ();

	/* Is FDC active? */
	if (FDC.Command!=FDCEMU_CMD_NULL)
	{
		FDC.ReplaceCommandPossible = false;

		/* If the command needed to restart the motor, the motor is now ON */
		/* so we must init a new index position */
		if ( FDC.UpdateIndexPulse == true )
		{
			FDC_IndexPulse_Init_old ();
			FDC.UpdateIndexPulse = false;
		}

		/* Which command are we running ? */
		switch(FDC.Command)
		{
		 case FDCEMU_CMD_RESTORE:
			FdcCycles = FDC_UpdateRestoreCmd();
			break;
		 case FDCEMU_CMD_SEEK:
			FdcCycles = FDC_UpdateSeekCmd();
			break;
		 case FDCEMU_CMD_STEP:
			FdcCycles = FDC_UpdateStepCmd();
			break;

		 case FDCEMU_CMD_READSECTORS:
			FdcCycles = FDC_UpdateReadSectorsCmd();
			break;
		 case FDCEMU_CMD_WRITESECTORS:
			FdcCycles = FDC_UpdateWriteSectorsCmd();
			break;

		 case FDCEMU_CMD_READADDRESS:
			FdcCycles = FDC_UpdateReadAddressCmd();
			break;

		 case FDCEMU_CMD_READTRACK:
			FdcCycles = FDC_UpdateReadTrackCmd();
			break;

		 case FDCEMU_CMD_FORCEINT:
			FdcCycles = FDC_UpdateForceIntCmd();
			break;

		 case FDCEMU_CMD_MOTOR_STOP:
			FdcCycles = FDC_UpdateMotorStop();
			break;
		}
	}

	if (FDC.Command != FDCEMU_CMD_NULL)
	{
		FDC_StartTimer_FdcCycles ( FdcCycles , -PendingCyclesOver );
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Update the FDC's Status Register.
 * All bits in DisableBits are cleared in STR, then all bits in EnableBits
 * are set in STR.
 */
static void FDC_Update_STR ( Uint8 DisableBits , Uint8 EnableBits )
{
	FDC.STR &= (~DisableBits);					/* Clear bits in DisableBits */
	FDC.STR |= EnableBits;						/* Set bits in EnableBits */

	FDC_SetDriveLedBusy ();
//fprintf ( stderr , "fdc str 0x%x\n" , FDC.STR );
}


/*-----------------------------------------------------------------------*/
/**
 * Common to all commands once they're completed :
 * - remove busy bit
 * - acknowledge interrupt if necessary
 * - stop motor after 2 sec
 */
static int FDC_CmdCompleteCommon ( bool DoInt )
{
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );
	LOG_TRACE(TRACE_FDC, "fdc complete command VBL=%d video_cyc=%d %d@%d pc=%x\n",
		nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	FDC_Update_STR ( FDC_STR_BIT_BUSY , 0 );			/* Remove busy bit */

	if ( DoInt )
		FDC_AcknowledgeInterrupt();

	FDC.Command = FDCEMU_CMD_MOTOR_STOP;				/* Fake command to stop the motor */
	FDC.CommandState = FDCEMU_RUN_MOTOR_STOP;
//	return FDC_DelayToFdcCycles ( FDC_DELAY_US_MOTOR_OFF );
	return FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
}


/*-----------------------------------------------------------------------*/
/**
 * Verify track after a type I command.
 * The FDC will read the first ID field of the current track and will
 * compare the track number in this ID field with the current Track Register.
 * If they don't match, an error is set with the RNF bit.
 * NOTE : in the case of Hatari when using ST/MSA images, the track is always the correct one,
 * so the verify will always be good (except if no disk is inserted or the physical head is
 * not on the same track as FDC.TR)
 * This function could be improved to support other images format where logical track
 * could be different from physical track (eg Pasti)
 */
static void FDC_VerifyTrack ( void )
{
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	/* Set RNF if no drive selected, or drive not enabled, or no disk in drive */
	if ( ( FDC.DriveSelSignal < 0 ) || ( !FDC_DRIVES[ FDC.DriveSelSignal ].Enabled )
		|| ( !FDC_DRIVES[ FDC.DriveSelSignal ].DiskInserted ) )
	{
		LOG_TRACE(TRACE_FDC, "fdc type I verify track failed disabled/empty drive=%d VBL=%d video_cyc=%d %d@%d pc=%x\n",
			FDC.DriveSelSignal , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

		FDC_Update_STR ( 0 , FDC_STR_BIT_RNF );			/* Set RNF bit */
		return;
	}

	/* Most of the time, the physical track and the track register should be the same. */
	/* Else, it means TR was not correctly set before running the type I command */
	if ( FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack != FDC.TR )
	{
		LOG_TRACE(TRACE_FDC, "fdc type I verify track failed TR=0x%x head=0x%x drive=%d VBL=%d video_cyc=%d %d@%d pc=%x\n",
			FDC.TR , FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack , FDC.DriveSelSignal ,
			nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

		FDC_Update_STR ( 0 , FDC_STR_BIT_RNF );			/* Set RNF bit */
		return;
	}

	/* The track is the correct one */
	FDC_Update_STR ( FDC_STR_BIT_RNF , 0 );				/* remove RNF bit */
}


/*-----------------------------------------------------------------------*/
/**
 * When the motor really stops (2 secs after the last command), clear all related bits in SR
 * We clear motor bit, but spinup bit remains to 1 (verified on a real STF)
 */
static int FDC_UpdateMotorStop_old ( void )
{
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );
	LOG_TRACE(TRACE_FDC, "fdc motor stopped VBL=%d video_cyc=%d %d@%d pc=%x\n",
		nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	FDC_Update_STR ( FDC_STR_BIT_MOTOR_ON , 0 );			/* Unset motor bit and keep spinup bit */

	FDC.Command = FDCEMU_CMD_NULL;					/* Motor stopped, this is the last state */
	return 0;
}


/*-----------------------------------------------------------------------*/
/**
 * Run the 'motor stop' sequence : wait for 9 revolutions (1.8 sec)
 * and stop the motor.
 * We clear motor bit, but spinup bit remains to 1 (verified on a real STF)
 */
static int FDC_UpdateMotorStop ( void )
{
	int	FdcCycles = 0;
	int	FrameCycles, HblCounterVideo, LineCycles;

	/* Which command is running? */
	switch (FDC.CommandState)
	{
	 case FDCEMU_RUN_MOTOR_STOP:
		FDC.IndexPulse_Counter = 0;
		FDC.CommandState = FDCEMU_RUN_MOTOR_STOP_WAIT;
	 case FDCEMU_RUN_MOTOR_STOP_WAIT:
		if ( FDC.IndexPulse_Counter < FDC_DELAY_IP_MOTOR_OFF )
		{
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Wait for the correct number of IP */
			break;
		}
		/* If IndexPulse_Counter reached, we go directly to the _COMPLETE state */
	 case FDCEMU_RUN_MOTOR_STOP_COMPLETE:
		Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );
		LOG_TRACE(TRACE_FDC, "fdc motor stopped VBL=%d video_cyc=%d %d@%d pc=%x\n",
			nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

		FDC_Update_STR ( FDC_STR_BIT_MOTOR_ON , 0 );		/* Unset motor bit and keep spin up bit */
		FDC.Command = FDCEMU_CMD_NULL;				/* Motor stopped, this is the last state */
		FdcCycles = 0;
		break;
	}
	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Run 'RESTORE' command
 */
static int FDC_UpdateRestoreCmd ( void )
{
	int	FdcCycles = 0;

	/* Which command is running? */
	switch (FDC.CommandState)
	{
	 case FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO:
		if ( FDC_Set_MotorON ( FDC.CR ) )
		{
			FDC.CommandState = FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO_SPIN_UP;
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Spin up needed */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO_MOTOR_ON;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;		/* No spin up needed */
		}
		break;
	 case FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO_SPIN_UP:
		if ( FDC.IndexPulse_Counter < FDC_DELAY_IP_SPIN_UP )
		{
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Wait for the correct number of IP */
			break;
		}
		/* If IndexPulse_Counter reached, we go directly to the _MOTOR_ON state */
	 case FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO_MOTOR_ON:
		FDC_Update_STR ( 0 , FDC_STR_BIT_SPIN_UP );		/* At this point, spin up sequence is ok */

		/* The FDC will try 255 times to reach track 0 using step out signals */
		/* If track 0 signal is not detected after 255 attempts, the command is interrupted */
		/* and FDC_STR_BIT_RNF is set in the Status Register. */
		/* This can happen if no drive is selected or if the selected drive is disabled */
		/* TR should be set to 255 once the spin-up sequence is made and the command */
		/* can't be interrupted anymore by another command (else TR value will be wrong */
		/* for other type I commands) */
		FDC.TR = 0xff;				
		FDC.CommandState = FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO_LOOP;	/* continue in the _LOOP state */
	 case FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO_LOOP:
		if ( FDC.TR == 0 )					/* Track 0 not reached after 255 attempts ? */
		{							/* (this can happen if the drive is disabled) */
			FDC_Update_STR ( 0 , FDC_STR_BIT_RNF );
			FDC_Update_STR ( FDC_STR_BIT_TR00 , 0 );	/* Unset bit TR00 */
			FdcCycles = FDC_CmdCompleteCommon( true );
		}

		if ( ( FDC.DriveSelSignal < 0 ) || ( !FDC_DRIVES[ FDC.DriveSelSignal ].Enabled )
			|| ( FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack != 0 ) )	/* Are we at track zero ? */
		{
			FDC_Update_STR ( FDC_STR_BIT_TR00 , 0 );	/* Unset bit TR00 */
			FDC.TR--;					/* One less attempt */
			if ( ( FDC.DriveSelSignal >= 0 ) && ( FDC_DRIVES[ FDC.DriveSelSignal ].Enabled ) )
				FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack--;	/* Move physical head only if an enabled drive is selected */
			FdcCycles = FDC_DelayToFdcCycles ( FDC_StepRate_ms[ FDC_STEP_RATE ] * 1000 );
		}
		else							/* Drive is enabled and head is at track 0 */
		{
			FDC_Update_STR ( 0 , FDC_STR_BIT_TR00 );	/* Set bit TR00 */
			FDC.TR = 0;					/* Update Track Register to 0 */
			FDC.CommandState = FDCEMU_RUN_RESTORE_VERIFY;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
		}
		break;
	 case FDCEMU_RUN_RESTORE_VERIFY:
		if ( FDC.CR & FDC_COMMAND_BIT_VERIFY )
		{
			FDC.CommandState = FDCEMU_RUN_RESTORE_VERIFY_HEAD_OK;
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_US_HEAD_LOAD );	/* Head settle delay */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_RESTORE_COMPLETE;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_COMPLETE;
		}
		break;
	 case FDCEMU_RUN_RESTORE_VERIFY_HEAD_OK:
		FDC.CommandState = FDCEMU_RUN_RESTORE_VERIFY_LOOP;
		FdcCycles = FDC_TransferByte_FdcCycles ( FDC_NextSectorID_NbBytes () + 10 );	/* Delay to read 3xA1, FE, ID field */
		break;
	 case FDCEMU_RUN_RESTORE_VERIFY_LOOP:
		FDC_VerifyTrack();
		FDC.CommandState = FDCEMU_RUN_RESTORE_COMPLETE;
		FdcCycles = FDC_DELAY_CYCLE_COMMAND_COMPLETE;
		break;
	 case FDCEMU_RUN_RESTORE_COMPLETE:
		FdcCycles = FDC_CmdCompleteCommon( true );
		break;
	}

	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Run 'SEEK' command
 */
static int FDC_UpdateSeekCmd ( void )
{
	int	FdcCycles = 0;

	/* Which command is running? */
	switch (FDC.CommandState)
	{
	 case FDCEMU_RUN_SEEK_TOTRACK:
		if ( FDC_Set_MotorON ( FDC.CR ) )
		{
			FDC.CommandState = FDCEMU_RUN_SEEK_TOTRACK_SPIN_UP;
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Spin up needed */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_SEEK_TOTRACK_MOTOR_ON;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;		/* No spin up needed */
		}
		break;
	 case FDCEMU_RUN_SEEK_TOTRACK_SPIN_UP:
		if ( FDC.IndexPulse_Counter < FDC_DELAY_IP_SPIN_UP )
		{
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Wait for the correct number of IP */
			break;
		}
		/* If IndexPulse_Counter reached, we go directly to the _MOTOR_ON state */
	 case FDCEMU_RUN_SEEK_TOTRACK_MOTOR_ON:
		FDC_Update_STR ( 0 , FDC_STR_BIT_SPIN_UP );		/* At this point, spin up sequence is ok */

		if ( FDC.TR == FDC.DR )					/* Are we at the selected track ? */
		{
			FDC.CommandState = FDCEMU_RUN_SEEK_VERIFY;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
		}
		else
		{
			if ( FDC.DR < FDC.TR )				/* Set StepDirection to the correct value */
				FDC.StepDirection = -1;
			else
				FDC.StepDirection = 1;

			/* Move head by one track depending on FDC.StepDirection and update Track Register */
			FDC.TR += FDC.StepDirection;

			FdcCycles = FDC_DelayToFdcCycles ( FDC_StepRate_ms[ FDC_STEP_RATE ] * 1000 );
			FDC_Update_STR ( FDC_STR_BIT_TR00 , 0 );	/* By default, unset bit TR00 */

			/* Check / move physical head only if an enabled drive is selected */
			if ( ( FDC.DriveSelSignal >= 0 ) && ( FDC_DRIVES[ FDC.DriveSelSignal ].Enabled ) )
			{
				if ( ( FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack == FDC_PHYSICAL_MAX_TRACK ) && ( FDC.StepDirection == 1 ) )
				{
					FDC.CommandState = FDCEMU_RUN_SEEK_VERIFY;
					FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;	/* No delay if trying to go after max track */
				}

				else if ( ( FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack == 0 ) && ( FDC.StepDirection == -1 ) )
				{
					FDC.TR = 0;			/* If we reach track 0, we stop there */
					FDC.CommandState = FDCEMU_RUN_SEEK_VERIFY;
					FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
				}

				else
					FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack += FDC.StepDirection;	/* Move physical head */

				if ( FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack == 0 )
					FDC_Update_STR ( 0 , FDC_STR_BIT_TR00 );	/* Set bit TR00 */
			}
		}

		break;
	 case FDCEMU_RUN_SEEK_VERIFY:
		if ( FDC.CR & FDC_COMMAND_BIT_VERIFY )
		{
			FDC.CommandState = FDCEMU_RUN_SEEK_VERIFY_HEAD_OK;
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_US_HEAD_LOAD );	/* Head settle delay */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_SEEK_COMPLETE;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_COMPLETE;
		}
		break;
	 case FDCEMU_RUN_SEEK_VERIFY_HEAD_OK:
		FDC.CommandState = FDCEMU_RUN_SEEK_VERIFY_LOOP;
		FdcCycles = FDC_TransferByte_FdcCycles ( FDC_NextSectorID_NbBytes () + 10 );	/* Delay to read 3xA1, FE, ID field */
		break;
	 case FDCEMU_RUN_SEEK_VERIFY_LOOP:
		FDC_VerifyTrack();
		FDC.CommandState = FDCEMU_RUN_SEEK_COMPLETE;
		FdcCycles = FDC_DELAY_CYCLE_COMMAND_COMPLETE;
		break;
	 case FDCEMU_RUN_SEEK_COMPLETE:
		FdcCycles = FDC_CmdCompleteCommon( true );
		break;
	}

	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Run 'STEP' command
 */
static int FDC_UpdateStepCmd ( void )
{
	int	FdcCycles = 0;

	/* Which command is running? */
	switch (FDC.CommandState)
	{
	 case FDCEMU_RUN_STEP_ONCE:
		if ( FDC_Set_MotorON ( FDC.CR ) )
		{
			FDC.CommandState = FDCEMU_RUN_STEP_ONCE_SPIN_UP;
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Spin up needed */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_STEP_ONCE_MOTOR_ON;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;		/* No spin up needed */
		}
		break;
	 case FDCEMU_RUN_STEP_ONCE_SPIN_UP:
		if ( FDC.IndexPulse_Counter < FDC_DELAY_IP_SPIN_UP )
		{
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Wait for the correct number of IP */
			break;
		}
		/* If IndexPulse_Counter reached, we go directly to the _MOTOR_ON state */
	 case FDCEMU_RUN_STEP_ONCE_MOTOR_ON:
		FDC_Update_STR ( 0 , FDC_STR_BIT_SPIN_UP );		/* At this point, spin up sequence is ok */

		/* Move head by one track depending on FDC.StepDirection */
		if ( FDC.CR & FDC_COMMAND_BIT_UPDATE_TRACK )
			FDC.TR += FDC.StepDirection;			/* Update Track Register */

		FdcCycles = FDC_DelayToFdcCycles ( FDC_StepRate_ms[ FDC_STEP_RATE ] * 1000 );
		FDC_Update_STR ( FDC_STR_BIT_TR00 , 0 );		/* By default, unset bit TR00 */

		/* Check / move physical head only if an enabled drive is selected */
		if ( ( FDC.DriveSelSignal >= 0 ) && ( FDC_DRIVES[ FDC.DriveSelSignal ].Enabled ) )
		{
			if ( ( FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack == FDC_PHYSICAL_MAX_TRACK ) && ( FDC.StepDirection == 1 ) )
				FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;	/* No delay if trying to go after max track */

			else if ( ( FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack == 0 ) && ( FDC.StepDirection == -1 ) )
				FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;	/* No delay if trying to go before track 0 */

			else
				FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack += FDC.StepDirection;	/* Move physical head */

			if ( FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack == 0 )
				FDC_Update_STR ( 0 , FDC_STR_BIT_TR00 );	/* Set bit TR00 */
		}

		FDC.CommandState = FDCEMU_RUN_STEP_VERIFY;
		break;
	 case FDCEMU_RUN_STEP_VERIFY:
		if ( FDC.CR & FDC_COMMAND_BIT_VERIFY )
		{
			FDC.CommandState = FDCEMU_RUN_STEP_VERIFY_HEAD_OK;
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_US_HEAD_LOAD );	/* Head settle delay */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_STEP_COMPLETE;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_COMPLETE;
		}
		break;
	 case FDCEMU_RUN_STEP_VERIFY_HEAD_OK:
		FDC.CommandState = FDCEMU_RUN_STEP_VERIFY_LOOP;
		FdcCycles = FDC_TransferByte_FdcCycles ( FDC_NextSectorID_NbBytes () + 10 );	/* Delay to read 3xA1, FE, ID field */
		break;
	 case FDCEMU_RUN_STEP_VERIFY_LOOP:
		FDC_VerifyTrack();
		FDC.CommandState = FDCEMU_RUN_STEP_COMPLETE;
		FdcCycles = FDC_DELAY_CYCLE_COMMAND_COMPLETE;
		break;
	 case FDCEMU_RUN_STEP_COMPLETE:
		FdcCycles = FDC_CmdCompleteCommon( true );
		break;
	}

	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Run 'READ SECTOR/S' command
 */
static int FDC_UpdateReadSectorsCmd ( void )
{
	int	FdcCycles = 0;
	int	FrameCycles, HblCounterVideo, LineCycles;
	int	SectorSize;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );


	/* Which command is running? */
	switch (FDC.CommandState)
	{
	 case FDCEMU_RUN_READSECTORS_READDATA:
		if ( FDC_Set_MotorON ( FDC.CR ) )
		{
			FDC.CommandState = FDCEMU_RUN_READSECTORS_READDATA_SPIN_UP;
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Spin up needed */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_READSECTORS_READDATA_HEAD_LOAD;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;		/* No spin up needed */
		}
		break;
	 case FDCEMU_RUN_READSECTORS_READDATA_SPIN_UP:
		if ( FDC.IndexPulse_Counter < FDC_DELAY_IP_SPIN_UP )
		{
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Wait for the correct number of IP */
			break;
		}
		/* If IndexPulse_Counter reached, we go directly to the _HEAD_LOAD state */
	 case FDCEMU_RUN_READSECTORS_READDATA_HEAD_LOAD:
		if ( FDC.CR & FDC_COMMAND_BIT_HEAD_LOAD )
		{
			FDC.CommandState = FDCEMU_RUN_READSECTORS_READDATA_MOTOR_ON;
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_US_HEAD_LOAD );	/* Head settle delay */
			break;
		}
		/* If there's no head settle, we go directly to the _MOTOR_ON state */
	 case FDCEMU_RUN_READSECTORS_READDATA_MOTOR_ON:
		if ( ( FDC.DriveSelSignal < 0 ) || ( !FDC_DRIVES[ FDC.DriveSelSignal ].Enabled )
			|| ( !FDC_DRIVES[ FDC.DriveSelSignal ].DiskInserted ) )
		{
			FdcCycles = FDC_DELAY_CYCLE_WAIT_NO_DRIVE_FLOPPY;	/* Wait for a valid drive/floppy */
		}
		else
		{
			/* We search the sector FDC.SR during 5 revolutions max */
			FDC.CommandExpire_Time = CyclesGlobalClockCounter + FDC_DelayToCpuCycles ( FDC_DELAY_US_RNF );

			/* Read bytes to reach the next sector's ID field and skip 7 more bytes to reach SR in this ID field */
			FdcCycles = FDC_TransferByte_FdcCycles ( FDC_NextSectorID_NbBytes () + 7 );	/* Add delay to read 3xA1, FE, TR, SIDE, SR */
			FDC.CommandState = FDCEMU_RUN_READSECTORS_READDATA_CHECK_SECTOR_HEADER;
		}
		break;
	 case FDCEMU_RUN_READSECTORS_READDATA_CHECK_SECTOR_HEADER:
		/* If we're looking for sector FDC.SR for more than 5 revolutions, we abort with RNF */
		if ( CyclesGlobalClockCounter > FDC.CommandExpire_Time )
		{
			FDC.CommandState = FDCEMU_RUN_READSECTORS_RNF;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
			break;
		}

		/* Check if the current ID Field is the one we're looking for */
		if ( FDC.NextSector_ID_Field_SR == FDC.SR )
		{
			FDC.CommandState = FDCEMU_RUN_READSECTORS_READDATA_TRANSFER_START;
			/* Read bytes to reach the sector's data : rest of ID field (length+crc) + GAP3a + GAP3b + 3xA1 + FB */
			FdcCycles = FDC_TransferByte_FdcCycles ( 1+2 + FDC_TRACK_LAYOUT_STANDARD_GAP3a + FDC_TRACK_LAYOUT_STANDARD_GAP3b + 3 + 1  );
		}
		else
		{
			/* This is not the ID field we're looking for ; check the next one */
			FdcCycles = FDC_TransferByte_FdcCycles ( FDC_NextSectorID_NbBytes () + 7 );
			FDC.CommandState = FDCEMU_RUN_READSECTORS_READDATA_CHECK_SECTOR_HEADER;
		}
		break;
	 case FDCEMU_RUN_READSECTORS_READDATA_TRANSFER_START:
		/* Read a single sector into temporary buffer (512 bytes for ST/MSA) */
		FDC_DMA_InitTransfer ();				/* Update FDC_DMA.PosInBuffer */
		if ( FDC_ReadSectorFromFloppy ( DMADiskWorkSpace + FDC_DMA.PosInBuffer , FDC.SR , &SectorSize ) )
		{
			FDC_DMA.BytesToTransfer += SectorSize;		/* 512 bytes per sector for ST/MSA disk images */
			FDC_DMA.PosInBuffer += SectorSize;

			FDC.CommandState = FDCEMU_RUN_READSECTORS_READDATA_TRANSFER_LOOP;
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_TRANSFER_DMA_16 );	/* Transfer blocks of 16 bytes from the sector we just read */
		}
		else							/* Sector FDC.SR was not found */
		{
			FDC.CommandState = FDCEMU_RUN_READSECTORS_RNF;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
		}
		break;
	 case FDCEMU_RUN_READSECTORS_READDATA_TRANSFER_LOOP:
		/* Transfer the sector as blocks of 16 bytes using DMA */
		if ( ! FDC_DMA_ReadFromFloppy () )
		{
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_TRANSFER_DMA_16 );	/* Continue transferring blocks of 16 bytes */
		}
		else							/* Sector transferred, check the CRC */
		{
			FDC.CommandState = FDCEMU_RUN_READSECTORS_CRC;
			FdcCycles = FDC_TransferByte_FdcCycles ( 2 );	/* Read 2 bytes for CRC */
		}
		break;
	 case FDCEMU_RUN_READSECTORS_CRC:
		/* Sector completely transferred, CRC is always good for ST/MSA. Check for multi bit */
		if ( FDC.CR & FDC_COMMAND_BIT_MULTIPLE_SECTOR  )
		{
			FDC.SR++;					/* Try to read next sector and set RNF if not possible */
			FDC.CommandState = FDCEMU_RUN_READSECTORS_READDATA;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
		}
		else							/* Multi=0, stop here with no error */
		{
			FDC.CommandState = FDCEMU_RUN_READSECTORS_COMPLETE;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_COMPLETE;
		}
		break;
	 case FDCEMU_RUN_READSECTORS_RNF:
		LOG_TRACE(TRACE_FDC, "fdc type II read sector=%d track=%d drive=%d RNF VBL=%d video_cyc=%d %d@%d pc=%x\n",
			  FDC.SR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_DRIVE , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

		FDC_Update_STR ( 0 , FDC_STR_BIT_RNF );
		FdcCycles = FDC_CmdCompleteCommon( true );
		break;
	 case FDCEMU_RUN_READSECTORS_COMPLETE:
		FdcCycles = FDC_CmdCompleteCommon( true );
		break;
	}

	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Run 'WRITE SECTOR/S' command
 */
static int FDC_UpdateWriteSectorsCmd ( void )
{
	int	FdcCycles = 0;
	int	FrameCycles, HblCounterVideo, LineCycles;
	int	SectorSize;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	if ( Floppy_IsWriteProtected ( FDC_DRIVE ) )
	{
		LOG_TRACE(TRACE_FDC, "fdc type II write sector=%d track=%d drive=%d WPRT VBL=%d video_cyc=%d %d@%d pc=%x\n",
			  FDC.SR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_DRIVE , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

		FDC_Update_STR ( 0 , FDC_STR_BIT_WPRT );		/* Set WPRT bit */
		FdcCycles = FDC_CmdCompleteCommon( true );
	}
	else
		FDC_Update_STR ( FDC_STR_BIT_WPRT , 0 );		/* Unset WPRT bit */

	
	/* Which command is running? */
	switch (FDC.CommandState)
	{
	 case FDCEMU_RUN_WRITESECTORS_WRITEDATA:
		if ( FDC_Set_MotorON ( FDC.CR ) )
		{
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_WRITEDATA_SPIN_UP;
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Spin up needed */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_WRITEDATA_HEAD_LOAD;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;		/* No spin up needed */
		}
		break;
	 case FDCEMU_RUN_WRITESECTORS_WRITEDATA_SPIN_UP:
		if ( FDC.IndexPulse_Counter < FDC_DELAY_IP_SPIN_UP )
		{
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Wait for the correct number of IP */
			break;
		}
		/* If IndexPulse_Counter reached, we go directly to the _HEAD_LOAD state */
	 case FDCEMU_RUN_WRITESECTORS_WRITEDATA_HEAD_LOAD:
		if ( FDC.CR & FDC_COMMAND_BIT_HEAD_LOAD )
		{
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_WRITEDATA_MOTOR_ON;
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_US_HEAD_LOAD );	/* Head settle delay */
			break;
		}
		/* If there's no head settle, we go directly to the _MOTOR_ON state */
	 case FDCEMU_RUN_WRITESECTORS_WRITEDATA_MOTOR_ON:
		if ( ( FDC.DriveSelSignal < 0 ) || ( !FDC_DRIVES[ FDC.DriveSelSignal ].Enabled )
			|| ( !FDC_DRIVES[ FDC.DriveSelSignal ].DiskInserted ) )
		{
			FdcCycles = FDC_DELAY_CYCLE_WAIT_NO_DRIVE_FLOPPY;	/* Wait for a valid drive/floppy */
		}
		else
		{
			/* We search the sector FDC.SR during 5 revolutions max */
			FDC.CommandExpire_Time = CyclesGlobalClockCounter + FDC_DelayToCpuCycles ( FDC_DELAY_US_RNF );

			/* Read bytes to reach the next sector's ID field and skip 7 more bytes to reach SR in this ID field */
			FdcCycles = FDC_TransferByte_FdcCycles ( FDC_NextSectorID_NbBytes () + 7 );	/* Add delay to read 3xA1, FE, TR, SIDE, SR */
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_WRITEDATA_CHECK_SECTOR_HEADER;
		}
		break;
	 case FDCEMU_RUN_WRITESECTORS_WRITEDATA_CHECK_SECTOR_HEADER:
		/* If we're looking for sector FDC.SR for more than 5 revolutions, we abort with RNF */
		if ( CyclesGlobalClockCounter > FDC.CommandExpire_Time )
		{
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_RNF;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
			break;
		}

		/* Check if the current ID Field is the one we're looking for */
		if ( FDC.NextSector_ID_Field_SR == FDC.SR )
		{
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_WRITEDATA_TRANSFER_START;
			/* Read bytes to reach the sector's data : rest of ID field (length+crc) + GAP3a + GAP3b + 3xA1 + FB */
			FdcCycles = FDC_TransferByte_FdcCycles ( 1+2 + FDC_TRACK_LAYOUT_STANDARD_GAP3a + FDC_TRACK_LAYOUT_STANDARD_GAP3b + 3 + 1 );
		}
		else
		{
			/* This is not the ID field we're looking for ; check the next one */
			FdcCycles = FDC_TransferByte_FdcCycles ( FDC_NextSectorID_NbBytes () + 7 );
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_WRITEDATA_CHECK_SECTOR_HEADER;
		}
		break;
	 case FDCEMU_RUN_WRITESECTORS_WRITEDATA_TRANSFER_START:
		/* Write a single sector from RAM (512 bytes for ST/MSA) */
		FDC_DMA_InitTransfer ();				/* Update FDC_DMA.PosInBuffer */
		if ( FDC_WriteSectorToFloppy ( FDC_DMA.SectorCount , FDC.SR , &SectorSize ) )
		{
			FDC_DMA.BytesToTransfer += SectorSize;		/* 512 bytes per sector for ST/MSA disk images */
			FDC_DMA.PosInBuffer += SectorSize;
				
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_WRITEDATA_TRANSFER_LOOP;
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_TRANSFER_DMA_16 );	/* Transfer blocks of 16 bytes from the sector we just wrote */
		}
		else							/* Sector FDC.SR was not found */
		{
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_RNF;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
		}
		break;
	 case FDCEMU_RUN_WRITESECTORS_WRITEDATA_TRANSFER_LOOP:
		/* Transfer the sector as blocks of 16 bytes using DMA */
		if ( ! FDC_DMA_WriteToFloppy () )
		{
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_TRANSFER_DMA_16 );	/* Continue transferring blocks of 16 bytes */
		}
		else							/* Sector transferred, check the CRC */
		{
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_CRC;
			FdcCycles = FDC_TransferByte_FdcCycles ( 2 );	/* Write 2 bytes for CRC */
		}
		break;
	 case FDCEMU_RUN_WRITESECTORS_CRC:
		/* Sector completely transferred, CRC is always good for ST/MSA. Check for multi bit */
		if ( FDC.CR & FDC_COMMAND_BIT_MULTIPLE_SECTOR  )
		{
			FDC.SR++;					/* Try to write next sector and set RNF if not possible */
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_WRITEDATA;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
		}
		else							/* Multi=0, stop here with no error */
		{
			FDC.CommandState = FDCEMU_RUN_WRITESECTORS_COMPLETE;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_COMPLETE;
		}
		break;
	 case FDCEMU_RUN_WRITESECTORS_RNF:
		LOG_TRACE(TRACE_FDC, "fdc type II write sector=%d track=%d drive=%d RNF VBL=%d video_cyc=%d %d@%d pc=%x\n",
			  FDC.SR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_DRIVE , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

		FDC_Update_STR ( 0 , FDC_STR_BIT_RNF );
		FdcCycles = FDC_CmdCompleteCommon( true );
		break;
	 case FDCEMU_RUN_WRITESECTORS_COMPLETE:
		FdcCycles = FDC_CmdCompleteCommon( true );
		break;
	}

	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Run 'READ ADDRESS' command
 */
static int FDC_UpdateReadAddressCmd ( void )
{
	int	FdcCycles = 0;
	Uint16	CRC;
	Uint8	buf[ 4+6 ];
	Uint8	*p;
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	/* Which command is running? */
	switch (FDC.CommandState)
	{
	 case FDCEMU_RUN_READADDRESS:
		if ( FDC_Set_MotorON ( FDC.CR ) )
		{
			FDC.CommandState = FDCEMU_RUN_READADDRESS_SPIN_UP;
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Spin up needed */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_READADDRESS_HEAD_LOAD;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;		/* No spin up needed */
		}
		break;
	 case FDCEMU_RUN_READADDRESS_SPIN_UP:
		if ( FDC.IndexPulse_Counter < FDC_DELAY_IP_SPIN_UP )
		{
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Wait for the correct number of IP */
			break;
		}
		/* If IndexPulse_Counter reached, we go directly to the _HEAD_LOAD state */
	 case FDCEMU_RUN_READADDRESS_HEAD_LOAD:
		if ( FDC.CR & FDC_COMMAND_BIT_HEAD_LOAD )
		{
			FDC.CommandState = FDCEMU_RUN_READADDRESS_MOTOR_ON;
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_US_HEAD_LOAD );	/* Head settle delay */
			break;
		}
		/* If there's no head settle, we go directly to the _MOTOR_ON state */
	 case FDCEMU_RUN_READADDRESS_MOTOR_ON:
		if ( ( FDC.DriveSelSignal < 0 ) || ( !FDC_DRIVES[ FDC.DriveSelSignal ].Enabled )
			|| ( !FDC_DRIVES[ FDC.DriveSelSignal ].DiskInserted ) )
		{
			FdcCycles = FDC_DELAY_CYCLE_WAIT_NO_DRIVE_FLOPPY;	/* Wait for a valid drive/floppy */
		}
		else
		{
			/* Read bytes to reach the next sector's ID field and add 10 more bytes to read this ID field */
			FdcCycles = FDC_TransferByte_FdcCycles ( FDC_NextSectorID_NbBytes () + 10 );	/* Add delay to read 3xA1, FE, ID field */
			FDC.CommandState = FDCEMU_RUN_READADDRESS_DMA;
		}
		break;
	 case FDCEMU_RUN_READADDRESS_DMA:
		/* In the case of Hatari, only ST/MSA images are supported, so we build */
		/* a standard ID field with a valid CRC based on current track/sector/side */
		p = buf;
		*p++ = 0xa1;					/* SYNC bytes and IAM byte are included in the CRC */
		*p++ = 0xa1;
		*p++ = 0xa1;
		*p++ = 0xfe;
		*p++ = FDC_DRIVES[ FDC_DRIVE ].HeadTrack;
		FDC.SR = FDC_DRIVES[ FDC_DRIVE ].HeadTrack;	/* The 1st byte of the ID field is also copied into Sector Register */
		*p++ = FDC_SIDE;
		*p++ = FDC.NextSector_ID_Field_SR;
		*p++ = FDC_SECTOR_SIZE_512;			/* ST/MSA images are 512 bytes per sector */

		FDC_CRC16 ( buf , 8 , &CRC );

		*p++ = CRC >> 8;
		*p++ = CRC & 0xff;

		LOG_TRACE(TRACE_FDC, "fdc read address 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x VBL=%d video_cyc=%d %d@%d pc=%x\n",
			buf[4] , buf[5] , buf[6] , buf[7] , buf[8] , buf[9] ,
			nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

		FDC_DMA_InitTransfer ();			/* Update FDC_DMA.PosInBuffer */
		memcpy ( DMADiskWorkSpace + FDC_DMA.PosInBuffer , buf + 4 , 6 );	/* Don't return the 3 x $A1 and $FE in the Address Field */
		FDC_DMA.BytesToTransfer += 6;			/* 6 bytes per ID field */
		FDC_DMA.PosInBuffer += 6;

		FDC_DMA_ReadFromFloppy ();			/* Transfer bytes if 16 bytes or more are in the DMA buffer */

		FDC.CommandState = FDCEMU_RUN_READADDRESS_COMPLETE;
		FdcCycles = FDC_DELAY_CYCLE_COMMAND_COMPLETE;
		break;
	 case FDCEMU_RUN_READADDRESS_COMPLETE:
		FdcCycles = FDC_CmdCompleteCommon( true );
		break;
	}

	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Run 'READ TRACK' command
 */
static int FDC_UpdateReadTrackCmd ( void )
{
	int	FdcCycles = 0;
	Uint16	CRC;
	Uint8	*buf;
	Uint8	*buf_crc;
	int	Sector;
	int	SectorSize;
	int	i;

	/* Which command is running? */
	switch (FDC.CommandState)
	{
	 case FDCEMU_RUN_READTRACK:
		if ( FDC_Set_MotorON ( FDC.CR ) )
		{
			FDC.CommandState = FDCEMU_RUN_READTRACK_SPIN_UP;
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Spin up needed */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_READTRACK_HEAD_LOAD;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;		/* No spin up needed */
		}
		break;
	 case FDCEMU_RUN_READTRACK_SPIN_UP:
		if ( FDC.IndexPulse_Counter < FDC_DELAY_IP_SPIN_UP )
		{
			FdcCycles = FDC_DELAY_CYCLE_REFRESH_INDEX_PULSE;	/* Wait for the correct number of IP */
			break;
		}
		/* If IndexPulse_Counter reached, we go directly to the _HEAD_LOAD state */
	 case FDCEMU_RUN_READTRACK_HEAD_LOAD:
		if ( FDC.CR & FDC_COMMAND_BIT_HEAD_LOAD )
		{
			FDC.CommandState = FDCEMU_RUN_READTRACK_MOTOR_ON;
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_US_HEAD_LOAD );	/* Head settle delay */
			break;
		}
		/* If there's no head settle, we go directly to the _MOTOR_ON state */
	 case FDCEMU_RUN_READTRACK_MOTOR_ON:
		if ( ( FDC.DriveSelSignal < 0 ) || ( !FDC_DRIVES[ FDC.DriveSelSignal ].Enabled )
			|| ( !FDC_DRIVES[ FDC.DriveSelSignal ].DiskInserted ) )
		{
			FdcCycles = FDC_DELAY_CYCLE_WAIT_NO_DRIVE_FLOPPY;	/* Wait for a valid drive/floppy */
		}
		else
		{
			FDC.CommandState = FDCEMU_RUN_READTRACK_INDEX;
#ifdef old_index
			FdcCycles = FDC_TransferByte_FdcCycles ( FDC_NextIndexPulse_NbBytes () );	/* Wait for the next index pulse */
#else
			FdcCycles = FDC_NextIndexPulse_FdcCycles ();		/* Wait for the next index pulse */
#endif
		}
		break;
	 case FDCEMU_RUN_READTRACK_INDEX:
		FDC.IndexPulse_Time = CyclesGlobalClockCounter;			/* We're on an index pulse */

		/* Build the track data */
		FDC_DMA_InitTransfer ();					/* Update FDC_DMA.PosInBuffer */
		buf = DMADiskWorkSpace + FDC_DMA.PosInBuffer;

		if ( ( FDC_SIDE == 1 )						/* Try to read side 1 on a disk that doesn't have 2 sides */
			&& ( FDC_GetSidesPerDisk ( FDC_DRIVES[ FDC_DRIVE ].HeadTrack ) != 2 ) )
		{
			for ( i=0 ; i<FDC_GetBytesPerTrack () ; i++ )
				*buf++ = rand() & 0xff;				/* Fill the track buffer with random bytes */
		}
		
		else								/* Track/side available in the disk image */
		{
			for ( i=0 ; i<FDC_TRACK_LAYOUT_STANDARD_GAP1 ; i++ )		/* GAP1 */
				*buf++ = 0x4e;

			for ( Sector=1 ; Sector <= FDC_GetSectorsPerTrack ( FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_SIDE ) ; Sector++ )
			{
				for ( i=0 ; i<FDC_TRACK_LAYOUT_STANDARD_GAP2 ; i++ )	/* GAP2 */
					*buf++ = 0x00;

				buf_crc = buf;
				for ( i=0 ; i<3 ; i++ )		*buf++ = 0xa1;		/* SYNC (write $F5) */
				*buf++ = 0xfe;						/* Index Address Mark */
				*buf++ = FDC_DRIVES[ FDC_DRIVE ].HeadTrack;		/* Track */
				*buf++ = FDC_SIDE;					/* Side */
				*buf++ = Sector;					/* Sector */
				*buf++ = FDC_SECTOR_SIZE_512;				/* 512 bytes/sector for ST/MSA */
				FDC_CRC16 ( buf_crc , buf - buf_crc , &CRC );
				*buf++ = CRC >> 8;					/* CRC1 (write $F7) */
				*buf++ = CRC & 0xff;					/* CRC2 */

				for ( i=0 ; i<FDC_TRACK_LAYOUT_STANDARD_GAP3a ; i++ )	/* GAP3a */
					*buf++ = 0x4e;
				for ( i=0 ; i<FDC_TRACK_LAYOUT_STANDARD_GAP3b ; i++ )	/* GAP3b */
					*buf++ = 0x00;

				buf_crc = buf;
				for ( i=0 ; i<3 ; i++ )		*buf++ = 0xa1;		/* SYNC (write $F5) */
				*buf++ = 0xfb;						/* Data Address Mark */

				if ( ! FDC_ReadSectorFromFloppy ( buf , Sector , &SectorSize ) )	/* Read a single 512 bytes sector into temporary buffer */
				{
					/* Do nothing in case of error, we could put some random bytes, but this case should */
					/* not happen with ST/MSA disk images, all sectors should be present on each track. */
				}
				buf += SectorSize;

				FDC_CRC16 ( buf_crc , buf - buf_crc , &CRC );
				*buf++ = CRC >> 8;					/* CRC1 (write $F7) */
				*buf++ = CRC & 0xff;					/* CRC2 */

				for ( i=0 ; i<FDC_TRACK_LAYOUT_STANDARD_GAP4 ; i++ )	/* GAP4 */
					*buf++ = 0x4e;
			}

			while ( buf < DMADiskWorkSpace + FDC_DMA.PosInBuffer + FDC_GetBytesPerTrack () )	/* Complete the track buffer */
			      *buf++ = 0x4e;						/* GAP5 */
		}

		/* Transfer Track data to RAM using DMA */
		FDC_DMA.BytesToTransfer += FDC_GetBytesPerTrack ();
		FDC_DMA.PosInBuffer += FDC_GetBytesPerTrack ();

		FDC.CommandState = FDCEMU_RUN_READTRACK_DMA;
		FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_TRANSFER_DMA_16 );			/* Transfer blocks of 16 bytes from the track we just read */
		break;
	 case FDCEMU_RUN_READTRACK_DMA:
		if ( ! FDC_DMA_ReadFromFloppy () )
		{
			FdcCycles = FDC_DelayToFdcCycles ( FDC_DELAY_TRANSFER_DMA_16 );		/* Continue transferring blocks of 16 bytes */
		}
		else								/* Track completely transferred */
		{
			FDC.CommandState = FDCEMU_RUN_READTRACK_COMPLETE;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_COMPLETE;
		}
		break;
	 case FDCEMU_RUN_READTRACK_COMPLETE:
		FdcCycles = FDC_CmdCompleteCommon( true );
		break;
	}

	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Run 'FORCE INT ON INDEX' command
 */
static int FDC_UpdateForceIntCmd ( void )
{
	int	FdcCycles = 0;
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	/* Which command is running? */
	switch (FDC.CommandState)
	{
	 case FDCEMU_RUN_FORCEINT:
		if ( ( FDC.STR & FDC_STR_BIT_MOTOR_ON ) == 0 )		/* Motor is already OFF, we won't get any index */
		{
			FDC.CommandState = FDCEMU_RUN_FORCEINT_COMPLETE;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
			break;
		}

		/* At the same time as we check for indexes, we must also stop the motor when necessary */
		FDC.CommandExpire_Time = CyclesGlobalClockCounter + FDC_DelayToCpuCycles ( FDC_DELAY_US_MOTOR_OFF );

		FDC.CommandState = FDCEMU_RUN_FORCEINT_LOOP;
		FdcCycles = FDC_NextIndexPulse_FdcCycles ();		/* Wait for the next index pulse */

		LOG_TRACE(TRACE_FDC, "fdc type IV force int on index, first index in %d cycles VBL=%d video_cyc=%d %d@%d pc=%x\n",
			FdcCycles , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
		break;
	 case FDCEMU_RUN_FORCEINT_LOOP:
		/* If we reach the time where motor turns off, disk won't spin anymore */
		/* and we won't get more indexes either */
		if ( CyclesGlobalClockCounter > FDC.CommandExpire_Time )
		{
			FDC.CommandState = FDCEMU_RUN_FORCEINT_COMPLETE;
			FdcCycles = FDC_DELAY_CYCLE_COMMAND_IMMEDIATE;
			break;
		}

		FDC.IndexPulse_Time = CyclesGlobalClockCounter;		/* We're on an index pulse */
		FDC_AcknowledgeInterrupt();

		FdcCycles = FDC_NextIndexPulse_FdcCycles ();		/* Wait for the next index pulse */

		LOG_TRACE(TRACE_FDC, "fdc type IV force int on index, next index in %d cycles VBL=%d video_cyc=%d %d@%d pc=%x\n",
			FdcCycles , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
		break;
	 case FDCEMU_RUN_FORCEINT_COMPLETE:
		/* We reach this state only when the motor stops, because we won't get index pulse anymore */
		/* The WD1772 doesn't really stop the Force Int command in that case, but as the FDC's state */
		/* won't change anymore we go to the FDCEMU_CMD_NULL state to save cpu in the case of the emulation */
		FdcCycles = FDC_UpdateMotorStop_old ();
		break;
	}

	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Common to types I, II and III
 *
 * Start motor / spin up sequence if needed
 * Return true if spin up sequence is needed, else false
 */

static bool FDC_Set_MotorON ( Uint8 FDC_CR )
{
	int	FrameCycles, HblCounterVideo, LineCycles;
	bool	SpinUp;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	if ( ( ( FDC_CR & FDC_COMMAND_BIT_SPIN_UP ) == 0 )		/* Command wants motor's spin up */
	  && ( ( FDC.STR & FDC_STR_BIT_MOTOR_ON ) == 0 ) )		/* Motor on not enabled yet */
	{
		LOG_TRACE(TRACE_FDC, "fdc start motor with spinup VBL=%d video_cyc=%d %d@%d pc=%x\n",
			nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

		FDC_Update_STR ( FDC_STR_BIT_SPIN_UP , 0 );		/* Unset spin up bit */
		FDC.UpdateIndexPulse = true;
		FDC.IndexPulse_Counter = 0;				/* Reset counter to measure the spin up sequence */
		SpinUp = true;
	}
	else								/* No spin up : don't add delay to start the motor */
	{
		LOG_TRACE(TRACE_FDC, "fdc start motor without spinup VBL=%d video_cyc=%d %d@%d pc=%x\n",
			nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

		SpinUp = false;
	}

	FDC_Update_STR ( 0 , FDC_STR_BIT_MOTOR_ON );			/* Start motor */

	if ( ( FDC.DriveSelSignal < 0 ) || ( !FDC_DRIVES[ FDC.DriveSelSignal ].Enabled )
		|| ( !FDC_DRIVES[ FDC.DriveSelSignal ].DiskInserted ) )
	{
		LOG_TRACE(TRACE_FDC, "fdc start motor : no disk/drive VBL=%d video_cyc=%d %d@%d pc=%x\n",
			nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
	}
	else if ( FDC_DRIVES[ FDC.DriveSelSignal ].IndexPulse_Time == 0 )
		FDC_IndexPulse_Init ( FDC.DriveSelSignal );		/* Index Pulse's position is random when motor starts */
	
	return SpinUp;
}


static int FDC_Check_MotorON ( Uint8 FDC_CR )
{
	int	FrameCycles, HblCounterVideo, LineCycles;

return 0;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	if ( ( ( FDC_CR & FDC_COMMAND_BIT_SPIN_UP ) == 0 )			/* Command wants motor on / spin up */
	  && ( ( FDC.STR & FDC_STR_BIT_MOTOR_ON ) == 0 ) )			/* Motor on not enabled yet */
	{
		LOG_TRACE(TRACE_FDC, "fdc start motor VBL=%d video_cyc=%d %d@%d pc=%x\n",
			nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
		FDC_Update_STR ( FDC_STR_BIT_SPIN_UP , FDC_STR_BIT_MOTOR_ON );	/* Unset spin up bit and set motor bit */
		FDC.UpdateIndexPulse = true;
		return FDC_DelayToFdcCycles ( FDC_DELAY_US_MOTOR_ON );		/* Motor's delay */
	}

	/* Other cases : set bit in STR and don't add delay */
	LOG_TRACE(TRACE_FDC, "fdc motor already on VBL=%d video_cyc=%d %d@%d pc=%x\n",
		nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
	FDC_Update_STR ( 0 , FDC_STR_BIT_MOTOR_ON );
	return 0;
}


/*-----------------------------------------------------------------------*/
/**
 * Type I Commands
 *
 * Restore, Seek, Step, Step-In and Step-Out
 */


/*-----------------------------------------------------------------------*/
static int FDC_TypeI_Restore(void)
{
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type I restore spinup=%s verify=%s steprate=%d drive=%d tr=0x%x head_track=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  ( FDC.CR & FDC_COMMAND_BIT_SPIN_UP ) ? "off" : "on" ,
		  ( FDC.CR & FDC_COMMAND_BIT_VERIFY ) ? "on" : "off" ,
		  FDC_StepRate_ms[ FDC_STEP_RATE ] ,
		  FDC_DRIVE , FDC.TR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* Set emulation to seek to track zero */
	FDC.Command = FDCEMU_CMD_RESTORE;
	FDC.CommandState = FDCEMU_RUN_RESTORE_SEEKTOTRACKZERO;

	FDC_Update_STR ( FDC_STR_BIT_INDEX | FDC_STR_BIT_CRC_ERROR | FDC_STR_BIT_RNF , FDC_STR_BIT_BUSY );

	return FDC_DELAY_CYCLE_TYPE_I_PREPARE;
}


/*-----------------------------------------------------------------------*/
static int FDC_TypeI_Seek ( void )
{
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type I seek dest_track=0x%x spinup=%s verify=%s steprate=%d drive=%d tr=0x%x head_track=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  FDC.DR,
		  ( FDC.CR & FDC_COMMAND_BIT_SPIN_UP ) ? "off" : "on" ,
		  ( FDC.CR & FDC_COMMAND_BIT_VERIFY ) ? "on" : "off" ,
		  FDC_StepRate_ms[ FDC_STEP_RATE ] ,
		  FDC_DRIVE , FDC.TR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* Set emulation to seek to chosen track */
	FDC.Command = FDCEMU_CMD_SEEK;
	FDC.CommandState = FDCEMU_RUN_SEEK_TOTRACK;

	FDC_Update_STR ( FDC_STR_BIT_INDEX | FDC_STR_BIT_CRC_ERROR | FDC_STR_BIT_RNF , FDC_STR_BIT_BUSY );

	return FDC_DELAY_CYCLE_TYPE_I_PREPARE;
}


/*-----------------------------------------------------------------------*/
static int FDC_TypeI_Step ( void )
{
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type I step %d spinup=%s verify=%s steprate=%d drive=%d tr=0x%x head_track=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  FDC.StepDirection,
		  ( FDC.CR & FDC_COMMAND_BIT_SPIN_UP ) ? "off" : "on" ,
		  ( FDC.CR & FDC_COMMAND_BIT_VERIFY ) ? "on" : "off" ,
		  FDC_StepRate_ms[ FDC_STEP_RATE ] ,
		  FDC_DRIVE , FDC.TR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* Set emulation to step (using same direction as latest seek executed, ie 'FDC.StepDirection') */
	FDC.Command = FDCEMU_CMD_STEP;
	FDC.CommandState = FDCEMU_RUN_STEP_ONCE;

	FDC_Update_STR ( FDC_STR_BIT_INDEX | FDC_STR_BIT_CRC_ERROR | FDC_STR_BIT_RNF , FDC_STR_BIT_BUSY );

	return FDC_DELAY_CYCLE_TYPE_I_PREPARE;
}


/*-----------------------------------------------------------------------*/
static int FDC_TypeI_StepIn(void)
{
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type I step in spinup=%s verify=%s steprate=%d drive=%d tr=0x%x head_track=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  ( FDC.CR & FDC_COMMAND_BIT_SPIN_UP ) ? "off" : "on" ,
		  ( FDC.CR & FDC_COMMAND_BIT_VERIFY ) ? "on" : "off" ,
		  FDC_StepRate_ms[ FDC_STEP_RATE ] ,
		  FDC_DRIVE , FDC.TR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* Set emulation to step in (direction = +1) */
	FDC.Command = FDCEMU_CMD_STEP;
	FDC.CommandState = FDCEMU_RUN_STEP_ONCE;
	FDC.StepDirection = 1;						/* Increment track*/

	FDC_Update_STR ( FDC_STR_BIT_INDEX | FDC_STR_BIT_CRC_ERROR | FDC_STR_BIT_RNF , FDC_STR_BIT_BUSY );

	return FDC_DELAY_CYCLE_TYPE_I_PREPARE;
}


/*-----------------------------------------------------------------------*/
static int FDC_TypeI_StepOut ( void )
{
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type I step out spinup=%s verify=%s steprate=%d drive=%d tr=0x%x head_track=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  ( FDC.CR & FDC_COMMAND_BIT_SPIN_UP ) ? "off" : "on" ,
		  ( FDC.CR & FDC_COMMAND_BIT_VERIFY ) ? "on" : "off" ,
		  FDC_StepRate_ms[ FDC_STEP_RATE ] ,
		  FDC_DRIVE , FDC.TR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* Set emulation to step out (direction = -1) */
	FDC.Command = FDCEMU_CMD_STEP;
	FDC.CommandState = FDCEMU_RUN_STEP_ONCE;
	FDC.StepDirection = -1;						/* Decrement track */

	FDC_Update_STR ( FDC_STR_BIT_INDEX | FDC_STR_BIT_CRC_ERROR | FDC_STR_BIT_RNF , FDC_STR_BIT_BUSY );

	return FDC_DELAY_CYCLE_TYPE_I_PREPARE;
}


/*-----------------------------------------------------------------------*/
/**
 * Type II Commands
 *
 * Read Sector, Write Sector
 */


/*-----------------------------------------------------------------------*/
static int FDC_TypeII_ReadSector ( void )
{
	int	FdcCycles = 0;
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type II read sector sector=0x%x multi=%s spinup=%s settle=%s tr=0x%x head_track=0x%x side=%d drive=%d dmasector=%d addr=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  FDC.SR, ( FDC.CR & FDC_COMMAND_BIT_MULTIPLE_SECTOR ) ? "on" : "off" ,
		  ( FDC.CR & FDC_COMMAND_BIT_SPIN_UP ) ? "off" : "on" ,
		  ( FDC.CR & FDC_COMMAND_BIT_HEAD_LOAD ) ? "on" : "off" ,
		  FDC.TR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_SIDE, FDC_DRIVE , FDC_DMA.SectorCount ,
		  FDC_GetDMAAddress(), nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* Set emulation to read sector(s) */
	FDC.Command = FDCEMU_CMD_READSECTORS;
	FDC.CommandState = FDCEMU_RUN_READSECTORS_READDATA;

	FDC_Update_STR ( FDC_STR_BIT_DRQ | FDC_STR_BIT_LOST_DATA | FDC_STR_BIT_CRC_ERROR
		| FDC_STR_BIT_RNF | FDC_STR_BIT_RECORD_TYPE | FDC_STR_BIT_WPRT , FDC_STR_BIT_BUSY );

	return FDC_DELAY_CYCLE_TYPE_II_PREPARE + FdcCycles;
}


/*-----------------------------------------------------------------------*/
static int FDC_TypeII_WriteSector ( void )
{
	int	FdcCycles = 0;
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type II write sector sector=0x%x multi=%s spinup=%s settle=%s tr=0x%x head_track=0x%x side=%d drive=%d dmasector=%d addr=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  FDC.SR, ( FDC.CR & FDC_COMMAND_BIT_MULTIPLE_SECTOR ) ? "on" : "off" ,
		  ( FDC.CR & FDC_COMMAND_BIT_SPIN_UP ) ? "off" : "on" ,
		  ( FDC.CR & FDC_COMMAND_BIT_HEAD_LOAD ) ? "on" : "off" ,
		  FDC.TR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_SIDE, FDC_DRIVE , FDC_DMA.SectorCount,
		  FDC_GetDMAAddress(), nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* Set emulation to write a sector(s) */
	FDC.Command = FDCEMU_CMD_WRITESECTORS;
	FDC.CommandState = FDCEMU_RUN_WRITESECTORS_WRITEDATA;

	FDC_Update_STR ( FDC_STR_BIT_DRQ | FDC_STR_BIT_LOST_DATA | FDC_STR_BIT_CRC_ERROR
		| FDC_STR_BIT_RNF | FDC_STR_BIT_RECORD_TYPE , FDC_STR_BIT_BUSY );

	return FDC_DELAY_CYCLE_TYPE_II_PREPARE + FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Type III Commands
 *
 * Read Address, Read Track, Write Track
 */


/*-----------------------------------------------------------------------*/
static int FDC_TypeIII_ReadAddress ( void )
{
	int	FdcCycles = 0;
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type III read address spinup=%s settle=%s tr=0x%x head_track=0x%x side=%d drive=%d addr=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  ( FDC.CR & FDC_COMMAND_BIT_SPIN_UP ) ? "off" : "on" ,
		  ( FDC.CR & FDC_COMMAND_BIT_HEAD_LOAD ) ? "on" : "off" ,
		  FDC.TR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_SIDE, FDC_DRIVE , FDC_GetDMAAddress(),
		  nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* Set emulation to seek to track zero */
	FDC.Command = FDCEMU_CMD_READADDRESS;
	FDC.CommandState = FDCEMU_RUN_READADDRESS;

	FDC_Update_STR ( FDC_STR_BIT_DRQ | FDC_STR_BIT_LOST_DATA | FDC_STR_BIT_CRC_ERROR
		| FDC_STR_BIT_RNF | FDC_STR_BIT_RECORD_TYPE | FDC_STR_BIT_WPRT , FDC_STR_BIT_BUSY );

	return FDC_DELAY_CYCLE_TYPE_III_PREPARE + FdcCycles;
}


/*-----------------------------------------------------------------------*/
static int FDC_TypeIII_ReadTrack ( void )
{
	int	FdcCycles = 0;
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type III read track spinup=%s settle=%s tr=0x%x head_track=0x%x side=%d drive=%d addr=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  ( FDC.CR & FDC_COMMAND_BIT_SPIN_UP ) ? "off" : "on" ,
		  ( FDC.CR & FDC_COMMAND_BIT_HEAD_LOAD ) ? "on" : "off" ,
		  FDC.TR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_SIDE, FDC_DRIVE , FDC_GetDMAAddress(),
		  nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* Set emulation to read a single track */
	FDC.Command = FDCEMU_CMD_READTRACK;
	FDC.CommandState = FDCEMU_RUN_READTRACK;

	FDC_Update_STR ( FDC_STR_BIT_DRQ | FDC_STR_BIT_LOST_DATA | FDC_STR_BIT_CRC_ERROR
		| FDC_STR_BIT_RNF | FDC_STR_BIT_RECORD_TYPE | FDC_STR_BIT_WPRT , FDC_STR_BIT_BUSY );

	return FDC_DELAY_CYCLE_TYPE_III_PREPARE + FdcCycles;
}


/*-----------------------------------------------------------------------*/
static int FDC_TypeIII_WriteTrack ( void )
{
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type III write track spinup=%s settle=%s tr=0x%x head_track=0x%x side=%d drive=%d addr=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  ( FDC.CR & FDC_COMMAND_BIT_SPIN_UP ) ? "off" : "on" ,
		  ( FDC.CR & FDC_COMMAND_BIT_HEAD_LOAD ) ? "on" : "off" ,
		  FDC.TR , FDC_DRIVES[ FDC_DRIVE ].HeadTrack , FDC_SIDE, FDC_DRIVE , FDC_GetDMAAddress(),
		  nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	Log_Printf(LOG_TODO, "FDC type III command 'write track' does not work yet!\n");

	/* FIXME: "Write track" should write all the sectors after extracting them from the track data */

	/* Set emulation to write a single track */
	FDC_Update_STR ( 0 , FDC_STR_BIT_RNF );				/* FIXME : Not supported yet, set RNF bit */
	FDC.Command = FDCEMU_CMD_NULL;
	FDC.CommandState = FDCEMU_RUN_NULL;

	return FDC_DELAY_CYCLE_TYPE_III_PREPARE;
}


/*-----------------------------------------------------------------------*/
/**
 * Type IV Commands
 *
 * Force Interrupt
 */


/*-----------------------------------------------------------------------*/
static int FDC_TypeIV_ForceInterrupt ( bool bCauseCPUInterrupt )
{
	int	FdcCycles;
	int	FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc type IV force int 0x%x irq=%d index=%d VBL=%d video_cyc=%d %d@%dpc=%x\n",
		  FDC.CR , ( FDC.CR & 0x8 ) >> 3 , ( FDC.CR & 0x4 ) >> 2 ,
		  nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

#ifdef old_str
	/* For Type II/III commands, LOST DATA bit is never set (DRQ is always handled by the DMA) */
	/* (eg Super Monaco GP on Superior 65 : loader fails if LOST DATA is set when there're not enough DMA sectors to transfer bytes) */
	FDC_Update_STR ( FDC_STR_BIT_LOST_DATA , 0 );			/* Remove LOST DATA / TR00 bit */

	/* TR00 is updated when a type I command is interrupted or when no command was running */
	/* MOTOR ON is also set when a type I command is interrupted or when no command was running */
	/* (eg Knightmare on DBUG 24 : loader fails if motor is off because of the added delay to start it) */
	if ( ( ( FDC.STR & FDC_STR_BIT_BUSY ) == 0 )			/* No command running */
	  || ( FDC.CommandType == 1 ) )					/* Or busy command is Type I */
	{
		if ( FDC_DRIVES[ FDC_DRIVE ].HeadTrack == 0 )
			FDC_Update_STR ( 0 , FDC_STR_BIT_TR00 );	/* Set bit TR00 */

		FDC_Update_STR ( 0 , FDC_STR_BIT_MOTOR_ON );		/* Set Motor ON */

		if ( FDC_IndexPulse_GetState () )
			FDC_Update_STR ( 0 , FDC_STR_BIT_INDEX );	/* Set INDEX bit */
		else
			FDC_Update_STR ( FDC_STR_BIT_INDEX , 0 );	/* Unset INDEX bit */
	}
#else
	/* If a command was running, just remove busy bit and keep the current content of Status reg */
	/* If FDC was idle, the content of Status reg is forced to type I */
	if ( ( FDC.STR & FDC_STR_BIT_BUSY ) == 0 )			
		FDC.StatusTypeI = true;
#endif

	if ( ( FDC.CR & 0x4 ) == 0 )					/* Force Int immediate */
	{
		/* Remove busy bit, ack int and stop the motor */
		FdcCycles = FDC_CmdCompleteCommon( bCauseCPUInterrupt );
	}
	else								/* Force Int on index pulse */
	{
		FDC_Update_STR ( FDC_STR_BIT_BUSY , 0 );		/* Remove busy bit */
		FdcCycles = 0;

		FDC.Command = FDCEMU_CMD_FORCEINT;
		FDC.CommandState = FDCEMU_RUN_FORCEINT;
	}

	return FDC_DELAY_CYCLE_TYPE_IV_PREPARE + FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Execute Type I commands
 */
static int FDC_ExecuteTypeICommands ( void )
{
	int	FdcCycles = 0;

	FDC.CommandType = 1;
	FDC.StatusTypeI = true;
	MFP_GPIP |= 0x20;

	/* Check Type I Command */
	switch ( FDC.CR & 0xf0 )
	{
	 case 0x00:             /* Restore */
		FdcCycles = FDC_TypeI_Restore();
		break;
	 case 0x10:             /* Seek */
		FdcCycles = FDC_TypeI_Seek();
		break;
	 case 0x20:             /* Step */
	 case 0x30:
		FdcCycles = FDC_TypeI_Step();
		break;
	 case 0x40:             /* Step-In */
	 case 0x50:
		FdcCycles = FDC_TypeI_StepIn();
		break;
	 case 0x60:             /* Step-Out */
	 case 0x70:
		FdcCycles = FDC_TypeI_StepOut();
		break;
	}

	/* Check if motor needs to be started and add possible delay */
	FdcCycles += FDC_Check_MotorON ( FDC.CR );
	
	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Execute Type II commands
 */
static int FDC_ExecuteTypeIICommands ( void )
{
	int	FdcCycles = 0;

	FDC.CommandType = 2;
	FDC.StatusTypeI = false;
	MFP_GPIP |= 0x20;

	/* Check Type II Command */
	switch ( FDC.CR & 0xf0 )
	{
	 case 0x80:             /* Read Sector multi=0*/
	 case 0x90:             /* Read Sectors multi=1 */
		FdcCycles = FDC_TypeII_ReadSector();
		break;
	 case 0xa0:             /* Write Sector multi=0 */
	 case 0xb0:             /* Write Sectors multi=1 */
		FdcCycles = FDC_TypeII_WriteSector();
		break;
	}

	/* Check if motor needs to be started and add possible delay */
	FdcCycles += FDC_Check_MotorON ( FDC.CR );

	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Execute Type III commands
 */
static int FDC_ExecuteTypeIIICommands ( void )
{
	int	FdcCycles = 0;

	FDC.CommandType = 3;
	FDC.StatusTypeI = false;
	MFP_GPIP |= 0x20;

	/* Check Type III Command */
	switch ( FDC.CR & 0xf0 )
	{
	 case 0xc0:             /* Read Address */
		FdcCycles = FDC_TypeIII_ReadAddress();
		break;
	 case 0xe0:             /* Read Track */
		FdcCycles = FDC_TypeIII_ReadTrack();
		break;
	 case 0xf0:             /* Write Track */
		FdcCycles = FDC_TypeIII_WriteTrack();
		break;
	}

	/* Check if motor need to be started and add possible delay */
	FdcCycles += FDC_Check_MotorON ( FDC.CR );

	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Execute Type IV commands
 */
static int FDC_ExecuteTypeIVCommands ( void )
{
	int	FdcCycles;

	/* Check Type IV command */
	/* Most of the time a 0xD8 command is followed by a 0xD0 command to clear the IRQ signal */
	if ( FDC.CR & 0x8 )						/* I3 set (0xD8) : immediate interrupt with IRQ */
		FdcCycles = FDC_TypeIV_ForceInterrupt ( true );

	else if ( FDC.CR & 0x4 )					/* I2 set (0xD4) : IRQ on next index pulse */
	{
		FdcCycles = FDC_TypeIV_ForceInterrupt ( false );
	}

	else								/* I3-I2 clear (0xD0) : stop command without IRQ */
	{
		MFP_GPIP |= 0x20;					/* reset IRQ signal */
		FdcCycles = FDC_TypeIV_ForceInterrupt ( false );
	}
		
	FDC.CommandType = 4;						/* Change CommandType after interrupting the current command */
	return FdcCycles;
}


/*-----------------------------------------------------------------------*/
/**
 * Find FDC command type and execute
 */
static void FDC_ExecuteCommand ( void )
{
	int	FdcCycles;

	/* Check type of command and execute */
	if ( ( FDC.CR & 0x80 ) == 0 )					/* Type I - Restore, Seek, Step, Step-In, Step-Out */
		FdcCycles = FDC_ExecuteTypeICommands();
	else if ( ( FDC.CR & 0x40 ) == 0 )				/* Type II - Read Sector, Write Sector */
		FdcCycles = FDC_ExecuteTypeIICommands();
	else if ( ( FDC.CR & 0xf0 ) != 0xd0 )				/* Type III - Read Address, Read Track, Write Track */
		FdcCycles = FDC_ExecuteTypeIIICommands();
	else								/* Type IV - Force Interrupt */
		FdcCycles = FDC_ExecuteTypeIVCommands();

	FDC.ReplaceCommandPossible = true;				/* This new command can be replaced during the Delay_micro phase */
	FDC_StartTimer_FdcCycles ( FdcCycles , 0 );
}


/*-----------------------------------------------------------------------*/
/**
 * Write to SectorCount register $ff8604
 */
static void FDC_WriteSectorCountRegister ( void )
{
	int FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc write 8604 dma sector count=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  IoMem_ReadByte(0xff8605), nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	FDC_DMA.SectorCount = IoMem_ReadByte(0xff8605);
}


/*-----------------------------------------------------------------------*/
/**
 * Write to Command register $ff8604
 */
static void FDC_WriteCommandRegister ( void )
{
	int FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc write 8604 command=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
		  IoMem_ReadByte(0xff8605), nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());

	/* If fdc is busy, only 'Force Interrupt' is possible */
	/* [NP] : it's also possible to start a new command just after another command */
	/* was started and spinup phase was not completed yet (or is this only possible during the 'prepare' delay ?) */
	/* FIXME : this delay was not measured, it should be at least 880 cycles for Overdrive Demos by Phalanx */
	/* For now, we allow to cancel the current command if we're in the prepare+spinup delay */
	if ( FDC.STR & FDC_STR_BIT_BUSY )
	{
		if ( ( IoMem_ReadByte(0xff8605) & 0xf0 ) == 0xd0 )		/* 'Force Interrupt' command */
		{
			LOG_TRACE(TRACE_FDC, "fdc write 8604 while fdc busy, current command=0x%x interrupted by command=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
				FDC.CR , IoMem_ReadByte(0xff8605), nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
		}

		else if ( FDC.ReplaceCommandPossible )
		{
			LOG_TRACE(TRACE_FDC, "fdc write 8604 while fdc busy in prepare+spinup, current command=0x%x replaced by command=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n",
				FDC.CR , IoMem_ReadByte(0xff8605), nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
		}

		else								/* Other cases : new command is ignored */
		{
			LOG_TRACE(TRACE_FDC, "fdc write 8604 fdc busy, command=0x%x ignored VBL=%d video_cyc=%d %d@%d pc=%x\n",
				IoMem_ReadByte(0xff8605), nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
			return;
		}
	}


	if ( ( ( IoMem_ReadByte(0xff8605) & 0xf0 ) != 0xd0 )			/* Type I, II and III commands */
	  && ( !FDC_ValidFloppyDrive() ) )
	{
		LOG_TRACE(TRACE_FDC, "fdc write 8604 no drive selected, command=0x%x ignored VBL=%d video_cyc=%d %d@%d pc=%x\n",
			IoMem_ReadByte(0xff8605), nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
		return;
	}

	FDC.CR = IoMem_ReadByte(0xff8605);
	FDC_ExecuteCommand();
}


/*-----------------------------------------------------------------------*/
/**
 * Write to Track register $ff8604
 */
static void FDC_WriteTrackRegister ( void )
{
	int FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc write 8604 track=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		IoMem_ReadByte(0xff8605) , nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );

	/* [NP] Contrary to what is written in the WD1772 doc, Track Register can be changed */
	/* while the fdc is busy */
	if ( FDC.STR & FDC_STR_BIT_BUSY )
	{
		LOG_TRACE(TRACE_FDC, "fdc write 8604 fdc busy, track=0x%x may be ignored VBL=%d video_cyc=%d %d@%d pc=%x\n",
			IoMem_ReadByte(0xff8605), nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
	}

	FDC.TR = IoMem_ReadByte(0xff8605);
}


/*-----------------------------------------------------------------------*/
/**
 * Write to Sector register $ff8604
 */
static void FDC_WriteSectorRegister ( void )
{
	int FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc write 8604 sector=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		IoMem_ReadByte(0xff8605) , nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );

	/* [NP] Contrary to what is written in the WD1772 doc, Sector Register can be changed */
	/* while the fdc is busy (but it will have no effect once the sector's header is found) */
	/* (fix Delirious Demo IV's loader, which is bugged and set SR after starting the Read Sector command) */
	if ( FDC.STR & FDC_STR_BIT_BUSY )
	{
		LOG_TRACE(TRACE_FDC, "fdc write 8604 fdc busy, sector=0x%x may be ignored VBL=%d video_cyc=%d %d@%d pc=%x\n",
			IoMem_ReadByte(0xff8605), nVBLs, FrameCycles, LineCycles, HblCounterVideo, M68000_GetPC());
	}

	FDC.SR = IoMem_ReadByte(0xff8605);
}


/*-----------------------------------------------------------------------*/
/**
 * Write to Data register $ff8604
 */
static void FDC_WriteDataRegister ( void )
{
	int FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc write 8604 data=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		IoMem_ReadByte(0xff8605), nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );

	FDC.DR = IoMem_ReadByte(0xff8605);
}


/*-----------------------------------------------------------------------*/
/**
 * Store byte in FDC registers or DMA sector count, when writing to $ff8604
 */
void FDC_DiskController_WriteWord ( void )
{
	int FrameCycles, HblCounterVideo, LineCycles;

	if ( nIoMemAccessSize == SIZE_BYTE )
	{
		/* This register does not like to be accessed in byte mode on a normal ST */
		M68000_BusError(IoAccessBaseAddress, BUS_ERROR_WRITE);
		return;
	}

	M68000_WaitState(4);

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc write 8604 data=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		IoMem_ReadWord(0xff8604), nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );

	/* Is it an ACSI (or Falcon SCSI) HD command? */
	if ( ( FDC_DMA.Mode & 0x0018 ) == 8 )
	{
		/*  Handle HDC functions */
		HDC_WriteCommandByte(FDC_DMA.Mode & 0x7, IoMem_ReadByte(0xff8605));
		return;
	}

	/* Are we trying to set the SectorCount ? */
	if ( FDC_DMA.Mode & 0x10 )					/* Bit 4 */
		FDC_WriteSectorCountRegister();
	else
	{
		/* Update FDC's internal variables */
		FDC_UpdateAll ();

		/* Write to FDC registers */
		switch ( FDC_DMA.Mode & 0x6 )
		{   /* Bits 1,2 (A1,A0) */
		 case 0x0:						/* 0 0 - Command register */
			FDC_WriteCommandRegister();
			break;
		 case 0x2:						/* 0 1 - Track register */
			FDC_WriteTrackRegister();
			break;
		 case 0x4:						/* 1 0 - Sector register */
			FDC_WriteSectorRegister();
			break;
		 case 0x6:						/* 1 1 - Data register */
			FDC_WriteDataRegister();
			break;
		}
	}
}


/*-----------------------------------------------------------------------*/
/**
 * Return Status/FDC register when reading from $ff8604
 */
void FDC_DiskControllerStatus_ReadWord ( void )
{
	Uint16 DiskControllerByte = 0;					/* Used to pass back the parameter */
	int FrameCycles, HblCounterVideo, LineCycles;
	int ForceWPRT;

	if (nIoMemAccessSize == SIZE_BYTE)
	{
		/* This register does not like to be accessed in byte mode on a normal ST */
		M68000_BusError(IoAccessBaseAddress, BUS_ERROR_READ);
		return;
	}

	M68000_WaitState(4);

	if ((FDC_DMA.Mode & 0x18) == 0x08)				/* HDC status reg selected? */
	{
		/* return the HDC status byte */
		DiskControllerByte = HDC_ReadCommandByte(FDC_DMA.Mode & 0x7);
	}
	else if ((FDC_DMA.Mode & 0x18) == 0x18)				/* HDC sector counter??? */
	{
		Log_Printf(LOG_DEBUG, "*** Read HDC sector counter???\n");
		DiskControllerByte = HDC_GetSectorCount();
	}
	else
	{
		/* Update FDC's internal variables */
		FDC_UpdateAll ();

		/* Read FDC registers */
		switch (FDC_DMA.Mode & 0x6)				/* Bits 1,2 (A1,A0) */
		{
		 case 0x0:						/* 0 0 - Status register */
			/* [NP] Contrary to what is written in the WD1772 doc, the WPRT bit */
			/* is updated after a Type I command */
			/* (eg : Procopy or Terminators Copy 1.68 do a Restore/Seek to test WPRT) */
#ifdef old_str
			if ( ( ( FDC.STR & FDC_STR_BIT_BUSY ) == 0 )			/* No command running */
			  || ( FDC.CommandType == 1 ) )					/* Or busy command is Type I */
			{
				if ( Floppy_IsWriteProtected ( FDC_DRIVE ) )
					FDC_Update_STR ( 0 , FDC_STR_BIT_WPRT );	/* Set WPRT bit */
				else
					FDC_Update_STR ( FDC_STR_BIT_WPRT , 0 );	/* Unset WPRT bit */

				if ( FDC_IndexPulse_GetState () )
					FDC_Update_STR ( 0 , FDC_STR_BIT_INDEX );	/* Set INDEX bit */
				else
					FDC_Update_STR ( FDC_STR_BIT_INDEX , 0 );	/* Unset INDEX bit */
			}

			/* When there's no disk in drive, the floppy drive hardware can't see */
			/* the difference with an inserted disk that would be write protected */
			if ( ! EmulationDrives[ FDC_DRIVE ].bDiskInserted )
				FDC_Update_STR ( 0 , FDC_STR_BIT_WPRT );                /* Set WPRT bit */

			DiskControllerByte = FDC.STR;

			/* Temporarily change the WPRT bit if we're in a transition phase */
			/* regarding the disk in the drive (inserting or ejecting) */
			ForceWPRT = Floppy_DriveTransitionUpdateState ( FDC_DRIVE );
			if ( ForceWPRT == 1 )
				DiskControllerByte |= FDC_STR_BIT_WPRT;		/* Force setting WPRT */
			if ( ForceWPRT == -1 )
				DiskControllerByte &= ~FDC_STR_BIT_WPRT;	/* Force clearing WPRT */

			if ( ForceWPRT != 0 )
				LOG_TRACE(TRACE_FDC, "force wprt=%d VBL=%d drive=%d str=%x\n", ForceWPRT==1?1:0, nVBLs, FDC_DRIVE, DiskControllerByte );

#else
			/* If we report a type I status, some bits are updated in real time */
			/* depending on the corresponding signals. If this is not a type I, we return STR unmodified */
			if ( FDC.StatusTypeI )
			{
				/* If no drive available, FDC's input signals TR00, INDEX and WPRT are off */
				if ( ( FDC.DriveSelSignal < 0 ) || ( !FDC_DRIVES[ FDC.DriveSelSignal ].Enabled ) )
					FDC_Update_STR ( FDC_STR_BIT_TR00 | FDC_STR_BIT_INDEX | FDC_STR_BIT_WPRT , 0 );

				else
				{
					if ( FDC_DRIVES[ FDC.DriveSelSignal ].HeadTrack == 0 )
						FDC_Update_STR ( 0 , FDC_STR_BIT_TR00 );	/* Set bit TR00 */
					else
						FDC_Update_STR ( FDC_STR_BIT_TR00 , 0 );	/* Unset bit TR00 */

					if ( FDC_IndexPulse_GetState () )
						FDC_Update_STR ( 0 , FDC_STR_BIT_INDEX );	/* Set INDEX bit */
					else
						FDC_Update_STR ( FDC_STR_BIT_INDEX , 0 );	/* Unset INDEX bit */

					/* When there's no disk in drive, the floppy drive hardware can't see */
					/* the difference with an inserted disk that would be write protected */
					if ( ! FDC_DRIVES[ FDC.DriveSelSignal ].DiskInserted )
						FDC_Update_STR ( 0 , FDC_STR_BIT_WPRT );	/* Set WPRT bit */
					else if ( Floppy_IsWriteProtected ( FDC.DriveSelSignal ) )
						FDC_Update_STR ( 0 , FDC_STR_BIT_WPRT );	/* Set WPRT bit */
					else
						FDC_Update_STR ( FDC_STR_BIT_WPRT , 0 );	/* Unset WPRT bit */

					/* Temporarily change the WPRT bit if we're in a transition phase */
					/* regarding the disk in the drive (inserting or ejecting) */
					ForceWPRT = Floppy_DriveTransitionUpdateState ( FDC.DriveSelSignal );
					if ( ForceWPRT == 1 )
						FDC_Update_STR ( 0 , FDC_STR_BIT_WPRT );	/* Force setting WPRT */
					else if ( ForceWPRT == -1 )
						FDC_Update_STR ( FDC_STR_BIT_WPRT , 0 );	/* Force clearing WPRT */

					if ( ForceWPRT != 0 )
						LOG_TRACE(TRACE_FDC, "force wprt=%d VBL=%d drive=%d str=%x\n",
							  ForceWPRT==1?1:0, nVBLs, FDC.DriveSelSignal, FDC.STR );
				}
			}

			DiskControllerByte = FDC.STR;
#endif
			/* When Status Register is read, FDC's INTRQ is reset */
			MFP_GPIP |= 0x20;
			break;
		 case 0x2:						/* 0 1 - Track register */
			DiskControllerByte = FDC.TR;
			break;
		 case 0x4:						/* 1 0 - Sector register */
			DiskControllerByte = FDC.SR;
			break;
		 case 0x6:						/* 1 1 - Data register */
			DiskControllerByte = FDC.DR;
			break;
		}
	}

	IoMem_WriteWord(0xff8604, DiskControllerByte);

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc read 8604 ctrl status=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		DiskControllerByte , nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );
}


/*-----------------------------------------------------------------------*/
/**
 * Write word to $ff8606 (DMA Mode Control)
 *
 * Eg.
 * $80 - Selects command/status register
 * $82 - Selects track register
 * $84 - Selects sector register
 * $86 - Selects data register
 * NOTE - OR above values with $100 is transfer from memory to floppy
 * Also if bit 4 is set, write to DMA sector count register
 */
void FDC_DmaModeControl_WriteWord ( void )
{
	Uint16 Mode_prev;						/* Store previous write to 0xff8606 for 'toggle' checks */
	int FrameCycles, HblCounterVideo, LineCycles;


	if (nIoMemAccessSize == SIZE_BYTE)
	{
		/* This register does not like to be accessed in byte mode on a normal ST */
		M68000_BusError(IoAccessBaseAddress, BUS_ERROR_WRITE);
		return;
	}

	Mode_prev = FDC_DMA.Mode;					/* Store previous to check for _read/_write toggle (DMA reset) */
	FDC_DMA.Mode = IoMem_ReadWord(0xff8606);			/* Store to DMA Mode control */

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc write 8606 ctrl=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		FDC_DMA.Mode , nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );

	/* When write to 0xff8606, check bit '8' toggle. This causes DMA status reset */
	if ((Mode_prev ^ FDC_DMA.Mode) & 0x0100)
		FDC_ResetDMA();
}


/*-----------------------------------------------------------------------*/
/**
 * Read DMA Status at $ff8606
 *
 * Bit 0 - Error Status (0=Error)
 * Bit 1 - Sector Count Zero Status (0=Sector Count Zero)
 * Bit 2 - Data Request Inactive Status
 */
void FDC_DmaStatus_ReadWord ( void )
{
	if (nIoMemAccessSize == SIZE_BYTE)
	{
		/* This register does not like to be accessed in byte mode on a normal ST */
		M68000_BusError(IoAccessBaseAddress, BUS_ERROR_READ);
		return;
	}

	/* Set zero sector count */
	FDC_DMA.Status &= ~0x2;						/* Clear bit 1 */
	if ( FDC_DMA.Mode & 0x08 )					/* Get which sector count ? */
		FDC_DMA.Status |= (HDC_GetSectorCount())?0x2:0;		/* HDC */
	else
		FDC_DMA.Status |= (FDC_DMA.SectorCount)?0x2:0;		/* FDC */

	/* In the case of the ST, DRQ is always 0 because it's handled by the DMA and its 16 bytes buffer */

	IoMem_WriteWord(0xff8606, FDC_DMA.Status);
}


/*-----------------------------------------------------------------------*/
/**
 * Read hi/med/low DMA address byte at $ff8609/0b/0d
 */
void	FDC_DmaAddress_ReadByte ( void )
{
	int FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc read dma address %x val=0x%02x address=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		IoAccessCurrentAddress , IoMem[ IoAccessCurrentAddress ] , FDC_GetDMAAddress() ,
		nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );
}


/*-----------------------------------------------------------------------*/
/**
 * Write hi/med/low DMA address byte at $ff8609/0b/0d
 */
void	FDC_DmaAddress_WriteByte ( void )
{
	int FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc write dma address %x val=0x%02x address=0x%x VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		IoAccessCurrentAddress , IoMem[ IoAccessCurrentAddress ] , FDC_GetDMAAddress() ,
		nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );
}


/*-----------------------------------------------------------------------*/
/**
 * Get DMA address used to transfer data between FDC and RAM
 */
Uint32 FDC_GetDMAAddress(void)
{
	Uint32 Address;

	/* Build up 24-bit address from hardware registers */
	Address = ((Uint32)STMemory_ReadByte(0xff8609)<<16) | ((Uint32)STMemory_ReadByte(0xff860b)<<8) | (Uint32)STMemory_ReadByte(0xff860d);

	return Address;
}


/*-----------------------------------------------------------------------*/
/**
 * Write a new address to the FDC DMA address registers at $ff8909/0b/0d
 */
void FDC_WriteDMAAddress ( Uint32 Address )
{
	int FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc write 0x%x to dma address VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		Address , nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );

	/* Store as 24-bit address */
	STMemory_WriteByte(0xff8609, Address>>16);
	STMemory_WriteByte(0xff860b, Address>>8);
	STMemory_WriteByte(0xff860d, Address);
}


/*-----------------------------------------------------------------------*/
/**
 * Read sector from floppy drive into workspace
 * We copy the bytes in chunks to simulate reading of the floppy using DMA
 */
static bool FDC_ReadSectorFromFloppy ( Uint8 *buf , Uint8 Sector , int *pSectorSize )
{
	int FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc read sector addr=0x%x dev=%d sect=%d track=%d side=%d VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		FDC_GetDMAAddress(), FDC_DRIVE, Sector, FDC_DRIVES[ FDC_DRIVE ].HeadTrack, FDC_SIDE,
		nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );

	/* Copy 1 sector to our workspace */
	if ( Floppy_ReadSectors ( FDC_DRIVE, buf, Sector, FDC_DRIVES[ FDC_DRIVE ].HeadTrack, FDC_SIDE, 1, NULL, pSectorSize ) )
		return true;

	/* Failed */
	LOG_TRACE(TRACE_FDC, "fdc read sector failed\n" );
	return false;
}


/*-----------------------------------------------------------------------*/
/**
 * Write sector from RAM to floppy drive
 * We copy the bytes in chunks to simulate writing of the floppy using DMA
 * If DMASectorsCount==0, the DMA won't transfer any byte from RAM to the FDC
 * and some '0' bytes will be written to the disk.
 */
static bool FDC_WriteSectorToFloppy ( int DMASectorsCount , Uint8 Sector , int *pSectorSize )
{
	Uint8 *pBuffer;
	int FrameCycles, HblCounterVideo, LineCycles;

	Video_GetPosition ( &FrameCycles , &HblCounterVideo , &LineCycles );

	LOG_TRACE(TRACE_FDC, "fdc write sector addr=0x%x dev=%d sect=%d track=%d side=%d VBL=%d video_cyc=%d %d@%d pc=%x\n" ,
		FDC_GetDMAAddress(), FDC_DRIVE, Sector, FDC_DRIVES[ FDC_DRIVE ].HeadTrack, FDC_SIDE,
		nVBLs , FrameCycles, LineCycles, HblCounterVideo , M68000_GetPC() );

	if ( DMASectorsCount > 0 )
		pBuffer = &STRam[ FDC_GetDMAAddress() ];
	else
	{
		pBuffer = DMADiskWorkSpace;				/* If DMA can't transfer data, we write '0' bytes */
		memset ( pBuffer , 0 , DMA_DISK_SECTOR_SIZE );
	}
	
	/* Write 1 sector from our workspace */
	if ( Floppy_WriteSectors ( FDC_DRIVE, pBuffer, Sector, FDC_DRIVES[ FDC_DRIVE ].HeadTrack, FDC_SIDE, 1, NULL, pSectorSize ) )
		return true;

	/* Failed */
	LOG_TRACE(TRACE_FDC, "fdc write sector failed\n" );
	return false;
}


/*-----------------------------------------------------------------------*/
/**
 * Write to floppy mode/control (?) register (0xff860F).
 * Used on Falcon only!
 * FIXME: I've found hardly any documentation about this register, only
 * the following description of the bits:
 *
 *   __________54__10  Floppy Controll-Register
 *             ||  ||
 *             ||  |+- Prescaler 1
 *             ||  +-- Media detect 1
 *             |+----- Prescaler 2
 *             +------ Media detect 2
 *
 * For DD - disks:  0x00
 * For HD - disks:  0x03
 * for ED - disks:  0x30 (not supported by TOS)
 */
void FDC_FloppyMode_WriteByte ( void )
{
	// printf("Write to floppy mode reg.: 0x%02x\n", IoMem_ReadByte(0xff860f));
}


/*-----------------------------------------------------------------------*/
/**
 * Read from floppy mode/control (?) register (0xff860F).
 * Used on Falcon only!
 * FIXME: I've found hardly any documentation about this register, only
 * the following description of the bits:
 *
 *   ________76543210  Floppy Controll-Register
 *           ||||||||
 *           |||||||+- Prescaler 1
 *           ||||||+-- Mode select 1
 *           |||||+--- Media detect 1
 *           ||||+---- accessed during DMA transfers (?)
 *           |||+----- Prescaler 2
 *           ||+------ Mode select 2
 *           |+------- Media detect 2
 *           +-------- Disk changed
 */
void FDC_FloppyMode_ReadByte ( void )
{
	IoMem_WriteByte(0xff860f, 0x80);  // FIXME: Is this ok?
	// printf("Read from floppy mode reg.: 0x%02x\n", IoMem_ReadByte(0xff860f));
}
