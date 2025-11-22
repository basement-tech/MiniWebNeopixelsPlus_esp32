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
 * return:
 *   -1 : argument too long
 *   -2 : too many arguments
 * 
 * 
 * note: leading and trailing blanks are expected to be
 * trimmed off before calling this function.
 */

int8_t mdi_parse_command(char *cmd, int8_t max_args, int8_t max_size)  {
    int8_t num_args = 0;
    int8_t i = 0;
    bool done = false;

    max_size--; // make room for null termination of string

    while((done == false) && (num_args >= 0))  {
        switch(*cmd)  {
            case ' ':
                argv[num_args][i] = '\0';  // write string termination over space
                i = 0;
                cmd++;  // skip past the delimiter
                if(++num_args >= max_args)  // next arg, too many?
                    num_args = -2;
            break;

            case '\0':  // end of buffer
                argv[num_args][i] = '\0';  // string termination
                done = true;
            break;

            default:  // A-Z, etc.
                if(i < max_size)
                    argv[num_args][i++] = *cmd++;
                else
                    num_args = -1;  // too long
            break;
        }
    }
    return(++num_args);  // convert from index to count
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

    if((argc = mdi_parse_command(cmd, MDI_MAX_ARGS, MDI_MAX_ARG_SIZE)) > 0)  {
        if((mdi_index = mdi_find_command(argv[0])) >= 0)  {
            ESP_LOGI(TAG, "MDI action found at index %d", mdi_index);
            ret = mdi_cmds[mdi_index].mdi_action(argv[0]);
        }
        else
            ESP_LOGE(TAG, "MDI action not found");
    }
    else
        ESP_LOGE(TAG, "error parsing MDI command");

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