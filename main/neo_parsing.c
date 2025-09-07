/*
 * neo_parsing.c
 *
 * functions to parse various types of sequence file formats
 */
#include <stdio.h>

#include "esp_log.h"
#include "json_parser.h"

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
 * buf is expected to be all json; it is parsed as a null terminated string
 * (i.e. strlen(buf))
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
  char comment[MAX_NEO_COMMENT] = {0};

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
    json_obj_get_string(&jctx, "__comment", comment, sizeof(comment));  // extract the comment for display only

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
 * selection of functions for copying points from the user json
 * file to memory for playback.  malloc() memory if needed.
 * 
 * parse_pts_OG() : parse points like in the original implementation.
 * points have color(r, g, b, w), and a time delay parameters.  They are copied
 * to a preallocated space in the neo_sequences[] array of sequence data.
 * 
 *
 * 
 * arguments:
 *   jparse_ctx_t jctx  : pointer to points in the json heirarchy (from)
 *   uint8_t seq_idx    : place in the sequence array to put the data (to)
 *   int count          : how many points
 *   void *user         : points to any other data that is needed process the points
 * 
 * return:
 *   manupulating jctx directly via passed pointer thereto
 */
int8_t parse_pts_OG(jparse_ctx_t *pjctx, uint8_t seq_idx, void *user)  {

  int r = 0;
  int g = 0;
  int b = 0;
  int w = 0;
  int t = 0;

  int count = 0; // number of points

  json_obj_get_array(pjctx, "points", &count);  // down one level into the array of points

  if(count > MAX_NUM_SEQ_POINTS)  {
    ESP_LOGI(TAG, "Too many points in sequence file ... truncating");
    count = MAX_NUM_SEQ_POINTS;
  }

  for(uint16_t i = 0; i < count; i++)  {
    json_arr_get_object(pjctx, i); // index into the array, set jctx
    json_obj_get_int(pjctx, "r", &r);
    json_obj_get_int(pjctx, "g", &g);
    json_obj_get_int(pjctx, "b", &b);
    json_obj_get_int(pjctx, "t", &t);
    ESP_LOGD(TAG, "colors = %d %d %d %d  interval = %d", r, g, b, w, t);
    neo_sequences[seq_idx].point[i].red = r;
    neo_sequences[seq_idx].point[i].green = g;
    neo_sequences[seq_idx].point[i].blue = b;
    neo_sequences[seq_idx].point[i].white = w;
    neo_sequences[seq_idx].point[i].ms_after_last = t;
    json_arr_leave_object(pjctx);
  }

  json_obj_leave_array(pjctx);  // pop back out of the points array
  return(NEO_SUCCESS);
}


/*
 *
 * parse_pts_BW : "bitwise" sequence files contain on/off data in bit r, g, b, w fields
 * to describe a sequence where in each pixel can be addressed to form unique patterns.
 * the memory has to be malloc'ed based on the number of points in the sequence. the
 * pointer to the malloc'ed data is saved in a slot in the neo_sequences[] array.
 * the memory must be free'ed by the sequences/strategies stopping() function.
 * 
 * The idea is for this function to convert the json description of bitwise points to
 * the same binary format in memory.  That way the json based and binary file formatted
 * bitwise strategy files can share the same state machine functions.
 * 
 * 
 * ***NOTE: I never got this to fully work (you can see all of the debug statements, etc.)
 * for the json based bitwise functionality.
 * It seems that there's a bug in the json component whereby the bits json array spits out 
 * the first point correctly, but then just repeats the first point data from there on.
 * All of the mechanics seem to parse correctly, just the data is wrong.  I'll leave it for
 * reference and get back to it.  Meanwhile, I'm going to pursue a binary file version of 
 * the bitwise sequence so that the filesize (and required buffers) are managable.
 * WARNING: underlying structures, etc may have changed since I wrote this ... probably would
 * need to be updated to attempt getting it working again.
 * 
 * The intention was for this function to parse the json binary point data and 
 * store it in binary/bitwise format in the space that it malloc()'ed and pointed to
 * by neo_sequences[seq_idx].alt_points.
 * 
 * TODO: come back to this
 * 
 */
int8_t parse_pts_BW(jparse_ctx_t *pjctx, uint8_t seq_idx, void *bin_data)  {

  int8_t ret = -1;
  char color_str[16];
  char *jcolors[4] = {
    "r",
    "g",
    "b",
    "w",
    "t"
  };

  int r = 0;
  int g = 0;
  int b = 0;
  int w = 0;
  int t = 0;
  int cbits = 0;  // number of rows of "bits" in the json
  uint32_t p, d, c;  // traversing down the json

  jparse_ctx_t bjctx;  // for locally parsing the bonus string
  char depth[MAX_DEPTH_C_STR];  // how many pixels/pixels/row
  int count = 0; // number of points

  neo_sequences[seq_idx].alt_points = NULL;  // to detect if space was malloc'ed

  ESP_LOGI(TAG, "Parsing points as (BW) json bitwise");

  json_obj_get_array(pjctx, "points", &count);  // down one level into the array of points

  if(count > MAX_NUM_SEQ_POINTS)  {
    ESP_LOGI(TAG, "Too many points in sequence file ... truncating");
    count = MAX_NUM_SEQ_POINTS;
  }

  /*
   * malloc'ed and bitwise points
   * 
   * json_obj_get_array(&jctx, "points", &count) returns the number of elements in the json array.
   *   use it in the size of buffer to allocate using malloc()
   */
  uint16_t *bpoint = neo_sequences[seq_idx].alt_points;  // shorthand and mapping
  ESP_LOGI(TAG, "%d points to parse", count);
  for(p = 0; p < count; p++) {  //points
    ESP_LOGI(TAG, "For point %lu", p);
    if(json_arr_get_object(pjctx, p) != OS_SUCCESS) // index into the points array, set jctx
      ESP_LOGE(TAG, "json_arr_get_object(pjctx, p) error");

    if(json_obj_get_array(pjctx, "bits", &cbits) != OS_SUCCESS)  // into bits
      ESP_LOGE(TAG, "json_obj_get_array(pjctx, \"bits\", &cbits) error");
    else  {
      ESP_LOGI(TAG, "found %d elements in \"bits\" array", cbits);
      //now that we know all required, malloc()
      if(neo_sequences[seq_idx].alt_points == NULL)  {
        int32_t msize = count * (((PIXELS_PER_JSON_ROW/sizeof(uint8_t)) * NEO_NUM_COLORS) * cbits + sizeof(uint32_t));
        ESP_LOGI(TAG, "parse_pts_BW(): sequence demands malloc'ed memory of size %ld", msize);
        neo_sequences[seq_idx].alt_points = malloc(msize);
      }

      for(d = 0; d < cbits; d++)  {  //rows
        ESP_LOGI(TAG, "Row %lu", d);
        /*
        * pull the data from the json data
        */

        if(json_arr_get_object(pjctx, d) != OS_SUCCESS) // index into the row array, set pjctx
          ESP_LOGE(TAG, "json_arr_get_object(pjctx, d) error");
        for(c = 0; c < NEO_NUM_COLORS; c++)  {  //colors
          if(json_obj_get_string(pjctx, jcolors[c], color_str, sizeof(color_str)) != OS_SUCCESS)  // because json doesn't support hex
            ESP_LOGE(TAG, "json_obj_get_string(pjctx, jcolors[c], color_str, sizeof(color_str)) error");
          ESP_LOGI(TAG, "  %s: %s", jcolors[c], color_str);
          //*(bpoint  += (r * sizeof(neo_seq_cpoint_t)) + (c * sizeof(uint16_t))) = atoi(color_str);
          // Convert hex strings to values, if needed
          //uint16_t r_val = strtol(r, NULL, 0); // base 0 auto-detects "0x"
        }
        if(json_arr_leave_object(pjctx) != OS_SUCCESS)  // leave the row array element
          ESP_LOGE(TAG, "json_arr_leave_object(pjctx) error");
      }
    }
    if(json_arr_leave_array(pjctx) != OS_SUCCESS) // leave the bits array
      ESP_LOGE(TAG, "json_arr_leave_array(pjctx) error"); 

    // add read the time interval

    if(json_arr_leave_object(pjctx) != OS_SUCCESS)  // leave the points array element
      ESP_LOGE(TAG, "json_arr_leave_object(pjctx) error"); 
  }

  json_obj_leave_array(pjctx);  // pop back out of the points array

  //ESP_LOGI(TAG, "bitwise data in memory:");
  //for(int i = 0; i < (msize/2); i++)
    //ESP_LOGI(TAG, "%d:0x%x", i, bpoint[i]);

  return(ret);
}


/*
 * data validation for type "BIN_BBW"
 */
bool data_valid_BIN_BBW(void *pbin_len)  {
  if(*((uint16_t *)pbin_len) % sizeof(seq_bin_t) != 0)
    return(false);
  return(true);
}

/*
 * parse file for type "BIN_BBW"
 * arguments:
 *  char *buf    : buffer containing the balance of the file after filetype json header
 *  uint16_t len : the number of bytes of json in the balance of the file
 */
int8_t neo_proc_BIN_BBW(char *buf, int json_len, int binsize)  {
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
   * deserialize the portion of the file that is header json contents
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
     * if the label was found, copy the fields from the json header
     * then call the appropriate point parser
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

/*
 *
 * parse_pts_BBW : "bbitwise" binary sequence files contain on/off data in bit r, g, b, w fields
 * to describe a sequence where in each pixel can be addressed to form unique patterns.
 * the memory has to be malloc'ed based on the number of points in the sequence. the
 * pointer to the malloc'ed data is saved in a slot in the neo_sequences[] array.
 * the memory must be free'ed by the sequences/strategies stopping() function.
 * 
 * 
 */
int8_t parse_pts_BBW(jparse_ctx_t *pjctx, uint8_t seq_idx, void *bin_data)  {

  int8_t ret = -1;

  neo_sequences[seq_idx].alt_points = NULL;  // to detect if space was malloc'ed

  ESP_LOGI(TAG, "Parsing points as (BBW) binary bitwise");

  /*
   * if this function is called from a binary file type, there
   * is no json to parse.  Just copy alloate the space and copy it to 
   * the place indicated by the seq_idx structure and get out of Dodge.
   */
  if(pjctx == NULL)  {
    if((neo_sequences[seq_idx].alt_points = malloc(((bin_data_loc_t *)bin_data)->size)) == NULL) {
      ESP_LOGE(TAG, "Error allocating memory for points ... aborting");
      ret = NEO_FILE_LOAD_OTHER;
    }
    else  {
      memcpy(neo_sequences[seq_idx].alt_points, ((bin_data_loc_t *)bin_data)->loc, ((bin_data_loc_t *)bin_data)->size);
      ret = NEO_SUCCESS;
    }
  }
  else  {
    ESP_LOGE(TAG, "Error: binary point parser called with inappropriate arguments");
    ret = NEO_FILE_LOAD_OTHER;
  }

  return(ret);
}