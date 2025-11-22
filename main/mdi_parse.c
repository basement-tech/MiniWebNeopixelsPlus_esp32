/*
 * parse an mdi command and take action
 *
 * valid commands and arguments:
 * 
 * STOP                 : stop the current sequence
 * START  filename.ext  : start the sequence file
 * START  label         : start the build-in sequence (previously loaded files too)
 * SERVOA ch  value     : move the servo at ch to angle given by value
 * SERVOP ch  value     : set the servo channel at ch to the pulsewidth given as value (0-255)
 * NEO   px  r g b      : set the pixel at px to the r g b values given
 * STRAND r g b         : set the whole strand to r g b
 * SCRIPT filename      : start the script file
 * NEXT                 : move to the next step in the script
 * PREVIOUS             : move to the previous step in the script
 * SYSINFO   where?
 * LIST
 * UPLOAD
 * DELETE
 * CAT filename  where?
 * 
 */

#include <string.h>

#include "esp_log.h"

#include "mdi_parse.h"
#include "neo_data.h"

static const char *TAG = "mdi_parse";

/*
 * TODO: decide whether to change this construct
 * or malloc() these strings
 */
int8_t argc = 0;  // number of arguments
char arg0[MDI_MAX_ARG_SIZE] = {0};
char arg1[MDI_MAX_ARG_SIZE] = {0};
char arg2[MDI_MAX_ARG_SIZE] = {0};
char arg3[MDI_MAX_ARG_SIZE] = {0};
char arg4[MDI_MAX_ARG_SIZE] = {0};
char arg5[MDI_MAX_ARG_SIZE] = {0};

char *argv[MDI_MAX_ARGS] = {
    arg0,
    arg1,
    arg2,
    arg3
};

mdi_command_t mdi_cmds[];

/*
 * parse an mdi command
 * mdi commands are delimited by \b characters.
 * just break the command line into individual strings
 * and return in global argc[][].
 * return the number of arguments (the command is argc[0])
 * and set the global argv if no errors.
 * 
 * note: leading and trailing blanks are expected to be
 * trimmed off before calling this function.
 */
int8_t mdi_parse_command(char *cmd, int8_t max_args, int8_t max_size)  {
    int8_t num_args = 0;
    int8_t i = 0;

    max_size--; // make room for null termination of string

    while((*cmd != '\0') && (num_args >= 0))  {
        if(*cmd != ' ')  {
            if(i < max_size)
                argv[num_args][i++] = *cmd;
            else
                num_args = -1;  // error, get out
        }
        else  {
            argv[num_args][i] = '\0';
            ESP_LOGI(TAG, "argument %d = %s", num_args, argv[num_args]);
            i = 0;
            if(++num_args >= max_args)  // next arg, if space is available
                num_args = -1;
        }
        cmd++;
    }
    return(num_args);
}

/*
 * return the index in mdi_cmds[] that matches
 * the cmd given as an argument.
 * -1 indicates not found.
 */
int8_t mdi_find_command(const char *cmd)  {
  int8_t ret = -1;
  for(int i = 0; mdi_cmds[i].cmdstr != NULL; i++)  {
    if(strcmp(cmd, mdi_cmds[i].cmdstr) == 0)
      ret = i;
  }
  return(ret);
}

/*
 * command action functions
 */
int8_t null_action(void *arg)  {
    ESP_LOGI(TAG, "MDI command = %s", (char *)arg);
    return NEO_SUCCESS;
}

int8_t mdi_servoa_action(void *arg)  {
    int8_t ret = NEO_SUCCESS;

    ESP_LOGI(TAG, "servoa command has %d arguments as follows:", argc);
    for(int8_t i = 0; i < argc; i++)
        ESP_LOGI(TAG, "  %s", argv[i]);
    
    return(ret);
}

int8_t mdi_master_action(char *cmd)  {
    int8_t ret = NEO_MDI_ERROR;
    int8_t mdi_index = -1;

    argc = mdi_parse_command(cmd, MDI_MAX_ARGS, MDI_MAX_ARG_SIZE);

    if((mdi_index = mdi_find_command(argv[0])) >= 0)  {
        ESP_LOGI(TAG, "MDI action found at index %d", mdi_index);
        ret = mdi_cmds[mdi_index].mdi_action(argv[0]);
    }
    else
        ESP_LOGE(TAG, "MDI action not found");

    return(ret);
}


mdi_command_t mdi_cmds[] = {
    { "STOP",   null_action },
    { "START",  null_action },
    { "SERVOA", mdi_servoa_action },
    { "SERVOP", null_action },
    { "NEO",    null_action },
    { NULL,     null_action }
};