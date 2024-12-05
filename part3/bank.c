#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdatomic.h>
#include "account.h"
#include "string_parser.h"

#define INITIAL_SIZE 16
#define NUM_WORKERS 10

int NUM_ACCS = 0;
account *account_arr;
command_line *cmd_arr;
pthread_mutex_t account_mutex;
int resources_freed = 0;  // Track if resources have been freed

typedef struct {
    command_line *transactions;
    int start_index;
    int end_index;
} thread_data;

// pipe time
int pipe_fd[2];

stats_t stats = {0}; // Initialize all stats to 0

struct timeval start_time;

pthread_barrier_t start_barrier;
pthread_cond_t update_cond;
pthread_mutex_t update_mutex;
int transactions_processed = 0;
int update_ready = 0;

// Add these debug counters as global variables
atomic_int total_processed = 0;
atomic_int barrier_wait_count = 0;
atomic_int update_cycles = 0;

volatile int should_exit = 0;

// Add this global variable at the top
atomic_int total_updates = 0;

// Add to global variables
atomic_int valid_transaction_count = 0;  // Only counts valid non-check transactions
pthread_mutex_t bank_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t bank_cond = PTHREAD_COND_INITIALIZER;
int bank_ready = 0;
atomic_int active_threads = NUM_WORKERS;  // Track active threads

// Add to global variables at the top
atomic_int ledger_line_count = 0;

void* process_transaction(void* arg);
void* update_balance(void* arg);
void auditor_process(int read_fd);

// Add cleanup function
void cleanup() {
    if (!resources_freed) {
        if (account_arr) free(account_arr);
        if (cmd_arr) free(cmd_arr);
        pthread_mutex_destroy(&account_mutex);
        resources_freed = 1;
    }
}

void print_elapsed_time(const char* message) {
    struct timeval now;
    gettimeofday(&now, NULL);
    long elapsed = (now.tv_sec - start_time.tv_sec) * 1000 + 
                  (now.tv_usec - start_time.tv_usec) / 1000;
    printf("[%ldms] %s\n", elapsed, message);
}

command_line* read_file_to_command_lines(const char* filename, int* num_lines) {
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        perror("Failed to open file");
        exit(1);
    }

    int capacity = INITIAL_SIZE;
    command_line* cmd_array = malloc(sizeof(command_line) * capacity); 

    if (cmd_array == NULL) {
        perror("Memory allocation failed");
        fclose(file);
        exit(1);
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    // read the file line by line
    while ((read = getline(&line, &len, file)) != -1) {
        if (line[read - 1] == '\n') {
            line[read - 1] = '\0';
        }

        // resize the array if needed
        if (*num_lines >= capacity) {
            capacity *= 2;
            command_line* temp = realloc(cmd_array, sizeof(command_line) * capacity);
            if (temp == NULL) {
                perror("Memory allocation failed");
                free(line);
                fclose(file);
                free(cmd_array);
                exit(1);
            }
            cmd_array = temp;
        }

        // tokenize the line and store it in cmd_array
        cmd_array[*num_lines] = str_filler(line, " ");
        (*num_lines)++;
    }

    free(line);
    fclose(file);

    return cmd_array;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        exit(1);
    }

    if (mkdir("Output", 0777) == -1 && errno != EEXIST) {
        perror("Failed to create Output directory");
        exit(1);
    }

    atexit(cleanup);

    /* process input file and do bank stuff */
    // fork auditor process
    // Before forking the auditor process
    if (pipe(pipe_fd) == -1) {
        perror("pipe creation failed");
        exit(1);
    }
    pid_t auditor_pid = fork();
    if (auditor_pid == -1) {
        perror("forking error");
        exit(1);
    }

    if (auditor_pid == 0) {
        // child process: auditor
        close(pipe_fd[1]);
        auditor_process(pipe_fd[0]);
        exit(0);
    }

    // parent process: Duck Bank
    close(pipe_fd[0]);

    int num_lines = 0;

    command_line *cmd_arr = read_file_to_command_lines(argv[1], &num_lines);
    NUM_ACCS = atoi(cmd_arr[0].command_list[0]);
    account_arr = malloc(sizeof(account) * NUM_ACCS);

    pthread_mutex_init(&account_mutex, NULL); // initialize mutex

    command_line *transactions = NULL;
    int num_transactions = 0;
    int transactions_per_worker = 0;

    gettimeofday(&start_time, NULL);

    print_elapsed_time("Processing transactions (multi-threaded)");

    for (int i = 1; i < num_lines; i++) {
        // account block
        if (strcmp(cmd_arr[i].command_list[0], "index") == 0) {
            int acc_i = atoi(cmd_arr[i].command_list[1]);
            strncpy(account_arr[acc_i].account_number, cmd_arr[++i].command_list[0], 16); // account number
            account_arr[acc_i].account_number[16] = '\0';
            strncpy(account_arr[acc_i].password, cmd_arr[++i].command_list[0], 8); // password
            account_arr[acc_i].password[8] = '\0';
            account_arr[acc_i].balance = strtod(cmd_arr[++i].command_list[0], NULL); // balance
            account_arr[acc_i].reward_rate = strtod(cmd_arr[++i].command_list[0], NULL); // reward rate
            account_arr[acc_i].transaction_tracter = 0; // transaction tracter init
        } else {
            // use rest of cmd_arr for transaction arr
            transactions = &cmd_arr[i];
            num_transactions = num_lines - i;
            transactions_per_worker = num_transactions / NUM_WORKERS;
            break;
        }
    }

    printf("[Debug] Initializing synchronization primitives\n");
    pthread_barrier_init(&start_barrier, NULL, NUM_WORKERS + 2);
    pthread_cond_init(&update_cond, NULL);
    pthread_mutex_init(&update_mutex, NULL);

    // Create bank thread FIRST
    pthread_t bank_thread;
    printf("[Debug] Creating bank thread\n");
    pthread_create(&bank_thread, NULL, update_balance, NULL);

    // Create worker threads
    pthread_t worker_threads[NUM_WORKERS];
    thread_data worker_data[NUM_WORKERS];
    
    for (int i = 0; i < NUM_WORKERS; i++) {
        worker_data[i].transactions = transactions;
        worker_data[i].start_index = i * transactions_per_worker;
        worker_data[i].end_index = (i == NUM_WORKERS - 1) ? num_transactions : (i + 1) * transactions_per_worker;

        printf("[Debug] Creating worker thread %d to process transactions %d-%d\n", 
                i, worker_data[i].start_index, worker_data[i].end_index);
        pthread_create(&worker_threads[i], NULL, process_transaction, &worker_data[i]);
    }

    // Wait at barrier for all threads to be ready
    printf("[Debug] Main thread waiting at barrier\n");
    pthread_barrier_wait(&start_barrier);
    printf("[Debug] All threads have started\n");

    // Wait for worker threads to finish
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(worker_threads[i], NULL);
        char msg[100];
        snprintf(msg, sizeof(msg), "Worker thread %d is finished", i);
        print_elapsed_time(msg);
    }

    // Signal bank thread to exit and wait for it
    printf("[Debug] Signaling bank thread to exit\n");
    pthread_mutex_lock(&update_mutex);
    should_exit = 1;  // Add this as a global variable
    pthread_cond_signal(&update_cond);
    pthread_mutex_unlock(&update_mutex);
    pthread_join(bank_thread, NULL);

    // Cleanup
    pthread_barrier_destroy(&start_barrier);
    pthread_cond_destroy(&update_cond);
    pthread_mutex_destroy(&update_mutex);

    struct timeval end_time;
    gettimeofday(&end_time, NULL);
    long total_time = (end_time.tv_sec - start_time.tv_sec) * 1000 + 
                     (end_time.tv_usec - start_time.tv_usec) / 1000;

    printf("\nProgram Statistics (Total time: %ld ms):\n", total_time);
    printf("----------------------------------------\n");
    printf("Total Transactions Processed: %d\n", stats.total_transactions);
    printf("Invalid Transactions Caught: %d\n", stats.invalid_transactions);
    printf("Successful Transfers: %d\n", stats.transfers);
    printf("Successful Deposits: %d\n", stats.deposits);
    printf("Successful Withdrawals: %d\n", stats.withdrawals);
    printf("Balance Checks Performed: %d\n", stats.checks);
    printf("Total Balance Updates: %d\n", atomic_load(&total_updates));
    printf("----------------------------------------\n");
    printf("Program completed successfully.\n\n");

    return 0;
}

void* process_transaction(void* arg) {
    thread_data *data = (thread_data*) arg;
    pthread_barrier_wait(&start_barrier);

    for (int i = data->start_index; i < data->end_index; i++) {
        command_line *transaction = &data->transactions[i];
        int src_acc_ind = -1;
        int dst_acc_ind = -1;
        double trans_amount = -1;
        int valid_transaction = 0;

        pthread_mutex_lock(&account_mutex);

        // Find source account
        for (int j = 0; j < NUM_ACCS; j++) {
            if (strcmp(account_arr[j].account_number, transaction->command_list[1]) == 0) {
                src_acc_ind = j;
                break;
            }
        }

        if (src_acc_ind == -1) {
            printf("[Debug] Source account not found: %s\n", transaction->command_list[1]);
            stats.invalid_transactions++;
            pthread_mutex_unlock(&account_mutex);
            continue;
        }

        // Check password
        if (strcmp(account_arr[src_acc_ind].password, transaction->command_list[2]) != 0) {
            printf("[Debug] Invalid password for account: %s\n", account_arr[src_acc_ind].account_number);
            stats.invalid_transactions++;
            pthread_mutex_unlock(&account_mutex);
            continue;
        }

        // Process transaction
        char trans = transaction->command_list[0][0];
        switch (trans) {
            case 'T':
                // Find destination account
                for (int j = 0; j < NUM_ACCS; j++) {
                    if (strcmp(account_arr[j].account_number, transaction->command_list[3]) == 0) {
                        dst_acc_ind = j;
                        break;
                    }
                }

                if (dst_acc_ind != -1) {
                    trans_amount = strtod(transaction->command_list[4], NULL);
                    if (account_arr[src_acc_ind].balance >= trans_amount) {
                        account_arr[src_acc_ind].balance -= trans_amount;
                        account_arr[dst_acc_ind].balance += trans_amount;
                        account_arr[src_acc_ind].transaction_tracter += trans_amount;
                        atomic_fetch_add(&stats.transfers, 1);
                        atomic_fetch_add(&stats.total_transactions, 1);
                        valid_transaction = 1;
                        printf("[Debug] Transfer successful: %s to %s, Amount: %.2f\n",
                               account_arr[src_acc_ind].account_number,
                               account_arr[dst_acc_ind].account_number,
                               trans_amount);
                    } else {
                        printf("[Debug] Insufficient funds for transfer: %s\n", account_arr[src_acc_ind].account_number);
                        atomic_fetch_add(&stats.invalid_transactions, 1);
                    }
                } else {
                    printf("[Debug] Destination account not found: %s\n", transaction->command_list[3]);
                    atomic_fetch_add(&stats.invalid_transactions, 1);
                }
                break;

            case 'C':
                {
                    // Get current time
                    time_t now = time(NULL);
                    char time_str[26];
                    ctime_r(&now, time_str);
                    time_str[strlen(time_str) - 1] = '\0';  // Remove newline
                    
                    // Open ledger file in append mode
                    FILE *ledger = fopen("Output/ledger.txt", "a");
                    if (ledger) {
                        int line_num = atomic_fetch_add(&ledger_line_count, 1) + 1;
                        fprintf(ledger, "%d Worker checked balance of Account %s. Balance is $%.2f. Check occurred at %s\n",
                                line_num,
                                account_arr[src_acc_ind].account_number,
                                account_arr[src_acc_ind].balance,
                                time_str);
                        fclose(ledger);
                        
                        // Add debug output to track line count
                        printf("[Debug] Ledger line count: %d\n", line_num);
                    }
                    
                    printf("[Debug] Balance check for account %s: %.2f\n",
                           account_arr[src_acc_ind].account_number,
                           account_arr[src_acc_ind].balance);
                    atomic_fetch_add(&stats.checks, 1);
                    atomic_fetch_add(&stats.total_transactions, 1);
                }
                break;

            case 'D':
                // deposit
                trans_amount = strtod(transaction->command_list[3], NULL);
                account_arr[src_acc_ind].balance += trans_amount;
                account_arr[src_acc_ind].transaction_tracter += trans_amount;
                printf("[Debug] Deposit to account %s: %.2f, New balance: %.2f\n",
                       account_arr[src_acc_ind].account_number,
                       trans_amount,
                       account_arr[src_acc_ind].balance);
                atomic_fetch_add(&stats.deposits, 1);
                atomic_fetch_add(&stats.total_transactions, 1);
                valid_transaction = 1;
                break;

            case 'W':
                // withdrawal
                trans_amount = strtod(transaction->command_list[3], NULL);
                if (account_arr[src_acc_ind].balance >= trans_amount) {
                    account_arr[src_acc_ind].balance -= trans_amount;
                    account_arr[src_acc_ind].transaction_tracter += trans_amount;
                    printf("[Debug] Withdrawal from account %s: %.2f, New balance: %.2f\n",
                           account_arr[src_acc_ind].account_number,
                           trans_amount,
                           account_arr[src_acc_ind].balance);
                    atomic_fetch_add(&stats.withdrawals, 1);
                    atomic_fetch_add(&stats.total_transactions, 1);
                    valid_transaction = 1;
                } else {
                    printf("[Debug] Insufficient funds for withdrawal: Account %s, Amount: %.2f, Balance: %.2f\n",
                           account_arr[src_acc_ind].account_number,
                           trans_amount,
                           account_arr[src_acc_ind].balance);
                    atomic_fetch_add(&stats.invalid_transactions, 1);
                }
                break;

            default:
                printf("[Debug] Invalid transaction type: %c\n", trans);
                stats.invalid_transactions++;
                pthread_mutex_unlock(&account_mutex);
                return NULL;
        }

        pthread_mutex_unlock(&account_mutex);

        if (valid_transaction && trans != 'C') {
            int current = atomic_fetch_add(&valid_transaction_count, 1) + 1;
            if (current % 5000 == 0) {
                pthread_mutex_lock(&bank_mutex);
                bank_ready = 1;
                pthread_cond_signal(&bank_cond);
                while (bank_ready) {
                    pthread_cond_wait(&bank_cond, &bank_mutex);
                }
                pthread_mutex_unlock(&bank_mutex);
            }
        }
    }

    // Signal that this thread is done
    int remaining = atomic_fetch_sub(&active_threads, 1) - 1;
    if (remaining == 0) {
        pthread_mutex_lock(&bank_mutex);
        bank_ready = 1;
        should_exit = 1;
        pthread_cond_signal(&bank_cond);
        pthread_mutex_unlock(&bank_mutex);
    }

    return NULL;
}

void* update_balance(void* arg) {
    pthread_barrier_wait(&start_barrier);
    
    while (!should_exit) {
        pthread_mutex_lock(&bank_mutex);
        while (!bank_ready && !should_exit) {
            pthread_cond_wait(&bank_cond, &bank_mutex);
        }
        
        // Get current time for logging
        time_t now = time(NULL);
        char time_str[26];
        ctime_r(&now, time_str);
        time_str[strlen(time_str) - 1] = '\0';  // Remove newline
        
        pthread_mutex_lock(&account_mutex);
        printf("[Debug] Starting balance update cycle\n");
        
        // Open ledger file in append mode
        FILE *ledger = fopen("Output/ledger.txt", "a");
        if (!ledger) {
            perror("Failed to open ledger.txt");
            pthread_mutex_unlock(&account_mutex);
            pthread_mutex_unlock(&bank_mutex);
            continue;
        }
        
        // Process each account
        for (int i = 0; i < NUM_ACCS; i++) {
            // Calculate and apply interest
            double reward = account_arr[i].reward_rate * account_arr[i].transaction_tracter;
            account_arr[i].balance += reward;
            account_arr[i].transaction_tracter = 0;
            
            // Log with line number
            int line_num = atomic_fetch_add(&ledger_line_count, 1) + 1;
            fprintf(ledger, "%d Applied Interest to account %s. New Balance: $%.2f. Time of Update: %s\n",
                    line_num,
                    account_arr[i].account_number,
                    account_arr[i].balance,
                    time_str);
            
            // Write to individual account file
            char filename[32];
            snprintf(filename, sizeof(filename), "Output/act_%d.txt", i);
            FILE* f_out = fopen(filename, "a");
            fprintf(f_out, "%.2f\n", account_arr[i].balance);
            fclose(f_out);
        }
        
        fclose(ledger);
        pthread_mutex_unlock(&account_mutex);
        
        atomic_fetch_add(&total_updates, 1);
        
        bank_ready = 0;
        pthread_cond_broadcast(&bank_cond);
        pthread_mutex_unlock(&bank_mutex);
        
        if (should_exit) break;
    }
    
    return NULL;
}

void auditor_process(int read_fd) {
    /* write pipe info to ledger.txt */
    FILE *ledger = fopen("Output/ledger.txt", "w");
    if (!ledger) {
        perror("Failed to open ledger.txt");
        exit(1);
    }

    char buffer[256];
    ssize_t bytes_read;
    while ((bytes_read = read(read_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        fprintf(ledger, "%s", buffer);
    }

    fclose(ledger);
}
