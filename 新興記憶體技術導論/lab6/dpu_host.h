#ifndef DPU_HOST_H
#define DPU_HOST_H

#include "configure.h"

void DPU_calculate_hamming_distance(char strings[NUM_STRINGS][STRING_LENGTH], 
                                    char input_str[STRING_LENGTH], 
                                    int results[NUM_STRINGS]);

#endif