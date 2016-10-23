#ifndef __GETDELIM_H__
#define __GETDELIM_H__

ssize_t getdelim (char **lineptr, size_t *n, int delimiter, FILE *fp);
ssize_t getline (char **lineptr, size_t *n, FILE *stream);

#endif //__GETDELIM_H__