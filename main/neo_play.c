/*
 * functions to play out the neo_pixel patterns
 */
#include <Arduino.h>
#include <Arduino_DebugUtils.h>
#include <Adafruit_NeoPixel.h>

#include <FS.h>        // File System for Web Server Files
#include <LittleFS.h>  // This file system is used.

#include <ArduinoJson.h>

#include "neo_data.h"
#include "app_pins.h"

// TRACE output simplified, can be deactivated here ... switched to arduino debug library
//#define TRACE(...) Serial.printf(__VA_ARGS__)

// When setting up the NeoPixel library, we tell it how many pixels,
// and which pin to use to send signals. Note that for older NeoPixel
// strips you might need to change the third parameter -- see the
// strandtest example for more information on possible values.
// Argument 1 = Number of pixels in NeoPixel strip
// Argument 2 = Arduino pin number (most are valid)
// Argument 3 = Pixel type flags, add together as needed:
//   NEO_KHZ800  800 KHz bitstream (most NeoPixel products w/WS2812 LEDs)
//   NEO_KHZ400  400 KHz (classic 'v1' (not v2) FLORA pixels, WS2811 drivers)
//   NEO_GRB     Pixels are wired for GRB bitstream (most NeoPixel products)
//   NEO_RGB     Pixels are wired for RGB bitstream (v1 FLORA pixels, not v2)
//   NEO_RGBW    Pixels are wired for RGBW bitstream (NeoPixel RGBW products)
Adafruit_NeoPixel *pixels;


/*
 * housekeeping for the sequence state machine
 */
#define NEO_SEQ_START    0
#define NEO_SEQ_WAIT     1
#define NEO_SEQ_WRITE    2
#define NEO_SEQ_STOPPING 3
#define NEO_SEQ_STOPPED  4
static uint8_t neo_state = NEO_SEQ_START;  // state of the cycling state machine

seq_strategy_t current_strategy = SEQ_STRAT_POINTS;

uint64_t current_millis = 0; // mS of last update
int32_t current_index = 0;   // index into the pattern array

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
 * which/set sequence are we playing out
 * returns: -1 if the label doesn't match a sequence
 * reset the playout index and state if the found index
 * is different than the currently running index.
 */
int8_t seq_index = -1;  // global used to hold the index of the currently running sequence
int8_t neo_set_sequence(const char *label, const char *strategy)  {
  int8_t ret = NEO_SEQ_ERR;
  int8_t new_index = 0;
  seq_strategy_t new_strat;


  /*
   * attempt to set the sequence
   */
  new_index = neo_find_sequence(label);
  if((new_index >= 0) && (new_index != seq_index))  {
    seq_index = new_index;  // set the sequence index that is to be played
    ret = NEO_SUCCESS; // success
  }

  /*
   * if sequence setting was successful, attempt to set the strategy
   *
   * allow for the strategy argument to be a null string so,
   * for example, in the case of a built-in it might remain
   * the initialized value
   */
  if(strategy[0] == '\0')  {
    DEBUG_INFO("neo_set_sequence: using built in strategy %s for seq_index %d\n", neo_sequences[seq_index].strategy,seq_index);
    if(ret == NEO_SUCCESS)  {
      if((new_strat = neo_set_strategy(neo_sequences[seq_index].strategy)) == SEQ_STRAT_UNDEFINED)
        ret = NEO_STRAT_ERR;
    }
  }
  else {
    /*
    * if sequence setting was successful, attempt to set the strategy
    */
    if(ret == NEO_SUCCESS)  {
      if((new_strat = neo_set_strategy(strategy)) == SEQ_STRAT_UNDEFINED)
        ret = NEO_STRAT_ERR;
    }
  }

  /*
  * if all above was successful, set up the globals and start the sequence
  */
  if(ret == NEO_SUCCESS)  {
    current_index = 0;  // reset the pixel count
    neo_state = NEO_SEQ_START;  // cause the state machine to start at the start
    current_strategy = new_strat;
    DEBUG_INFO("neo_set_sequence: set sequence to %d and strategy to %d\n", seq_index, current_strategy);
  }

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
 * look for a label matching the argument, label,
 * and load a sequence from file of the same name.
 * NOTE: currently the requested sequence placeholder of the name
 * requested must exist in neo_sequences[] for this to succeed.
 *
 * return:   0: successfully loaded
 *          -1: file not found or error opening
 *          -2: error deserializing file
 */
int8_t neo_load_sequence(const char *file)  {

  FSInfo fs_info;
  LittleFS.info(fs_info);

  JsonDocument jsonDoc;
  DeserializationError err;

  int8_t ret = 0;
  File fd;  // file pointer to read from
  char buf[1024];  // buffer in which to read the file contents  TODO: paramaterize
  char *pbuf;  // helper
 
  pbuf = buf;

  /*
   * can I see the FS from here ? ... yep.
   */
  DEBUG_INFO("Total bytes in FS = %d\n", fs_info.totalBytes);
  DEBUG_INFO("Total bytes used in FS = %d\n", fs_info.usedBytes);

  /*
   * read the contents of the user sequence file and put it
   * in the character buffer buf
   */
  if (LittleFS.exists(file) == false)  {
      DEBUG_ERROR("ERROR: Filename %s does not exist in file system\n", file);
      ret = NEO_FILE_LOAD_NOFILE;
  }
  else  {

    DEBUG_INFO("Loading filename %s ...\n", file);
    if((fd = LittleFS.open(file, "r")) == false)  
      ret = NEO_FILE_LOAD_NOFILE;

    else  {
      while(fd.available())  {
        *pbuf++ = fd.read();
      }
      *pbuf = '\0';  // terminate the char string
      fd.close();
      DEBUG_VERBOSE("Raw file contents:\n%s\n", buf);

      /*
      * deserialize the json contents of the file which
      * is now in buf  -> JsonDocument jsonDoc
      */
      err = deserializeJson(jsonDoc, buf);
      if(err)  {
        DEBUG_ERROR("ERROR: Deserialization of file %s failed ... no change in sequence\n", file);
        ret = NEO_FILE_LOAD_DESERR;
      }

      /*
      * jsonDoc contains an array of points as JsonObjects
      * convert to a JsonArray points[]
      */
      else  {
        JsonArray points = jsonDoc["points"].as<JsonArray>();
        const char *label, *bonus;
        label = jsonDoc["label"];

        DEBUG_INFO("For sequence \"%s\" : \n", label);

        /*
         * find the place in neo_sequences[] where the file contents should be copied/stored
         */
        int8_t seq_idx = neo_find_sequence(label);

        /*
        * iterate over the points in the array
        * this syntax was introduced in C++11 and is equivalent to:
        * for (size_t i = 0; i < points.size(); i++) {
        *   JsonObject obj = points[i];
        */
        if(seq_idx < 0)  {
          ret = NEO_FILE_LOAD_NOPLACE;
          DEBUG_ERROR("ERROR: neo_load_sequence: no placeholder for %s in sequence array\n", label);
        }

        /*
        * if the label was found, load the points from the json file
        * into the neo_sequences[] array to be played out
        *
        * TODO: super-verbose for now for debugging
        */
        else  {
          /*
           * reserialize the bonus object for later deserialization
           * (may be interpretted differently buy eash strategy)
           */
          serializeJson(jsonDoc["bonus"], neo_sequences[seq_idx].bonus);

          uint16_t i = 0;
          for(JsonObject obj : points)  {
            uint8_t r, g, b, w;
            int32_t t;
            r = obj["r"];
            g = obj["g"];
            b = obj["b"];
            w = obj["w"];
            t = obj["t"];
            DEBUG_INFO("colors = %d %d %d %d  interval = %d\n", r, g, b, w, t);
            neo_sequences[seq_idx].point[i].red = r;
            neo_sequences[seq_idx].point[i].green = g;
            neo_sequences[seq_idx].point[i].blue = b;
            neo_sequences[seq_idx].point[i].white = w;
            neo_sequences[seq_idx].point[i].ms_after_last = t;
            i++;
          }
          ret = neo_set_sequence(label, jsonDoc["strategy"]);
        }
      }
    }
  }
  return(ret);
}

/*
 * convert r, g, b to Adafruit color with/without gamma32()
 * based on eeprom configuration parameter.
 * this is called once in setup() to set a function pointer
 * for efficient operation
 */
uint32_t (*neo_convert_color)(uint8_t r, uint8_t g, uint8_t b);

uint32_t neo_color_gamma(uint8_t r, uint8_t g, uint8_t b)  {
  return pixels->gamma32(pixels->Color(r, g, b));
}
uint32_t neo_color_nogamma(uint8_t r, uint8_t g, uint8_t b)  {
    return (pixels->Color(r, g, b));
}

void neo_set_gamma_color(bool gamma_enable)  {
  if(gamma_enable)
    neo_convert_color = neo_color_gamma;
  else
    neo_convert_color = neo_color_nogamma;
}



/*
 * helper for writing a single color to all pixels
 */
void neo_write_pixel(bool clear)  {
  if(clear != 0)  pixels->clear(); // Set all pixel colors to 'off'

  /*
    * send the next point in the sequence to the strand
    */
  for(int i=0; i < pixels->numPixels(); i++) { // For each pixel...
    pixels->setPixelColor(i, neo_convert_color( neo_sequences[seq_index].point[current_index].red, 
                                                neo_sequences[seq_index].point[current_index].green,
                                                neo_sequences[seq_index].point[current_index].blue));
  }
  pixels->show();   // Send the updated pixel colors to the hardware.
}

/*
 * blink a status color to the strip reps times
 * the color is from the Adafruit colorwheel representation of colors
 * t is the numbe of mS between changes on/off (i.e. blink rate/2)
 *
 * NOTE: this is a blocking function i.e. not suitable for use in the loop()
 * NOTE: the neopixel strand must have been initialized prior to calling this function
 *
 */
void neo_n_blinks(uint8_t r, uint8_t g, uint8_t b, int8_t reps, int32_t t)  {
  uint32_t color = pixels->Color(r, g, b);

  for(int8_t j = reps; j > 0; j--)  {
    /*
    * send the next point in the sequence to the strand
    */
    for(int i=0; i < pixels->numPixels(); i++) // For each pixel...
      pixels->setPixelColor(i, color);
    pixels->show();   // Send the updated pixel colors to the hardware.

    delay(t);

    pixels->clear();
    pixels->show();
    
    delay(t);
  }
}

/*
 * initialize the neopixel strand and set it to off/idle
 */
void neo_init(uint16_t numPixels, int16_t pin, neoPixelType pixelFormat)  {
  pixels = new Adafruit_NeoPixel(numPixels, pin, pixelFormat);

  pixels->begin(); // INITIALIZE NeoPixel strip object (REQUIRED)
  pixels->clear(); // Set all pixel colors to 'off'
  pixels->show();   // Send the updated pixel colors to the hardware.
  neo_state = NEO_SEQ_STOPPED;
}

/*
 * TODO: figure out a better way to do this
 */
void noop(void) {}
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
  pixels->clear(); // Set all pixel colors to 'off'
  pixels->show();   // Send the updated pixel colors to the hardware.
  current_index = 0;
  seq_index = -1; // so it doesn't match

  neo_state = NEO_SEQ_STOPPED;
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
  JsonDocument jsonDoc;
  DeserializationError err;
  const char *jbuf;  // jsonDoc[] requires this type

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
    DEBUG_DEBUG("neo_single_start: bonus = %s\n", neo_sequences[seq_index].bonus);

    err = deserializeJson(jsonDoc, neo_sequences[seq_index].bonus);

    if(err)  {
      DEBUG_ERROR("ERROR: Deserialization of bonus failed ... using zero\n");
      single_repeats = 1;  // default to 1 time through
    }
    else  {
      if(jsonDoc["count"].isNull())  {
        DEBUG_ERROR("WARNING: slowp bonus has no member \"count\" ... using zero\n");
        single_repeats = 1;
      }
      else  {
        jbuf = jsonDoc["count"];
        if((single_repeats = atoi(jbuf)) > INT8_MAX) single_repeats = INT8_MAX;
        DEBUG_DEBUG("neo_single_start: single_repeats set to %d\n", single_repeats);
      }
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
    return (int(c - d));
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

  JsonDocument jsonDoc;
  DeserializationError err;
  const char *jbuf;  // jsonDoc[] requires this type

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
    DEBUG_DEBUG("neo_slowp_start: bonus = %s\n", neo_sequences[seq_index].bonus);

    err = deserializeJson(jsonDoc, neo_sequences[seq_index].bonus);

    if(err)  {
      DEBUG_ERROR("ERROR: Deserialization of bonus failed ... using zero\n");
      flicker_count = 0;
    }
    else  {
      if(jsonDoc["count"].isNull())  {
        DEBUG_ERROR("WARNING: slowp bonus has no member \"count\" ... using zero\n");
        flicker_count = 0;
      }
      else  {
        jbuf = jsonDoc["count"];
        if((flicker_count = atoi(jbuf)) > INT8_MAX) flicker_count = INT8_MAX;
      }

      flicker_r = flicker_g = flicker_b = 255;
      if( (jsonDoc["flicker"]["r"].isNull() == false) &&
          (jsonDoc["flicker"]["g"].isNull() == false) &&
          (jsonDoc["flicker"]["r"].isNull() == false)) {
        flicker_r = neo_check_range(jsonDoc["flicker"]["r"]);
        flicker_g = neo_check_range(jsonDoc["flicker"]["g"]);
        flicker_b = neo_check_range(jsonDoc["flicker"]["b"]);
        DEBUG_INFO("Setting slowp rgb color to (%d %d %d)\n", flicker_r, flicker_g, flicker_b);
      }
      else
        DEBUG_ERROR("WARNING: slowp bonus has incomplete member \"flicker\" ... using white\n");
    }


    if(abs(flicker_count) > NEO_SLOWP_FLICKERS) flicker_count = NEO_SLOWP_FLICKERS;  //boundary check
    flicker_count = abs(flicker_count);  // legacy

  }
  randomSeed(analogRead(0));  // different each time through
  for(uint8_t j = 0; j < flicker_count; j++)  {
    slowp_flickers[j] = random(0, NEO_SLOWP_POINTS);
    if(slowp_flickers[j] == (int16_t)0)
      slowp_flickers[j] = 1;  // stay away from the turn-arounds
    else if(slowp_flickers[j] == (int16_t)(NEO_SLOWP_POINTS-1))
      slowp_flickers[j] = (NEO_SLOWP_POINTS-2);
  }

  DEBUG_INFO("Starting slowp: dr = %f, dg = %f, db = %f dt = %d\n", delta_r, delta_g, delta_b, delta_time);
  DEBUG_VERBOSE("Randoms are (unsorted):");
  for(uint8_t j = 0; j < flicker_count; j++)
    DEBUG_VERBOSE("%d  ", slowp_flickers[j]);
  DEBUG_VERBOSE("\n");

  /*
   * Sort the array in place
   * TODO: strange that it seems to only work with ints despite
   * all of the syntax to the contrary? ... solved: compare function must
   * return an int no matter which type is being sorted: implemented, works.
   */
  qsort(slowp_flickers, flicker_count, sizeof(int16_t), compare_int16_t);

  DEBUG_INFO("Randoms are (sorted):");
  for(uint8_t j = 0; j < flicker_count; j++)
    DEBUG_INFO("%d  ", slowp_flickers[j]);
  DEBUG_INFO("\n");

  uint8_t r = neo_check_range(slowp_r);
  uint8_t g = neo_check_range(slowp_g);
  uint8_t b = neo_check_range(slowp_b);

  /*
   * clear and write the starting value
   */
  pixels->clear();
  for(int i=0; i < pixels->numPixels(); i++)  // For each pixel...
      pixels->setPixelColor(i, neo_convert_color(r, g, b));
  pixels->show();   // Send the updated pixel colors to the hardware.

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
  for(int i=0; i < pixels->numPixels(); i++)  // For each pixel...
      pixels->setPixelColor(i, neo_convert_color(r, g, b));

  pixels->show();   // Send the updated pixel colors to the hardware.

#ifdef DEBUG_HACK
  DEBUG_VERBOSE("neo_slowp_write: Showed %d  %d  %d\n", slowp_r, slowp_g, slowp_b);
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

  JsonDocument jsonDoc;
  DeserializationError err;
  const char *jbuf;  // jsonDoc[] requires this type

  pong_repeats = -1; // start with continuous

  /*
   * obtain the number of times the sequence will be run
   * based on the "bonus" parameter from the json sequence file
   */
  if(strlen(neo_sequences[seq_index].bonus) > 0)  {
    DEBUG_DEBUG("neo_pong_start: bonus = %s\n", neo_sequences[seq_index].bonus);

    err = deserializeJson(jsonDoc, neo_sequences[seq_index].bonus);

    if(err)  {
      DEBUG_ERROR("ERROR: Deserialization of bonus failed ... using zero\n");
    }
    else  {
      if(jsonDoc["count"].isNull())  {
        DEBUG_ERROR("WARNING: pong bonus has no member \"count\" ... using inf.\n");
      }
      else  {
        jbuf = jsonDoc["count"];
        if((pong_repeats = atoi(jbuf)) > INT16_MAX) pong_repeats = INT16_MAX;
        DEBUG_DEBUG("neo_pong_start: pong_repeats set to %d\n", pong_repeats);
      }
    }
  }

  p_num_pixels = pixels->numPixels();

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
  pixels->clear();
  pixels->setPixelColor(slowp_idx, neo_convert_color( neo_check_range(slowp_r),
                                                      neo_check_range(slowp_g),
                                                      neo_check_range(slowp_b)));  // turn on the next one
  pixels->show();

  current_millis = millis();

  DEBUG_INFO("Starting pong: dr = %f, dg = %f, db = %f dt = %d\n", delta_r, delta_g, delta_b, delta_time);

  neo_state = NEO_SEQ_WAIT;
}

void neo_pong_write(void) {
  uint8_t r, g, b;
  
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
  pixels->clear();  // first turn them all off
  pixels->setPixelColor(slowp_idx, neo_convert_color( neo_check_range(slowp_r),
                                                      neo_check_range(slowp_g),
                                                      neo_check_range(slowp_b)));  // turn on the next one
  pixels->show();   // Send the updated pixel colors to the hardware.

  if(pong_repeats == (int16_t)(-1))  // not counting keep going
    neo_state = NEO_SEQ_WAIT;
  else if (pong_repeats > 0)         // counting and still have some repeats to go
    neo_state = NEO_SEQ_WAIT;
  else                               // counting and done
    neo_state = NEO_SEQ_STOPPING;
}

// end of SEQ_STRAT_PONG

/*
 * SEQ_STRAT_RAINBOW
 * cycle a rainbow color pallette along the whole strip
 * (adapted from the Adafruit strandtest example)
 */
long firstPixelHue = 0;
void neo_rainbow_start(bool clear)  {
  pixels->clear();
  pixels->show();

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
  pixels->rainbow(firstPixelHue);
  pixels->show();

  firstPixelHue += 256;

  if(firstPixelHue >= 5*65536)
    firstPixelHue = 0;

  neo_state = NEO_SEQ_WAIT;

}

void neo_rainbow_stopping(void)  {
  pixels->clear(); // Set all pixel colors to 'off'
  pixels->show();   // Send the updated pixel colors to the hardware.

  seq_index = -1; // so it doesn't match

  neo_state = NEO_SEQ_STOPPED;
}

// end of SEQ_STRAT_RAINBOW callbacks

/*
 * function calls by strategy for each state in the playback machine
 * TODO: delete the 'x' before the labels after implementing a strategy
 */
seq_callbacks_t seq_callbacks[NEO_SEQ_STRATEGIES] = {
//  strategy              label                start                wait              write                stopping             stopped
  { SEQ_STRAT_POINTS,    "points",         neo_points_start,  neo_points_wait,   neo_points_write,    neo_points_stopping,      noop},
  { SEQ_STRAT_SINGLE,    "single",         neo_single_start,  neo_points_wait,   neo_single_write,    neo_points_stopping,      noop},
  { SEQ_STRAT_CHASE,     "xchase",          start_noop,           noop,               noop,                 noop,               noop},
  { SEQ_STRAT_PONG,      "pong",           neo_pong_start,    neo_slowp_wait,    neo_pong_write,      neo_points_stopping,      noop},
  { SEQ_STRAT_RAINBOW,   "rainbow",       neo_rainbow_start, neo_rainbow_wait,  neo_rainbow_write,    neo_rainbow_stopping,     noop},
  { SEQ_STRAT_SLOWP,     "slowp",          neo_slowp_start,   neo_slowp_wait,    neo_slowp_write,     neo_points_stopping,      noop},
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
  uint64_t new_millis = 0;

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
      break;
  }
}

/*
 * stop the sequence i.e. turn off neopixel strand
 */
void neo_cycle_stop(void)  {
  neo_state = NEO_SEQ_STOPPING;
  seq_index = -1;  // so it doesn't match
}