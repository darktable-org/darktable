#ifndef STRPTIME_H
#define STRPTIME_H

#ifdef _WIN32
    char* strptime(const char* buf, const char* format, struct tm* tm);
#endif

#endif