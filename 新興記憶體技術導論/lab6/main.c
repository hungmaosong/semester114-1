#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "configure.h"
#include "dpu_host.h"
/* 註：該檔案是整個程式的進入點，不需修改任何程式碼。 */

void check_results(int *cpu_results, int *dpu_results)
{
    for (int i = 0; i < NUM_STRINGS; ++i)
    {
        if (cpu_results[i] != dpu_results[i])
        {
            printf("[ERROR!!!] DPU results do NOT match CPU results!\n");
            return;
        }
    }
    printf("DPU results match CPU results.\n");
    return;
}

void CPU_calculate_hamming_distance(char strings[NUM_STRINGS][STRING_LENGTH], char input_string[STRING_LENGTH], int results[NUM_STRINGS])
{
    for (int i = 0; i < NUM_STRINGS; ++i)
    {
        // 計算input_str與word[i]的hamming distance
        int sum = 0;
        for (int j = 0; j < STRING_LENGTH; ++j)
            if (strings[i][j] != input_string[j])
                sum++;

        // 將結果寫入results[i]
        char tmp[10];
        memcpy(tmp, strings[i], 8);
        tmp[8] = '\0';
        results[i] = sum;
    }
    return;
}

// 程式的進入點
int main()
{

    // 產生多條字串(strings)
    char strings[NUM_STRINGS][STRING_LENGTH];
    for (int i = 0; i < NUM_STRINGS; ++i)
    {
        for (int j = 0; j < STRING_LENGTH; ++j)
            strings[i][j] = 'a' + (rand() % 26);
    }

    // 產生單條輸入字串資料(input_string)，待會要一一和words裡的每條字串計算Hamming distance。
    char input_string[STRING_LENGTH];
    for (int i = 0; i < STRING_LENGTH; ++i)
        input_string[i] = 'a' + (rand() % 26);

    // 分配results的記憶體空間
    int *cpu_results = malloc(sizeof(int) * NUM_STRINGS);
    int *dpu_results = malloc(sizeof(int) * NUM_STRINGS);

    // 使用CPU計算hamming distance
    CPU_calculate_hamming_distance(strings, input_string, cpu_results);

    // 使用DPU計算hamming distances (你需要完成的函式！)
    DPU_calculate_hamming_distance(strings, input_string, dpu_results);

    // 比較DPU的計算結果是否和CPU的結果相同
    check_results(cpu_results, dpu_results);
}
