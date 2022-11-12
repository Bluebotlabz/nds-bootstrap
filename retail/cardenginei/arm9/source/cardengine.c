/*
    NitroHax -- Cheat tool for the Nintendo DS
    Copyright (C) 2008  Michael "Chishm" Chisholm

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>
#include <nds/ndstypes.h>
#include <nds/arm9/exceptions.h>
#include <nds/arm9/cache.h>
#include <nds/bios.h>
#include <nds/system.h>
#include <nds/dma.h>
#include <nds/interrupts.h>
#include <nds/ipc.h>
#include <nds/timers.h>
#include <nds/fifomessages.h>
#include <nds/memory.h> // tNDSHeader
#include "ndma.h"
#include "tonccpy.h"
#include "hex.h"
#include "igm_text.h"
#include "nds_header.h"
#include "cardengine.h"
#include "locations.h"
#include "cardengine_header_arm9.h"
#include "unpatched_funcs.h"

#define saveOnFlashcard BIT(0)
#define ROMinRAM BIT(1)
#define dsiMode BIT(3)
#define enableExceptionHandler BIT(4)
#define isSdk5 BIT(5)
#define overlaysCached BIT(6)
#define cacheFlushFlag BIT(7)
#define cardReadFix BIT(8)
#define cacheDisabled BIT(9)
#define slowSoftReset BIT(10)
#define dsiBios BIT(11)
#define asyncCardRead BIT(12)
#define softResetMb BIT(13)
#define cloneboot BIT(14)

//#ifdef DLDI
#include "my_fat.h"
#include "card.h"
//#endif

#define _16KB_READ_SIZE  0x4000
#define _32KB_READ_SIZE  0x8000
#define _64KB_READ_SIZE  0x10000
#define _128KB_READ_SIZE 0x20000
#define _192KB_READ_SIZE 0x30000
#define _256KB_READ_SIZE 0x40000
#define _512KB_READ_SIZE 0x80000
#define _768KB_READ_SIZE 0xC0000
#define _1MB_READ_SIZE   0x100000

#define ICACHE_SIZE      0x2000      
#define DCACHE_SIZE      0x1000      
#define CACHE_LINE_SIZE  32

#define THRESHOLD_CACHE_FLUSH 0x500

#define END_FLAG   0
#define BUSY_FLAG   4  

extern cardengineArm9* volatile ce9;

extern void ndsCodeStart(u32* addr);
extern u32 getDtcmBase(void);

#ifdef TWLSDK
vu32* volatile sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK5;
#else
vu32* volatile sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK1;
#endif

#ifndef TWLSDK
static unpatchedFunctions* unpatchedFuncs = (unpatchedFunctions*)UNPATCHED_FUNCTION_LOCATION;
#endif

#ifdef TWLSDK
tNDSHeader* ndsHeader = (tNDSHeader*)NDS_HEADER_SDK5;
aFile* romFile = (aFile*)ROM_FILE_LOCATION_TWLSDK;
aFile* savFile = (aFile*)SAV_FILE_LOCATION_TWLSDK;
aFile* apFixOverlaysFile = (aFile*)OVL_FILE_LOCATION_TWLSDK;
aFile* sharedFontFile = (aFile*)FONT_FILE_LOCATION_TWLSDK;
#else
tNDSHeader* ndsHeader = (tNDSHeader*)NDS_HEADER;
aFile* romFile = (aFile*)ROM_FILE_LOCATION_MAINMEM;
aFile* savFile = (aFile*)SAV_FILE_LOCATION_MAINMEM;
aFile* apFixOverlaysFile = (aFile*)OVL_FILE_LOCATION_MAINMEM;
#endif
//static aFile* gbaFile = (aFile*)GBA_FILE_LOCATION_MAINMEM;
//static aFile* gbaSavFile = (aFile*)GBA_SAV_FILE_LOCATION_MAINMEM;
#ifndef DLDI
//static u32 sdatAddr = 0;
//static u32 sdatSize = 0;
#ifdef TWLSDK
u32 cacheDescriptor[dev_CACHE_SLOTS_16KB_TWLSDK];
int cacheCounter[dev_CACHE_SLOTS_16KB_TWLSDK];
#else
u32 cacheDescriptor[dev_CACHE_SLOTS_16KB];
int cacheCounter[dev_CACHE_SLOTS_16KB];
#endif
int accessCounter = 0;
#endif
bool flagsSet = false;
static bool driveInitialized = false;
#ifndef TWLSDK
static bool region0FixNeeded = false;
#endif
bool igmReset = false;

extern bool isDma;

extern void endCardReadDma();
extern void continueCardReadDmaArm7();
extern void continueCardReadDmaArm9();

s8 mainScreen = 0;

void myIrqHandlerDMA(void);

extern void SetBrightness(u8 screen, s8 bright);

// Alternative to swiWaitForVBlank()
extern void waitFrames(int count);

/*static void waitMs(int count) {
	for (int i = 0; i < count; i++) {
		while ((REG_VCOUNT % 32) != 31);
		while ((REG_VCOUNT % 32) == 31);
	}
}*/

#ifndef DLDI
void sleepMs(int ms) {
	if (!(ce9->valueBits & asyncCardRead) || REG_IME == 0 || REG_IF == 0) {
		swiDelay(50);
		return;
	}

	if(ce9->patches->sleepRef) {
		volatile void (*sleepRef)(int ms) = (volatile void*)ce9->patches->sleepRef;
		(*sleepRef)(ms);
	} else if(ce9->thumbPatches->sleepRef) {
		extern void callSleepThumb(int ms);
		callSleepThumb(ms);
	} else {
		swiDelay(50);
	}
}

/*static void getSdatAddr(u32 sector, u32 buffer) {
	if ((!ce9->patches->sleepRef && !ce9->thumbPatches->sleepRef) || sdatSize != 0) return;

	for (u32 i = 0; i < ce9->cacheBlockSize; i+=4) {
		if (*(u32*)(buffer+i) == 0x54414453 && *(u32*)(buffer+i+8) <= 0x20000000) {
			sdatAddr = sector+i;
			sdatSize = *(u32*)(buffer+i+8);
			break;
		}
	}
}*/

#ifdef TWLSDK
void resetSlots(void) {
	for (int i = 0; i < ce9->cacheSlots; i++) {
		cacheDescriptor[i] = 0;
		cacheCounter[i] = 0;
	}
	accessCounter = 0;
}
#endif

int allocateCacheSlot(void) {
	int slot = 0;
	int lowerCounter = accessCounter;
	for (int i = 0; i < ce9->cacheSlots; i++) {
		if (cacheCounter[i] <= lowerCounter) {
			lowerCounter = cacheCounter[i];
			slot = i;
			if (!lowerCounter) {
				break;
			}
		}
	}
	return slot;
}

int getSlotForSector(u32 sector) {
	for (int i = 0; i < ce9->cacheSlots; i++) {
		if (cacheDescriptor[i] == sector) {
			return i;
		}
	}
	return -1;
}

/*int getSlotForSectorManual(int i, u32 sector) {
	if (i >= ce9->cacheSlots) {
		i -= ce9->cacheSlots;
	}
	if (cacheDescriptor[i] == sector) {
		return i;
	}
	return -1;
}*/

vu8* getCacheAddress(int slot) {
	//return (vu32*)(ce9->cacheAddress + slot*ce9->cacheBlockSize);
	return (vu8*)(ce9->cacheAddress + slot*ce9->cacheBlockSize);
}

void updateDescriptor(int slot, u32 sector) {
	cacheDescriptor[slot] = sector;
	cacheCounter[slot] = accessCounter;
}
#endif

/*static void sleep(u32 ms) {
    if(ce9->patches->sleepRef) {
        volatile void (*sleepRef)(u32) = ce9->patches->sleepRef;
        (*sleepRef)(ms);
    } else if(ce9->thumbPatches->sleepRef) {
        callSleepThumb(ms);
    }    
}*/


extern void setExceptionHandler2();

static inline void waitForArm7(void) {
	IPC_SendSync(0x4);
	while (sharedAddr[3] != (vu32)0) {
		swiDelay(100);
	}
}

#ifndef DLDI
static inline bool checkArm7(void) {
    IPC_SendSync(0x4);
	return (sharedAddr[3] == (vu32)0);
}
#endif

extern bool IPC_SYNC_hooked;
extern void hookIPC_SYNC(void);
extern void enableIPC_SYNC(void);

#ifndef TWLSDK
extern void initialize(void);
#endif

//static void clearIcache (void) {
      // Seems to have no effect
      // disable interrupt
      /*int oldIME = enterCriticalSection();
      IC_InvalidateAll();
      // restore interrupt
      leaveCriticalSection(oldIME);*/
//}

#ifdef TWLSDK
#ifndef DLDI
// For testing with FS SDK functions
/*static bool ctxInited = false;
static const char* rompath = "";
static const char* savepath = "";
static const char* apFixOverlaysPath = "sdmc:/_nds/nds-bootstrap/apFixOverlays.bin";
static u32 fsCtx[0x80/sizeof(u32)];*/

// Face Training (EUR)
/*volatile void (*FS_InitCtx)(u8*) = (volatile void*)0x020F1320;
volatile void (*FS_Open)(u32*, const char*, u32) = (volatile void*)0x020F1470;
volatile void (*FS_Close)(u32*) = (volatile void*)0x020F14F4;
volatile void (*FS_Seek)(u32*, u32, u32) = (volatile void*)0x020F15B8;
volatile void (*FS_Read)(u32*, u32, u32) = (volatile void*)0x020F15E4;
volatile void (*FS_Write)(u32*, u32, u32) = (volatile void*)0x020F1638;*/

// Rabbids Go Home (USA)
/*volatile void (*FS_InitCtx)(u8*) = (volatile void*)0x0203C264;
volatile void (*FS_Open)(u32*, const char*, u32) = (volatile void*)0x0203C3FC;
volatile void (*FS_Close)(u32*) = (volatile void*)0x0203C480;
volatile void (*FS_Seek)(u32*, u32, u32) = (volatile void*)0x0203C54C;
volatile void (*FS_Read)(u32*, u32, u32) = (volatile void*)0x0203C578;
volatile void (*FS_Write)(u32*, u32, u32) = (volatile void*)0x0203C5C8;*/

#endif
#endif

#ifndef DLDI
static u32 newOverlayOffset = 0;
static u32 newOverlaysSize = 0;
#endif

static inline void cardReadNormal(u8* dst, u32 src, u32 len) {
#ifdef DLDI
	while (sharedAddr[3]==0x444D4152);	// Wait during a RAM dump
	fileRead((char*)dst, ((ce9->valueBits & overlaysCached) && src >= ndsHeader->arm9romOffset+ndsHeader->arm9binarySize && src < ndsHeader->arm7romOffset) ? *apFixOverlaysFile : *romFile, src, len, 0);
#else
	if (newOverlayOffset == 0) {
		newOverlayOffset = ((ndsHeader->arm9romOffset+ndsHeader->arm9binarySize)/ce9->cacheBlockSize)*ce9->cacheBlockSize;
		for (u32 i = newOverlayOffset; i < ndsHeader->arm7romOffset; i+= ce9->cacheBlockSize) {
			newOverlaysSize += ce9->cacheBlockSize;
		}
	}

	u32 sector = (src/ce9->cacheBlockSize)*ce9->cacheBlockSize;

	accessCounter++;

	#ifdef ASYNCPF
	processAsyncCommand();
	#endif

	//if (src >= sdatAddr && src < sdatAddr+sdatSize) {
	//	sleepMsEnabled = true;
	//}

	if ((ce9->valueBits & cacheDisabled) && (u32)dst >= 0x02000000 && (u32)dst < 0x03000000) {
		fileRead((char*)dst, ((ce9->valueBits & overlaysCached) && src >= newOverlayOffset && src < newOverlayOffset+newOverlaysSize) ? *apFixOverlaysFile : *romFile, src, len, 0);
	} else {
		// Read via the main RAM cache
		//bool runSleep = true;
		while(len > 0) {
			int slot = getSlotForSector(sector);
			vu8* buffer = getCacheAddress(slot);
			#ifdef ASYNCPF
			u32 nextSector = sector+ce9->cacheBlockSize;
			#endif
			// Read max CACHE_READ_SIZE via the main RAM cache
			if (slot == -1) {
				#ifdef ASYNCPF
				getAsyncSector();
				#endif

				slot = allocateCacheSlot();

				buffer = getCacheAddress(slot);

				/*u32 len2 = (src - sector) + len;
				u16 readLen = ce9->cacheBlockSize;
				if (len2 > ce9->cacheBlockSize*3 && slot+3 < ce9->cacheSlots) {
					readLen = ce9->cacheBlockSize*4;
				} else if (len2 > ce9->cacheBlockSize*2 && slot+2 < ce9->cacheSlots) {
					readLen = ce9->cacheBlockSize*3;
				} else if (len2 > ce9->cacheBlockSize && slot+1 < ce9->cacheSlots) {
					readLen = ce9->cacheBlockSize*2;
				}*/

				fileRead((char*)buffer, ((ce9->valueBits & overlaysCached) && src >= newOverlayOffset && src < newOverlayOffset+newOverlaysSize) ? *apFixOverlaysFile : *romFile, sector, ce9->cacheBlockSize, 0);
				/*updateDescriptor(slot, sector);
				if (readLen >= ce9->cacheBlockSize*2) {
					updateDescriptor(slot+1, sector+ce9->cacheBlockSize);
				}
				if (readLen >= ce9->cacheBlockSize*3) {
					updateDescriptor(slot+2, sector+(ce9->cacheBlockSize*2));
				}
				if (readLen >= ce9->cacheBlockSize*4) {
					updateDescriptor(slot+3, sector+(ce9->cacheBlockSize*3));
				}*/

				#ifdef ASYNCPF
				if (REG_IME != 0 && REG_IF != 0) {
					triggerAsyncPrefetch(nextSector);
				}
				#endif
				//runSleep = false;
			} else {
				#ifdef ASYNCPF
				if(cacheCounter[slot] == 0x0FFFFFFF) {
					// prefetch successfull
					getAsyncSector();

					triggerAsyncPrefetch(nextSector);
				} /*else {
					int i;
					for(i=0; i<5; i++) {
						if(asyncQueue[i]==sector) {
							// prefetch successfull
							triggerAsyncPrefetch(nextSector);
							break;
						}
					}
				}*/
				#endif
				//updateDescriptor(slot, sector);
			}
			updateDescriptor(slot, sector);

			//getSdatAddr(sector, (u32)buffer);

			u32 len2 = len;
			if ((src - sector) + len2 > ce9->cacheBlockSize) {
				len2 = sector - src + ce9->cacheBlockSize;
			}

    		#ifdef DEBUG
    		// Send a log command for debug purpose
    		// -------------------------------------
   			commandRead = 0x026ff800;
    
    		sharedAddr[0] = dst;
    		sharedAddr[1] = len2;
    		sharedAddr[2] = buffer+src-sector;
    		sharedAddr[3] = commandRead;
    
    		waitForArm7();
    		// -------------------------------------
    		#endif
    
    		// Copy directly
			/*if (isDma) {
				ndmaCopyWordsAsynch(0, (u8*)buffer+(src-sector), dst, len2);
				while (ndmaBusy(0)) {
					if (runSleep) {
						sleepMs(1);
					}
				}
			} else {*/
				tonccpy(dst, (u8*)buffer+(src-sector), len2);
			//}

			len -= len2;
			if (len > 0) {
				src = src + len2;
				dst = (u8*)(dst + len2);
				sector = (src/ce9->cacheBlockSize)*ce9->cacheBlockSize;
				accessCounter++;
				//slot = getSlotForSectorManual(slot+1, sector);
			}
		}
	}
#endif

	//sleepMsEnabled = false;

	#ifndef TWLSDK
	if (ce9->valueBits & cacheFlushFlag) {
		cacheFlush(); //workaround for some weird data-cache issue in Bowser's Inside Story.
	}
	#endif
}

static inline void cardReadRAM(u8* dst, u32 src, u32 len/*, int romPartNo*/) {
	#ifdef DEBUG
	// Send a log command for debug purpose
	// -------------------------------------
	commandRead = 0x026ff800;

	sharedAddr[0] = dst;
	sharedAddr[1] = len;
	sharedAddr[2] = ce9->romLocation[romPartNo]+src;
	sharedAddr[3] = commandRead;

	waitForArm7();
	// -------------------------------------
	#endif

	// Copy directly
	#ifdef TWLSDK
	u32 newSrc = ce9->romLocation/*[romPartNo]*/+src;
	if (src > *(u32*)0x02FFE1C0) {
		newSrc -= *(u32*)0x02FFE1CC;
	}
	tonccpy(dst, (u8*)newSrc, len);
	#else
	tonccpy(dst, (u8*)ce9->romLocation/*[romPartNo]*/+src, len);
	#endif
}

bool isNotTcm(u32 address, u32 len) {
    u32 base = (getDtcmBase()>>12) << 12;
    return    // test data not in ITCM
    address > 0x02000000
    // test data not in DTCM
    && (address < base || address> base+0x4000)
    && (address+len < base || address+len> base+0x4000);
}  

#ifndef TWLSDK
static int counter=0;
#endif
int cardReadPDash(u32* cacheStruct, u32 src, u8* dst, u32 len) {
	#ifndef TWLSDK
	vu32* volatile cardStruct = (vu32* volatile)ce9->cardStruct0;

    cardStruct[0] = src;
    cardStruct[1] = (vu32)dst;
    cardStruct[2] = len;

	cardRead(cacheStruct, dst, src, len);

    counter++;
	return counter;
	#else
	return 0;
	#endif
}

//extern void region2Disable();

// Required for proper access to the extra DSi RAM
extern void debugRamMpuFix();

// Revert region 0 patch
extern void region0Fix();

void cardRead(u32* cacheStruct, u8* dst0, u32 src0, u32 len0) {
	//nocashMessage("\narm9 cardRead\n");
	#ifndef TWLSDK
	initialize();
	#endif

	if (!flagsSet) {
		#ifndef TWLSDK
		if (!(ce9->valueBits & isSdk5)) {
			if (region0FixNeeded) {
				region0Fix();
			}
			if (!(ce9->valueBits & ROMinRAM)) {
				debugRamMpuFix();
			}
		}
		#endif
		if (!driveInitialized) {
			FAT_InitFiles(false, 0);
			driveInitialized = true;
		}
		if (ce9->valueBits & enableExceptionHandler) {
			setExceptionHandler2();
		}
		flagsSet = true;
	}

	enableIPC_SYNC();

	#ifdef TWLSDK
	u32 src = src0;
	u8* dst = dst0;
	u32 len = len0;
	#else
	vu32* volatile cardStruct = (vu32* volatile)ce9->cardStruct0;

	u32 src = ((ce9->valueBits & isSdk5) ? src0 : cardStruct[0]);
	u8* dst = ((ce9->valueBits & isSdk5) ? dst0 : (u8*)(cardStruct[1]));
	u32 len = ((ce9->valueBits & isSdk5) ? len0 : cardStruct[2]);
	#endif

	#ifdef DEBUG
	u32 commandRead;

	// send a log command for debug purpose
	// -------------------------------------
	commandRead = 0x026ff800;

	sharedAddr[0] = dst;
	sharedAddr[1] = len;
	sharedAddr[2] = src;
	sharedAddr[3] = commandRead;

	waitForArm7();
	// -------------------------------------*/
	#endif

	if ((ce9->valueBits & cardReadFix) && src < 0x8000) {
		// Fix reads below 0x8000
		src = 0x8000 + (src & 0x1FF);
	}

	bool romPart = false;
	//int romPartNo = 0;
	if (!(ce9->valueBits & ROMinRAM)) {
		/*for (int i = 0; i < 2; i++) {
			if (ce9->romPartSize[i] == 0) {
				break;
			}
			romPart = (src >= ce9->romPartSrc[i] && src < ce9->romPartSrc[i]+ce9->romPartSize[i]);
			if (romPart) {
				romPartNo = i;
				break;
			}
		}*/
		romPart = (ce9->romPartSize > 0 && src >= ce9->romPartSrc && src < ce9->romPartSrc+ce9->romPartSize);
	}
	if ((ce9->valueBits & ROMinRAM) || romPart) {
		cardReadRAM(dst, src, len/*, romPartNo*/);
	} else {
		cardReadNormal(dst, src, len);
	}
    isDma=false;
}

/*void cardPullOut(void) {
	if (*(vu32*)(0x027FFB30) != 0) {
		// volatile int (*terminateForPullOutRef)(u32*) = *ce9->patches->terminateForPullOutRef;
        // (*terminateForPullOutRef);
		// sharedAddr[3] = 0x5245424F;
		// waitForArm7();
	}
}*/

bool nandRead(void* memory,void* flash,u32 len,u32 dma) {
#ifdef DLDI
	if (ce9->valueBits & saveOnFlashcard) {
		fileRead(memory, *savFile, (u32)flash, len, 0);
		return true;
	} else {
		// Send a command to the ARM7 to read the nand save
		u32 commandNandRead = 0x025FFC01;

		// Write the command
		sharedAddr[0] = (u32)memory;
		sharedAddr[1] = len;
		sharedAddr[2] = (u32)flash;
		sharedAddr[3] = commandNandRead;

		waitForArm7();
	}
#else
	fileRead(memory, *savFile, (u32)flash, len, 0);
#endif
    return true; 
}

bool nandWrite(void* memory,void* flash,u32 len,u32 dma) {
#ifdef DLDI
	if (ce9->valueBits & saveOnFlashcard) {
		fileWrite(memory, *savFile, (u32)flash, len, 0);
		return true;
	} else {
		// Send a command to the ARM7 to write the nand save
		u32 commandNandWrite = 0x025FFC02;

		// Write the command
		sharedAddr[0] = (u32)memory;
		sharedAddr[1] = len;
		sharedAddr[2] = (u32)flash;
		sharedAddr[3] = commandNandWrite;

		waitForArm7();
	}
#else
	fileWrite(memory, *savFile, (u32)flash, len, 0);
#endif
    return true; 
}

#ifdef TWLSDK
#ifdef DLDI
static bool sharedFontOpened = false;
static bool dsiSaveInited = false;
static bool dsiSaveExists = false;
static u32 dsiSavePerms = 0;
static s32 dsiSaveSeekPos = 0;
static s32 dsiSaveSize = 0;
static s32 dsiSaveResultCode = 0;

typedef struct dsiSaveInfo
{
	u32 attributes;
	u32 ctime[6];
	u32 mtime[6];
	u32 atime[6];
	u32 filesize;
	u32 id;
}
dsiSaveInfo;

static void dsiSaveInit(void) {
	if (dsiSaveInited) {
		return;
	}
	u32 existByte = 0;

	int oldIME = enterCriticalSection();
	u16 exmemcnt = REG_EXMEMCNT;
	sysSetCardOwner(true);	// Give Slot-1 access to arm9
	fileRead((char*)&dsiSaveSize, *savFile, ce9->saveSize-4, 4, 0);
	fileRead((char*)&existByte, *savFile, ce9->saveSize-8, 4, 0);
	REG_EXMEMCNT = exmemcnt;
	leaveCriticalSection(oldIME);

	dsiSaveExists = (existByte != 0);
	dsiSaveInited = true;
}
#endif

u32 dsiSaveGetResultCode(const char* path) {
#ifdef DLDI
	if (savFile->firstCluster == CLUSTER_FREE || savFile->firstCluster == CLUSTER_EOF) {
		return 0xE;
	}

	dsiSaveInit();

	if (strcmp(path, "data") == 0) // Specific to EnjoyUp-developed games
	{
		return dsiSaveExists ? 8 : 0xE;
	} else
	if (strcmp(path, "dataPub:") == 0 || strcmp(path, "dataPub:/") == 0
	 || strcmp(path, "dataPrv:") == 0 || strcmp(path, "dataPrv:/") == 0)
	{
		return 8;
	}
	return dsiSaveResultCode;
#else
	return 0xB;
#endif
}

bool dsiSaveCreate(const char* path, u32 permit) {
#ifdef DLDI
	dsiSaveSeekPos = 0;
	if (savFile->firstCluster == CLUSTER_FREE || savFile->firstCluster == CLUSTER_EOF) {
		dsiSaveResultCode = 0xE;
		return false;
	}

	dsiSaveInit();
	//if ((!dsiSaveExists && permit == 1) || (dsiSaveExists && permit == 2)) {
	//	return false;
	//}

	if (strcmp(path, "data") == 0) // Specific to EnjoyUp-developed games
	{
		return !dsiSaveExists;
	} else
	if (!dsiSaveExists) {
		u32 existByte = 1;

		int oldIME = enterCriticalSection();
		u16 exmemcnt = REG_EXMEMCNT;
		sysSetCardOwner(true);	// Give Slot-1 access to arm9
		fileWrite((char*)&existByte, *savFile, ce9->saveSize-8, 4, 0);
		REG_EXMEMCNT = exmemcnt;
		leaveCriticalSection(oldIME);

		dsiSaveExists = true;
		dsiSaveResultCode = 0;
		return true;
	}
	dsiSaveResultCode = 8;
#endif
	return false;
}

bool dsiSaveDelete(const char* path) {
#ifdef DLDI
	dsiSaveSeekPos = 0;
	if (savFile->firstCluster == CLUSTER_FREE || savFile->firstCluster == CLUSTER_EOF) {
		return false;
	}

	if (dsiSaveExists) {
		dsiSaveSize = 0;

		int oldIME = enterCriticalSection();
		u16 exmemcnt = REG_EXMEMCNT;
		sysSetCardOwner(true);	// Give Slot-1 access to arm9
		fileWrite((char*)&dsiSaveSize, *savFile, ce9->saveSize-4, 4, 0);
		fileWrite((char*)&dsiSaveSize, *savFile, ce9->saveSize-8, 4, 0);
		REG_EXMEMCNT = exmemcnt;
		leaveCriticalSection(oldIME);

		dsiSaveExists = false;
		dsiSaveResultCode = 0;
		return true;
	}
	dsiSaveResultCode = 8;
#endif
	return false;
}

#ifdef DLDI
bool dsiSaveGetInfo(const char* path, dsiSaveInfo* info) {
	toncset(info, 0, sizeof(dsiSaveInfo));
	if (savFile->firstCluster == CLUSTER_FREE || savFile->firstCluster == CLUSTER_EOF) {
		return false;
	}

	dsiSaveInit();

	if (strcmp(path, "dataPub:") == 0 || strcmp(path, "dataPub:/") == 0
	 || strcmp(path, "dataPrv:") == 0 || strcmp(path, "dataPrv:/") == 0)
	{
		return true;
	} else if (!dsiSaveExists) {
		return false;
	}

	info->filesize = dsiSaveSize;
	return true;
}
#else
bool dsiSaveGetInfo(const char* path, u32* info) {
	return false;
}
#endif

u32 dsiSaveSetLength(void* ctx, s32 len) {
#ifdef DLDI
	dsiSaveSeekPos = 0;
	if (savFile->firstCluster == CLUSTER_FREE || savFile->firstCluster == CLUSTER_EOF) {
		dsiSaveResultCode = 1;
		return 1;
	}

	dsiSaveSize = len;

	int oldIME = enterCriticalSection();
	u16 exmemcnt = REG_EXMEMCNT;
	sysSetCardOwner(true);	// Give Slot-1 access to arm9
	bool res = fileWrite((char*)&dsiSaveSize, *savFile, ce9->saveSize-4, 4, 0);
	dsiSaveResultCode = res ? 0 : 1;
	REG_EXMEMCNT = exmemcnt;
	leaveCriticalSection(oldIME);

	return dsiSaveResultCode;
#else
	return 1;
#endif
}

bool dsiSaveOpen(void* ctx, const char* path, u32 mode) {
#ifdef DLDI
	dsiSaveSeekPos = 0;
	if (strcmp(path, "nand:/<sharedFont>") == 0) {
		if (sharedFontFile->firstCluster == CLUSTER_FREE || sharedFontFile->firstCluster == CLUSTER_EOF) {
			dsiSaveResultCode = 0xE;
			return false;
		}
		dsiSaveResultCode = 0;
		sharedFontOpened = true;
		return true;
	}
	if (savFile->firstCluster == CLUSTER_FREE || savFile->firstCluster == CLUSTER_EOF) {
		dsiSaveResultCode = 0xE;
		return false;
	}

	dsiSaveInit();
	dsiSaveResultCode = 0;
	toncset32(ctx+0x14, dsiSaveGetResultCode(path), 1);

	dsiSavePerms = mode;
	return dsiSaveExists;
#else
	return false;
#endif
}

bool dsiSaveClose(void* ctx) {
#ifdef DLDI
	dsiSaveSeekPos = 0;
	if (sharedFontOpened) {
		sharedFontOpened = false;
		if (sharedFontFile->firstCluster == CLUSTER_FREE || sharedFontFile->firstCluster == CLUSTER_EOF) {
			dsiSaveResultCode = 0xE;
			return false;
		}
		dsiSaveResultCode = 0;
		return true;
	}
	if (savFile->firstCluster == CLUSTER_FREE || savFile->firstCluster == CLUSTER_EOF) {
		dsiSaveResultCode = 0xE;
		return false;
	}
	//toncset(ctx, 0, 0x80);
	dsiSaveResultCode = 0;
	return dsiSaveExists;
#else
	return false;
#endif
}

u32 dsiSaveGetLength(void* ctx) {
#ifdef DLDI
	dsiSaveSeekPos = 0;
	if (savFile->firstCluster == CLUSTER_FREE || savFile->firstCluster == CLUSTER_EOF) {
		return 0;
	}

	return dsiSaveSize;
#else
	return 0;
#endif
}

bool dsiSaveSeek(void* ctx, s32 pos, u32 mode) {
#ifdef DLDI
	if (sharedFontOpened) {
		if (sharedFontFile->firstCluster == CLUSTER_FREE || sharedFontFile->firstCluster == CLUSTER_EOF) {
			dsiSaveResultCode = 0xE;
			return false;
		}
		dsiSaveSeekPos = pos;
		dsiSaveResultCode = 0;
		return true;
	}
	if (savFile->firstCluster == CLUSTER_FREE || savFile->firstCluster == CLUSTER_EOF) {
		dsiSaveResultCode = 0xE;
		return false;
	}
	if (!dsiSaveExists) {
		dsiSaveResultCode = 1;
		return false;
	}
	dsiSaveSeekPos = pos;
	dsiSaveResultCode = 0;
	return true;
#else
	return false;
#endif
}

s32 dsiSaveRead(void* ctx, void* dst, u32 len) {
#ifdef DLDI
	if (!sharedFontOpened) {
		if (dsiSavePerms == 2 || !dsiSaveExists) {
			dsiSaveResultCode = 1;
			return -1; // Return if only write perms are set
		}

		if (dsiSaveSize == 0) {
			dsiSaveResultCode = 1;
			return 0;
		}

		while (dsiSaveSeekPos+len > dsiSaveSize) {
			len--;
		}

		if (len == 0) {
			dsiSaveResultCode = 1;
			return 0;
		}
	}

	int oldIME = enterCriticalSection();
	u16 exmemcnt = REG_EXMEMCNT;
	sysSetCardOwner(true);	// Give Slot-1 access to arm9
	bool res = fileRead(dst, sharedFontOpened ? *sharedFontFile : *savFile, dsiSaveSeekPos, len, 0);
	dsiSaveResultCode = res ? 1 : 0;
	REG_EXMEMCNT = exmemcnt;
	leaveCriticalSection(oldIME);
	if (res) {
		dsiSaveSeekPos += len;
		return len;
	}
#endif
	return -1;
}

s32 dsiSaveWrite(void* ctx, void* src, s32 len) {
#ifdef DLDI
	if (dsiSavePerms == 1 || !dsiSaveExists) {
		dsiSaveResultCode = 1;
		return -1; // Return if only read perms are set
	}

	while (dsiSaveSeekPos+len > ce9->saveSize-0x200) {
		// Do not overwrite exist flag and save file size
		len--;
	}

	int oldIME = enterCriticalSection();
	u16 exmemcnt = REG_EXMEMCNT;
	sysSetCardOwner(true);	// Give Slot-1 access to arm9
	bool res = fileWrite(src, *savFile, dsiSaveSeekPos, len, 0);
	dsiSaveResultCode = res ? 1 : 0;
	REG_EXMEMCNT = exmemcnt;
	leaveCriticalSection(oldIME);
	if (res) {
		if (dsiSaveSize < dsiSaveSeekPos+len) {
			dsiSaveSize = dsiSaveSeekPos+len;

			int oldIME = enterCriticalSection();
			u16 exmemcnt = REG_EXMEMCNT;
			sysSetCardOwner(true);	// Give Slot-1 access to arm9
			fileWrite((char*)&dsiSaveSize, *savFile, ce9->saveSize-4, 4, 0);
			REG_EXMEMCNT = exmemcnt;
			leaveCriticalSection(oldIME);
		}
		dsiSaveSeekPos += len;
		return len;
	}
#endif
	return -1;
}

void initMBKARM9_dsiMode(void) {
	*(vu32*)REG_MBK1 = *(u32*)0x02FFE180;
	*(vu32*)REG_MBK2 = *(u32*)0x02FFE184;
	*(vu32*)REG_MBK3 = *(u32*)0x02FFE188;
	*(vu32*)REG_MBK4 = *(u32*)0x02FFE18C;
	*(vu32*)REG_MBK5 = *(u32*)0x02FFE190;
	REG_MBK6 = *(u32*)0x02FFE194;
	REG_MBK7 = *(u32*)0x02FFE198;
	REG_MBK8 = *(u32*)0x02FFE19C;
	REG_MBK9 = *(u32*)0x02FFE1AC;
}
#else
u32 dsiSaveGetResultCode(const char* path) {
	return 0xB;
}

bool dsiSaveCreate(const char* path, u32 permit) {
	return false;
}

bool dsiSaveDelete(const char* path) {
	return false;
}

bool dsiSaveGetInfo(const char* path, u32* info) {
	return false;
}

u32 dsiSaveSetLength(void* ctx, s32 len) {
	return 1;
}

bool dsiSaveOpen(void* ctx, const char* path, u32 mode) {
	return false;
}

bool dsiSaveClose(void* ctx) {
	return false;
}

u32 dsiSaveGetLength(void* ctx) {
	return 0;
}

bool dsiSaveSeek(void* ctx, s32 pos, u32 mode) {
	return false;
}

s32 dsiSaveRead(void* ctx, void* dst, u32 len) {
	return -1;
}

s32 dsiSaveWrite(void* ctx, void* src, s32 len) {
	return -1;
}
#endif

/*u32 cartRead(u32 dma, u32 src, u8* dst, u32 len, u32 type) {
	if (src >= 0x02000000 ? (gbaSavFile->firstCluster == CLUSTER_FREE) : (gbaFile->firstCluster == CLUSTER_FREE)) {
		return false;
	}

	fileRead(dst, src >= 0x02000000 ? *gbaSavFile : *gbaFile, src, len, 0);
	return true;
	return 0;
}*/

extern void reset(u32 param, u32 tid2);

void inGameMenu(s32* exRegisters) {
	#ifdef TWLSDK
	if (ce9->consoleModel > 0) {
		*(u32*)(INGAME_MENU_LOCATION_DSIWARE + IGM_TEXT_SIZE_ALIGNED) = (u32)sharedAddr;
		volatile void (*inGameMenu)(s8*, u32, s32*) = (volatile void*)INGAME_MENU_LOCATION_DSIWARE + IGM_TEXT_SIZE_ALIGNED + 0x10;
		(*inGameMenu)(&mainScreen, ce9->consoleModel, exRegisters);
	} else {
		*(u32*)(INGAME_MENU_LOCATION_TWLSDK + IGM_TEXT_SIZE_ALIGNED) = (u32)sharedAddr;
		volatile void (*inGameMenu)(s8*, u32, s32*) = (volatile void*)INGAME_MENU_LOCATION_TWLSDK + IGM_TEXT_SIZE_ALIGNED + 0x10;
		(*inGameMenu)(&mainScreen, ce9->consoleModel, exRegisters);
	}
	#else
	*(u32*)(INGAME_MENU_LOCATION + IGM_TEXT_SIZE_ALIGNED) = (u32)sharedAddr;
	volatile void (*inGameMenu)(s8*, u32, s32*) = (volatile void*)INGAME_MENU_LOCATION + IGM_TEXT_SIZE_ALIGNED + 0x10;
	(*inGameMenu)(&mainScreen, ce9->consoleModel, exRegisters);
	#endif
	#ifdef TWLSDK
	if (sharedAddr[3] == 0x54495845) {
		igmReset = true;
		if (*(u32*)0x02FFE234 == 0x00030004 || *(u32*)0x02FFE234 == 0x00030005) {
			reset(0, 0);
		} else {
			reset(0xFFFFFFFF, 0);
		}
	} else
	#endif
	if (sharedAddr[3] == 0x52534554) {
		igmReset = true;
	#ifdef TWLSDK
		if (*(u32*)0x02FFE234 == 0x00030004 || *(u32*)0x02FFE234 == 0x00030005) { // If DSiWare...
			reset(*(u32*)0x02FFE230, *(u32*)0x02FFE234);
		} else {
	#endif
			reset(0, 0);
	#ifdef TWLSDK
		}
	#endif
	}
}

//---------------------------------------------------------------------------------
void myIrqHandlerVBlank(void) {
//---------------------------------------------------------------------------------
	#ifdef DEBUG		
	nocashMessage("myIrqHandlerVBlank");
	#endif	

	#ifndef TWLSDK
	if (sharedAddr[4] == 0x554E454D) {
		while (sharedAddr[4] != 0x54495845);
	}
	#endif
}

//---------------------------------------------------------------------------------
void myIrqHandlerIPC(void) {
//---------------------------------------------------------------------------------
	#ifdef DEBUG		
	nocashMessage("myIrqHandlerIPC");
	#endif

	switch (IPC_GetSync()) {
		case 0x3:
			extern bool dmaDirectRead;
#ifdef DLDI
		if (dmaDirectRead) {
			endCardReadDma();
		}
#else
		if (dmaDirectRead) {
			endCardReadDma();
		}
		#ifndef TWLSDK
		else if (ce9->patches->cardEndReadDmaRef || ce9->thumbPatches->cardEndReadDmaRef) { // new dma method
			continueCardReadDmaArm7();
			continueCardReadDmaArm9();
		}
		#endif
#endif
			break;
		case 0x4:
			extern bool dmaOn;
			dmaOn = !dmaOn;
			break;
		case 0x5:
			igmReset = true;
			sharedAddr[3] = 0x54495845;
			#ifdef TWLSDK
			if (*(u32*)0x02FFE234 == 0x00030004 || *(u32*)0x02FFE234 == 0x00030005) {
				reset(0, 0);
			} else {
				reset(0xFFFFFFFF, 0);
			}
			#else
			reset(0xFFFFFFFF, 0);
			#endif
			break;
		case 0x6:
			if(mainScreen == 1)
				REG_POWERCNT &= ~POWER_SWAP_LCDS;
			else if(mainScreen == 2)
				REG_POWERCNT |= POWER_SWAP_LCDS;
			break;
		case 0x7: {
			mainScreen++;
			if(mainScreen > 2)
				mainScreen = 0;

			if(mainScreen == 1)
				REG_POWERCNT &= ~POWER_SWAP_LCDS;
			else if(mainScreen == 2)
				REG_POWERCNT |= POWER_SWAP_LCDS;
		}
			break;
		case 0x9:
			inGameMenu((s32*)0);
			break;
	}

	if (sharedAddr[4] == (vu32)0x57534352) {
		enterCriticalSection();
		if (ce9->consoleModel < 2) {
			// Make screens white
			SetBrightness(0, 31);
			SetBrightness(1, 31);
		}
		cacheFlush();
		while (1);
	}
}

u32 myIrqEnable(u32 irq) {	
	int oldIME = enterCriticalSection();

	#ifdef DEBUG
	nocashMessage("myIrqEnable\n");
	#endif

	#ifndef TWLSDK
	initialize();
	#endif

	if (ce9->valueBits & enableExceptionHandler) {
		setExceptionHandler2();
	}

	#ifndef TWLSDK
	if (!(ce9->valueBits & isSdk5) && unpatchedFuncs->mpuDataOffset) {
		region0FixNeeded = unpatchedFuncs->mpuInitRegionOldData == 0x4000033;
	}
	#endif

	hookIPC_SYNC();

	u32 irq_before = REG_IE | IRQ_IPC_SYNC;		
	irq |= IRQ_IPC_SYNC;
	REG_IPC_SYNC |= IPC_SYNC_IRQ_ENABLE;

	REG_IE |= irq;
	leaveCriticalSection(oldIME);
	return irq_before;
}
