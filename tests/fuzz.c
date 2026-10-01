#include "header.h"
#include "analyzer.h"
#include <stdlib.h>
#include <string.h>

/* libFuzzer drives the actual C parsers under ASan and UBSan. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 512)
        return 0;
    char text[513];
    memcpy(text, data, size);
    text[size] = '\0';
    int id, ratio;
    int64_t amount;
    Date date;
    (void)parse_id(text, &id);
    (void)parse_date(text, &date);
    (void)valid_name(text);
    (void)valid_country(text);
    (void)valid_phone(text);
    (void)valid_kind(text);
    if (parse_money(text, &amount)) {
        char canonical[32];
        int64_t roundtrip;
        money_text(amount, canonical);
        if (!parse_money(canonical, &roundtrip) || roundtrip != amount)
            abort();
    }
    if (size >= 16) {
        int64_t deposited, withdrawn;
        memcpy(&deposited, data, 8);
        memcpy(&withdrawn, data + 8, 8);
        if (spending_ratio(deposited, withdrawn, &ratio) && (ratio < 0 || ratio > 100))
            abort();
    }
    return 0;
}
