
struct CChord {
  char *name;
  int duration;
  struct CChord *next;
};

struct CMeasure {
  int duration;
  int leadin;
  struct CChord *chords;
  struct CChord *last_chord;
  struct CMeasure *next;
};

struct CMeter {
  char *time_signature;
  struct CMeasure *measures;
  struct CMeasure *last_measure;
  struct CMeter *next;
};

struct CPart {
  char name;
  int repeat;
  struct CMeter *meters;
  struct CMeter *endings;
  struct CPart *next;
};

struct CSong {
  char *title;
  char key;
  int accidental;
  int minor;
  int mode;
  char *time_signature;
  int time_signature_count;
  char *tempo;
  int index;
  struct CPart *parts;
  struct CSong *next;
};

