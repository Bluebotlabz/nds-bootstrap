#ifndef PATCH_H
#define PATCH_H

#include <nds/ndstypes.h>
#include <nds/memory.h> // tNDSHeader

typedef struct patchOffsetCacheContents {
    u16 ver;
    u16 type;
	u32 dldiOffset;
	u32 dldiChecked;
	u32* wordCommandOffset;
	u32* bootloaderOffset;
	u32 bootloaderChecked;
	u16* a9Swi12Offset;
	u32 a9Swi12Checked;
	u32* a7IrqHookOffset;
	u32* a7IrqHookAccelOffset;
	u16* swi00Offset;
	u32 swi00Checked;
} patchOffsetCacheContents;

extern u16 patchOffsetCacheFileVersion;
extern patchOffsetCacheContents patchOffsetCache;
extern bool patchOffsetCacheChanged;
extern void rsetPatchCache(const tNDSHeader* ndsHeader);

extern void patchBinary(const tNDSHeader* ndsHeader);

#endif // PATCH_H
