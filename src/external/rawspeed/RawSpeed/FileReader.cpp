#include "StdAfx.h"
#include "FileReader.h"
#ifdef __unix__
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
//#include <sys/mman.h>
#endif // __unix__
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

namespace RawSpeed {

FileReader::FileReader(LPCWSTR _filename) : mFilename(_filename) {
}

FileMap* FileReader::readFile() {
#ifdef __unix__
  int bytes_read = 0;
  FILE *file;
  char *dest;
  long size;

  file = fopen(mFilename, "rb");
  if (file == NULL)
    throw FileIOException("Could not open file.");
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  if (size <= 0) {
    fclose(file);
    throw FileIOException("File is 0 bytes.");
  }
  fseek(file, 0, SEEK_SET);

#if 0
  // Not used, as it is slower than sync read

  uchar8* pa = (uchar8*)mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  FileMap *fileData = new FileMap(pa, size);

#else
  FileMap *fileData = new FileMap(size);

  dest = (char *)fileData->getDataWrt(0);
  bytes_read = fread(dest, 1, size, file);
  fclose(file);
  if (size != bytes_read) {
    delete fileData;
    throw FileIOException("Could not read file.");
  }
#endif

#else // __unix__
  HANDLE file_h;  // File handle
  file_h = CreateFile(mFilename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (file_h == INVALID_HANDLE_VALUE) {
    throw FileIOException("Could not open file.");
  }

  LARGE_INTEGER f_size;
  GetFileSizeEx(file_h , &f_size);

  if (!f_size.LowPart)
    throw FileIOException("File is 0 bytes.");

  FileMap *fileData = new FileMap(f_size.LowPart);

  DWORD bytes_read;
  if (! ReadFile(file_h, fileData->getDataWrt(0), fileData->getSize(), &bytes_read, NULL)) {
    CloseHandle(file_h);
    delete fileData;
    throw FileIOException("Could not read file.");
  }
  CloseHandle(file_h);

#endif // __unix__
  return fileData;
}

FileReader::~FileReader(void) {

}

} // namespace RawSpeed
