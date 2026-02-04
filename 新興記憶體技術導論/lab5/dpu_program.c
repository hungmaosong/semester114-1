#include <alloc.h>
#include <mram.h>
#include "configure.h"


/* MRAM：全部字串＋全部結果 */
__mram_noinit __attribute__((aligned(8))) char    strings[NUM_STRINGS * STRING_LENGTH];
__mram_noinit __attribute__((aligned(8))) int64_t results[NUM_STRINGS];

/* WRAM + __host：Host 傳進來的控制參數 */
__host char input_string[STRING_LENGTH];
__host int  start_index;   // 這顆 DPU 從第幾條字串開始算（全域 index）
__host int  count;         // 這顆 DPU 要算幾條字串

/* 為了保證對齊，放在 global 並加 __dma_aligned */
__dma_aligned char    local_str[STRING_LENGTH];
__dma_aligned int64_t sum64;

int main()
{
    for (int k = 0; k < count; ++k) {
        int i = start_index + k;  // 全域第 i 條字串

        // 從 MRAM 讀出第 i 條字串
        mram_read(
            &strings[i * STRING_LENGTH],
            local_str,
            STRING_LENGTH * sizeof(char)  // = 8 bytes
        );

        // 算 Hamming distance
        int sum = 0;
        for (int j = 0; j < STRING_LENGTH; ++j) {
            if (local_str[j] != input_string[j]) {
                sum++;
            }
        }

        // 結果寫回 MRAM 對應的 results[i]
        sum64 = sum;
        mram_write(
            &sum64,
            &results[i],
            sizeof(int64_t)              // 8 bytes，符合要求
        );
    }

    return 0;
}
