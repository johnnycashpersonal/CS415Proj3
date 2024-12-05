#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "account.h"
#include "string_parser.h"

#define MAX_LINE 1024

account_t* read_accounts(const char* filename, int* num_accounts) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        perror("Error opening file");
        return NULL;
    }

    char line[MAX_LINE];
    
    // Read number of accounts
    if (fgets(line, MAX_LINE, file) == NULL) {
        fclose(file);
        return NULL;
    }
    *num_accounts = atoi(line);

    account_t* accounts = malloc(sizeof(account_t) * (*num_accounts));
    if (!accounts) {
        fclose(file);
        return NULL;
    }

    for (int i = 0; i < *num_accounts; i++) {
        // Skip "index N" line
        if (fgets(line, MAX_LINE, file) == NULL) break;

        // Read account number
        if (fgets(line, MAX_LINE, file) == NULL) break;
        line[strcspn(line, "\n")] = 0;
        strncpy(accounts[i].account_number, line, sizeof(accounts[i].account_number) - 1);
        accounts[i].account_number[sizeof(accounts[i].account_number) - 1] = 0;

        // Read password
        if (fgets(line, MAX_LINE, file) == NULL) break;
        line[strcspn(line, "\n")] = 0;
        strncpy(accounts[i].password, line, sizeof(accounts[i].password) - 1);
        accounts[i].password[sizeof(accounts[i].password) - 1] = 0;

        // Read balance
        if (fgets(line, MAX_LINE, file) == NULL) break;
        accounts[i].balance = atof(line);

        // Read reward rate
        if (fgets(line, MAX_LINE, file) == NULL) break;
        accounts[i].reward_rate = atof(line);

        accounts[i].transaction_tracter = 0.0;
        pthread_mutex_init(&accounts[i].ac_lock, NULL);

        // Debug output
        printf("Account %d loaded: %s, Balance: %.2f, Rate: %.3f\n", 
               i, accounts[i].account_number, accounts[i].balance, accounts[i].reward_rate);
    }

    fclose(file);
    return accounts;
}

account_t* find_account(account_t* accounts, int num_accounts, const char* account_number) {
    for (int i = 0; i < num_accounts; i++) {
        if (strcmp(accounts[i].account_number, account_number) == 0) {
            return &accounts[i];
        }
    }
    return NULL;
}

void write_output(account_t* accounts, int num_accounts) {
    FILE* main_output = fopen("output.txt", "w");
    if (!main_output) {
        perror("Error opening output.txt");
        return;
    }

    for (int i = 0; i < num_accounts; i++) {
        fprintf(main_output, "%d balance:\t%.2f\n\n", 
                i, accounts[i].balance);
    }
    fclose(main_output);

    // Write individual account files
    mkdir("output", 0777);  // Create output directory if it doesn't exist
    for (int i = 0; i < num_accounts; i++) {
        char filename[64];
        snprintf(filename, sizeof(filename), "output/account%d.txt", i);
        
        FILE* acc_file = fopen(filename, "w");
        if (!acc_file) {
            perror("Error opening account file");
            continue;
        }
        
        fprintf(acc_file, "%.2f\n", accounts[i].balance);
        fclose(acc_file);
    }
}

void process_transactions(account_t* accounts, int num_accounts, const char* filename, stats_t* stats) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        perror("Error opening transaction file");
        return;
    }

    char line[MAX_LINE];
    
    // Skip past account information section
    int num_accounts_in_file;
    fscanf(file, "%d\n", &num_accounts_in_file);
    
    // Skip account blocks (5 lines per account)
    for (int i = 0; i < num_accounts_in_file; i++) {
        for (int j = 0; j < 5; j++) {
            if (!fgets(line, MAX_LINE, file)) {
                fclose(file);
                return;
            }
        }
    }

    // Now process transactions
    while (fgets(line, MAX_LINE, file)) {
        // Skip empty lines
        if (strlen(line) <= 1) continue;
        
        // Remove newline if present
        line[strcspn(line, "\n")] = 0;
        
        command_line cmd = str_filler(line, " ");
        
        if (cmd.num_token > 0) {
            char* type = cmd.command_list[0];
            account_t* acc;
            
            switch(type[0]) {
                case 'T':  // Transfer
                    if (cmd.num_token == 5) {
                        stats->transfers++;
                        account_t* src = find_account(accounts, num_accounts, cmd.command_list[1]);
                        account_t* dst = find_account(accounts, num_accounts, cmd.command_list[3]);
                        
                        if (src && dst && strcmp(src->password, cmd.command_list[2]) == 0) {
                            double amount = atof(cmd.command_list[4]);
                            src->balance -= amount;
                            dst->balance += amount;
                            src->transaction_tracter += amount;
                        } else {
                            stats->invalid_transactions++;
                        }
                    }
                    break;

                case 'D':  // Deposit
                    if (cmd.num_token == 4) {
                        stats->deposits++;
                        acc = find_account(accounts, num_accounts, cmd.command_list[1]);
                        if (acc && strcmp(acc->password, cmd.command_list[2]) == 0) {
                            double amount = atof(cmd.command_list[3]);
                            acc->balance += amount;
                            acc->transaction_tracter += amount;
                        } else {
                            stats->invalid_transactions++;
                        }
                    }
                    break;

                case 'W':  // Withdraw
                    if (cmd.num_token == 4) {
                        stats->withdrawals++;
                        acc = find_account(accounts, num_accounts, cmd.command_list[1]);
                        if (acc && strcmp(acc->password, cmd.command_list[2]) == 0) {
                            double amount = atof(cmd.command_list[3]);
                            acc->balance -= amount;
                            acc->transaction_tracter += amount;
                        } else {
                            stats->invalid_transactions++;
                        }
                    }
                    break;

                case 'C':  // Check Balance
                    if (cmd.num_token == 3) {
                        stats->checks++;
                        acc = find_account(accounts, num_accounts, cmd.command_list[1]);
                        if (!(acc && strcmp(acc->password, cmd.command_list[2]) == 0)) {
                            stats->invalid_transactions++;
                        }
                    }
                    break;
            }
            stats->total_transactions++;
        }
        free_command_line(&cmd);
    }
    
    fclose(file);
    
    // Apply rewards at the end
    for (int i = 0; i < num_accounts; i++) {
        double reward = accounts[i].transaction_tracter * accounts[i].reward_rate;
        accounts[i].balance += reward;
        accounts[i].transaction_tracter = 0.0;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage: %s <input_file>\n", argv[0]);
        return 1;
    }

    int num_accounts;
    account_t* accounts = read_accounts(argv[1], &num_accounts);
    if (!accounts) return 1;

    // Initialize statistics
    stats_t stats = {0, 0, 0, 0, 0, 0};

    // Process transactions from the same input file
    process_transactions(accounts, num_accounts, argv[1], &stats);

    // Write results to output file
    write_output(accounts, num_accounts);

    // Print statistics and program state
    printf("\nProgram Statistics:\n");
    printf("Total Transactions: %d\n", stats.total_transactions);
    printf("Invalid Transactions: %d\n", stats.invalid_transactions);
    printf("Transfers: %d\n", stats.transfers);
    printf("Deposits: %d\n", stats.deposits);
    printf("Withdrawals: %d\n", stats.withdrawals);
    printf("Balance Checks: %d\n", stats.checks);
    printf("\nUpdate times: 1\n");  // For Part 1, we only update once at the end

    // Cleanup
    for (int i = 0; i < num_accounts; i++) {
        pthread_mutex_destroy(&accounts[i].ac_lock);
    }
    free(accounts);

    return 0;
}
