#include <dpu.h>
#include "configure.h"


/* Host 端用 DPU 計算 Hamming distance */
void DPU_calculate_hamming_distance(char strings[NUM_STRINGS][STRING_LENGTH],char input_string[STRING_LENGTH],int results[NUM_STRINGS])
{
    // 分配 DPU 數量與載入 DPU 程式碼
    struct dpu_set_t set, dpu;
    DPU_ASSERT(dpu_alloc(NUM_DPUS, "backend=simulator", &set));
    DPU_ASSERT(dpu_load(set, "dpu_program", NULL));

    /* 1. 把二維 strings 攤平成一維，方便一次 copy 給 DPU */
    char flat_strings[NUM_STRINGS * STRING_LENGTH];
    for (int i = 0; i < NUM_STRINGS; ++i) {
        for (int j = 0; j < STRING_LENGTH; ++j) {
            flat_strings[i * STRING_LENGTH + j] = strings[i][j];
        }
    }

    /* 2. 算每顆 DPU 要處理幾條字串（start + count） */
    int base   = NUM_STRINGS / NUM_DPUS;
    int remain = NUM_STRINGS % NUM_DPUS;

    int offset = 0;
    unsigned int idx = 0;

    /* 3. 對每顆 DPU：丟整份 strings，還有 input_string + start_index + count */
    DPU_FOREACH(set, dpu, idx) {
        int count = base + (idx < (unsigned int)remain ? 1 : 0);
        int start = offset;  // 這顆 DPU 從第幾條字串開始算（全域 index）

        // 傳 input_string
        DPU_ASSERT(dpu_copy_to(
            dpu,
            "input_string",
            0,
            input_string,
            STRING_LENGTH * sizeof(char)));

        // 傳 start_index
        DPU_ASSERT(dpu_copy_to(
            dpu,
            "start_index",
            0,
            &start,
            sizeof(int)));

        // 傳 count
        DPU_ASSERT(dpu_copy_to(
            dpu,
            "count",
            0,
            &count,
            sizeof(int)));

        // 傳「全部」字串給這顆 DPU 的 strings[]
        DPU_ASSERT(dpu_copy_to(
            dpu,
            "strings",
            0,
            flat_strings,
            NUM_STRINGS * STRING_LENGTH * sizeof(char)));

        offset += count;
    }

    /* 4. 啟動所有 DPU */
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));

    /* 5. 把結果從各 DPU 的 results[] 分段搬回來 */
    base   = NUM_STRINGS / NUM_DPUS;
    remain = NUM_STRINGS % NUM_DPUS;
    offset = 0;
    idx    = 0;

    DPU_FOREACH(set, dpu, idx) {
        int count = base + (idx < (unsigned int)remain ? 1 : 0);
        int start = offset;

        if (count > 0) {
            int64_t local_results[count];

            DPU_ASSERT(dpu_copy_from(
                dpu,
                "results",
                start * sizeof(int64_t),          // 從 results[start] 開始
                local_results,
                count * sizeof(int64_t)));

            for (int j = 0; j < count; ++j) {
                results[start + j] = (int)local_results[j];
            }
        }

        offset += count;
    }

    // test
    // for (int i = 0; i < NUM_STRINGS; ++i) {
    //     printf("%d ", results[i]);
    // }
    // printf("\n");

    // 釋放 DPU
    DPU_ASSERT(dpu_free(set));
}
