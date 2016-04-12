#pragma once

float *read_pfm(const char *filename, int *wd, int *ht);
void write_pfm(const char *filename, int width, int height, float *data);
