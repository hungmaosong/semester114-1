#include <alloc.h>
#include <mram.h>
#include <defs.h>
#include <mutex.h>
#include <barrier.h>
#include "configure.h"

__mram_noinit char strings[100000]; // 整理成1維後的字串資料
__mram_noinit int results[1000];    // Hamming distance 結果
__host char input_string[1000];
__host int size; // 這次要計算的字串總數

// mutex：保護 MRAM write（避免 4B write race condition）
MUTEX_INIT(result_mutex);

int main()
{
    uint32_t tid = me(); // 取得 tasklet id （問系統：我是第幾個 tasklet?）

    __dma_aligned char str[STRING_LENGTH]; // 每個 tasklet 都有自己的 WRAM buffer （每個tasklet自己的工作桌)

    // stride-based 分工
    // tasklet 0: i = 0, 4, 8, ...
    // tasklet 1: i = 1, 5, 9, ...
    // tasklet 2: i = 2, 6, 10, ...
    // tasklet 3: i = 3, 7, 11, ...
    for (int i = tid; i < size; i += NR_TASKLETS)
    {
        int sum = 0;

        // 1. 從 MRAM 讀字串到 WRAM
        mram_read(strings + i * STRING_LENGTH,
                  str,
                  sizeof(char) * STRING_LENGTH);

        // 2. 計算 Hamming distance
        for (int j = 0; j < STRING_LENGTH; j++)
        {
            sum += (str[j] != input_string[j]);
        }

        // 3. 寫回結果（用 mutex 保護，避免 race）
        mutex_lock(result_mutex);
        results[i] = sum;
        mutex_unlock(result_mutex);
    }

    return 0;
}
