#include <dpu.h>
#include "configure.h"

void DPU_calculate_hamming_distance(char strings[NUM_STRINGS][STRING_LENGTH],
                                    char input_string[STRING_LENGTH],
                                    int results[NUM_STRINGS])
#define PAD_TO_EVEN(x) (x & 1 ? x + 1 : x)
{
    // 分配DPU數量與載入DPU程式碼
    int dpu_id;
    struct dpu_set_t set, dpu;
    DPU_ASSERT(dpu_alloc(NUM_DPUS, "backend=simulator", &set));
    DPU_ASSERT(dpu_load(set, "dpu_program", NULL));

    // 將strings字串資料先整理成1維的形式，方便傳入DPU
    char *words = malloc(sizeof(char) * NUM_STRINGS * STRING_LENGTH);
    for (int i = 0; i < NUM_STRINGS; ++i)
        memcpy(words + i * STRING_LENGTH, strings[i], sizeof(char) * STRING_LENGTH);

    // 計算每個DPU要傳入的size
    int size = NUM_STRINGS / NUM_DPUS;
    int sizes[NUM_DPUS];
    for (int i = 0; i < NUM_DPUS; ++i)
    {
        sizes[i] = size;
    }
    sizes[NUM_DPUS - 1] = NUM_STRINGS - size * (NUM_DPUS - 1);

    // 將每個DPU要計算的字串數量傳進去
    DPU_FOREACH(set, dpu, dpu_id)
    {
        DPU_ASSERT(dpu_copy_to(dpu, "size", 0, &sizes[dpu_id], sizeof(int)));
    }
    // 將input_string傳給每個DPU (因為資料都一樣，所以用broadcast)
    DPU_ASSERT(dpu_broadcast_to(set, "input_string", 0, input_string, sizeof(char) * STRING_LENGTH, DPU_XFER_DEFAULT));

    // 將字串資料傳入對應的DPU，交給他們計算hamming distance
    char *ptr = words;
    DPU_FOREACH(set, dpu, dpu_id)
    {
        DPU_ASSERT(dpu_copy_to(dpu, "strings", 0, ptr, sizeof(char) * STRING_LENGTH * sizes[dpu_id]));
        ptr += sizes[dpu_id] * STRING_LENGTH;
    }

    // 呼叫DPU運算HM
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    int tmp[1000];
    int *res = tmp;

    // 取回結果
    DPU_FOREACH(set, dpu, dpu_id)
    {
        DPU_ASSERT(dpu_copy_from(dpu, "results", 0, res, sizeof(int) * PAD_TO_EVEN(sizes[dpu_id])));
        res += sizes[dpu_id];
    }
    memcpy(results, tmp, sizeof(int) * NUM_STRINGS);

    // 釋放DPU
    DPU_ASSERT(dpu_free(set));
}