
struct CChord {
  char *name;
  int duration;
  int broken_bar;
  int diminished;
  struct CChord *next;
};

struct CMeasure {
  int duration;
  int notes;
  int leadin;
  int finished;
  int ending;
  char *time_signature;
  struct CChord *chords;
  struct CChord *last_chord;
  struct CMeasure *next;
};

#define MAX_ENDINGS 4

struct CPart {
  char *name;
  int repeat;
  int next_ending;
  struct CMeasure *measures;
  struct CMeasure *last_measure;
  struct CMeasure *endings[MAX_ENDINGS];
  struct CPart *next;
};

#define MAX_LONGEST_INTERVALS 10

struct CSong {
  char *title;
  char *composer;
  char key;
  char key_signature;
  int accidental;
  int minor;
  int mode;
  char *time_signature;
  int meter_change;
  int measure_duration;
  int beat_duration;
  int index;
  char longest_intervals[MAX_LONGEST_INTERVALS];
  struct CPart *parts;
  struct CSong *next;
};

