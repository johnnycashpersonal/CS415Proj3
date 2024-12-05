#define _XOPEN_SOURCE 600
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
#define TRANSACTION_THRESHOLD 5000

int NUM_ACCS = 0;
account *account_arr;
command_line *cmd_arr;
pthread_mutex_t account_mutex;
int resources_freed = 0;  // Track if resources have been freed
pthread_cond_t worker_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t bank_cond = PTHREAD_COND_INITIALIZER;
int bank_updating = 0;

typedef struct {
    command_line *transactions;
    int start_index;
    int end_index;
} thread_data;

// pipe time
int pipe_fd[2];

stats_t stats = {0}; // Initialize all stats to 0

struct timeval start_time;

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
            break;
        }
    }

    // Create bank thread BEFORE worker threads
    pthread_t bank_thread;
    pthread_create(&bank_thread, NULL, update_balance, NULL);

    // Initialize barrier for synchronized start
    pthread_barrier_t start_barrier;
    pthread_barrier_init(&start_barrier, NULL, NUM_WORKERS + 1);

    // Create worker threads
    pthread_t worker_threads[NUM_WORKERS];
    thread_data worker_data[NUM_WORKERS];
    int transactions_per_worker = num_transactions / NUM_WORKERS;

    // assign start, end indices and transaction vals to thread data
    for (int i = 0; i < NUM_WORKERS; i++) {
        worker_data[i].transactions = transactions;
        worker_data[i].start_index = i * transactions_per_worker;
        worker_data[i].end_index = (i == NUM_WORKERS - 1) ? num_transactions : (i + 1) * transactions_per_worker;

        char msg[100];
        snprintf(msg, sizeof(msg), "Creating worker thread %d to process transactions %d-%d", 
                i, worker_data[i].start_index, worker_data[i].end_index);
        print_elapsed_time(msg);
        pthread_create(&worker_threads[i], NULL, process_transaction, &worker_data[i]);
    }

    print_elapsed_time("Waiting for all threads to complete");

    // wait for all worker threads to finish
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(worker_threads[i], NULL);
        char msg[100];
        snprintf(msg, sizeof(msg), "Worker thread %d is finished", i);
        print_elapsed_time(msg);
    }

    print_elapsed_time("All workers finished. Creating bank thread to update balances");

    pthread_barrier_wait(&start_barrier);  // Signal threads to start

    // wait for bank thread to finish
    pthread_join(bank_thread, NULL);

    print_elapsed_time("All balances updated.");

    // Get final elapsed time for statistics
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
    printf("----------------------------------------\n");
    printf("Total Balance Updates: 1\n");
    printf("Program completed successfully.\n\n");

    resources_freed = 1;  // Mark resources as freed
    free(account_arr);
    free(cmd_arr);
    pthread_mutex_destroy(&account_mutex);

    return 0;
}

void* process_transaction(void* arg) {
    thread_data *data = (thread_data*) arg;
    static int check_balance_count = 0;

    for (int i = data->start_index; i < data->end_index; i++) {
        // Check if bank is updating - if so, wait
        pthread_mutex_lock(&transaction_mutex);
        while (bank_updating) {
            pthread_cond_wait(&worker_cond, &transaction_mutex);
        }
        pthread_mutex_unlock(&transaction_mutex);

        command_line *transaction = &data->transactions[i];
        int src_acc_ind = -1;
        int dst_acc_ind = -1;
        double trans_amount = -1;

        pthread_mutex_lock(&account_mutex);

        // Find source account
        for (int j = 0; j < NUM_ACCS; j++) {
            if (strcmp(account_arr[j].account_number, transaction->command_list[1]) == 0) {
                src_acc_ind = j;
                break;
            }
        }
        if (src_acc_ind == -1) {
            stats.invalid_transactions++;
            pthread_mutex_unlock(&account_mutex);
            continue;
        }

        // Validate transaction amount
        if (transaction->command_list[0][0] == 'W' || transaction->command_list[0][0] == 'T') {
            trans_amount = strtod(transaction->command_list[3], NULL);
            if (account_arr[src_acc_ind].balance < trans_amount) {
                stats.invalid_transactions++;
                pthread_mutex_unlock(&account_mutex);
                continue;
            }
        }

        // Validate password
        if (strcmp(account_arr[src_acc_ind].password, transaction->command_list[2]) != 0) {
            pthread_mutex_unlock(&account_mutex);
            continue;
        }

        // Process transaction
        char trans = transaction->command_list[0][0];
        switch (trans) {
            case 'T':
                for (int j = 0; j < NUM_ACCS; j++) {
                    if (strcmp(account_arr[j].account_number, transaction->command_list[3]) == 0) {
                        dst_acc_ind = j;
                        break;
                    }
                }
                trans_amount = strtod(transaction->command_list[4], NULL);
                account_arr[src_acc_ind].balance -= trans_amount;
                account_arr[src_acc_ind].transaction_tracter += trans_amount;
                account_arr[dst_acc_ind].balance += trans_amount;
                stats.transfers++;
                stats.total_transactions++;
                
                // Check threshold after non-check transaction
                int curr_trans = atomic_fetch_add(&processed_transactions, 1);
                if (curr_trans + 1 >= TRANSACTION_THRESHOLD) {
                    pthread_mutex_lock(&transaction_mutex);
                    bank_updating = 1;
                    pthread_cond_signal(&bank_cond);
                    pthread_mutex_unlock(&transaction_mutex);
                }
                break;

            case 'C':
                check_balance_count++;
                if (check_balance_count % 500 == 0) {
                    time_t now = time(NULL);
                    char time_str[26];
                    ctime_r(&now, time_str);
                    time_str[strlen(time_str) - 1] = '\0';

                    char message[128];
                    snprintf(message, sizeof(message), 
                            "Worker checked balance of Account %s. Balance is $%.2f. Check occured at %s\n",
                            account_arr[src_acc_ind].account_number,
                            account_arr[src_acc_ind].balance,
                            time_str);
                    write(pipe_fd[1], message, strlen(message));
                }
                stats.checks++;
                stats.total_transactions++;
                break;

            case 'D':
                trans_amount = strtod(transaction->command_list[3], NULL);
                account_arr[src_acc_ind].balance += trans_amount;
                account_arr[src_acc_ind].transaction_tracter += trans_amount;
                stats.deposits++;
                stats.total_transactions++;
                
                // Check threshold after non-check transaction
                curr_trans = atomic_fetch_add(&processed_transactions, 1);
                if (curr_trans + 1 >= TRANSACTION_THRESHOLD) {
                    pthread_mutex_lock(&transaction_mutex);
                    bank_updating = 1;
                    pthread_cond_signal(&bank_cond);
                    pthread_mutex_unlock(&transaction_mutex);
                }
                break;

            case 'W':
                trans_amount = strtod(transaction->command_list[3], NULL);
                account_arr[src_acc_ind].balance -= trans_amount;
                account_arr[src_acc_ind].transaction_tracter += trans_amount;
                stats.withdrawals++;
                stats.total_transactions++;
                
                // Check threshold after non-check transaction
                curr_trans = atomic_fetch_add(&processed_transactions, 1);
                if (curr_trans + 1 >= TRANSACTION_THRESHOLD) {
                    pthread_mutex_lock(&transaction_mutex);
                    bank_updating = 1;
                    pthread_cond_signal(&bank_cond);
                    pthread_mutex_unlock(&transaction_mutex);
                }
                break;

            default:
                stats.invalid_transactions++;
                printf("Error: Invalid transaction type.\n");
                pthread_mutex_unlock(&account_mutex);
                return NULL;
        }

        pthread_mutex_unlock(&account_mutex);
    }

    return NULL;
}

void* update_balance(void* arg) {
    pthread_barrier_wait(&start_barrier);
    
    while (1) {
        // Wait for signal from workers
        pthread_mutex_lock(&transaction_mutex);
        while (!bank_updating) {
            pthread_cond_wait(&bank_cond, &transaction_mutex);
        }
        
        // Update balances and create individual files
        for (int i = 0; i < NUM_ACCS; i++) {
            char filename[32];
            snprintf(filename, sizeof(filename), "Output/act_%d.txt", i);
            FILE* f_out = fopen(filename, "a");
            
            account_arr[i].balance += (account_arr[i].reward_rate * account_arr[i].transaction_tracter);
            fprintf(f_out, "Balance: %.2f\n", account_arr[i].balance);
            account_arr[i].transaction_tracter = 0;
            fclose(f_out);
        }
        
        // Reset and signal workers
        atomic_store(&processed_transactions, 0);
        bank_updating = 0;
        pthread_cond_broadcast(&worker_cond);
        pthread_mutex_unlock(&transaction_mutex);
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

// Add this barrier implementation
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int tripCount;
} barrier_t;

barrier_t barrier;
atomic_int processed_transactions = 0;
pthread_mutex_t transaction_mutex = PTHREAD_MUTEX_INITIALIZER;

void barrier_init(barrier_t *barrier, int count) {
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    barrier->tripCount = count;
    barrier->count = 0;
}

void barrier_wait(barrier_t *barrier) {
    pthread_mutex_lock(&barrier->mutex);
    barrier->count++;
    if (barrier->count == barrier->tripCount) {
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
    } else {
        pthread_cond_wait(&barrier->cond, &barrier->mutex);
    }
    pthread_mutex_unlock(&barrier->mutex);
}