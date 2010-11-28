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
  struct stat st;
  int bytes_read = 0;
  int fd;
  char *dest;

  stat(mFilename, &st);
  fd = open(mFilename, O_RDONLY);
  if (fd < 0)
    throw FileIOException("Could not open file.");
#if 0
  // Not used, as it is slower than sync read

  uchar8* pa = (uchar8*)mmap(0, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  FileMap *fileData = new FileMap(pa, st.st_size);

#else
  FileMap *fileData = new FileMap(st.st_size);

  while ((st.st_size > 0) && (bytes_read < st.st_size)) {
    printf("size: %d %d\n", st.st_size, bytes_read);
    dest = (char *) fileData->getDataWrt(bytes_read);
    ssize_t ret = read(fd, dest, st.st_size - bytes_read);
    if(ret < 0)
    {
      perror("stat read");
      break;
    }
    bytes_read += ret;
  }
  close(fd);
#endif

#else // __unix__
  HANDLE file_h;  // File handle
  file_h = CreateFile(mFilename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (file_h == INVALID_HANDLE_VALUE) {
    throw FileIOException("Could not open file.");
  }

  LARGE_INTEGER f_size;
  GetFileSizeEx(file_h , &f_size);

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
