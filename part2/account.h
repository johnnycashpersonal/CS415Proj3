#ifndef ACCOUNT_H_
#define ACCOUNT_H_

#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    char account_number[17];  // 16 digits + null terminator
    char password[9];         // 8 chars + null terminator
    double balance;
    double reward_rate;
    double transaction_tracter;
    pthread_mutex_t ac_lock;
} account_t, account;

// Statistics structure
typedef struct {
    atomic_int total_transactions;
    atomic_int invalid_transactions;
    atomic_int transfers;
    atomic_int deposits;
    atomic_int withdrawals;
    atomic_int checks;
} stats_t;

// Add these new structures
typedef struct {
    account_t* accounts;
    int num_accounts;
    char* filename;
    int start_transaction;
    int end_transaction;
    stats_t* stats;
    pthread_mutex_t* stats_lock;
    int transactions_per_worker;
} worker_args_t;

typedef struct {
    account_t* accounts;
    int num_accounts;
} bank_args_t;

// Function declarations
void* process_transaction(void* arg);
void* update_balance(void* arg);

// Helper function declarations
account_t* read_accounts(const char* filename, int* num_accounts);
void process_transactions(account_t* accounts, int num_accounts, const char* trans_filename, stats_t* stats);
void write_output(account_t* accounts, int num_accounts);

#endif /* ACCOUNT_H_ */