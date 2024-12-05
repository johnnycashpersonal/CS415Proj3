#ifndef ACCOUNT_H_
#define ACCOUNT_H_

#include <pthread.h>

typedef struct {
    char account_number[17];  // 16 digits + null terminator
    char password[9];         // 8 chars + null terminator
    double balance;
    double reward_rate;
    double transaction_tracter;
    pthread_mutex_t ac_lock;
} account_t;

// Statistics structure
typedef struct {
    int total_transactions;
    int invalid_transactions;
    int transfers;
    int deposits;
    int withdrawals;
    int checks;
} stats_t;

// Function declarations
void* process_transaction(void* arg);
void* update_balance(void* arg);

// Helper function declarations
account_t* read_accounts(const char* filename, int* num_accounts);
void process_transactions(account_t* accounts, int num_accounts, const char* trans_filename, stats_t* stats);
void write_output(account_t* accounts, int num_accounts);

#endif /* ACCOUNT_H_ */