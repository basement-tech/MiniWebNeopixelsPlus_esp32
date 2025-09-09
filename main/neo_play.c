/*
 * neo_play.c
 *
 * functions to play out the neo_pixel patterns
 * (mostly the state machine)
 */

#include <string.h>
#include <stdint.h>
#include <sys/param.h>
#include <ctype.h>
#include <fcntl.h>
#include "esp_random.h"  // encryption grade random number generator
#include "esp_vfs.h"  //resolves read, close
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"  // high res timer
#include "driver/gptimer.h"  // general purpose timer
#include "json_parser.h"

#include "neo_system.h"
#include "neo_ll_api.h"
#include "neo_data.h"
#include "neo_parsing.h"

#define TAG "neo_play"

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)

/*
 * housekeeping for the sequence state machine
 */
seq_callbacks_t seq_callbacks[];
#define NEO_SEQ_START    0
#define NEO_SEQ_WAIT     1
#define NEO_SEQ_WRITE    2
#define NEO_SEQ_STOPPING 3
#define NEO_SEQ_STOPPED  4
static uint8_t neo_state = NEO_SEQ_STOPPED;  // state of the cycling state machine

seq_strategy_t current_strategy = SEQ_STRAT_POINTS;

uint64_t current_millis = 0; // mS of last update
int32_t current_index = 0;   // index into the pattern array

/*
 * compatibility/porting convenience
 */
uint64_t millis(void)  {
  return(esp_timer_get_time()/1000);
}

/*
 * define the general purpose timer for running the state machine
 */
static bool neo_timer_on_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)  {
  static BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(xneo_cycle_next_flag, &xHigherPriorityTaskWoken);
  return(true);
}
#define INTR_SQWAVE_FREQ   1000000  // timer frequency 1MHz, 1 tick=1us
void neo_timer_setup(void)  {
    ESP_LOGI(TAG, "Create state machine timer handle");
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = INTR_SQWAVE_FREQ,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = neo_timer_on_alarm_cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));

    ESP_LOGI(TAG, "Enable state machine timer with frequency of %d Hz", INTR_SQWAVE_FREQ);
    ESP_ERROR_CHECK(gptimer_enable(gptimer));

    ESP_LOGI(TAG, "Start state machine timer, period is %d uS", NEO_UPDATE_INTERVAL);
    gptimer_alarm_config_t alarm_config1 = {
        .reload_count = 0,
        .alarm_count = NEO_UPDATE_INTERVAL, // period
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config1));
    ESP_ERROR_CHECK(gptimer_start(gptimer));
}


/*
 * return the index in neo_sequences[] that matches
 * the label given as an argument.  Do *not* set the global
 * index value that is used to play the sequence.
 */
int8_t neo_find_sequence(const char *label)  {
  int8_t ret = -1;
  for(int i = 0; i < MAX_SEQUENCES; i++)  {
    if(strcmp(label, neo_sequences[i].label) == 0)
      ret = i;
  }
  return(ret);
}

/*
 * return the index in neo_file_procs[] that matches
 * the filetype given as an argument.
 */
int8_t neo_find_filetype(const char *filetype)  {
  int8_t ret = -1;
  for(int i = 0; (neo_file_procs[i].filetypes[0] != '\0'); i++)  {
    if(strcmp(filetype, neo_file_procs[i].filetypes) == 0)
      ret = i;
  }
  return(ret);
}


/*
 * which/set sequence are we playing out
 * returns: -1 if the label doesn't match a sequence
 * reset the playout index and state if the found index
 * is different than the currently running index.
 */
int8_t seq_index = -1;  // global used to hold the index of the currently running sequence
int8_t neo_set_sequence(const char *label, const char *strategy)  {
  int8_t ret = NEO_SUCCESS;
  int8_t new_index = 0;
  seq_strategy_t new_strat;


  /*
   * attempt to set the sequence
   */
  new_index = neo_find_sequence(label);
  ESP_LOGI(TAG, "neo_find_sequence returned new_index = %d", new_index);
  if(new_index >= 0)  {
    /*
     * is this a new sequence from the one running?
     */
    if(new_index != seq_index)  {
      ret = NEO_NEW_SUCCESS;
      seq_index = new_index;  // set the sequence index that is to be played

      /*
       * if sequence setting was successful, attempt to set the strategy
       *
       * allow for the strategy argument to be a null string so,
       * for example, in the case of a built-in it might remain
       * the initialized value
       */
      if(strategy[0] == '\0')  {  // built-in
        if((new_strat = neo_set_strategy(neo_sequences[seq_index].strategy)) == SEQ_STRAT_UNDEFINED)
          ret = NEO_STRAT_ERR;
        else
          ESP_LOGI(TAG, "Using built-in strategy %d", new_strat);
      }
      else {  // user sequence
        if((new_strat = neo_set_strategy(strategy)) == SEQ_STRAT_UNDEFINED)
          ret = NEO_STRAT_ERR;
        else
          ESP_LOGI(TAG, "Using USER strategy %d", new_strat);
      }

      /*
       * if all above was successful, set up the globals and start the sequence
       */
      if(ret == NEO_NEW_SUCCESS)  {
        current_index = 0;  // reset the pixel count
        current_strategy = new_strat;
        ESP_LOGI(TAG, "neo_set_sequence: set sequence to %d and strategy to %d\n", seq_index, current_strategy);
        neo_state = NEO_SEQ_START;  // cause the state machine to start at the start
      }
    }
    else
      ret = NEO_OLD_SUCCESS; 
  }
  else
    ESP_LOGE(TAG, "neo_set_sequence: Invalid sequence label");
  return(ret);
}

/*
 * check if the label matches a predefined USER button
 * NOTE: this was simplified when the filename attribute was
 * added to the html file
 */
int8_t neo_is_user(const char *label)  {
  int8_t ret = NEO_FILE_LOAD_NOTUSER;

  if(strncmp(label, "USER", 4) == 0)
    ret = NEO_SUCCESS;

  return(ret);
}

/*
 * test if the strategy is of the type where the memory
 * for points will be malloc'ed and the alt_points pointer used
 */
int8_t neo_is_seq_malloc(seq_strategy_t sequence)  {
  int8_t ret = NEO_FILE_LOAD_NOTUSER;

  if(sequence == SEQ_STRAT_BWISE)
    ret = NEO_SUCCESS;

  return(ret);
}



/*
 * look for a label matching the argument, label,
 * and load a sequence from file of the same name.
 * NOTE: currently the requested sequence placeholder of the name
 * requested must exist in neo_sequences[] for this to succeed.
 * 
 * finally set the newly loaded sequence as current and start it.
 *
 * return:   0: successfully loaded
 *          -1: file not found or error opening
 *          -2: error deserializing file
 */
int8_t neo_load_sequence(const char *file)  {

  int8_t ret = NEO_SUCCESS;

  struct stat file_stat;
  char buf[NEO_MAX_SEQ_FILE_SIZE] = {0};  // buffer in which to read the file contents
  char *pbuf_data = NULL;  // pointer in buf[] after the preamble
  char filepath[FILE_PATH_MAX] = {0};  // fully qualified path to file

  jparse_ctx_t jctx;  // for json parsing

  char filetype[16];  // file type as string extracted from json first row
  int json_len = 0;  // size of the balance of the json part of the file
  uint16_t hdr_len;  // length of the preamble json string
  uint16_t bin_len;  // calculated size of binary data


  /*
   * verify access to the filesystem by displaying partition info
   */
  size_t total = 0, used = 0;
  if(esp_littlefs_info(LITTLE_FS_PARTITION_LABEL, &total, &used) != ESP_OK)  {
    ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
    return(NEO_FILE_LOAD_OTHER);
  } 
  else {
    ret = NEO_SUCCESS;
    ESP_LOGI(TAG, "Filesystem Partition size: total: %d, used: %d", total, used);
  }

  /*
   * create the fully qualified path to the file
   */
  strncpy(filepath, LITTLE_FS_MOUNT_POINT, FILE_PATH_MAX);
  strncat(filepath, "/", (FILE_PATH_MAX - strlen(filepath)));
  strncat(filepath, file, (FILE_PATH_MAX - strlen(filepath)));

  /*
   * verify that the file exists and if so,
   * read the contents of the user sequence file and put it
   * in the character buffer buf.  deserialize the json and 
   * load it into the sequence array.
   */

  if (stat(file, &file_stat) == 0)  {
      ESP_LOGE(TAG, "ERROR: Filename %s does not exist in file system\n", file);
      ret = NEO_FILE_LOAD_NOFILE;
  }
  else  {

    ESP_LOGI(TAG, "Loading filename %s ...\n", file);

    /*
     * switching to fopen(), fread() in hopes that the 
     * buffering will be more thread-safe.
     * Not sure it helped, but I'll leave it since it functions
     * properly.
     */
    FILE *fp = fopen(filepath, "rb");  // -> OPEN FILE
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        return NEO_FILE_LOAD_OTHER;
    }
    else  {
      /*
       * read the file contents into buf and close the file
       */
      int read_bytes = fread(buf, 1, sizeof(buf), fp);
      buf[read_bytes] = '\0';  // terminate the char string
      fclose(fp);                      // -> CLOSE FILE
      ESP_LOGI(TAG, "Raw file contents:\n%s\n", buf);  // char *buf takes it from here

      /*
       * mark the start of the data after the preamble line with char *pbuf_data
       *
       * seems that the files have \r\n (od -c) at the end of each line,
       * but when reading it seems that it's only one char.
       */
      pbuf_data = NULL;
      for(hdr_len = 0; hdr_len < read_bytes; hdr_len++)  {
          if(buf[hdr_len] == '\n')  {
            pbuf_data = buf + ++hdr_len;
            break;
          }
      }

      /*
       * if we got to the end of the buffer with no newline,
       * the file is corrupt ... no guess possible.
       */
      if(hdr_len >= read_bytes)  {
        ESP_LOGE(TAG, "Error: No preamble line present");
        ret = NEO_FILE_LOAD_OTHER;
      }
      else  {
        /*
         * parse the header first to determine file type
         */
        if(json_parse_start(&jctx, buf, hdr_len) != OS_SUCCESS)  {
          ESP_LOGE(TAG, "ERROR: Deserialization of preamble failed ... don't know file type");
          ret = NEO_DESERR;
        }
        else  {
          if(json_obj_get_string(&jctx, "filetype", filetype, sizeof(filetype)) != OS_SUCCESS)  {
            ESP_LOGE(TAG, "ERROR: Header does not contain \"filetype\" ... don't know file type");
            ret = NEO_DESERR;
          }
          else if(json_obj_get_int(&jctx, "jsonlen", &json_len) != OS_SUCCESS)  {
            ESP_LOGE(TAG, "ERROR: Header does not contain \"jsonlen\"");
            ret = NEO_DESERR;
          }
          else  {
            ESP_LOGI(TAG, "Preamble filetype determined = \"%s\"", filetype);

            int8_t filetype_idx = -1;
            if((filetype_idx = neo_find_filetype(filetype)) < 0)
            {
              ESP_LOGE(TAG, "ERROR: neo_load_sequence: no placeholder for %s in filetype array\n", filetype);
              ret = NEO_FILE_LOAD_NOPLACE;
            }
            else
            {
              /*
               * most of this is ignored by "OG", json only ascii data file
               */
              ESP_LOGI(TAG, "parsing balance of sequence file base on filetype %s", filetype);
              ESP_LOGI(TAG, "total bytes in file = %d", read_bytes);
              ESP_LOGI(TAG, "minus header length   %d", hdr_len);
              ESP_LOGI(TAG, "minus json header     %d", json_len);
              ESP_LOGI(TAG, "                    ------");
              bin_len = read_bytes-hdr_len-json_len;
              ESP_LOGI(TAG, "binary/bitwise data   %d\n", bin_len);
              if(neo_file_procs[filetype_idx].data_valid(&bin_len) != true)  {
                ESP_LOGE(TAG, "ERROR: size of binary data indicates file is malformed");
                ret = NEO_FILE_LOAD_OTHER;
              }
              else
                ret = neo_file_procs[filetype_idx].neo_proc_seqfile(pbuf_data, json_len, bin_len);
            }
          }
          json_parse_end(&jctx);
        }
      }
    }
  }
  return(ret);
}

/*
 * helper for writing a single color to all pixels
 */
void neo_write_pixel(bool clear)  {
  if(clear != 0)  pixels_clear(); // Set all pixel colors to 'off'

  /*
    * send the next point in the sequence to the strand
    */
  for(int i=0; i < pixels_numPixels(); i++) { // For each pixel...
    pixels_setPixelColorRGB(i, neo_sequences[seq_index].point[current_index].red, 
                               neo_sequences[seq_index].point[current_index].green,
                               neo_sequences[seq_index].point[current_index].blue, 0);
  }
  pixels_show();   // Send the updated pixel colors to the hardware.
}

/*
 * blink a status color to the strip reps times
 * the color is from the Adafruit colorwheel representation of colors
 * t is the numbe of mS between changes on/off (i.e. blink rate/2)
 *
 * NOTE: this is a blocking function i.e. not suitable for use in the loop()
 * NOTE: the neopixel strand must have been initialized prior to calling this function
 * NOTE: this is not a thread safe function
 *
 */
void neo_n_blinks(uint8_t r, uint8_t g, uint8_t b, uint8_t w, int8_t reps, int32_t t)  {

  for(int8_t j = reps; j > 0; j--)  {
    /*
    * send the next point in the sequence to the strand
    */
    for(int i=0; i < pixels_numPixels(); i++) // For each pixel...
      pixels_setPixelColorRGB(i, r, g, b, w);
    pixels_show();   // Send the updated pixel colors to the hardware.

    vTaskDelay(t / portTICK_PERIOD_MS);

    pixels_clear();
    pixels_show();
    
    vTaskDelay(t / portTICK_PERIOD_MS);
  }
}

/*
 * initialize strand to a known state and set it to state machine to off/idle
 */
void neo_init(void)  {

  pixels_clear(); // Set all pixel colors to 'off'
  pixels_show();   // Send the updated pixel colors to the hardware.
  neo_state = NEO_SEQ_STOPPED;
  xSemaphoreTake(xneo_cycle_next_flag, portMAX_DELAY);  // should be immediately available at startup


  neo_timer_setup();  // start the state machine timer
}

/*
 * TODO: figure out a better way to do this
 * __asm__ __volatile__("" ::: "memory") is an assembly
 * language directive to keep the empty function from being
 * optimized in any way:
 * __asm__ or asm: GCC keyword for inline assembly
 * __volatile__: Prevents the compiler from optimizing the statement away
 * "": Empty assembly instruction — this means no actual machine instruction is generated
 * ::: "memory": Declares a "memory clobber", which tells the compiler
 */
//void noop(void) {vTaskDelay(10/portTICK_PERIOD_MS);}
void noop(void) {__asm__ __volatile__("" ::: "memory");}
void start_noop(bool clear) {}


/*
 * SEQ_STRAT_POINTS
 * each line in the json is a single point in the sequence.
 * the "t" times are mS between points
 * the sequence restarts at the end and runs continuously
 *
 * bonus: none
 *
 * NOTE: keep neo_points_start() in sync with neo_single_start()
 * when making changes.
 */
void neo_points_start(bool clear) {
  neo_write_pixel(true);  // clear the strand and write the first value
  current_millis = millis();
  neo_state = NEO_SEQ_WAIT;
}

void neo_points_write(void) {
  if(neo_sequences[seq_index].point[current_index].ms_after_last < 0)  // list terminator: nothing to write
    current_index = 0;
  neo_write_pixel(false);
  neo_state = NEO_SEQ_WAIT;
}

void neo_points_wait(void)  {
  uint64_t new_millis = 0;

  /*
    * if the timer has expired (or assumed that if current_millis == 0, then it will be)
    * i.e. done waiting move to the next state
    */
  if(((new_millis = millis()) - current_millis) >= neo_sequences[seq_index].point[current_index].ms_after_last)  {
    current_millis = new_millis;
    current_index++;
    neo_state = NEO_SEQ_WRITE;
  }

}

void neo_points_stopping(void)  {
   /*
    * move to top to avoid potential collision with late
    * coming meo_cycle_next()
    */
  neo_state = NEO_SEQ_STOPPED;

  pixels_clear(); // Set all pixel colors to 'off'
  pixels_show();   // Send the updated pixel colors to the hardware.
  current_index = 0;  // housekeepinb
  seq_index = -1; // so it doesn't match
  current_strategy = SEQ_STRAT_POINTS;  // housekeeping
}

// end of SEQ_STRAT_POINTS callbacks

/*
 * SEQ_STRAT_SINGLE
 * each line in the json is a single point in the sequence.
 * the "t" times are mS between points
 * the sequence runs the number of times given by the "count"
 * json parameter in the bonus string.
 *
 * NOTE: keep neo_points_start() in sync with neo_single_start()
 * when making changes.
 */
static int8_t single_repeats = 1;

void neo_single_start(bool clear) {
  jparse_ctx_t jctx;  // for json parsing
  char jbuf[MAX_NEO_BONUS];

  neo_write_pixel(true);  // clear the strand and write the first value

  /*
   * if the number of repeats given in the bonus value
   * seems to be valid use it to set the number of 
   * times the sequence is repeated, otherwise just indicate
   * that the sequence should be played once.
   */

  /*
   * obtain the number of times the "single" sequence will be run
   * based on the "bonus" parameter from the json sequence file
   */
  if(strlen(neo_sequences[seq_index].bonus) > 0)  {
    ESP_LOGD(TAG, "neo_single_start: bonus = %s", neo_sequences[seq_index].bonus);

    if(json_parse_start(&jctx, neo_sequences[seq_index].bonus, strlen(neo_sequences[seq_index].bonus)) != OS_SUCCESS)  {
      ESP_LOGE(TAG, "ERROR: Deserialization of bonus failed ... using zero\n");
      single_repeats = 1;  // default to 1 time through
    }
    else  {
      json_obj_get_string(&jctx, "count", jbuf, sizeof(jbuf));  // used to point to place in sequence array

      if(*jbuf == '\0')  {
        ESP_LOGE(TAG, "WARNING: slowp bonus has no member \"count\" ... using one\n");
        single_repeats = 1;
      }
      else  {
        if(atoi(jbuf) > INT8_MAX) 
          single_repeats = INT8_MAX;
        else
          single_repeats = atoi(jbuf);
        ESP_LOGI(TAG, "neo_single_start: single_repeats set to %d", single_repeats);
      }
      json_parse_end(&jctx);  // done with json
    }
  }
  else
    single_repeats = 1;

  /*
   * get the timing started
   */
  current_millis = millis();
  neo_state = NEO_SEQ_WAIT;
}

void neo_single_write(void) {
  if(neo_sequences[seq_index].point[current_index].ms_after_last < 0)  {  // list terminator
    current_index = 0;  // rewind in case we're going to play it again
    if(--single_repeats > 0)  {  // are we going to play it again?
      neo_state = NEO_SEQ_WAIT;  // yep
      neo_write_pixel(false);
    }
    else
      neo_state = NEO_SEQ_STOPPING;  // nope
  }
  else  {  // just write the point and continue
    neo_write_pixel(false);
    neo_state = NEO_SEQ_WAIT;
  }
}

// end of SEQ_STRAT_SINGLE callbacks

/*
 * SEQ_STRAT_SLOWP
 * this is a slowly moving pulse sequence
 * only a two points are expected in the json, from which
 * the endpoint/maximum (color and intensity)  and the starting intensity
 * of the pulse is taken
 *
 * "t" from the first line is interpreted a the total number of seconds for the wave
 * this is a calculated sequence (based on NEO_SLOWP_POINTS):
 * - the interval between changes is based on the "t" seconds parameter
 * - the delta change is calculated
 *
 * "bonus"  from the json sequence file is itself a little json string
 * that indicates as "count" the number of flickers and ["flicker"]["r"], ["g"] and ["b"],
 * the color of the flicker (could be 0, 0, 0 for dark or 255, 255, 255 for bright, for example).
 * the ["flicker"]["t"] might be used someday for the duration of the flicker,
 * especially for very long running fades.
 */
static int32_t slowp_idx = 0;  // counting through the NEO_SLOWP_POINTS
static int8_t slowp_dir = 1;  // +1 -1 to indicate the direction we're traveling
static uint32_t delta_time;  // calculated time between changes
static float delta_r, delta_g, delta_b;  // calculated increment for each color ... must be floats or gets rounded to 0 between calls
static float slowp_r, slowp_g, slowp_b;  // remember where we are in the sequence
static int16_t slowp_flickers[NEO_SLOWP_FLICKERS];  // random points to flicker
static int16_t slowp_flicker_idx = 0;
static int8_t flicker_count = 0;  // how many flickers
static uint8_t flicker_r, flicker_g, flicker_b;  // colors to flicker to

/*
 * Comparison function for qsort (seems that there's an "int" somewhere
 * buried that causes an invalid conversion of int16_t is used for type)
 * TODO: figure it out whilst wasting 200 bytes of RAM ... solved:
 * function must return an int no matter what types are being compared ... implemented, works.
 */
static int compare_int16_t(const void *a, const void *b) {
    int16_t c = *(int16_t*)a;
    int16_t d = *(int16_t*)b;
    return ((int)(c - d));
}

static uint8_t neo_check_range(int32_t testval)  {
  uint8_t retval = testval;
  if(testval < 0) retval = 0;
  if(testval > 255)  retval = 255;
  return(retval);
}

void neo_slowp_start(bool clear)  {

  slowp_idx = 0;
  slowp_dir = 1;  // start by going up
  slowp_flicker_idx = 0;  // start at the start
  flicker_count = 0;  // assume none to Start

  jparse_ctx_t jctx;  // for json parsing
  char jbuf[MAX_NEO_BONUS];
  int err = OS_SUCCESS;

  /*
   * calculate delta time in mS based on the first (and only)
   * line in the json sequence file
   */
  delta_time = (neo_sequences[seq_index].point[0].ms_after_last * 1000) / NEO_SLOWP_POINTS;

  /*
   * calculate the delta chance for each color
   * the first line in the json sequence has the max/endpoint
   * of the sequence
   *
   */
  delta_r = (neo_sequences[seq_index].point[1].red - neo_sequences[seq_index].point[0].red) / (float)NEO_SLOWP_POINTS;  // cast needed to force floating point math
  delta_g = (neo_sequences[seq_index].point[1].green - neo_sequences[seq_index].point[0].green) / (float)NEO_SLOWP_POINTS;
  delta_b = (neo_sequences[seq_index].point[1].blue - neo_sequences[seq_index].point[0].blue) / (float)NEO_SLOWP_POINTS;

  /*
   * start from the json specified starting point
   */
  slowp_r = neo_sequences[seq_index].point[0].red;
  slowp_g = neo_sequences[seq_index].point[0].green;
  slowp_b = neo_sequences[seq_index].point[0].blue;

  /*
   * obtain the random places where the lights will flicker
   * based on the "bonus" parameter from the json sequence file
   */
  if(strlen(neo_sequences[seq_index].bonus) > 0)  {
    ESP_LOGD(TAG, "neo_slowp_start: bonus = %s", neo_sequences[seq_index].bonus);

    if(json_parse_start(&jctx, neo_sequences[seq_index].bonus, strlen(neo_sequences[seq_index].bonus)) != OS_SUCCESS)  {
      ESP_LOGE(TAG, "ERROR: Deserialization of bonus failed ... using zero\n");
      flicker_count = 0;  // default to 1 time through
    }
    else  {
      json_obj_get_string(&jctx, "count", jbuf, sizeof(jbuf));  // used to point to place in sequence array

      if(*jbuf == '\0')  {
        ESP_LOGE(TAG, "WARNING: slowp bonus has no member \"count\" ... using one\n");
        flicker_count = 0;
      }
      else  {
        if(atoi(jbuf) > INT8_MAX)
          flicker_count = INT8_MAX;
        else
          flicker_count = atoi(jbuf);
        ESP_LOGI(TAG, "neo_slowp_start: flicker_count set to %d", flicker_count);

        /*
         * what color should the flickers be
         * in case colors are missing, set the ones we have
         */
        flicker_r = flicker_g = flicker_b = 255;  // set fallback
        if((err = json_obj_get_object(&jctx, "flicker")) != OS_SUCCESS)
          ESP_LOGE(TAG, "WARNING: slowp bonus has incomplete member \"flicker\" ... using white\n");
        else  {
          if(json_obj_get_string(&jctx, "r", jbuf, sizeof(jbuf)) == OS_SUCCESS)
            flicker_r = neo_check_range(atoi(jbuf));

          if(json_obj_get_string(&jctx, "g", jbuf, sizeof(jbuf)) == OS_SUCCESS)
            flicker_g = neo_check_range(atoi(jbuf));

          if(json_obj_get_string(&jctx, "b", jbuf, sizeof(jbuf)) == OS_SUCCESS)
            flicker_b = neo_check_range(atoi(jbuf));

          json_obj_leave_object(&jctx);
        }
        ESP_LOGI(TAG, "Setting slowp rgb color to (%u %u %u)\n", flicker_r, flicker_g, flicker_b);
      }
      json_parse_end(&jctx);  // done with json
    }
  }
  else
    flicker_count = 0;

  /*
   * boundary check and legacy where negative was ok
   */
  if(abs(flicker_count) > NEO_SLOWP_FLICKERS) flicker_count = NEO_SLOWP_FLICKERS;  //boundary check
  flicker_count = abs(flicker_count);  // legacy

  /*
   * calculate where to put the flickers based on a random number
   *
   * esp_random() returns uin32_t; slowp_flickers[] contains step number
   * in the sequence where to substitute a flicker.  The step numbers are
   * between 0 and NEO_SLOWP_POINTS.  So, divide by the difference to pull
   * the randoms into the right range.
   */
  static uint32_t scale = UINT32_MAX/NEO_SLOWP_POINTS;
  for(uint8_t j = 0; j < flicker_count; j++)  {
    slowp_flickers[j] = esp_random()/scale;
    if(slowp_flickers[j] == (int16_t)0)
      slowp_flickers[j] = 1;  // stay away from the turn-arounds
    else if(slowp_flickers[j] == (int16_t)(NEO_SLOWP_POINTS-1))
      slowp_flickers[j] = (NEO_SLOWP_POINTS-2);
  }

  ESP_LOGD(TAG, "Starting slowp: dr = %f, dg = %f, db = %f dt = %lu\n", delta_r, delta_g, delta_b, delta_time);
  ESP_LOGD(TAG, "Randoms are (unsorted):");
  for(uint8_t j = 0; j < flicker_count; j++)
    ESP_LOGD(TAG, "%d  ", slowp_flickers[j]);
  ESP_LOGD(TAG, "\n");

  /*
   * Sort the array in place
   * TODO: strange that it seems to only work with ints despite
   * all of the syntax to the contrary? ... solved: compare function must
   * return an int no matter which type is being sorted: implemented, works.
   */
  qsort(slowp_flickers, flicker_count, sizeof(int16_t), compare_int16_t);

  ESP_LOGD(TAG, "Randoms are (sorted):");
  for(uint8_t j = 0; j < flicker_count; j++)
    ESP_LOGD(TAG, "%d  ", slowp_flickers[j]);
  ESP_LOGD(TAG, "\n");

  uint8_t r = neo_check_range(slowp_r);
  uint8_t g = neo_check_range(slowp_g);
  uint8_t b = neo_check_range(slowp_b);

  /*
   * clear and write the starting value
   */
  pixels_clear();
  for(int i=0; i < pixels_numPixels(); i++)  // For each pixel...
      pixels_setPixelColorRGB(i, r, g, b, 0);
  pixels_show();   // Send the updated pixel colors to the hardware.

  current_millis = millis();

  neo_state = NEO_SEQ_WAIT;

}


void neo_slowp_write(void) {
  uint8_t r, g, b;

  //DEBUG_DEBUG("slowp_idx = %d\n", slowp_idx); // warning: burps out a lot of stuff

  /*
   * currently going up
   */
  if(slowp_dir > 0)  {
    if(++slowp_idx < NEO_SLOWP_POINTS)  {  // have not reached the top of the sequence
      slowp_r += delta_r;  // increment by the delta per point change
      slowp_g += delta_g;
      slowp_b += delta_b;
    }
    else  {
      slowp_dir = -1;  // change to going down
      slowp_idx--;  // decrement back to the top since we incremented past

      /*
       * reset to the ending point in case of rounding error
       */
      slowp_r = neo_sequences[seq_index].point[1].red;
      slowp_g = neo_sequences[seq_index].point[1].green;
      slowp_b = neo_sequences[seq_index].point[1].blue;
    }
  }

  /*
   * currently going down
   */
  else  {
    if(--slowp_idx >= 0)  { 
      slowp_r -= delta_r;
      slowp_g -= delta_g;
      slowp_b -= delta_b;
    }
    else  {
      slowp_dir = 1;  // change to going down
      slowp_idx++;

      /*
       * reset to the starting point  in case of rounding error
       */
      slowp_r = neo_sequences[seq_index].point[0].red;
      slowp_g = neo_sequences[seq_index].point[0].green;
      slowp_b = neo_sequences[seq_index].point[0].blue;
    }
  }

  /*
   * send the next point in the sequence to the strand
   */
  if(flicker_count == 0)  {  // no flickers
          r = neo_check_range(slowp_r);
          g = neo_check_range(slowp_g);
          b = neo_check_range(slowp_b);
  }
  else  {
    if(slowp_idx == slowp_flickers[slowp_flicker_idx])  {
        r = flicker_r;
        g = flicker_g;
        b = flicker_b;

      if(slowp_dir > 0)  {
        if(++slowp_flicker_idx >= flicker_count)
          slowp_flicker_idx = flicker_count - 1;
      }
      else  {
        if(--slowp_flicker_idx < 0)
          slowp_flicker_idx = 0;
      }
//      DEBUG_DEBUG("slowp_flicker_idx = %d\n", slowp_flicker_idx);
    }
    else  {
      r = neo_check_range(slowp_r);
      g = neo_check_range(slowp_g);
      b = neo_check_range(slowp_b);

    }
  }
  for(int i=0; i < pixels_numPixels(); i++)  // For each pixel...
      pixels_setPixelColorRGB(i, r, g, b, 0);

  pixels_show();   // Send the updated pixel colors to the hardware.

#ifdef DEBUG_HACK
  ESP_LOGD(TAG, "neo_slowp_write: Showed %d  %d  %d\n", slowp_r, slowp_g, slowp_b);
  while(Serial.available() == 0);
  Serial.read();
#endif

  neo_state = NEO_SEQ_WAIT;
}


void neo_slowp_wait(void)  {
  uint64_t new_millis = 0;

  /*
    * if the timer has expired (or assumed that if current_millis == 0, then it will be)
    * i.e. done waiting move to the next state
    */
  if(((new_millis = millis()) - current_millis) >= delta_time)  {
    current_millis = new_millis;
    neo_state = NEO_SEQ_WRITE;
  }

}

// end of SEQ_STRAT_SLOWP callbacks

/*
 * SEQ_STRAT_PONG
 * one lighted pixel moves from one end to the other and back
 * in a continuous ping-poing
 * two lines are expected in the sequence file:
 *   - first line is the starting intensity/color and time between movements
 *   - second line is the ending intensity/color
 *
 * NOTE: we're going to borrow much of the functionality and global variables
 *       from slowP ... but no flicker stuff:
 *  slowp_idx : tracks the lit pixel
 *  slowp_dir : going up or down the strand (pinging or ponging)
 */
uint16_t p_num_pixels = 0;  // will use this shortcut alot
int16_t pong_repeats = -1;
void neo_pong_start(bool clear)  {

  slowp_idx = 0;
  slowp_dir = 1;  // start by going up

  jparse_ctx_t jctx;  // for json parsing
  char jbuf[MAX_NEO_BONUS];

  pong_repeats = -1; // start with continuous

  /*
   * obtain the number of times the "single" sequence will be run
   * based on the "bonus" parameter from the json sequence file
   */
  if(strlen(neo_sequences[seq_index].bonus) > 0)  {
    ESP_LOGD(TAG, "neo_pong_start: bonus = %s", neo_sequences[seq_index].bonus);

    if(json_parse_start(&jctx, neo_sequences[seq_index].bonus, strlen(neo_sequences[seq_index].bonus)) != OS_SUCCESS)  {
      ESP_LOGE(TAG, "ERROR: Deserialization of bonus failed ... using minus one\n");
    }
    else  {
      json_obj_get_string(&jctx, "count", jbuf, sizeof(jbuf));  // used to point to place in sequence array

      if(*jbuf == '\0')  {
        ESP_LOGE(TAG, "WARNING: pong bonus has no member \"count\" ... using minus one\n");
      }
      else  {
        if(atoi(jbuf) > INT16_MAX)
          pong_repeats = INT16_MAX;
        else
          pong_repeats = atoi(jbuf);
        ESP_LOGI(TAG, "neo_pong_start: pong_repeats set to %d", pong_repeats);
      }
      json_parse_end(&jctx);  // done with json
    }
  }



  p_num_pixels = pixels_numPixels();

  /*
   * calculate delta time in mS based on the first
   * line in the json sequence file
   */
  delta_time = (neo_sequences[seq_index].point[0].ms_after_last) / p_num_pixels;

  /*
   * calculate the delta chance for each color
   * the first line in the json sequence has the max/endpoint
   * of the sequence
   *
   */
  delta_r = (neo_sequences[seq_index].point[1].red - neo_sequences[seq_index].point[0].red) / (float)(p_num_pixels-1);  // cast needed to force floating point math
  delta_g = (neo_sequences[seq_index].point[1].green - neo_sequences[seq_index].point[0].green) / (float)(p_num_pixels-1);
  delta_b = (neo_sequences[seq_index].point[1].blue - neo_sequences[seq_index].point[0].blue) / (float)(p_num_pixels-1);

  /*
   * start from the json specified starting point
   */
  slowp_r = neo_sequences[seq_index].point[0].red;
  slowp_g = neo_sequences[seq_index].point[0].green;
  slowp_b = neo_sequences[seq_index].point[0].blue;

  /*
   * clear and set the first point here
   */
  pixels_clear();
  pixels_setPixelColorRGB(slowp_idx, neo_check_range(slowp_r),
                                  neo_check_range(slowp_g),
                                  neo_check_range(slowp_b), 0);  // turn on the next one
  pixels_show();

  current_millis = millis();

  ESP_LOGD(TAG, "Starting pong: dr = %f, dg = %f, db = %f dt = %lu\n", delta_r, delta_g, delta_b, delta_time);

  neo_state = NEO_SEQ_WAIT;
}

void neo_pong_write(void) {
  
  /*
   * calculate the rgb values
   */
  if(slowp_dir > 0)  {  // currently going up
    slowp_idx++;
    if(slowp_idx < p_num_pixels)  {  // have not reached the top of the sequence
      slowp_r += delta_r;
      slowp_g += delta_g;
      slowp_b += delta_b;
    }
    else  {
      slowp_dir = -1;  // change to going down
      slowp_idx--;

      /*
       * reset to the ending point in case of rounding error
       */
      slowp_r = neo_sequences[seq_index].point[1].red;
      slowp_g = neo_sequences[seq_index].point[1].green;
      slowp_b = neo_sequences[seq_index].point[1].blue;
    }
  }

  /*
   * currently going down
   */
  else  {
    slowp_idx--;
    if(slowp_idx >= 0)  {
      slowp_r -= delta_r;
      slowp_g -= delta_g;
      slowp_b -= delta_b;
    }
    else  {
      slowp_dir = 1;  // change to going down
      slowp_idx++;

      /*
       * reset to the starting point  in case of rounding error
       */
      slowp_r = neo_sequences[seq_index].point[0].red;
      slowp_g = neo_sequences[seq_index].point[0].green;
      slowp_b = neo_sequences[seq_index].point[0].blue;

      if(pong_repeats > (int16_t)0)
        pong_repeats--;
    }
  }

  /*
   * send the next point in the sequence to the strand
   */
  pixels_clear();  // first turn them all off
  pixels_setPixelColorRGB(slowp_idx, neo_check_range(slowp_r),
                                     neo_check_range(slowp_g),
                                     neo_check_range(slowp_b), 0);  // turn on the next one
  pixels_show();   // Send the updated pixel colors to the hardware.

  if(pong_repeats == (int16_t)(-1))  // not counting keep going
    neo_state = NEO_SEQ_WAIT;
  else if (pong_repeats > 0)         // counting and still have some repeats to go
    neo_state = NEO_SEQ_WAIT;
  else                               // counting and done
    neo_state = NEO_SEQ_STOPPING;
}

// end of SEQ_STRAT_PONG

#ifdef RAINBOW_PORTED
/*
 * SEQ_STRAT_RAINBOW
 * cycle a rainbow color pallette along the whole strip
 * (adapted from the Adafruit strandtest example)
 */
long firstPixelHue = 0;
void neo_rainbow_start(bool clear)  {
  pixels_clear();
  pixels_show();

  firstPixelHue = 0;

  current_millis = millis();

  neo_state = NEO_SEQ_WRITE;

}

/*
 * wait a fixed 10mS
 */
void neo_rainbow_wait(void)  {
  uint64_t new_millis = 0;

  /*
    * if the timer has expired (or assumed that if current_millis == 0, then it will be)
    * i.e. done waiting move to the next state
    */
  if(((new_millis = millis()) - current_millis) >= 10)  {
    current_millis = new_millis;
    neo_state = NEO_SEQ_WRITE;
  }
}

/*
 * advance and write a pixel
 */
void neo_rainbow_write(void) {
  pixels_rainbow(firstPixelHue);
  pixels_show();

  firstPixelHue += 256;

  if(firstPixelHue >= 5*65536)
    firstPixelHue = 0;

  neo_state = NEO_SEQ_WAIT;

}

void neo_rainbow_stopping(void)  {
  pixels_clear(); // Set all pixel colors to 'off'
  pixels_show();   // Send the updated pixel colors to the hardware.

  seq_index = -1; // so it doesn't match

  neo_state = NEO_SEQ_STOPPED;
}

// end of SEQ_STRAT_RAINBOW callbacks
#endif // RAINBOW_PORTED

/*
 * SEQ_STRAT_BWISE
 * read a file containing bit pattern, addressing of individual
 * pixels in the strand using a single R, G, B value set, and 
 * play it out
 */
uint8_t bw_r, bw_g, bw_b, bw_w;  // for the on color
int8_t bw_idepth = 1;  // number of n pixel chunks ... extracted on start
int32_t bw_ms_after_last;  // time interval between points; read dynamically

/*
 * extract the on/off desired bitwise state of the array
 * and write r, g, b to the array
 * 
 * use a mask to extract the bitwise on/off pixel/color value
 * and write the global bw_r, g, b as indicated.
 * 
 * this function will set pixel values up to the physical number
 * of pixels defined in pmonconfig (i.e. eeprom)
 * 
 * 
 * arguments:
 *   int current_index : the point in the sequence
 *    (NOTE: this is a bitwise point representing all pixels)
 */
int32_t neo_bitwise_write_point(bool clear) {
  uint32_t mask = 0x00000001;  // first pixel
  p_num_pixels = pixels_numPixels();  // number of pixels set in config eeprom
  uint16_t pixel_cnt = 0;  // how many pixels have been set
  seq_bin_t *cpointrow;  // shorthand pointer to start of 32 bit row
  int8_t d = 0;  // depth/row counter
  uint8_t r, g, b, w;
  int32_t t = 1000;  // time between points

  if(clear == true)  pixels_clear(); // Set all pixel colors to 'off'

  cpointrow = (seq_bin_t *)neo_sequences[seq_index].alt_points;
  cpointrow += (current_index * bw_idepth);  // increments by the size of cpointrow
  ESP_LOGI(TAG, "cpointrow - 0x%x", (unsigned int)cpointrow);

  pixel_cnt = 0;  // being overly obvious
  for(d = 0; d < bw_idepth; d++)  {  // rows (i.e. pixel depth )
    ESP_LOGI(TAG, "Offset from data %d", cpointrow->o);
    mask = 0x00000001;
    for(int8_t p = 0; p < PIXELS_PER_JSON_ROW; p++)  {  // pixels
      if(pixel_cnt < p_num_pixels)  {
        r = (cpointrow->r  & mask) * bw_r;
        g = (cpointrow->g  & mask) * bw_g;
        b = (cpointrow->b  & mask) * bw_b;
        w = (cpointrow->w  & mask) * bw_w;
        pixels_setPixelColorRGB(p, r, g, b, w);
        t = cpointrow->d;  // delay, only the last one counts
        pixel_cnt++;
      }
      mask = mask << 1;  // next pixel in this color's mask
    }
    cpointrow++;
  }
  pixels_show();
  return(t);  // pointing to time interval
}

/*
 * parse the json bonus to know the on rgb, set the timer
 * and write the first point.
 */
void neo_bitwise_start(bool clear)  {
  jparse_ctx_t bjctx;  // for locally parsing the bonus string
  int bval = 0;

  /*
   * set a default color in case the json parsing doesn't work
   * red to indicate issue
   */
  bw_r = 64;
  bw_g = 0;
  bw_b = 0;
  bw_w = 0;

  /*
   * parse the depth and color to be assigned to on pixels from the passed
   * bonus string
   * NOTE: don't really need to parse the bonus string here ... leave for reference.
   */
  if(json_parse_start(&bjctx, neo_sequences[seq_index].bonus, strlen(neo_sequences[seq_index].bonus)) != OS_SUCCESS)  {
    ESP_LOGE(TAG, "ERROR: bitwise_start: Deserialization of bonus\n");

  }
  else  {
    json_obj_get_int(&bjctx, "depth", &bval);  // used to point to place in sequence array
      bw_idepth = bval;
    json_obj_get_object(&bjctx, "brightness");  // reserialized for later use
    json_obj_get_int(&bjctx, "r", &bval);
    bw_r = bval;
    json_obj_get_int(&bjctx, "g", &bval);
    bw_g = bval;
    json_obj_get_int(&bjctx, "b", &bval);
    bw_b = bval;
    json_obj_get_int(&bjctx, "w", &bval);
    bw_w = bval;
    json_obj_leave_object(&bjctx);  // leave brightness
    json_parse_end(&bjctx);  // leave bonus
  }
  ESP_LOGI(TAG, "Using on values of %u %u %u %u", bw_r, bw_g, bw_b, bw_w);

  current_millis = millis();  // set the timer

  // write the first pixel
  bw_ms_after_last =  neo_bitwise_write_point(true);
  ESP_LOGI(TAG, "Using time interval of %ld", bw_ms_after_last);
  neo_state = NEO_SEQ_WAIT;

}

void neo_bitwise_write(void) {
  if(bw_ms_after_last < 0)  {  // list terminator from the last write: nothing to write
    current_index = 0;
    ESP_LOGI(TAG, "Back to point %ld", current_index);
  }
  bw_ms_after_last = neo_bitwise_write_point(false);
  ESP_LOGI(TAG, "Using time interval of %ld", bw_ms_after_last);
  neo_state = NEO_SEQ_WAIT;
}

void neo_bitwise_wait(void)  {
  uint64_t new_millis = 0;

  /*
    * if the timer has expired (or assumed that if current_millis == 0, then it will be)
    * i.e. done waiting move to the next state
    */
  if(((new_millis = millis()) - current_millis) >= bw_ms_after_last)  {
    current_millis = new_millis;
    current_index++;
    neo_state = NEO_SEQ_WRITE;
  }

}

void neo_bitwise_stopping(void)  {
   /*
    * move to top to avoid potential collision with late
    * coming meo_cycle_next()
    */
  neo_state = NEO_SEQ_STOPPED;

  pixels_clear(); // Set all pixel colors to 'off'
  pixels_show();   // Send the updated pixel colors to the hardware.

  if(neo_sequences[seq_index].alt_points != NULL)  {
    free(neo_sequences[seq_index].alt_points);
    ESP_LOGI(TAG, "binary point data free()'ed");
  }

  current_index = 0;  // housekeeping
  seq_index = -1; // so it doesn't match
  current_strategy = SEQ_STRAT_POINTS;  // housekeeping

}
// end of SEQ_STRAT_BWISE

/*
 * function calls by filetype
 * one of these functions are called to process the char *buf 
 * containing the balance of the file after the first line header
 * determines the filetype.
 */

neo_ftype_t neo_file_procs[] = {
  {"OG", neo_proc_OG, data_valid_OG},
  {"BIN_BW", neo_proc_BIN_BBW, data_valid_BIN_BBW},
  {"\0", NULL, NULL}
};

/*
 * function calls by strategy for each state in the playback machine
 * TODO: delete the 'x' before the labels after implementing a strategy
 */
seq_callbacks_t seq_callbacks[NEO_SEQ_STRATEGIES] = {
//  strategy              label             prep            start                wait              write                stopping             stopped
  { SEQ_STRAT_POINTS,    "points",    parse_pts_OG,     neo_points_start,   neo_points_wait,   neo_points_write,    neo_points_stopping,     noop},
  { SEQ_STRAT_SINGLE,    "single",    parse_pts_OG,     neo_single_start,   neo_points_wait,   neo_single_write,    neo_points_stopping,     noop},
  { SEQ_STRAT_CHASE,     "xchase",    parse_pts_OG,      start_noop,           noop,               noop,                 noop,               noop},
  { SEQ_STRAT_PONG,      "pong",      parse_pts_OG,     neo_pong_start,     neo_slowp_wait,    neo_pong_write,      neo_points_stopping,     noop},
//  { SEQ_STRAT_RAINBOW,   "rainbow", parse_pts_OG,      neo_rainbow_start, neo_rainbow_wait,  neo_rainbow_write,    neo_rainbow_stopping,     noop},
  { SEQ_STRAT_SLOWP,     "slowp",     parse_pts_OG,     neo_slowp_start,    neo_slowp_wait,    neo_slowp_write,     neo_points_stopping,     noop},
  { SEQ_STRAT_BWISE,     "bitwise",   parse_pts_BW,       start_noop,            noop,                noop,          neo_bitwise_stopping,    noop},
  { SEQ_STRAT_BBWISE,    "bbitwise",  parse_pts_BBW,    neo_bitwise_start,  neo_bitwise_wait,  neo_bitwise_write,     neo_bitwise_stopping,    noop},
};




/*
 * expose a method to set the strategy from the "main"
 * look through the labels for a match with the argument
 * and set the global 
 */
seq_strategy_t neo_set_strategy(const char *sstrategy)  {
  seq_strategy_t ret = SEQ_STRAT_UNDEFINED;

  for(int8_t i=0; i < NEO_SEQ_STRATEGIES; i++)  {
    if(strcmp(sstrategy, seq_callbacks[i].label) == 0)  {
      ret = seq_callbacks[i].strategy;
    }
  }
  return(ret);
}

/*
 * check if the specified time since last change has occured
 * and update the strand if so.
 */
void neo_cycle_next(void)  {

  switch(neo_state)  {

    case NEO_SEQ_STOPPED:
      seq_callbacks[current_strategy].stopped();
      break;

    case NEO_SEQ_STOPPING:
      seq_callbacks[current_strategy].stopping();
      break;

    case NEO_SEQ_START:
      seq_callbacks[current_strategy].start(true);  // clear the strand and write the first value
      break;
    
    case NEO_SEQ_WAIT:
      seq_callbacks[current_strategy].wait();
      break;
    
    case NEO_SEQ_WRITE:
      seq_callbacks[current_strategy].write();
      break;

    default:
      ESP_LOGD(TAG, "Invalid State");  //thread safety test
      break;
  }
}

/*
 * stop the sequence i.e. turn off neopixel strand
 */
void neo_cycle_stop(void)  {
  neo_state = NEO_SEQ_STOPPING;
  //seq_index = -1;  // so it doesn't match ... move to _stopping()
}

/*
 * was a new sequence posted from the controlling process
 * NOTE: neo_set_sequence() determines if this is different
 * than the currently running sequence.
 */
int8_t neo_new_sequence(void)  {
  int8_t neoerr = NEO_SUCCESS;
  neo_mutex_data_t l_neo;  // local copy so as not to hold the mutex

  l_neo.new_data = false;  // was a new request made

  /*
   * try to grab the semaphore to check if a new sequence was requesed
   * (just poll, don't wait ... you'll get it the next time around)
   */
  if(xSemaphoreTake(xneoMutex, 0) == pdTRUE)  {
    if(neo_mutex_data.new_data == true)  {
      memcpy(&l_neo, &neo_mutex_data, sizeof(neo_mutex_data_t));
      neo_mutex_data.new_data = false;
      // only uncomment the following msg statement if you slow down the update rate
      //ESP_LOGI(TAG, "neo_new_sequence: new data received and successful xneoMutex take");
    }
    xSemaphoreGive(xneoMutex);  // be done with it ASAP
  }

  if(l_neo.new_data == true)  {
    /*
    * process the button that was pressed based on the seq string
    */
    if(l_neo.sequence[0] != '\0')  {
      ESP_LOGI(TAG, "neo_new_sequence:  %s", l_neo.sequence);

      /*
        * was it the stop button
        */
      if(strcmp(l_neo.sequence, "STOP") == 0)  {
        if((neo_state != NEO_SEQ_STOPPED) && (neo_state != NEO_SEQ_STOPPING))  {
          neoerr = NEO_NEW_SUCCESS;
          neo_cycle_stop();
        }
        else
          neoerr = NEO_OLD_SUCCESS;  // no change
      }

      /*
        * if not STOP, see if it was a USER defined sequence
        * if so, load the file and set the sequence and strategy
        */
      else if((neo_is_user(l_neo.sequence)) == NEO_SUCCESS)  {
        if((neoerr = neo_load_sequence(l_neo.file)) < NEO_SUCCESS)
          ESP_LOGE(TAG, "Error loading sequence file after proper detection");
      }

      /*
        * if not STOP or USER-x, then attempt to set the sequence,
        * assuming that it's a pre-defined button.
        * strategies are hardcoded for built in sequences.
        */
      else  {
        if((neoerr = neo_set_sequence(l_neo.sequence, "")) < NEO_SUCCESS)
          ESP_LOGI(TAG, "ERROR: Error setting sequence after proper detection");
      }
    }
    else  {
      neoerr = NEO_NOPLACE;
      ESP_LOGI(TAG, "ERROR: \"sequence\" not found in json data");
    }
  }
  if((neoerr >= NEO_SUCCESS) && (l_neo.resp_reqd == false))
    neoerr = NEO_NOR_SUCCESS;
  return(neoerr);
}

