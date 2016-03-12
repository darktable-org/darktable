#include "StdAfx.h"
#include "FileWriter.h"
#if defined(__unix__) || defined(__APPLE__) 
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
//#include <sys/mman.h>
#endif // __unix__
/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post

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

FileWriter::FileWriter(LPCWSTR _filename) : mFilename(_filename) {
}

void FileWriter::writeFile(FileMap* filemap, uint32 size) {
  if (size > filemap->getSize())
    size = filemap->getSize();
#if defined(__unix__) || defined(__APPLE__) 
  size_t bytes_written = 0;
  FILE *file;
  char *src;

  file = fopen(mFilename, "wb");
  if (file == NULL)
    throw FileIOException("Could not open file.");

  src = (char *)filemap->getData(0, filemap->getSize());
  bytes_written = fwrite(src, 1, size ? size : filemap->getSize(), file);
  fclose(file);
  if (size != bytes_written) {
    throw FileIOException("Could not write file.");
  }

#else // __unix__
  HANDLE file_h;  // File handle
  file_h = CreateFile(mFilename, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (file_h == INVALID_HANDLE_VALUE) {
    throw FileIOException("Could not open file.");
  }

  DWORD bytes_written;
  if (! WriteFile(file_h, filemap->getData(0, filemap->getSize()), size ? size : filemap->getSize(), &bytes_written, NULL)) {
    CloseHandle(file_h);
    throw FileIOException("Could not read file.");
  }
  CloseHandle(file_h);

#endif // __unix__
}

FileWriter::~FileWriter(void) {

}

} // namespace RawSpeed
