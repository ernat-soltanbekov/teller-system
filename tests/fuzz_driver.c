#include <stdint.h>
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Reproducible sanitizer stress, usable with Apple's compiler without the
 * optional libFuzzer runtime. CI additionally runs coverage-guided libFuzzer. */
int main(void) {
    const char *seeds[] = {"1001.20",         "29/02/2000", "2147483647", "fixed03",
                           "100000000000.00", "NaN",        "-1",         "31/02/2026"};
    uint64_t random = 2008;
    uint8_t data[512];
    for (unsigned i = 0; i < 1000000; i++) {
        random = random * UINT64_C(6364136223846793005) + 1;
        size_t size = (size_t)(random % sizeof(data));
        if (i % 2 == 0) {
            const char *seed = seeds[i % (sizeof(seeds) / sizeof(seeds[0]))];
            size = strlen(seed);
            memcpy(data, seed, size);
            data[(random >> 10) % size] = (uint8_t)(random >> 32);
        } else {
            for (size_t j = 0; j < size; j++) {
                random = random * UINT64_C(6364136223846793005) + 1;
                data[j] = (uint8_t)(random >> 32);
            }
        }
        LLVMFuzzerTestOneInput(data, size);
    }
    puts("PASS: 1000000 reproducible parser mutations under sanitizers.");
    return 0;
}
