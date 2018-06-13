/*-----------------------------------------------------------------
 Copyright (C) 2005 - 2010
	Michael "Chishm" Chisholm
	Dave "WinterMute" Murphy

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

------------------------------------------------------------------*/

#ifndef NDS_LOADER_ARM9_H
#define NDS_LOADER_ARM9_H


#ifdef __cplusplus
extern "C" {
#endif

#define LOAD_DEFAULT_NDS 0

int runNds (const void* loader, u32 loaderSize, u32 cluster, u32 useArm7Donor, u32 donorSdkVer, u32 patchMpuRegion, u32 patchMpuSize, u32 consoleModel, u32 ntrTouch, u32 loadingScreen, u32 romread_LED, u32 gameSoftReset, u32 noSoundStutter, bool initDisc, bool dldiPatchNds, int argc, const char** argv, u32* cheat_data);

int runNdsFile (const char* filename, const char* savename, int donorSdkVer, int patchMpuRegion, int patchMpuSize, int consoleModel, int ntrTouch, int loadingScreen, int romread_LED, int gameSoftReset, int noSoundStutter, int argc, const char** argv, u32* cheat_data);

#ifdef __cplusplus
}
#endif

#endif // NDS_LOADER_ARM7_H
