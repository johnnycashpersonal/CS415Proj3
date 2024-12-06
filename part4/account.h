#ifndef ACCOUNT_H_
#define ACCOUNT_H_

#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <fcntl.h>

typedef struct {
    char account_number[17];  // 16 digits + null terminator
    char password[9];         // 8 chars + null terminator
    double balance;
    double reward_rate;
    double transaction_tracter;
    pthread_mutex_t ac_lock;
} account_t, account;

// Shared memory structure for inter-process communication
typedef struct {
    char account_number[17];
    double initial_balance;
    double current_balance;
    int account_index;
    atomic_bool needs_update;  // Flag to indicate when updates are needed
} shared_account_info_t;

// Statistics structure
typedef struct {
    atomic_int total_transactions;
    atomic_int invalid_transactions;
    atomic_int transfers;
    atomic_int deposits;
    atomic_int withdrawals;
    atomic_int checks;
} stats_t;

// Worker arguments structure
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

// Bank arguments structure
typedef struct {
    account_t* accounts;
    int num_accounts;
    shared_account_info_t* shared_accounts;  // Add pointer to shared memory
} bank_args_t;

// Function declarations
void* process_transaction(void* arg);
void* update_balance(void* arg);

// Helper function declarations
account_t* read_accounts(const char* filename, int* num_accounts);
void process_transactions(account_t* accounts, int num_accounts, const char* trans_filename, stats_t* stats);
void write_output(account_t* accounts, int num_accounts);

// New function declarations for shared memory operations
void setup_shared_memory(int num_accounts);
void cleanup_shared_memory(size_t size);
void puddles_bank_process(int num_accounts);

#endif /* ACCOUNT_H_ */