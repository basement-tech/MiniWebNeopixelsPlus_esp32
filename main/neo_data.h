/*
 * data to play out on neopixels
 * neopixel strand and connection data is here too
 */
#ifndef __NEO_DATA_H__

#include <c_types.h>
#include <Adafruit_NeoPixel.h>

#define NEO_SEQ_STRATEGIES 6
#define MAX_USER_SEQ       5      // maximum number of user buttons/files
#define MAX_SEQUENCES      10     // number of sequences to allocate
#define MAX_NUM_SEQ_POINTS 256    // maximum number of points per sequence
#define MAX_NEO_BONUS      128     // max chars  in strategy bonus
#define MAX_NEO_STRATEGY   16     // max chars in a strategy string
#define NEO_SLOWP_POINTS   1024   // number of points (smoothness) in SLOWP sequence
#define NEO_SLOWP_FLICKERS 100    // max number of slowp random flickers
#define NEO_FLICKER_MAX    255    // value for bright flickers
#define NEO_FLICKER_MIN    0      // value for dim flickers

#define NEO_UPDATE_INTERVAL 2000  // neopixel strand update rate in uS

/*
 * return error codes for reading a user sequence file
 * and maybe other functions
 */
#define   NEO_LOADED              1
#define   NEO_EMPTY               0
#define   NEO_STALE              -1

#define   NEO_SUCCESS             0
#define   NEO_DESERR             -1
#define   NEO_NOPLACE            -2
#define   NEO_SEQ_ERR            -3
#define   NEO_STRAT_ERR          -4
#define   NEO_FILE_LOAD_NOTUSER  -5
#define   NEO_FILE_LOAD_NOFILE   -6
#define   NEO_FILE_LOAD_DESERR   -7
#define   NEO_FILE_LOAD_NOPLACE  -8
#define   NEO_FILE_LOAD_OTHER    -9

/*
 * struct for individual points in the pattern
 */
typedef struct {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t white;  // not always used
  int32_t ms_after_last;  // wait this many mS after last change to play
} neo_seq_point_t;

typedef struct  {
  const char *label;
  char strategy[MAX_NEO_STRATEGY];
  char bonus[MAX_NEO_BONUS];
  neo_seq_point_t point[MAX_NUM_SEQ_POINTS];
} neo_data_t;

/*
 * how should the contents of the sequence file be interpreted
 * an played out
 */
typedef enum {
  SEQ_STRAT_POINTS,   // each point in the sequence is specified
  SEQ_STRAT_SINGLE,   // single shot : play the sequence once and STOP
  SEQ_STRAT_CHASE,    // attributes of a chase sequence are specified
  SEQ_STRAT_PONG,     // attributes of single moving pixel are specified
  SEQ_STRAT_RAINBOW,  // attributes of a dynamic rainbow pattern are specified
  SEQ_STRAT_SLOWP,    // slow pulse - calculated sequence
  SEQ_STRAT_UNDEFINED
}  seq_strategy_t;


/*
 * mapping of the functions called for each state in the playback machine
 * to the strategy being used to play it.
 */
typedef struct {
  seq_strategy_t strategy;
  const char *label;
  void (*start)(bool clear);
  void (*wait)(void);
  void (*write)(void);
  void (*stopping)(void);
  void (*stopped)(void);
} seq_callbacks_t;

/*
 * describes the hardware configuration of the neopixel strip
 */
#define NEO_NUMPIXELS 10
//#define NEO_PIN 15  // moved to application pins file
#define NEO_TYPE NEO_GRB+NEO_KHZ800

/*
 * public functions relating to neopixels
 */
void neo_cycle_next(void);
void neo_init(uint16_t numPixels, int16_t pin, neoPixelType pixelFormat);
int8_t neo_is_user(const char *label);
int8_t neo_load_sequence(const char *file);
int8_t neo_set_sequence(const char *label, const char *strategy);
seq_strategy_t neo_set_strategy(const char *sstrategy);
void neo_cycle_stop(void);
void neo_n_blinks(uint8_t r, uint8_t g, uint8_t b, int8_t reps, int32_t t);
void neo_set_gamma_color(bool gamma_enable);

/*
 * array of neopixel sequences and the index to the currently playing one
 */
extern neo_data_t neo_sequences[MAX_SEQUENCES];  // sequence specifications
extern int8_t seq_index;  // which sequence is being played out
extern int8_t strategy_idx; // which strategy should be used to play a user file

#define __NEO_DATA_H__
#endif