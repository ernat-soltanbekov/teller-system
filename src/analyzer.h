#ifndef TELLER_ANALYZER_H
#define TELLER_ANALYZER_H

#include "header.h"

typedef struct {
    int64_t deposited, withdrawn;
    uint64_t deposits, withdrawals;
    int ratio;
} Spending;

bool spending_ratio(int64_t deposited, int64_t withdrawn, int *ratio);
const char *spending_label(int ratio);
const char *spending_message(int ratio);
bool spending_read(const char *path, int account_id, Spending *result, char error[256]);
void analyzer_use(App *app);
bool analyze_spending(int account_id);

#endif
