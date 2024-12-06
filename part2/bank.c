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
} thread_data;

// pipe time
int pipe_fd[2];

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

    printf("Processing transactions (multi-threaded)\n");

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

    // create worker threads
    pthread_t worker_threads[NUM_WORKERS];
    thread_data worker_data[NUM_WORKERS];
    int transactions_per_worker = num_transactions / NUM_WORKERS;

    // assign start, end indices and transaction vals to thread data
    for (int i = 0; i < NUM_WORKERS; i++) {
        worker_data[i].transactions = transactions;
        worker_data[i].start_index = i * transactions_per_worker;
        worker_data[i].end_index = (i == NUM_WORKERS - 1) ? num_transactions : (i + 1) * transactions_per_worker;

        // start thread execution
        printf("Creating worker thread %d to process transactions %d-%d\n", i, worker_data[i].start_index, worker_data[i].end_index);
        pthread_create(&worker_threads[i], NULL, process_transaction, &worker_data[i]);
    }

    printf("Waiting for all threads to complete\n");

    // wait for all worker threads to finish
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(worker_threads[i], NULL);
        printf("Worker thread %d is finished\n", i);
    }

    printf("All workers finshed. Creating bank thread to update balances\n");

    // create bank thread
    pthread_t bank_thread;
    pthread_create(&bank_thread, NULL, update_balance, NULL);

    // wait for bank thread to finish
    pthread_join(bank_thread, NULL);

    // Print final statistics
    printf("\nProgram Statistics:\n");
    printf("----------------------------------------\n");
    printf("Total Transactions Processed: %d\n", stats.total_transactions);
    printf("Invalid Transactions Caught: %d\n", stats.invalid_transactions);
    printf("Successful Transfers: %d\n", stats.transfers);
    printf("Successful Deposits: %d\n", stats.deposits);
    printf("Successful Withdrawals: %d\n", stats.withdrawals);
    printf("Balance Checks Performed: %d\n", stats.checks);
    printf("----------------------------------------\n");
    printf("Process complete, all balances updated & Program Ending Successfully.\n\n");

    free(account_arr);
    free(cmd_arr);
    pthread_mutex_destroy(&account_mutex);

    return 0;
}

void* process_transaction(void* arg) {
    /* run by worker to handle transaction requests */
    thread_data *data = (thread_data*) arg;
    static int check_balance_count = 0; // check bal call counter (static to retain val between calls)

    // run process_trans for each trans in thread range
    for (int i = data->start_index; i < data->end_index; i++) {
        command_line *transaction = &data->transactions[i];
        int src_acc_ind = -1;
        int dst_acc_ind = -1;
        double trans_amount = -1;

        pthread_mutex_lock(&account_mutex); // lock before accessing shared data

        // get account index for account_arr
        for (int j = 0; j < NUM_ACCS; j++) {
            if (strcmp(account_arr[j].account_number, transaction->command_list[1]) == 0) {
                src_acc_ind = j;
                break;
            }
        }

        // check password
        if (strcmp(account_arr[src_acc_ind].password, transaction->command_list[2]) != 0) {
            pthread_mutex_unlock(&account_mutex); // unlock if password doesn't match (restarting loop)
            continue;
        }

        // do the transaction
        char trans = transaction->command_list[0][0];
        switch (trans) {
            case 'T':
                // transfer
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
                break;

            case 'C':
                // check balance
                check_balance_count++;
                if (check_balance_count % 500 == 0) {
                    // set time val
                    time_t now = time(NULL);
                    char time_str[26];
                    ctime_r(&now, time_str);
                    time_str[strlen(time_str) - 1] = '\0';

                    // write to pipe for auditor
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
                // deposit
                trans_amount = strtod(transaction->command_list[3], NULL);
                account_arr[src_acc_ind].balance += trans_amount;
                account_arr[src_acc_ind].transaction_tracter += trans_amount;
                stats.deposits++;
                stats.total_transactions++;
                break;

            case 'W':
                // withdrawal
                trans_amount = strtod(transaction->command_list[3], NULL);
                account_arr[src_acc_ind].balance -= trans_amount;
                account_arr[src_acc_ind].transaction_tracter += trans_amount;
                stats.withdrawals++;
                stats.total_transactions++;
                break;

            default:
                stats.invalid_transactions++;
                printf("Error: Invalid transaction type.\n");
                pthread_mutex_unlock(&account_mutex);
                return NULL;
        }

        pthread_mutex_unlock(&account_mutex); // unlock after accessing shared data
    }

    return 0;
}

void* update_balance(void* arg) {
    /* run by bank thread to update each account.
       return number of times updated each account */
    FILE* f_out = fopen("Output/output.txt", "w");

    pthread_mutex_lock(&account_mutex); // lock before updating balances
    for (int i = 0; i < NUM_ACCS; i++) {
        account_arr[i].balance += (account_arr[i].reward_rate * account_arr[i].transaction_tracter);
        account_arr[i].transaction_tracter = 0;

        // set time val
        time_t now = time(NULL);
        char time_str[26];
        ctime_r(&now, time_str);
        time_str[strlen(time_str) - 1] = '\0';
        // write final balance to pipe
        char message[128];
        snprintf(message, sizeof(message), 
                "Applied interest to account %s. New Balance: $%.2f. Time of Update: %s\n",
                account_arr[i].account_number, 
                account_arr[i].balance,
                time_str);
        write(pipe_fd[1], message, strlen(message));

        fprintf(f_out, "%i balance:  %.2f\n\n", i, account_arr[i].balance);
    }
    pthread_mutex_unlock(&account_mutex); // unlock after updating balances

    fclose(f_out);
    return 0;
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
// im a goat