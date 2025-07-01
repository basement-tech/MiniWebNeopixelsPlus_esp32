/*
 * data to play out on neopixels
 * neopixel strand and connection data is here too
 */
#ifndef __NEO_DATA_H__
#define __NEO_DATA_H__

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define NEO_MAX_SEQ_FILE_SIZE 1024 // maximum size of a sequence file
#define NEO_SEQ_STRATEGIES 5      // number of strategies defined (i.e. size of array of strategy callbacks)
#define MAX_USER_SEQ       5      // maximum number of user buttons/files
#define MAX_SEQUENCES      10     // number of sequences to allocate
#define MAX_NUM_SEQ_POINTS 256    // maximum number of points per sequence
#define MAX_FILENAME       128    // length of filename (without base)
#define MAX_NUM_LABEL      32     // max number of chars in label
#define MAX_NEO_BONUS      128     // max chars  in strategy bonus
#define MAX_NEO_STRATEGY   16     // max chars in a strategy string
#define MAX_NEO_SEQUENCE   32     // max chars in a sequence string
#define NEO_SLOWP_POINTS   1024   // number of points (smoothness) in SLOWP sequence
#define NEO_SLOWP_FLICKERS 100    // max number of slowp random flickers
#define NEO_FLICKER_MAX    255    // value for bright flickers
#define NEO_FLICKER_MIN    0      // value for dim flickers

#define NEO_UPDATE_INTERVAL   2000  // neopixel strand update rate in uS i.e. speed of state machine updates uS
#define NEO_CHK_NEWS_INTERVAL 200/portTICK_PERIOD_MS   // timeout for state machine update semaphore, becomes check for new sequence interval (mS)

/*
 * return error codes for reading a user sequence file
 * and maybe other functions
 */
#define   NEO_LOADED              1
#define   NEO_EMPTY               0
#define   NEO_STALE              -1

/*
 * neopixel engine (i.e. neo_play) error codes
 * 
 * NOTE: need to maintain success codes as positive
 * so that tests can use >= NEO_SUCCESS or < 0 work.
 */
#define   NEO_NEW_SUCCESS         1  // new sequence change successful
#define   NEO_SUCCESS             0  // success
#define   NEO_DESERR             -1  // deserialization error
#define   NEO_NOPLACE            -2  // no sequence placeholder
#define   NEO_SEQ_ERR            -3  // error executing sequence
#define   NEO_STRAT_ERR          -4  // bad strategy specified
#define   NEO_FILE_LOAD_NOTUSER  -5  // attempt to load file in nonuser space
#define   NEO_FILE_LOAD_NOFILE   -6  // requested sequence file doesn't exist
#define   NEO_FILE_LOAD_DESERR   -7  // deserialization of file error
#define   NEO_FILE_LOAD_NOPLACE  -8  // no placeholder for file load
#define   NEO_FILE_LOAD_OTHER    -9

/*
 * data for commnication to the neo_play related process
 */
typedef struct {
    char sequence[MAX_NEO_SEQUENCE];  // identifier of requested sequence
    char file[MAX_FILENAME];  // filename if user sequence
    bool new_data;   // true if new sequence request has been made
    bool resp_reqd;  // is a web client response required
}  neo_mutex_data_t;

/*
 * IPC between webserver and neopixel process
 */
extern SemaphoreHandle_t xneoMutex;  // used to protect communication to neo_play
extern neo_mutex_data_t neo_mutex_data;  // data to be sent to neo_play process from webserver

extern SemaphoreHandle_t xneo_cycle_next_flag;  // neo state machine cycle timer
extern SemaphoreHandle_t xseq_upd_flag;  // new sequence requested

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
//  SEQ_STRAT_RAINBOW,  // attributes of a dynamic rainbow pattern are specified
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
void neo_init(void);
int8_t neo_is_user(const char *label);
int8_t neo_new_sequence(void);
int8_t neo_load_sequence(const char *file);
int8_t neo_set_sequence(const char *label, const char *strategy);
seq_strategy_t neo_set_strategy(const char *sstrategy);
void neo_cycle_stop(void);
void neo_n_blinks(uint8_t r, uint8_t g, uint8_t b, uint8_t w, int8_t reps, int32_t t);
void neo_set_gamma_color(bool gamma_enable);

/*
 * array of neopixel sequences and the index to the currently playing one
 */
extern neo_data_t neo_sequences[MAX_SEQUENCES];  // sequence specifications
extern int8_t seq_index;  // which sequence is being played out
extern int8_t strategy_idx; // which strategy should be used to play a user file

#endif