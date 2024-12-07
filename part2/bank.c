#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "account.h"
#include "string_parser.h"

#define INITIAL_SIZE 16
#define NUM_WORKERS 10

int NUM_ACCS = 0;
account *account_arr;
// thread time
pthread_mutex_t account_mutex;

typedef struct {
    command_line *transactions;
    int start_index;
    int end_index;
    stats_t* stats;
} thread_data;


static int pipe_fd[2];

stats_t stats = {0}; // Initialize all stats to 0

void* process_transaction(void* arg);
void* update_balance(void* arg);
void auditor_process(int read_fd);

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

    // Set up pipe for auditor
    if (pipe(pipe_fd) == -1) {
        perror("pipe creation failed");
        exit(1);
    }

    // Fork auditor process
    pid_t auditor_pid = fork();
    if (auditor_pid == -1) {
        perror("forking error");
        exit(1);
    }

    if (auditor_pid == 0) {
        // Child process: auditor
        close(pipe_fd[1]);  // Close write end
        auditor_process(pipe_fd[0]);
        exit(0);
    }

    // Parent process: Duck Bank
    close(pipe_fd[0]);  // Close read end

    // Initialize atomic stats
    stats_t stats = {0};

    // Read input file
    int num_lines = 0;
    command_line *cmd_arr = read_file_to_command_lines(argv[1], &num_lines);
    if (!cmd_arr) {
        perror("Failed to read input file");
        exit(1);
    }

    // Get number of accounts
    NUM_ACCS = atoi(cmd_arr[0].command_list[0]);
    account_arr = malloc(sizeof(account) * NUM_ACCS);
    if (!account_arr) {
        perror("Failed to allocate account array");
        free(cmd_arr);
        exit(1);
    }

    // Initialize account locks
    for (int i = 0; i < NUM_ACCS; i++) {
        pthread_mutex_init(&account_arr[i].ac_lock, NULL);
    }

    command_line *transactions = NULL;
    int num_transactions = 0;

    printf("Processing transactions (multi-threaded)\n");

    // Process account information
    for (int i = 1; i < num_lines; i++) {
        if (strcmp(cmd_arr[i].command_list[0], "index") == 0) {
            int acc_i = atoi(cmd_arr[i].command_list[1]);
            
            // Account number
            strncpy(account_arr[acc_i].account_number, cmd_arr[++i].command_list[0], 16);
            account_arr[acc_i].account_number[16] = '\0';
            
            // Password
            strncpy(account_arr[acc_i].password, cmd_arr[++i].command_list[0], 8);
            account_arr[acc_i].password[8] = '\0';
            
            // Balance and rate
            account_arr[acc_i].balance = strtod(cmd_arr[++i].command_list[0], NULL);
            account_arr[acc_i].reward_rate = strtod(cmd_arr[++i].command_list[0], NULL);
            account_arr[acc_i].transaction_tracter = 0;
        } else {
            transactions = &cmd_arr[i];
            num_transactions = num_lines - i;
            break;
        }
    }

    // Set up worker threads
    pthread_t worker_threads[NUM_WORKERS];
    thread_data worker_data[NUM_WORKERS];
    int transactions_per_worker = num_transactions / NUM_WORKERS;

    // Create worker threads
    for (int i = 0; i < NUM_WORKERS; i++) {
        worker_data[i].transactions = transactions;
        worker_data[i].start_index = i * transactions_per_worker;
        worker_data[i].end_index = (i == NUM_WORKERS - 1) ? 
                                  num_transactions : 
                                  (i + 1) * transactions_per_worker;
        worker_data[i].stats = &stats;

        printf("Creating worker thread %d to process transactions %d-%d\n", 
               i, worker_data[i].start_index, worker_data[i].end_index);

        int create_status = pthread_create(&worker_threads[i], NULL, 
                                         process_transaction, &worker_data[i]);
        if (create_status != 0) {
            fprintf(stderr, "Failed to create worker thread %d: %s\n", 
                    i, strerror(create_status));
            exit(1);
        }
    }

    // Wait for worker threads to complete
    printf("Waiting for all threads to complete\n");
    for (int i = 0; i < NUM_WORKERS; i++) {
        int join_status = pthread_join(worker_threads[i], NULL);
        if (join_status != 0) {
            fprintf(stderr, "Failed to join worker thread %d: %s\n", 
                    i, strerror(join_status));
        } else {
            printf("Worker thread %d is finished\n", i);
        }
    }

    // Create and run bank thread
    printf("All workers finished. Creating bank thread to update balances\n");
    pthread_t bank_thread;
    int bank_create_status = pthread_create(&bank_thread, NULL, update_balance, NULL);
    if (bank_create_status != 0) {
        fprintf(stderr, "Failed to create bank thread: %s\n", 
                strerror(bank_create_status));
        exit(1);
    }

    // Wait for bank thread
    int bank_join_status = pthread_join(bank_thread, NULL);
    if (bank_join_status != 0) {
        fprintf(stderr, "Failed to join bank thread: %s\n", 
                strerror(bank_join_status));
    }

    // Print final statistics
    printf("\nProgram Statistics:\n");
    printf("----------------------------------------\n");
    printf("Total Transactions Processed: %d\n", atomic_load(&stats.total_transactions));
    printf("Invalid Transactions Caught: %d\n", atomic_load(&stats.invalid_transactions));
    printf("Successful Transfers: %d\n", atomic_load(&stats.transfers));
    printf("Successful Deposits: %d\n", atomic_load(&stats.deposits));
    printf("Successful Withdrawals: %d\n", atomic_load(&stats.withdrawals));
    printf("Balance Checks Performed: %d\n", atomic_load(&stats.checks));
    printf("----------------------------------------\n");
    printf("Process complete, all balances updated & Program Ending Successfully.\n\n");

    // Cleanup
    for (int i = 0; i < NUM_ACCS; i++) {
        pthread_mutex_destroy(&account_arr[i].ac_lock);
    }
    
    close(pipe_fd[1]);  // Close write end of pipe
    free(account_arr);
    free(cmd_arr);

    return 0;
}

void* process_transaction(void* arg) {
    thread_data *data = (thread_data*) arg;
    static atomic_int check_balance_count = 0;

    for (int i = data->start_index; i < data->end_index; i++) {
        command_line *transaction = &data->transactions[i];
        int src_acc_ind = -1;
        int dst_acc_ind = -1;
        double trans_amount = -1;

        // Find source account index - no lock needed for read-only search
        for (int j = 0; j < NUM_ACCS; j++) {
            if (strcmp(account_arr[j].account_number, transaction->command_list[1]) == 0) {
                src_acc_ind = j;
                break;
            }
        }

        if (src_acc_ind == -1) {
            atomic_fetch_add(&data->stats->invalid_transactions, 1);
            continue;
        }

        // Lock source account
        pthread_mutex_lock(&account_arr[src_acc_ind].ac_lock);

        // Verify password
        if (strcmp(account_arr[src_acc_ind].password, transaction->command_list[2]) != 0) {
            atomic_fetch_add(&data->stats->invalid_transactions, 1);
            pthread_mutex_unlock(&account_arr[src_acc_ind].ac_lock);
            continue;
        }

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

                if (dst_acc_ind == -1) {
                    atomic_fetch_add(&data->stats->invalid_transactions, 1);
                    pthread_mutex_unlock(&account_arr[src_acc_ind].ac_lock);
                    break;
                }

                // Lock destination account (ensure consistent order to prevent deadlock)
                if (dst_acc_ind > src_acc_ind) {
                    pthread_mutex_lock(&account_arr[dst_acc_ind].ac_lock);
                } else if (dst_acc_ind < src_acc_ind) {
                    pthread_mutex_unlock(&account_arr[src_acc_ind].ac_lock);
                    pthread_mutex_lock(&account_arr[dst_acc_ind].ac_lock);
                    pthread_mutex_lock(&account_arr[src_acc_ind].ac_lock);
                }

                trans_amount = strtod(transaction->command_list[4], NULL);
                account_arr[src_acc_ind].balance -= trans_amount;
                account_arr[src_acc_ind].transaction_tracter += trans_amount;
                account_arr[dst_acc_ind].balance += trans_amount;
                
                atomic_fetch_add(&data->stats->transfers, 1);
                atomic_fetch_add(&data->stats->total_transactions, 1);

                // Unlock in reverse order
                if (dst_acc_ind != src_acc_ind) {
                    pthread_mutex_unlock(&account_arr[dst_acc_ind].ac_lock);
                }
                break;

            case 'C':
                // Check balance
                int current_count = atomic_fetch_add(&check_balance_count, 1);
                if ((current_count + 1) % 500 == 0) {
                    time_t now = time(NULL);
                    char time_str[26];
                    ctime_r(&now, time_str);
                    time_str[strlen(time_str) - 1] = '\0';

                    char message[256];
                    snprintf(message, sizeof(message),
                            "Worker checked balance of Account %s. Balance is $%.2f. Check occurred at %s\n",
                            account_arr[src_acc_ind].account_number,
                            account_arr[src_acc_ind].balance,
                            time_str);
                    write(pipe_fd[1], message, strlen(message));
                }
                atomic_fetch_add(&data->stats->checks, 1);
                atomic_fetch_add(&data->stats->total_transactions, 1);
                break;

            case 'D':
                trans_amount = strtod(transaction->command_list[3], NULL);
                account_arr[src_acc_ind].balance += trans_amount;
                account_arr[src_acc_ind].transaction_tracter += trans_amount;
                atomic_fetch_add(&data->stats->deposits, 1);
                atomic_fetch_add(&data->stats->total_transactions, 1);
                break;

            case 'W':
                trans_amount = strtod(transaction->command_list[3], NULL);
                account_arr[src_acc_ind].balance -= trans_amount;
                account_arr[src_acc_ind].transaction_tracter += trans_amount;
                atomic_fetch_add(&data->stats->withdrawals, 1);
                atomic_fetch_add(&data->stats->total_transactions, 1);
                break;

            default:
                atomic_fetch_add(&data->stats->invalid_transactions, 1);
                printf("Error: Invalid transaction type.\n");
                break;
        }

        pthread_mutex_unlock(&account_arr[src_acc_ind].ac_lock);
    }

    return NULL;
}

void* update_balance(void* arg) {
    FILE* f_out = fopen("Output/output.txt", "w");
    if (!f_out) {
        perror("Failed to open output file");
        return NULL;
    }

    // Lock all accounts in order
    for (int i = 0; i < NUM_ACCS; i++) {
        pthread_mutex_lock(&account_arr[i].ac_lock);
    }

    // Update balances and write to output/pipe
    for (int i = 0; i < NUM_ACCS; i++) {
        account_arr[i].balance += (account_arr[i].reward_rate * account_arr[i].transaction_tracter);
        account_arr[i].transaction_tracter = 0;

        fprintf(f_out, "%i balance:  %.2f\n\n", i, account_arr[i].balance);

        // Write to auditor pipe
        char message[256];
        time_t now = time(NULL);
        char time_str[26];
        ctime_r(&now, time_str);
        time_str[strlen(time_str) - 1] = '\0';
        
        snprintf(message, sizeof(message), 
                "Applied interest to account %s. New Balance: $%.2f. Time of Update: %s\n",
                account_arr[i].account_number,
                account_arr[i].balance,
                time_str);
        write(pipe_fd[1], message, strlen(message));
    }

    // Unlock all accounts in reverse order
    for (int i = NUM_ACCS - 1; i >= 0; i--) {
        pthread_mutex_unlock(&account_arr[i].ac_lock);
    }

    fclose(f_out);
    return NULL;
}

// Auditor process function
void auditor_process(int read_fd) {
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