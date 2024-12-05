#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string_parser.h"

int count_token(char* buf, const char* delim) {
    if (!buf || !delim) return 0;
    
    char* temp = strdup(buf);
    if (!temp) return 0;
    
    int count = 0;
    char* token = strtok(temp, delim);
    
    while (token != NULL) {
        count++;
        token = strtok(NULL, delim);
    }
    
    free(temp);
    return count;
}

command_line str_filler(char* buf, const char* delim) {
    command_line cmd;
    cmd.command_list = NULL;
    cmd.num_token = 0;
    
    if (!buf || !delim) return cmd;
    
    // Count tokens
    cmd.num_token = count_token(buf, delim);
    if (cmd.num_token == 0) return cmd;
    
    // Allocate space for command list (including NULL terminator)
    cmd.command_list = (char**)malloc((cmd.num_token + 1) * sizeof(char*));
    if (!cmd.command_list) {
        cmd.num_token = 0;
        return cmd;
    }
    
    // Make a copy of buf since strtok modifies the string
    char* temp = strdup(buf);
    if (!temp) {
        free(cmd.command_list);
        cmd.command_list = NULL;
        cmd.num_token = 0;
        return cmd;
    }
    
    // Tokenize and copy strings
    char* token = strtok(temp, delim);
    int i = 0;
    
    while (token != NULL && i < cmd.num_token) {
        cmd.command_list[i] = strdup(token);
        if (!cmd.command_list[i]) {
            // Clean up if strdup fails
            for (int j = 0; j < i; j++) {
                free(cmd.command_list[j]);
            }
            free(cmd.command_list);
            free(temp);
            cmd.command_list = NULL;
            cmd.num_token = 0;
            return cmd;
        }
        token = strtok(NULL, delim);
        i++;
    }
    
    // NULL terminate the command list
    cmd.command_list[cmd.num_token] = NULL;
    
    free(temp);
    return cmd;
}

void free_command_line(command_line* command) {
    if (!command || !command->command_list) return;
    
    for (int i = 0; i < command->num_token; i++) {
        if (command->command_list[i]) {
            free(command->command_list[i]);
        }
    }
    
    free(command->command_list);
    command->command_list = NULL;
    command->num_token = 0;
}