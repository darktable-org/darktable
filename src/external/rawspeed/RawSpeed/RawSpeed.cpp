#include "stdafx.h"
#include "FileReader.h"
#include "RawParser.h"
#include "RawDecoder.h"
#include "CameraMetaData.h"
#include "ColorFilterArray.h"

/* 
    RawSpeed - RAW file decoder.

    Copyright (C) 2009 Klaus Post

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

    http://www.klauspost.com
*/

using namespace RawSpeed;

//#define _USE_GFL_
#ifdef _USE_GFL_
#include "libgfl.h"
#pragma comment(lib, "libgfl.lib") 
#endif

int startTime;

// Open file, or test corrupt file
#if 1
// Open file and save as tiff
void OpenFile(FileReader f, CameraMetaData *meta) {
  RawDecoder *d = 0;
  FileMap* m = 0;
  try {
//    wprintf(L"Opening:%s\n",f.Filename());
    try {
      m = f.readFile();    	
    } catch (FileIOException e) {
      printf("Could not open image:%s\n", e.what());
      return;
    }
    RawParser t(m);
    d = t.getDecoder();
    d->checkSupport(meta);
    startTime = GetTickCount();

    d->decodeRaw();
    d->decodeMetaData(meta);
    RawImage r = d->mRaw;
    r->scaleBlackWhite();

    uint32 time = GetTickCount()-startTime;
    float mpps = (float)r->dim.x * (float)r->dim.y * (float)r->getCpp()  / (1000.0f * (float)time);
    wprintf(L"Decoding %s took: %u ms, %4.2f Mpixel/s\n", f.Filename(), time, mpps);

    for (uint32 i = 0; i < d->errors.size(); i++) {
      printf("Error Encountered:%s", d->errors[i]);
    }
    if (r->isCFA) {
//        printf("DCRAW filter:%x\n",r->cfa.getDcrawFilter());
//        printf(r->cfa.asString().c_str());
    }
/*
    for (uint32 y=200;y<r->dim.y-400; y+=2) {
      printf("black,");
      for (uint32 x=20;x<=100; x+=2) {
        (x==100) ? printf("%d\n",(int)*(UINT16*)r->getData(x,y)) : printf("%d,",(int)*(UINT16*)r->getData(x,y));
      }
      printf("color,");
      for (uint32 x=1020;x<=1100; x+=2) {
        (x==1100) ? printf("%d\n",(int)*(UINT16*)r->getData(x,y)) : printf("%d,",(int)*(UINT16*)r->getData(x,y));
      }
    }
*/
#ifdef _USE_GFL_
    GFL_BITMAP* b;
    if (r->getCpp() == 1)
      b = gflAllockBitmapEx(GFL_GREY,d->mRaw->dim.x, d->mRaw->dim.y,16,16,NULL);
    else if (r->getCpp() == 3)
      b = gflAllockBitmapEx(GFL_RGB,d->mRaw->dim.x, d->mRaw->dim.y,16,8,NULL);
    else
      ThrowRDE("Unable to save image.");

    BitBlt(b->Data,b->BytesPerLine, r->getData(),r->pitch, r->dim.x*r->getBpp(), r->dim.y );

    GFL_SAVE_PARAMS s;
    gflGetDefaultSaveParams(&s);
    s.FormatIndex = gflGetFormatIndexByName("tiff");

    char ascii[1024];
    WideCharToMultiByte(CP_ACP, 0, f.Filename(), -1, ascii, 1024, NULL, NULL);
    string savename(ascii);
    size_t index = savename.rfind('.');
    replace(r->mode.begin(), r->mode.end(), ':', '-');
    savename = savename.substr(0,index).append("[").append(r->mode).append("].tiff");

    gflSaveBitmap((char*)savename.c_str(),b,&s);
    gflFreeBitmap(b);
#endif
  } catch (RawDecoderException e) {
    wchar_t uni[1024];
    MultiByteToWideChar(CP_ACP, 0, e.what(), -1, uni, 1024);
    //    MessageBox(0,uni, L"RawDecoder Exception",0);
    wprintf(L"Raw Decoder Exception:%s\n",uni);
  }
  if (d) delete d;
  if (m) delete m;

}

#else

// Test single file multiple times in corrupted state
// Used to test for states that might crash the app.

void OpenFile(FileReader f, CameraMetaData *meta) {
  RawDecoder *d = 0;
  FileMap* m = 0;
  wprintf(L"Opening:%s\n",f.Filename());
  try {
    m = f.readFile();
  } catch (FileIOException e) {
    printf("Could not open image:%s\n", e.what());
    return;
  }
  srand(0xC0CA1337);  // Hardcoded seed for re-producability (on the same platform)

  int tests = 200;
  // Try 50 permutations
  for (int i = 0 ; i < tests; i++) {  
    FileMap *m2 = m->clone();
    try {    
      // Insert 1000 random errors in file
      m2->corrupt(1000);
      RawParser t(m2);
      d = t.getDecoder();

      startTime = GetTickCount();

      d->decodeRaw();
      d->decodeMetaData(meta);
      RawImage r = d->mRaw;

      uint32 time = GetTickCount()-startTime;
      float mpps = (float)r->dim.x * (float)r->dim.y * (float)r->getCpp()  / (1000.0f * (float)time);
      wprintf(L"(%d/%d) Decoding %s took: %u ms, %4.2f Mpixel/s\n", i+1, tests*2, f.Filename(), time, mpps);
      if (d->errors.size())
        printf("%u Error Encountered.\n", d->errors.size());
/*      for (uint32 i = 0; i < d->errors.size(); i++) {
        printf("Error Encountered:%s\n", d->errors[i]);
      }*/
    } catch (RawDecoderException e) {
      wchar_t uni[1024];
      MultiByteToWideChar(CP_ACP, 0, e.what(), -1, uni, 1024);
      wprintf(L"Raw Decoder Exception:%s\n",uni);
    }
    delete m2;
    if (d)
      delete d;
    d = 0;
  }
  srand(0x77C0C077);  // Hardcoded seed for re-producability (on the same platform)
  wprintf(L"Performing truncation tests\n");
  for (int i = 0 ; i < tests; i++) {  
    // Get truncated file
    FileMap *m2 = m->cloneRandomSize();
    try {    
      RawParser t(m2);
      d = t.getDecoder();

      startTime = GetTickCount();
      d->decodeRaw();
      d->decodeMetaData(meta);
      RawImage r = d->mRaw;

      uint32 time = GetTickCount()-startTime;
      float mpps = (float)r->dim.x * (float)r->dim.y * (float)r->getCpp()  / (1000.0f * (float)time);
      wprintf(L"(%d/%d) Decoding %s took: %u ms, %4.2f Mpixel/s\n", i+1+tests, tests*2, f.Filename(), time, mpps);
      if (d->errors.size())
        printf("%u Error Encountered.\n", d->errors.size());
/*      for (uint32 i = 0; i < d->errors.size(); i++) {
        printf("Error Encountered:%s\n", d->errors[i]);
      }*/
    } catch (RawDecoderException e) {
      wchar_t uni[1024];
      MultiByteToWideChar(CP_ACP, 0, e.what(), -1, uni, 1024);
      wprintf(L"Raw Decoder Exception:%s\n",uni);
    }
    delete m2;
    if (d)
      delete d;
    d = 0;
  }
  delete m;
}
#endif

int wmain(int argc, _TCHAR* argv[])
{
  if (1) {  // for memory detection
#ifdef _USE_GFL_
  GFL_ERROR err;
  err = gflLibraryInit();
  if (err) {
    string errSt = string("Could not initialize GFL library. Library returned: ") + string(gflGetErrorString(err));
    return 1;
  }
#endif
  try {
    CameraMetaData meta("..\\data\\cameras.xml");
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz150_18.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz150_17.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz150_16.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz150_15.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz150_14.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz150_13.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz150_10.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz150_07.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz150_01.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Canon_PowerShot_S100-PS100hSLI0200.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Canon_PowerShot_S100-PS100LL64003.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Canon_PowerShot_S100-PS100LL32003.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Canon_PowerShot_S100-PS100LL16003.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Canon_PowerShot_S100-PS100LL08003.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Canon_PowerShot_S100-PS100LL04003.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Canon_PowerShot_S100-PS100LL00806.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Canon_PowerShot_S100-PS100hVFATB.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\samsung_ex1_07.srw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1hVFAI00200.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Pentax_Kx_IGP2252.PEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\nikon-p7100-200iso-nrstan-big.NRW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\nikon-p7100-400iso-nrstan-big.NRW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\E7DhMULTII00100.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\E7DhMULTII06400.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\E7DhMULTII12800.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\E7DhMULTII03200.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_a550-black.arw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_a550-white.arw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMC_LX5-P1020733.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMC_LX5-P1020732.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMC_LX5-P1020714.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMC_LX5-P1020713.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\PanasonicDMC-G3-FARI0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\PanasonicDMC-G3-INBI0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\PanasonicDMC-G3-LL02003.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\PanasonicDMC-G3-LL04006.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\PanasonicDMC-G3-LL08005.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\PanasonicDMC-G3-hREST.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\PanasonicDMC-G3-hSLI0200_NR1.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\PanasonicDMC-G3-hVFAI0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf3_07.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf3_15.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf3_18.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf3_04.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf3_03.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf3_01.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMCG1-aspect3x2_detail_on_the_right.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMCG1-aspect3x2.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic-FZ28-FZ28VFAWL.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic-FZ28-FZ28hMULTII0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic-FZ28-p1030602.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic-FZ28-p1030657.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gh2_DMCGH2hHOUSE.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gh2_DMCGH2hVFAI00200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gh2_DMCGH2hVFAI06400.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gh2_01.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gh2_05.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gh2_07.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gh2_12.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gh2_06.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf2_05.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf2_08.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf2_02.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf2_09.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gf2_10.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMCG2hSLI0200_NR1.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_LX5FARI0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_LX5FARWTT.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_LX5hRESM.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_LX5INBI00200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_LX5LL002003.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_LX5hSLI00200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_LX5hVFATB.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lx-3_3-2.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lx-3_16-9.RW2"),&meta);

    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz100_09.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz100_07.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz100_03.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz100_08.rw2"),&meta);

    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz45_04.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz45_01.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz45_08.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_fz45_12.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMCG2hVFATB.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMCG2hMULTII0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_g10_07.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMCG2FARI0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_g10_12.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_g10_06.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_g10_02.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic DMC-LX3.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_G1-2.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_LX3.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic DMC-LX3.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A230_1.arw"),&meta);

    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ35FARI0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ35hSLI0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ35hVFAWB.RW2"),&meta);

    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMCGF1hSLI0200_NR_LOW.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMCGF1hMULTII0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_lx3_02.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\gh1_sample_iso100.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\gh1_sample_iso400.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\gh1_studio_iso100.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\gh1_studio_iso1600.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMC-FX150.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ28.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_lx3_01.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_lx3_02.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_lx3_03.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_lx3_04.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_lx3_05.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_lx3_06.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_LX3.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_DMC_gh1_sample_iso100.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_DMC_gh1_sample_iso400.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_DMC_gh1_studio_iso100.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_DMC_gh1_studio_iso1600.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_DMC-G1hMULTII0200.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_DMC-G1hSLI0400.RW2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_g1_04_portrait.rw2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\panasonic_lumix_dmc_gh1_02_portrait.rw2"),&meta);

    OpenFile(FileReader(L"..\\testimg\\nikon-v1-100iso-nrstan-big.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\nikon-v1-1600iso-nrstan-big.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\nikon-v1-3200iso-nrstan-big.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\nikon-v1-6400iso-nrstan-big.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\nikon-p7100-100iso-nrstan-big.NRW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\nikon-p7100-800iso-nrstan-big.NRW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\nikon-p7100-1600iso-nrstan-big.NRW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\nikon-p7100-3200iso-nrstan-big.NRW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\nikon-p7100-6400iso-nrstan-big.NRW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1INBI0100.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1LL32003.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1LL64003.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1hHOUSE_M1.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1hVFAI0100.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1hSLI0100NR0.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1hVFAI0200.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1hVFAI0400.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1LL01003.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1LL02003.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1LL04003.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1LL08003.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_J1-J1LL16003.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1LL002003.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1LL004003.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1LL008003.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1LL016003.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1LL032003.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1hHOUSE.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1hREST.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1LL064003.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1hSLI00200NR_OFF.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1LL128003.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_E-PM1-EPM1hVFAI00200.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Pentax_Kx_IGP2252.PEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\pentax_kx_03.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\pentax_kx_04.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\pentax_kx_10.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\pentax_kx_12.pef"),&meta);

    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35hVFAWB.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35hVFAI00200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35hSLI12800NRA.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35hSLI00200NRA.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35LL128003.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35LL064003.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35LL032003.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35LL016003.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35LL008003_full.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35LL004003_full.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35LL002003.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35LL001003.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35INBI00200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony SLT-A35-AA35FARI0200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\olympus_epl3_31.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_30.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_24.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_29.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_28.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_27.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_26.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_25.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_17.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_20.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_11.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\olympus_epl3_02.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NLL004003.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NINBI00100.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NhSLI00200_NR_LOW.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NLL256003.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NLL128003.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NLL064003.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NhVFAI00400.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NhVFAI00200.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NLL032003.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NLL008006.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NLL008003.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-5N-NEX5NFARI0200.ARW"),&meta);

      OpenFile(FileReader(L"..\\testimg\\Sony NEX-C3-NEXC3hVFAI00200.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-C3-NEXC3hSLI00400NRA.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-C3-NEXC3hREST.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-C3-NEXC3LL128006.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-C3-NEXC3LL032003.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-C3-NEXC3LL008006.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-C3-NEXC3LL004006.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-C3-NEXC3LL002006.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-C3-NEXC3FARI6400.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony NEX-C3-NEXC3FARI0200.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony Alpha SLT-A77-AA77FARI0200.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony Alpha SLT-A77-AA77hSLI00050NR1.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony Alpha SLT-A77-AA77hSLI00100NR1.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony Alpha SLT-A77-AA77hSLI03200NR1.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony Alpha SLT-A77-AA77hSLI16000NR1.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony Alpha SLT-A77-AA77hVFAI00200.ARW"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Sony Alpha SLT-A77-AA77hVFAWS_DISTORT_OFF.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Phase One H25 Capture One PRO 3.7.10 IIQ Raw Large-001.tif"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Phase One H25 Capture One PRO 3.7.10 IIQ Raw Small-001.tif"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Phase One H25 Capture One PRO 3.7.10 Raw Compatible with 3.0-001.Cap"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Phase One H25 Capture One PRO 3.7.10 Raw Compatible with 3.1-001.tif"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Phase One H25 Capture One PRO 4.8.3 IIQ 001.IIQ"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Phase One H25 Capture One PRO 4.8.3 TIF 001.TIF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Phase One H25 Capture One PRO 5.2.1 IIQ-001.IIQ"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Phase One H25 Capture One PRO 5.2.1 TIF-001.TIF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Phase One H25 Capture One PRO 6.0.1 IIQ-001.IIQ"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Phase One H25 Capture One PRO 6.0.1 TIF-001.TIF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3INBI00200.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3INBI00800.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3LL002004.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3INBI12800.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3LL004005.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3LL008007.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3LL016007XNR.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3LL128007.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3hSLI00400NR0.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3hSLI01600NR0.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3hVFAI00200.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_PEN_E-P3-EP3hVFAWB.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\_DSC5230.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D5100-dsc_0081.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_D5100-dsc_0064.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_D5100-dsc_0060.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_D5100-dsc_0059.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_D5100-D5100LL002004.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_D5100-D5100hVFAI00400.NEF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Nikon_D5100-D5100hSLI00200_NR_0.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Samsung-NX-5_SAM0252.SRW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K200D-_IGP9477.DNG"),&meta);
      OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K7_IGP3526.DNG"),&meta);
      OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K7-_IGP3528.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_E5700.nef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_E5700_(sRGB).nef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_E8400-DSCN0947.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\NikonCoolPix8800.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_E5400.nef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\350d-color_problem.cr2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\samsung_nx100_02.srw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\samsung_ex1_10.srw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\olympus_xz1_26.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\olympus_xz1_06.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\olympus_xz1_05.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\olympus_xz1_08.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\olympus_xz1_01.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\samsung_ex1_07.srw"),&meta);

    OpenFile(FileReader(L"..\\testimg\\Olympus-EP2hVFAO.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus-EP2hSLI0200NR0.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus-EP2hRESM.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus-EP2FARWTT.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus-EP2FARI0200.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_500UZ.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_C7070WZ.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_C8080.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E1.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E10.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E20.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E3-2.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E3-3.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E3-4.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E3.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E300.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E330.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E400.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E410-2.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E410.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E420.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E500.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E510-2.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E510.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E520-2.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E520-3.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E520-4.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E520-5.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E520.orf"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_SP350.orf"),&meta);
      OpenFile(FileReader(L"..\\testimg\\samsung_nx10_05.srw"),&meta);
      OpenFile(FileReader(L"..\\testimg\\samsung_nx10_08.srw"),&meta);
      OpenFile(FileReader(L"..\\testimg\\samsung_nx10_11.srw"),&meta);
      OpenFile(FileReader(L"..\\testimg\\samsung_nx10_12.srw"),&meta);
      OpenFile(FileReader(L"..\\testimg\\samsung_nx100_04.srw"),&meta);
      OpenFile(FileReader(L"..\\testimg\\samsung_nx100_12.srw"),&meta);
      OpenFile(FileReader(L"..\\testimg\\samsung_nx100_13.srw"),&meta);
      OpenFile(FileReader(L"..\\testimg\\samsung_wb2000_05.srw"),&meta);
      OpenFile(FileReader(L"..\\testimg\\samsung_wb2000_11.srw"),&meta);
      OpenFile(FileReader(L"..\\testimg\\samsung_wb2000_12.srw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Canon_EOS_600D-T3IhSLI00100_NR_OFF.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E20.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Leica-X1-L1090994.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Leica-X1-ISO100-L1090324.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Leica_M8.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\leica_m82_01.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\leica_m82_07.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\leica_m82_09.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\leica_m82_11.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Leica_M_8.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-KXhMULTII12800.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\CRW_0740.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E520-4.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Adobe-DNG-Converter-0425-IMG_0530.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Adobe-DNG-Converter-IMG_2312(210609).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Adobe-DNG-Converter-IMG_7903.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\5d-raw.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\5d.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\CANON-EOS10-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\CANON-EOS10.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\CANON-EOS20D-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\CANON-EOS20D.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\CANON-EOS300D-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\CANON-POWERSHOTPRO1-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\CANON-POWERSHOTPRO1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_1000D.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_1Ds_Mk2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_1Ds_Mk3-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_1Ds_Mk3.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_1D_Mk2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_1D_Mk2_N.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_1D_Mk3.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_20D-demosaic.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_20d.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_30D-uga1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_30D-uga2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_30D.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_350d-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_350D-3.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_350d.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_400D.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_40D-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_40D.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_450D-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_450D-3.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_450D-4.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_450D-5.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_450D.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_5D.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_5D_Mk2-ISO100_sRAW1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_5D_Mk2-ISO12800_sRAW1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_5D_Mk2-ISO12800_sRAW2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_EOS_Mk2-ISO100_sRAW2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_Powershot_G10.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_Powershot_G9-1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_Powershot_G9-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Canon_PowerShot_G9.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\FUJI-FINEPIXS2PRO-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\FUJI-FINEPIXS2PRO.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\KODAK-DCSPRO.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\M8-1-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\M8-1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DIMAGE5-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DIMAGE5.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DIMAGE7HI-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DIMAGE7HI.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DIMAGEA1-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DIMAGEA1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DYNAX7D-01-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DYNAX7D-01.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DYNAX7D-02-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DYNAX7D-02.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DYNAX7D-03-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DYNAX7D-03.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DYNAX7D-04-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DYNAX7D-04.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DYNAX7D-05-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\MINOLTA-DYNAX7D-05.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\NIKON-COOLPIX5700-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\NIKON-COOLPIX5700.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\NIKON-D100-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\NIKON-D100.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\NIKON-D70-01-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\NIKON-D70-01.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\NIKON-D70-02-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\NIKON-D70-02.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\NikonCoolPix8800.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D100-1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D1H.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D1X.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D200-1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D200_compressed-1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D2H.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D2X_sRGB.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D3.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D300.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D40X.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D40_(sRGB).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D60-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D60.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D70.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D700.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D70s-3.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D80_(sRGB).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_D90.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_E5400.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_E5700.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Nikon_E5700_(sRGB).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\OLYMPUS-C5050Z-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\OLYMPUS-C5050Z.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\OLYMPUS-E10-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\OLYMPUS-E10.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_500UZ.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_C7070WZ.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_C8080.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E10.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E3-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E3-3.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E3-4.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E3.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E300.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E330.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E400.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E410-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E410.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E420.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E500.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E510-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E510.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E520-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E520-3.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E520-4.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E520-5.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_E520.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Olympus_SP350.dng"),&meta);

    OpenFile(FileReader(L"..\\testimg\\dng\\Panasonic_DMC-FX150(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\panasonic_DMC-G1FARI0200(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\panasonic_DMC-G1hMULTII0200(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\panasonic_DMC-G1hSLI0400(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\panasonic_DMC-G1LL0207LENROFF(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Panasonic_FZ28(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\panasonic_lumix_dmc_lx3_01(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\panasonic_lumix_dmc_lx3_02(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\panasonic_lumix_dmc_lx3_03(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\panasonic_lumix_dmc_lx3_04(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\panasonic_lumix_dmc_lx3_05(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\panasonic_lumix_dmc_lx3_06(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Panasonic_LX3(010909).dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Panasonic_LX3(300109).dng"),&meta);

    OpenFile(FileReader(L"..\\testimg\\dng\\PENTAX-ISD-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\PENTAX-ISD.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Pentax_K100D.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Pentax_K10D.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Pentax_K20D.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\SIGMA-SD10-linear.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\SIGMA-SD10.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\SONY-DSLR-A700.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\SONY_A200.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Sony_A300.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Sony_DSLR-A100-1.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Sony_DSLR-A350.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Sony_DSLR-A900-2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\Sony_DSLR-A900.dng"),&meta);

    OpenFile(FileReader(L"..\\testimg\\dng\\uncompressed.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\uncompressed2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\dng\\uncompressed3.dng"),&meta);


    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K200DFARI0100.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K200DFARI1600.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K20DFARI0100_43MM.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K20DFARI0200_43MM.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K20DFARI0400_43MM.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K20DFARI0800_43MM.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K20DFARI1600_43MM.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K20DFARI3200_43MM.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K20DFARI6400_43MM.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K7FARI0200.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K7FARI6400.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K7hMULTII0200.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K7hVFAO.DNG"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Ricoh_GR2.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\ricoh_gr_digital_iii_01.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\ricoh_gr_digital_iii_07.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\ricoh_gr_digital_iii_09.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\ricoh_gr_digital_iii_10.dng"),&meta);
    OpenFile(FileReader(L"..\\testimg\\camera_dngs\\ricoh_gr_digital_iii_14.dng"),&meta);


    OpenFile(FileReader(L"..\\testimg\\Canon_EOS_350d.cr2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\500D_NR-Std_ISO1600.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\500D_NR-Std_ISO1600.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\500D_NR-Std_ISO6400.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_500D-2.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon-500D.CR2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1100D-T3hSLI0100_NR_OFF.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1100D-T3hSLI3200_NR_OFF.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1100D-T3hVFAI0100.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1100D-T3hVFATO.CR2"),&meta);

      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_600D-T3IhSLI00200_NR_OFF.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_600D-T3IhVFAI00200.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_600D-T3IINBI00200.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_600D-T3INBI0200.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_60D-E60DFARI00200.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_60D-E60DFARI12800.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_60D-E60DhRESM.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_60D-E60DhSLI00100_NR_OFF.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_60D-E60DhSLI12800_NR_OFF.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_60D-E60DhVFAI00100.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_60D-E60DINBI00200_P2.CR2"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Canon_EOS_60D-E60DLL001006.CR2"),&meta);

    OpenFile(FileReader(L"..\\testimg\\Canon_EOS_400D.cr2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_EPL2-VFAI0200.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_EPL2-SLI0400NR0.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_EPL2-RESM.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_EPL2-L02007.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_EPL2-NBI6400.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_EPL2-NBI0400.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\Olympus_EPL2-ARWTM.ORF"),&meta);
      OpenFile(FileReader(L"..\\testimg\\5d-ISO-200.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\5d-ISO-H2.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_40D.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_G12FARI0100.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_G12hREST.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_G12hSLI0200.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_G12hVFATL.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_powershot_s95_15.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_powershot_s95_07.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_powershot_s95_14.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_powershot_s95_02.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_powershot_s95_09.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_60D-1.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_60D-2.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon-7d-IMG_0011.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon-7d-IMG_0012.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_550D_T2IhHOUSE.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_550D_T2IhMULTII00200.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_550D_T2IhRESM.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_550D_T2IhSLI00200_NR0.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon-7D.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon-1D-Mk4-A28C0180.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon-1D-Mk4-DD9C0097.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon-1D-Mk4-DD9C0069.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_5DMk2-sRaw2.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_450D.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_5DMk2-sRaw1.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_5D_Mk2-ISO100_sRAW1.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_50D-1.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_50D-2.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_50D-3.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_50D-4.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\kp.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1Ds_Mk2.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\5d.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1Ds_Mk3-2.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_20D-demosaic.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_30D.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_450D.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_450D-2.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_Powershot_G10.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_PowerShot_G9.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1D_Mk2.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1000D.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1D_Mk3.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1Ds_Mk3.cr2"),&meta);

  OpenFile(FileReader(L"..\\testimg\\500D_NR-Std_ISO1600.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_eos_1000d_01.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_eos_1000d_06.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_1D_Mk2_N.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_30D-uga1.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_350D-3.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_450D-4.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_50D.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_Powershot_G9-1.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_Mk2-ISO100_sRAW2.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_EOS_7DhMULTII00200.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_Powershot_SX1IShMULTII1600.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_Powershot_SX1ISFARI0200.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_Powershot_SX1IShMULTII0200.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_Powershot_SX1IShSLI0080.CR2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Canon_Powershot_SX1IShSLI0200.CR2"),&meta);

  OpenFile(FileReader(L"..\\testimg\\canon_powershot_g11_02.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_powershot_g11_07.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_powershot_g11_08.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_powershot_s90_02.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_powershot_s90_03.cr2"),&meta);
  OpenFile(FileReader(L"..\\testimg\\canon_powershot_s90_04.cr2"),&meta);

    OpenFile(FileReader(L"..\\testimg\\Canon_EOS_5D.cr2"),&meta);
    OpenFile(FileReader(L"..\\testimg\\20101222_IMGP3849.PEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\20101204_IMGP2730.PEF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Olympus_E5-L01004.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E5-L02006.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E5-HOUSE.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E5-SLI0200_NR_STD.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Olympus_E5-VFAI00200.ORF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Pentax_K-5-IMGP2058.PEF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Pentax_K-5-IMGP2032.PEF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Pentax_K-5-IMGP2028.PEF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\pentax_kx_03.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Pentax_K10D.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Pentax_K100D.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Pentax_K10D.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Pentax_K20D.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Pentax_optio_33wr.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Pentax_K200D-2.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\pentax_kr_06.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\pentax_kr_12.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\pentax_kr_01.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\pentax_kr_07.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\pentax_kr_03.pef"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Pentax_K10D-2.dng"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_A580hVFAI12800.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A580hVFAI00200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A580hSLI00100_NR_WEAK.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A580hREST.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A580hHOUSE.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D3100hVFAL.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D3100hVFAI00200.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D3100hSLI00200_NR_OFF.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D3100LL128007XNR.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D3100LL032004.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D3100LL001007.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D3100INBI00200.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D3100FARI06400.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D3100FARI00200.NEF"),&meta);

  OpenFile(FileReader(L"..\\testimg\\Nikon_D7000hVFAO.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D7000hSLI00200_NR3.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D7000hREST.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D7000LL004003.NEF"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_D7000LL001005.NEF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_A290FARI0200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A290hREST.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A290hSLI0100_NR_OFF.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A290hSLI0400_NR_OFF.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A290hVFAI0200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A290hVFATB.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_P7000hRESM.NRW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\nikon_coolpix_p6000_05.nrw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_P7000LL0200.NRW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_P7000hSLI03200.NRW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_P7000hVFATL.NRW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_P7000hSLI00200.NRW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Nikon_P7000FARI0200.NRW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA55-FAR4912.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA55-FARI00400.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA55-LL002003.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA55-hREST.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA55-hSLI00200NRA.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA55-hVFAI00200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA560-FAR4592.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA560-FARI00200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA560-INBI00200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA560-LL002005.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA560-hREST.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA560-hSLI00200NRW.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA560-hVFAI00200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA33-FAR4592.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA33-FARI00200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA33-LL002003.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA33-hREST.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA33-hSLI00200NRW.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_AA33-hVFAI00200.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\sonya55-ISO100.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\sonya55-ISO100-2.ARW"),&meta);
OpenFile(FileReader(L"..\\testimg\\sony_a390_ISO100.ARW"),&meta);
OpenFile(FileReader(L"..\\testimg\\sony_a390_ISO200.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Panasonic-FZ50.RAW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ18-6.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ18-5.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ18-4.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ18-3.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMC_L10.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ18.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ18-2.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_L1.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ8-2.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ8.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ50_2.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_LX2.RAW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_DMC_LX2.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ30.raw"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Panasonic_FZ50.raw"),&meta);

  OpenFile(FileReader(L"..\\testimg\\Sony-NEX3hVFA70L.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX3hSLI00200_NRW.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX3hREST.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX3hMULTI00200.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX3FARWTT.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX3FARI00200.ARW"),&meta);

  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A100-1.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A100-2.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A100-3.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A100-4.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A100-5.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A100-6.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A100-7.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A100-8.arw"),&meta);

  
  OpenFile(FileReader(L"..\\testimg\\sony_a450_03.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\sony_a450_04.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\sony_a450_06.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\sony_a450_08.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\sony_a450_09.arw"),&meta);

  OpenFile(FileReader(L"..\\testimg\\Sony-NEX5FARI00200.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX5hMULTII00200.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX5hMULTII00200.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX5hREST.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX5hSLI00200_NRW.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX5hSLI06400_NRW.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-NEX5hVFATL.ARW"),&meta);

  
  OpenFile(FileReader(L"..\\testimg\\Nikon-D3000hMULTII0200.NEF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon-D3000hSLI0200.NEF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon-D3x_ISO100.NEF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Olympus-E620_NF-Std_ISO100.ORF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-A500-hMULTII00200.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony-A500-hSLI00200_NR_1D.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Olympus-EPL1hVFATB.ORF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Olympus-EPL1hSLI0200NR0.ORF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Olympus-EPL1hREST.ORF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Olympus-EPL1hMULTII0200NR2D.ORF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Olympus-EPL1hHOUSE.ORF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Ricoh_GXR-A12-real_iso200.DNG"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D50.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Olympus_E30.orf"),&meta);


  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\ricoh_gr_digital_iii_01.dng"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\ricoh_gr_digital_iii_07.dng"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\ricoh_gr_digital_iii_09.dng"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\ricoh_gr_digital_iii_10.dng"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\ricoh_gr_digital_iii_14.dng"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K7FARI0200.DNG"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K7FARI6400.DNG"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K7hMULTII0200.DNG"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Pentax-K7hVFAO.DNG"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Leica_M8.dng"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Leica_M_8.dng"),&meta);  
  OpenFile(FileReader(L"..\\testimg\\sony_a330_02.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\sony_a330_04.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\sony_a330_05.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\sony_a330_06.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Olympus-E-620-1.ORF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\camera_dngs\\Leica-X1-L1090229.DNG"),&meta);
  OpenFile(FileReader(L"..\\testimg\\dng\\CANON-EOS300D.dng"),&meta);
  OpenFile(FileReader(L"..\\testimg\\dng\\KODAK-DCSPRO-linear.dng"),&meta);

  

  OpenFile(FileReader(L"..\\testimg\\SONY-DSLR-A700.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\SONY_A200.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_A300.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A100-1.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A350.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A900-2.arw"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_DSLR-A900.arw"),&meta);

  OpenFile(FileReader(L"..\\testimg\\Sony_a700_ISO1600_compressed.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_a700_ISO1600_uncompressed.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_a700_ISO200_compressed.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_a700_ISO200_uncompressed.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_a700_ISO6400_compressed.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_a700_ISO6400_uncompressed.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A900_ISO1600_uncompressed.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A900_ISO3200_uncompressed.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A900_ISO400_uncompressed.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A900_ISO6400_uncompressed.ARW"),&meta);
    OpenFile(FileReader(L"..\\testimg\\Sony_A900_ISO800_uncompressed.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D1.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D100-backhigh.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D200_compressed-1.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D1H.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D1X.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D2H.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D2X_sRGB.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D100-1.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D200-1.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D3.nef"),&meta); 
  OpenFile(FileReader(L"..\\testimg\\Nikon_D300.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D40X.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D40_(sRGB).nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D60-2.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D60.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D70.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D700.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D70s-3.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D80_(sRGB).nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Nikon_D90.nef"),&meta);



  OpenFile(FileReader(L"..\\testimg\\Sony_A550hVFAWB.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_A550hVFATB.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_A550hSLI00200_NR1D.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_A550hMULTII00200.ARW"),&meta);
  OpenFile(FileReader(L"..\\testimg\\Sony_A550FARI0200.ARW"),&meta);

  OpenFile(FileReader(L"..\\testimg\\nikon_d3s_Ycircus_vidrig_102400.NEF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\nikon_d3s_Ycircus_dogjump3_2500.NEF"),&meta);
  OpenFile(FileReader(L"..\\testimg\\nikon_d3s_Ycircus_granny_10000.NEF"),&meta);

  OpenFile(FileReader(L"..\\testimg\\nikon_d300s_01.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\nikon_d300s_03.nef"),&meta);
  OpenFile(FileReader(L"..\\testimg\\nikon_d300s_06.nef"),&meta);

  OpenFile(FileReader(L"..\\testimg\\Nikon-D3XFARI0100.NEF"),&meta);

  } catch (CameraMetadataException e) {

  }
  MessageBox(0,L"Finished", L"Finished",0);
#ifdef _USE_GFL_
  gflLibraryExit();
#endif
  } // Dump objects
  _CrtDumpMemoryLeaks();
	return 0;
}

