/*
 * neo_parsing.c
 *
 * functions to parse various types of sequence file formats
 */
#include <stdio.h>

#include "esp_log.h"

#include "neo_system.h"
#include "neo_ll_api.h"
#include "neo_data.h"
#include "neo_parsing.h"

#define TAG "neo_parsing"
#define B_RESERVE 14  // reserves space for the json tag, etc

/*
 * data validation for type "OG"
 */
bool data_valid_OG(void *user)  {
  return(true);
}
/*
 * parse file for type "OG"
 *
 * arguments:
 *   char *buf : balance of json after the preamble line
 *   int json_len : not used
 *   int binsize : not used
 */
int8_t neo_proc_OG(char *buf, int json_len, int binsize)  {
  int8_t ret = -1;

  jparse_ctx_t jctx;  // for json parsing
  int8_t seq_idx = -1;
  char label[MAX_NUM_LABEL] = {0};
  char strategy[MAX_NEO_STRATEGY] = {0};
  char bonus[MAX_NEO_BONUS-B_RESERVE] = {0};

  ESP_LOGI(TAG, "Balance of the file :\n%s", buf);

  /*
   * deserialize the json contents of the file which
   * is now in buf and is all json (no binary, i.e. "OG")
   */
  if(json_parse_start(&jctx, buf, strlen(buf)) != OS_SUCCESS)  {
    ESP_LOGE(TAG, "ERROR: Deserialization of file failed at the start ... no change in sequence\n");
    ret = NEO_FILE_LOAD_DESERR;
  }

  /*
   * parse it into the place indicated by the "label" in the file contents.
   */
  else  {
    json_obj_get_string(&jctx, "label", label, sizeof(label));  // used to point to place in sequence array
    json_obj_get_string(&jctx, "strategy", strategy, sizeof(strategy));  // copied to the sequence array
    json_obj_get_object_str(&jctx, "bonus", bonus, sizeof(bonus));  // reserialized for later use

    ESP_LOGI(TAG, "For sequence \"%s\" : ", label);

    /*
     * find the place in neo_sequences[] where the file contents should be copied/stored
     * (will also test for validity of label)
     */
    seq_idx = neo_find_sequence(label);  // use LABEL

    /*
     * iterate over the points in the array
     */
    if(seq_idx < 0)  {
      ret = NEO_FILE_LOAD_NOPLACE;
      ESP_LOGE(TAG, "ERROR: neo_load_sequence: no placeholder for %s in sequence array\n", label);
    }

    /*
     * if the label was found, load the points from the json file
     * into the neo_sequences[] array to be played out
     *
     * TODO: super-verbose for now for debugging
     */
    else  {
      /*
       * reserialize bonus for later use
       * Note: printf() is already in use to the memory footprint is blown already.
       */
      snprintf(neo_sequences[seq_idx].bonus,  MAX_NEO_BONUS, "%s", bonus);  // save BONUS
      ESP_LOGI(TAG, "Reserialized bonus: %s", neo_sequences[seq_idx].bonus);

      /*
       * save the strategy in the sequence array
       * NOTE: this will be more meaningful when the functionality
       * to detect if a file is alreadly loaded/don't reload is implemented
       * NOTE: neo_set_sequence(label, strategy) sets the active strategy (below).
       */
      strncpy(neo_sequences[seq_idx].strategy, strategy, sizeof(neo_sequences[seq_idx].strategy));  // save STRATEGY

      /*
       * validate strategy
       */
      seq_strategy_t strat = neo_set_strategy(strategy);
      if(strat == SEQ_STRAT_UNDEFINED)  {
        ret = NEO_STRAT_ERR;
        ESP_LOGE(TAG, "ERROR: neo_load_sequence: specified strategy not found");
      }
      else  {

        /*
        * move the color data into the sequence array using the function
        * appropriate for and registered in the jump table under the strategy.
        */
        seq_callbacks[strat].parse_pts(&jctx, seq_idx, NULL);

        /*
        * launch the newly loaded sequence
        */
        ret = neo_set_sequence(label, strategy);  // LAUNCH
      }
    }
    json_parse_end(&jctx);  // done with json
  }
  return(ret);
}


/*
 * data validation for type "BIN_BW"
 */
bool data_valid_BIN_BW(void *pbin_len)  {
  if(*((uint16_t *)pbin_len) % sizeof(seq_bin_t) != 0)
    return(false);
  return(true);
}
/*
 * parse file for type "BIN_BW"
 * arguments:
 *  char *buf    : buffer containing the balance of the file after filetype json header
 *  uint16_t len : the number of bytes of json in the balance of the file
 */
int8_t neo_proc_BIN_BW(char *buf, int json_len, int binsize)  {
  int8_t ret = -1;

  bin_data_loc_t bin_data;

  jparse_ctx_t jctx;  // for json parsing
  int8_t seq_idx = -1;
  char label[MAX_NUM_LABEL] = {0};
  char strategy[MAX_NEO_STRATEGY] = {0};
  char bonus[MAX_NEO_BONUS-B_RESERVE] = {0};
  char comment[MAX_NEO_COMMENT] = {0};

  //ESP_LOGI(TAG, "Balance of the file :\n%s", buf);

  /*
   * deserialize the json contents of the file which
   * is now in buf and is all json (no binary, i.e. "OG")
   */
  if(json_parse_start(&jctx, buf, json_len) != OS_SUCCESS)  {
    ESP_LOGE(TAG, "ERROR: Deserialization of file failed at the start ... no change in sequence\n");
    ret = NEO_FILE_LOAD_DESERR;
  }

  /*
   * parse it into the place indicated by the "label" in the file contents.
   */
  else  {
    json_obj_get_string(&jctx, "label", label, sizeof(label));  // used to point to place in sequence array
    json_obj_get_string(&jctx, "strategy", strategy, sizeof(strategy));  // copied to the sequence array
    json_obj_get_string(&jctx, "__comment", comment, sizeof(comment));  // extract the comment for display only
    json_obj_get_object_str(&jctx, "bonus", bonus, sizeof(bonus));  // reserialized for later use

    ESP_LOGI(TAG, "For sequence \"%s\" : ", label);

    /*
     * find the place in neo_sequences[] where the file contents should be copied/stored
     * (will also test for validity of label)
     */
    seq_idx = neo_find_sequence(label);  // use LABEL

    /*
     * iterate over the points in the array
     */
    if(seq_idx < 0)  {
      ret = NEO_FILE_LOAD_NOPLACE;
      ESP_LOGE(TAG, "ERROR: neo_load_sequence: no placeholder for %s in sequence array\n", label);
    }

    /*
     * if the label was found, load the points from the json file
     * into the neo_sequences[] array to be played out
     *
     * TODO: super-verbose for now for debugging
     */
    else  {
      /*
       * reserialize bonus for later use
       * Note: printf() is already in use to the memory footprint is blown already.
       */
      snprintf(neo_sequences[seq_idx].bonus,  MAX_NEO_BONUS, "%s", bonus);  // save BONUS
      ESP_LOGI(TAG, "Reserialized bonus: %s", neo_sequences[seq_idx].bonus);

      /*
       * save the strategy in the sequence array
       * NOTE: this will be more meaningful when the functionality
       * to detect if a file is alreadly loaded/don't reload is implemented
       * NOTE: neo_set_sequence(label, strategy) sets the active strategy (below).
       */
      strncpy(neo_sequences[seq_idx].strategy, strategy, sizeof(neo_sequences[seq_idx].strategy));  // save STRATEGY

      /*
       * validate strategy
       */
      seq_strategy_t strat = neo_set_strategy(strategy);
      if(strat == SEQ_STRAT_UNDEFINED)  {
        ret = NEO_STRAT_ERR;
        ESP_LOGE(TAG, "ERROR: neo_load_sequence: specified strategy not found");
      }
      else  {
        ESP_LOGI(TAG, "Using Strategy %s (%d)", strategy, strat);
        ESP_LOGI(TAG, "comment: %s", comment);

        /*
         * move the color data into the sequence array using the function
         * appropriate for and registered in the jump table under the strategy.
         */
        bin_data.size = binsize;
        bin_data.loc = (uint8_t*)buf+json_len;
        seq_callbacks[strat].parse_pts(NULL, seq_idx, &bin_data);

        /*
         * launch the newly loaded sequence
         */
        ret = neo_set_sequence(label, strategy);  // LAUNCH
      }
    }
    json_parse_end(&jctx);  // done with json
  }
  return(ret);
}
