#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "abcm2ps.h"
#include "chords.h"

char bar_buf[8];
int duration = 0;
int leadin_duration = 0;
char next_part = 'a';
int unit_note_length = -1;
int meter_note_length = -1;
int new_part = 0;
int previous_bar_type = 0;
int ending = 0;
int generate_chords_output = 0;
FILE *chord_out = NULL;
int l_divisor = 0;
int meter_num = 0;
int meter_denom = 0;
int measure_duration = BASE_LEN;
int skipping_voice = 0;
int song_finished = 0;
char primary_voice[64];
char *next_time_signature = NULL;

struct CSong *first_song = NULL;
struct CSong *cur_song = NULL;
struct CPart *cur_part = NULL;
struct CMeasure *cur_measure = NULL;
struct CChord *cur_chord = NULL;
struct CChord *previous_chord = NULL;
struct CChord *previous_ending_chord = NULL;

#define LOG_LEVEL 2

void log_message(int level, const char* fmt, ... ) {
  if (level <= LOG_LEVEL) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args );
    va_end(args);
  }
}

int empty_part(struct CPart *part) {
  if (part == NULL)
    return 0;
  for (struct CMeasure *measure = part->measures; measure; measure = measure->next) {
    if (measure->chords)
      return 0;
  }
  return 1;
}

void *allocate_bytes(int len) {
  //return getarena(len);
  return malloc(len);  // asan
}

void allocate_song() {
  struct CSong *song = allocate_bytes(sizeof(struct CSong));
  memset(song, 0, sizeof(*song));
  if (cur_song == NULL)
    first_song = song;
  else
    cur_song->next = song;
  cur_song = song;
  cur_part = NULL;
  cur_measure = NULL;
  cur_chord = NULL;
  previous_chord = NULL;
  next_part = 'a';
  unit_note_length = -1;
  meter_note_length = -1;
  l_divisor = 0;
  meter_num = 0;
  meter_denom = 0;
  measure_duration = BASE_LEN;
  duration = 0;
  leadin_duration = 0;
  new_part = 0;
  previous_bar_type = 0;
  ending = 0;
  skipping_voice = 0;
  primary_voice[0] = '\0';
  song_finished = 0;
  next_time_signature = NULL;
}

void allocate_part() {
  if (cur_song == NULL)
    allocate_song();
  struct CPart *part = allocate_bytes(sizeof(struct CPart));
  memset(part, 0, sizeof(*part));
  part->name = next_part++;
  if (cur_part == NULL)
    cur_song->parts = part;
  else {
    cur_part->next = part;
    if (cur_part->next_ending != 0)
      cur_part->repeat = 1;
  }
  cur_part = part;
  cur_measure = NULL;
  previous_chord = NULL;
  cur_chord = NULL;
  duration = 0;
  leadin_duration = 0;
  ending = 0;
}

void allocate_measure() {
  if (cur_part == NULL) {
    log_message(2, "(a) Allocating part %c\n", next_part);
    allocate_part();
  }
  if (cur_measure != NULL && cur_measure->chords == NULL)
    return;
  struct CMeasure *measure = allocate_bytes(sizeof(struct CMeasure));
  memset(measure, 0, sizeof(*measure));
  if (cur_measure == NULL) {
    cur_part->measures = measure;
  }
  else {
    cur_measure->next = measure;
  }
  cur_measure = measure;
  cur_part->last_measure = measure;
  if (next_time_signature != NULL) {
    cur_measure->time_signature = next_time_signature;
    next_time_signature = NULL;
  }
}

void allocate_ending() {
  if (cur_part == NULL) {
    log_message(2, "(b) Allocating part %c\n", next_part);
    allocate_part();
  }
  struct CMeasure *measure = allocate_bytes(sizeof(struct CMeasure));
  memset(measure, 0, sizeof(struct CMeasure));
  assert(cur_part->next_ending < MAX_ENDINGS);
  cur_part->endings[cur_part->next_ending++] = measure;
  cur_measure = measure;
  log_message(2, "(!) allocated measure %p\n", (void *)cur_measure);
  ending++;
  log_message(2, "Allocated ending %d\n", ending);
  previous_chord = NULL;
  cur_chord = NULL;
}

void allocate_chord() {
  struct CChord *chord = allocate_bytes(sizeof(struct CChord));
  memset(chord, 0, sizeof(*chord));
  cur_chord = chord;
}

int equal_chords(struct CChord *left, struct CChord *right) {
  return strcmp(left->name, right->name) == 0;
}

int equal_measures(struct CMeasure *left, struct CMeasure *right) {
  if (left->time_signature == NULL || right->time_signature == NULL) {
    if (left->time_signature != right->time_signature)
      return 0;
  } else if (strcmp(left->time_signature, right->time_signature) != 0) {
    return 0;
  }
  struct CChord *left_chord = left->chords;
  struct CChord *right_chord = right->chords;
  while (left_chord != NULL && right_chord != NULL) {
    if (equal_chords(left_chord, right_chord) == 0)
      return 0;
    left_chord = left_chord->next;
    right_chord = right_chord->next;
  }
  return left_chord == right_chord;
}

int equal_measure_sequence(struct CMeasure *left, struct CMeasure *right) {
  while (left != NULL && right != NULL) {
    if (equal_measures(left, right) == 0)
      return 0;
    left = left->next;
    right = right->next;
  }
  return left == right;
}

int equal_parts(struct CPart *left, struct CPart *right) {
  if (left->name != right->name || left->repeat != right->repeat)
    return 0;
  struct CMeasure *left_measure = left->measures;
  struct CMeasure *right_measure = right->measures;
  while (left_measure != NULL && right_measure != NULL) {
    if (equal_measures(left_measure, right_measure) == 0)
      return 0;
    left_measure = left_measure->next;
    right_measure = right_measure->next;
  }
  if (left_measure != right_measure)
    return 0;
  // Compare endings
  for (int i=0; i<MAX_ENDINGS; i++) {
    left_measure = left->endings[i];
    right_measure = right->endings[i];
    if ((left_measure == NULL || right_measure == NULL) &&
	left_measure != right_measure)
      return 0;
    if (equal_measure_sequence(left->endings[i], right->endings[i]) == 0)
      return 0;
  }
  return 1;
}

int equal_songs(struct CSong *left, struct CSong *right) {
  if (strcmp(left->title, right->title) != 0 ||
      left->key != right->key ||
      left->accidental != right->accidental ||
      left->minor != right->minor ||
      left->mode != right->mode ||
      strcmp(left->time_signature, right->time_signature) != 0)
    return 0;
  struct CPart *left_part = left->parts;
  struct CPart *right_part = right->parts;
  while (left_part != NULL && right_part != NULL) {
    if (equal_parts(left_part, right_part) == 0)
      return 0;
    left_part = left_part->next;
    right_part = right_part->next;
  }
  return left_part == right_part;
}

void add_chord_to_measure() {
  if (cur_chord == NULL) {
    return;
  }
  if (cur_measure == NULL) {
    allocate_measure();
    log_message(2, "(a) Allocated measure %p\n", cur_measure);
  }
  if (cur_measure->chords == NULL) {
    cur_measure->chords = cur_measure->last_chord = cur_chord;
  }
  else {
    cur_measure->last_chord->next = cur_chord;
    cur_measure->last_chord = cur_chord;
  }
  cur_measure->duration += cur_chord->duration;
  log_message(2, "Added %s to measure %p (duration=%d)\n", cur_chord->name, (void *)cur_measure, cur_chord->duration);
  previous_chord = cur_chord;
  cur_chord = NULL;
}

int song_empty(struct CSong *song) {
  for (struct CPart *part = song->parts; part != NULL; part = part->next) {
    for (struct CMeasure *measure = part->measures; measure != NULL; measure = measure->next) {
      for (struct CChord *chord = measure->chords; chord != NULL; chord = chord->next) {
	return 0;
      }
    }    
  }
  return 1;
}

int measure_empty(struct CMeasure *measure) {
  if (measure == NULL)
    return 0;
  for (struct CChord *chord = measure->chords; chord != NULL; chord = chord->next) {
    return 0;
  }
  return 1;
}

void strip_empty_measures(struct CMeasure **measurep) {
  while (*measurep != NULL) {
    if (measure_empty(*measurep)) {
      *measurep = (*measurep)->next;
    } else {
      measurep = &(*measurep)->next;
    }
  }
}

void squash_identical_repeats(struct CPart *part) {
  if (part->next_ending == 0)
    return;
  part->repeat = 1;

  strip_empty_measures(&part->measures);
  // reset last measure
  part->last_measure = part->measures;
  while (part->last_measure->next != NULL)
    part->last_measure = part->last_measure->next;

  for (int i=0; i<part->next_ending; i++) {
    strip_empty_measures(&part->endings[i]);
  }

  for (int i=1; i<part->next_ending; i++) {
    if (part->endings[i] == NULL)  // should be corrected above
      continue;
    if (equal_measure_sequence(part->endings[0], part->endings[i]) == 0) {
      return;
    }
  }
  part->last_measure->next = part->endings[0];
  memset(part->endings, 0, MAX_ENDINGS*sizeof(struct CMeasure *));
  part->next_ending = 0;
}

const char *bar_type(int type) {
  switch (type) {
  case (B_OBRA):
    return "[";
  case (B_CBRA):
    return "]";
  case (B_SINGLE):
    return "|";
  case (B_DOUBLE):
    return "||";
  case (B_THIN_THICK):
    return "|]";
  case (B_THICK_THIN):
    return "[|";
  case (B_LREP):
    return "|:";
  case (B_RREP):
    return ":|";
  case (B_DREP):
    return "::";
  case (B_DASH):
    return ":";
  default:
    break;
  }
  sprintf(bar_buf, ".%d", type);
  return bar_buf;
}

char key_buf[8];
char *circle_of_fifths_sharps = "FCGDAEB";
char *circle_of_fifths_flats = "BEADGCF";
const char *convert_sf_to_key(int sf) {
  if (sf >= 0) {
    if (sf < 6) {
      sprintf(key_buf, "%c", circle_of_fifths_sharps[sf+1]);
      return key_buf;
    } else if (sf == 7) {
      return "F#";
    } else if (sf == 8) {
      return "C#";
    }
  } else {
    sf *= -1;
    if (sf == 1)
      return "F";
    else if (sf <= 8) {
      sprintf(key_buf, "%cb", circle_of_fifths_flats[sf-2]);
      return key_buf;
    }
  }
  return "?";
}

void set_key_and_accidentals(const char *text, struct CSong *song) {
  song->key = text[0];
  if (text[1] == 'b')
    song->accidental = -1;
  else if (text[1] == '#')
    song->accidental = 1;
}

void set_key(struct SYMBOL *sym) {
  char *beginp = &sym->text[2];
  // skip leading whitespace
  while (isspace(*beginp))
    beginp++;
  char *endp = beginp;
  while (*endp && !isspace(*endp)) {
    endp++;
  }
  // lowercase the rest
  for (char *textp = endp; *textp; textp++) {
    if (isalpha(*textp) && !islower(*textp))
      *textp = tolower(*textp);
  }
  // Check for minor
  if (*(endp-1) == 'm' || strstr(endp, "minor")) {
    cur_song->minor = 1;
    set_key_and_accidentals(beginp, cur_song);
  } else if (strstr(endp, "mix")) {
    cur_song->mode = 5;
    set_key_and_accidentals(beginp, cur_song);
  } else if (strstr(endp, "dor")) {
    cur_song->mode = 2;
    set_key_and_accidentals(beginp, cur_song);
  }
  else {
    const char *key = convert_sf_to_key(sym->u.key.sf);
    set_key_and_accidentals(key, cur_song);
  }
}

char abc_type_buf[32];
const char *abc_type(struct SYMBOL *sym) {
  switch (sym->abc_type) {
  case(ABC_T_INFO):
    return "INFO";
  case(ABC_T_PSCOM):
    return "PSCOM";
  case(ABC_T_CLEF):
    return "CLEF";
  case(ABC_T_NOTE):
    return "NOTE";
  case(ABC_T_REST):
    return "REST";
  case(ABC_T_BAR):
    return "BAR";
  case(ABC_T_EOLN):
    return "EOLN";
  case(ABC_T_MREST):
    return "MREST";
  case(ABC_T_MREP):
    return "MREP";
  case(ABC_T_V_OVER):
    return "V_OVER";
  case(ABC_T_TUPLET):
    return "TUPLET";
  default:
    break;
  }
  sprintf(abc_type_buf, "%d", sym->abc_type);
  return abc_type_buf;
}

const char *diminished = "&#x05AF;";
char g_tmp_chord_buf[256];
const char *clean_chord(const char *text) {
  char *bufp = g_tmp_chord_buf;
  memset(g_tmp_chord_buf, 0, 256);

  // Strip whitespace and convert sharp/flat codes
  for (const char *textp = text; *textp && *textp != '\n'; textp++) {
    if (!isspace(*textp)) {
      if (*textp == 0x01)
	*bufp++ = '#';
      else if (*textp == 0x02)
	*bufp++ = 'b';
      else
	*bufp++ = *textp;
    }
  }
  *bufp = '\0';

  // strip other punct
  bufp = g_tmp_chord_buf;
  for (const char *textp = g_tmp_chord_buf; *textp; textp++) {
    if (!ispunct(*textp) ||
	*textp == '#' || *textp == '-' || *textp == '(' || *textp == ')' ||
	*textp == '/' || *textp == '>' || *textp == '.')
      *bufp++ = *textp;
  }
  *bufp = '\0';

  // strip "or"
  bufp = strstr(g_tmp_chord_buf, "(or");
  if (bufp) {
    bufp++;
    char *textp = bufp + 2;
    bcopy(textp, bufp, strlen(textp));
    bufp[strlen(textp)] = '\0';
  }

  // strip long parenthetical
  bufp = strchr(g_tmp_chord_buf, '(');
  if (bufp) {
    char *textp = strchr(bufp, ')');
    if (textp == NULL) {
      *bufp = '\0';
    } else if ((textp - bufp) > 10 ||
	       ((textp - bufp) > 5 && strlen(g_tmp_chord_buf) != (textp - bufp) + 1)) {
      char tmp_buf[256];
      strncpy(tmp_buf, bufp+1, (textp-bufp)-1);
      tmp_buf[(textp-bufp)-1] = '\0';
      bcopy(textp+1, bufp, strlen(textp+1));
      bufp[strlen(textp+1)] = '\0';
    }
  }

  log_message(2, "Name = '%s'\n", g_tmp_chord_buf);

  // Make sure it looks like a chord
  int index = *g_tmp_chord_buf != '(' ? 0 : 1;
  if (g_tmp_chord_buf[index] < 'A' || g_tmp_chord_buf[index] > 'G')
    g_tmp_chord_buf[0] = '\0';
  else if (g_tmp_chord_buf[index+1] != '\0' && g_tmp_chord_buf[index+1] != '#' &&
	   g_tmp_chord_buf[index+1] != ')' && g_tmp_chord_buf[index+1] != '(' &&
	   g_tmp_chord_buf[index+1] != '/' &&
	   !isalnum(g_tmp_chord_buf[index+1]))
    g_tmp_chord_buf[0] = '\0';
  else if (g_tmp_chord_buf[index+1] != '/') {
    // Lowercase everything after chord name
    char *p = &g_tmp_chord_buf[index+1];
    while (*p) {
      if (isupper(*p))
	*p = tolower(*p);
      p++;
    }
    p = &g_tmp_chord_buf[index+1];
    if (strstr(p, "maj") == NULL && strstr(p, "min") == NULL &&
	strstr(p, "dim") == NULL && strstr(p, "aug") == NULL) {
      for (p = &g_tmp_chord_buf[index+1]; *p; p++) {
	if (*p != 'm' && *p != '#' && *p != '(' && *p != ')' && !isdigit(*p) && (*p < 'a' || *p > 'g')) {
	  g_tmp_chord_buf[0] = '\0';
	  break;
	}
      }
    }
  }

  // Replaced "dim" with symbol
  bufp = strstr(g_tmp_chord_buf, "dim");
  if (bufp) {
    char temp_buf[256];
    strcpy(temp_buf, g_tmp_chord_buf);
    temp_buf[bufp-g_tmp_chord_buf] = '\0';
    strcat(temp_buf, diminished);
    bufp += 3;
    strcat(temp_buf, bufp);
    strcpy(g_tmp_chord_buf, temp_buf);
  }
  return g_tmp_chord_buf;
}

char g_chord_buf[256];
const char *get_chord_name(struct SYMBOL *sym, int *repeatp) {
  char tmp_buf[256];
  char *tptr = tmp_buf;
  char *parts[MAXGCH];
  int part_index = 0;
  for (struct gch *gch = sym->gch; gch->type; gch++) {
    int idx = gch->idx;
    if (gch->type == 'r')
      *repeatp = 1;
    if (sym->text[idx] == '?' || sym->text[idx] == '@' ||
	sym->text[idx] == '<' || sym->text[idx] == '>' ||
	sym->text[idx] == '^' || sym->text[idx] == '_' ||
	sym->text[idx] == '$' || isspace(sym->text[idx]))
      continue;
    const char *clean = clean_chord(&sym->text[gch->idx]);
    int len = strlen(clean);
    if (len) {
      strcpy(tptr, clean);
      parts[part_index++] = tptr;
      tptr += strlen(clean) + 1;
    }
  }
  if (part_index > 0) {
    g_chord_buf[0] = '\0';
    for (; part_index > 0; part_index--) {
      strcat(g_chord_buf, parts[part_index-1]);
    }
    return g_chord_buf;
  }
  return NULL;
}

void process_symbol(struct SYMBOL *sym) {

  if (sym == NULL)
    return;

  log_message(2, "sym->abc_type = %s, type=%d, sflags=0x%x\n", abc_type(sym), sym->type, sym->sflags);

  if (sym->abc_type == ABC_T_INFO) {
    if (sym->text != NULL) {
      if (sym->text[0] == 'X') {
	allocate_song();
	cur_song->index = (int)strtol(&sym->text[2], NULL, 0);
      } else if (sym->text[0] == 'T') {
	if (cur_song->title == NULL) {
	  int offset = 2;
	  if (!strncasecmp(&sym->text[2], "The ", 4))
	    offset = 6;
	  cur_song->title = allocate_bytes(strlen(sym->text));
	  strcpy(cur_song->title, &sym->text[offset]);
	}
      } else if (sym->text[0] == 'K') {
	if (cur_song->key == '\0')
	  set_key(sym);
      } else if (sym->text[0] == 'L') {
	int l1, l2;
	const char *p = &sym->text[2];
	if (sscanf(p, "%d/%d ", &l1, &l2) == 2) {
	  if (l2 == 16) {
	    l_divisor = 2;
	  } else if (l2 == 32) {
	    l_divisor = 4;
	  } else {
	    l_divisor = 1;
	  }
	  if (meter_num) {
	    measure_duration = ((BASE_LEN * meter_num) / meter_denom) / l_divisor;
	  } else {
	    measure_duration = BASE_LEN / l_divisor;
	  }
	}
      } else if (sym->text[0] == 'M') {
	if (cur_measure == NULL)
	  allocate_measure();
	char *time_signature = &sym->text[2];
	if (!strcmp(time_signature, "C")) {
	  time_signature = "4/4";
	} else if (!strcmp(time_signature, "C|")) {
	  time_signature = "2/2";
	}
	next_time_signature = allocate_bytes(strlen(time_signature)+1);
	strcpy(next_time_signature, time_signature);
	//log_message(2, "(ending=%d) Setting measure %p time signature to %p\n", ending, cur_measure, next_time_signature);
	if (cur_song->time_signature == NULL) {
	  cur_song->time_signature = next_time_signature;
	} else if (cur_song->meter_change == 0 &&
		   strcmp(next_time_signature, cur_song->time_signature) != 0) {
	  cur_song->meter_change = 1;
	}
	// If current measure is still being populated, add time signature
	if (cur_measure != NULL && cur_measure->finished != 1) {
	  cur_measure->time_signature = next_time_signature;
	  next_time_signature = NULL;
	}
	int l1, l2;
	if (sscanf(time_signature, "%d/%d ", &l1, &l2) == 2 &&
	    l1 != 0 && l2 != 0) {
	  meter_num = l1;
	  meter_denom = l2;
	  measure_duration = (BASE_LEN * meter_num) / meter_denom;
	  if (l_divisor > 1)
	    measure_duration /= l_divisor;
	}
      } else if (sym->text[0] == 'P' && strcmp(sym->text, "P:W") && !empty_part(cur_part) && !ending) {
	log_message(2, "(s) Allocating part %c\n", next_part);
	allocate_part();
      } else if (sym->text[0] == 'V') {
	log_message(2, "Voice = '%s'\n", &sym->text[2]);
	if (primary_voice[0] != '\0') {
	  if (!strcmp(&sym->text[2], primary_voice)) {
	    skipping_voice = 0;
	  } else {
	    skipping_voice = 1;
	  }
	} else {
	  char *vptr = &sym->text[2];
	  while (*vptr && !isspace(*vptr))
	    vptr++;
	  int len = (vptr - sym->text) - 2;
	  strncpy(primary_voice, &sym->text[2], (vptr - sym->text) - 2);
	  primary_voice[len] = '\0';
	  log_message(2, "Primary Voice = '%s'\n", primary_voice);
	}
      }
    }
  }

  if (song_finished || skipping_voice)
    return;

  if (sym->text)
    log_message(2, "sym->text = '%s'\n", sym->text);

  if (sym->gch) {
    int repeat = 0;
    const char *name = get_chord_name(sym, &repeat);
    if (name || repeat) {
      if (name)
	log_message(2, "name = '%s'\n", name);
      log_message(2, "gch = '%s' (duration=%d, ending=%d)\n", sym->text, duration, ending);
      if (duration != 0) {
	if (cur_chord) {
	  cur_chord->duration = duration;
	} else if (previous_chord) {
	  allocate_chord();
	  cur_chord->name = previous_chord->name;
	  cur_chord->duration = duration;
	} else if (ending && previous_ending_chord) {
	  log_message(2, "Adding previous_ending_chord\n");
	  allocate_chord();
	  cur_chord->name = previous_ending_chord->name;
	  cur_chord->duration = duration;
	}
	if (cur_chord) {
	  log_message(2, "(a) Adding chord %s to measure %p\n",
		      cur_chord->name, cur_measure);
	}
	add_chord_to_measure();
	new_part = 0;
	duration = 0;
      } else if (!ending && previous_bar_type != B_RREP) {
	if (new_part && !empty_part(cur_part)) {
	  int save_duration = duration;
	  struct CChord *save_chord = cur_chord;
	  log_message(2, "(c) Allocating part %c (bar = %s)\n", next_part, bar_type(sym->u.bar.type));
	  allocate_part();
	  duration = save_duration;
	  cur_chord = save_chord;
	}
      }
      if (repeat) {
	log_message(2, "prev_chord=%s, cur_chord=%s\n",
		    previous_chord ? previous_chord->name : "",
		    cur_chord ? cur_chord->name : "");
	if (ending == 0 && previous_chord) {
	  log_message(2, "Setting previous_ending_chord to %s\n", previous_chord->name);
	  previous_ending_chord = previous_chord;
	  previous_chord = NULL;
	}
	log_message(2, "(a) Allocate ending\n");
	allocate_ending();
      } else {
	allocate_chord();
	previous_chord = NULL;
	cur_chord->name = allocate_bytes(strlen(name)+1);
	strcpy(cur_chord->name, name);
	log_message(2, "new chord %s %s\n", cur_chord->name, repeat ? "repeat" : "");
      }
    }
  }

  if (sym->abc_type == ABC_T_NOTE || sym->abc_type == ABC_T_REST) {
    // skip middle note of tuplet
    if (sym->sflags != (S_SEQST | S_IN_TUPLET) &&
	(sym->sflags & S_SEQST) != 0) {
      duration += sym->u.note.notes[0].len;
    }
  } else if (sym->abc_type == ABC_T_BAR) {
    log_message(2, "bar %s cur=%s prev=%s dur=%d measure_duration=%d\n",
		bar_type(sym->u.bar.type),
		cur_chord ? cur_chord->name : "",
		previous_chord ? previous_chord->name : "", duration,
		measure_duration);
    if (cur_chord == NULL && previous_chord && duration >= measure_duration) {
      cur_chord = previous_chord;
    }
    int skip_chord = !ending && cur_part && cur_part->repeat && sym->u.bar.type == B_THIN_THICK;
    if (new_part && !empty_part(cur_part) && !ending && !skip_chord) {
      int save_duration = duration;
      struct CChord *save_chord = cur_chord;
      log_message(2, "(c) Allocating part %c (bar = %s)\n", next_part, bar_type(sym->u.bar.type));
      allocate_part();
      duration = save_duration;
      cur_chord = save_chord;
    }
    new_part = 0;
    if (duration != 0) {
      if (ending && cur_measure == NULL) {
	int save_duration = duration;
	struct CChord *save_chord = cur_chord;
	log_message(2, "(b) Allocate ending\n");
	allocate_ending();
	duration = save_duration;
	cur_chord = save_chord;
      }
      if (cur_chord) {
	cur_chord->duration = duration;
	if (sym->u.bar.type == B_RREP) {
	  log_message(2, "Adding leadin_duration %d\n", leadin_duration);
	  cur_chord->duration += leadin_duration;
	  duration = leadin_duration = 0;
	}
	// Leadin durtaion
	int tmp_duration = cur_measure ? cur_measure->duration + cur_chord->duration : cur_chord->duration;
	if (tmp_duration < measure_duration &&
	    (previous_bar_type == 0 ||
	     (sym->u.bar.type == B_SINGLE && sym->u.bar.dotted == 0 && !ending && cur_measure != NULL))) {
	  leadin_duration = tmp_duration;
	} else if (!skip_chord) {
	  if (sym->u.bar.dotted)
	    cur_chord->broken_bar = 1;
	  log_message(2, "(b) Adding chord %s to measure %p\n",
		      cur_chord->name, cur_measure);
	  add_chord_to_measure();
	}
      } else if (previous_chord) {
	allocate_chord();
	cur_chord->name = previous_chord->name;
	cur_chord->duration = duration;
	previous_chord = NULL;
	log_message(2, "(d) Adding chord %s to measure %p\n",
		    cur_chord->name, cur_measure);
	add_chord_to_measure();
      } else if (duration < measure_duration) {
	log_message(2, "(b) setting leadin_duration to %d\n", duration);
	leadin_duration = duration;
      } else if (previous_ending_chord) {
	allocate_chord();
	cur_chord->name = previous_ending_chord->name;
	cur_chord->duration = duration;
	log_message(2, "(p) Adding chord %s to measure %p\n",
		    cur_chord->name, cur_measure);
	add_chord_to_measure();
      }
      duration = 0;
      if (cur_measure != NULL)
	cur_measure->finished = 1;
    }
    if (sym->u.bar.type != B_DOUBLE &&
	sym->u.bar.type != B_THIN_THICK &&
	sym->u.bar.type != B_RREP &&
	sym->u.bar.dotted == '\0' &&
	!measure_empty(cur_measure)) {
      allocate_measure();
      log_message(2, "(b) Allocated measure %p (bar = %s)\n", cur_measure, bar_type(sym->u.bar.type));
    }
    if (sym->u.bar.type == B_LREP) {
      log_message(2, "ending = 0\n");
      ending = 0;
      previous_ending_chord = NULL;
      if (previous_bar_type == B_SINGLE) {
	log_message(2, "(a) new_part = 1\n");
	new_part = 1;
      } else if (previous_bar_type == B_RREP || previous_bar_type == B_DOUBLE || previous_bar_type == 0) {
	cur_part->repeat = 1;
      }
    }
    if (sym->u.bar.type == B_DOUBLE) {
      log_message(2, "(h) ending=%d, repeat=%d\n", ending, cur_part->repeat);
      if (!ending && cur_part->repeat) {
	allocate_measure();
	log_message(2, "(h) Allocated measure %p (bar = %s)\n", cur_measure, bar_type(sym->u.bar.type));
      } else {
	log_message(2, "(b) new_part = 1\n");
	new_part = 1;
	ending = 0;
	previous_ending_chord = NULL;
      }
    }
    if (sym->u.bar.type == B_RREP) {
      cur_part->repeat = 1;
      if (ending == 2 && !measure_empty(cur_measure)) {
	ending = 0;
	previous_ending_chord = NULL;
      } else if (ending) {
	log_message(2, "(c) new_part = 1\n");
	new_part = 1;
	previous_chord = NULL;
      } else {
	log_message(2, "(d) new_part = 1\n");
	new_part = 1;
      }
    }
    previous_bar_type = sym->u.bar.type;
    if (sym->u.bar.type == B_THIN_THICK)
      song_finished = 1;
  }
}

void add_chords() {
  int old_arena_level = lvlarena(0);
  for (struct SYMBOL *sym = parse.first_sym; sym != NULL; sym = sym->abc_next) {
    process_symbol(sym);
  }
  lvlarena(old_arena_level);
}

const char *left_upper_square_bracket = "&#x23A1;";
const char *right_upper_square_bracket = "&#x23A4;";


void print_measures(struct CSong *song, struct CMeasure *measures, int measures_per_line, int compressed_whitespace, int *measures_printed) {
  for (struct CMeasure *measure = measures; measure != NULL; measure = measure->next) {
    if (measure->leadin)
      continue;
    if (*measures_printed == measures_per_line) {
      fprintf(chord_out, "\n<br/>&nbsp;&nbsp;&nbsp;&nbsp;");
      *measures_printed = 0;
      if (measure->time_signature && song->meter_change) {
	fprintf(chord_out, "<b>%s</b>&nbsp;&nbsp;", measure->time_signature);
      }
    } else if (measure->time_signature && song->meter_change) {
      fprintf(chord_out, "&nbsp;&nbsp;<b>%s</b>&nbsp;&nbsp;", measure->time_signature);
    }
    char chord_buf[256];
    memset(chord_buf, 0, 256);
    char *buf = chord_buf;
    int first_chord = 1;
    int next_bar_broken = 0;
    for (struct CChord *chord = measure->chords; chord != NULL; chord = chord->next) {
      if (first_chord == 0) {
	//const char *clean = clean_chord(chord->name);
	//log_message(1, "chord = '%s', clean=%s\n", chord->name, clean);
	if (next_bar_broken)
	  sprintf(buf, "&#x00A6;%s", chord->name);
	else
	  sprintf(buf, "|%s", chord->name);
      } else {
	sprintf(buf, "%s", chord->name);
	first_chord = 0;
      }
      next_bar_broken = chord->broken_bar ? 1 : 0;
      buf += strlen(buf);
    }
    int buf_len = strlen(chord_buf);
    // Squash chords in form of A|A
    if (buf_len > 1 && buf_len % 2 == 1) {
      int mid = buf_len / 2;
      if (chord_buf[mid] == '|') {
	if (!strncmp(chord_buf, &chord_buf[mid+1], mid)) {
	  chord_buf[mid] = '\0';
	  buf_len = strlen(chord_buf);
	}
      }
    }
    // Check for Unicode char
    if (buf_len > 8 && strstr(chord_buf, "&#x")) {
      buf_len -= 8;
    }
    if (compressed_whitespace) {
      fprintf(chord_out, "&nbsp;%s", chord_buf);
    } else {
      switch (buf_len) {
      case (0):
	break;
      case (1):
	fprintf(chord_out, "&nbsp;&nbsp;&nbsp;%s&nbsp;&nbsp;&nbsp;", chord_buf);
	break;
      case (2):
	fprintf(chord_out, "&nbsp;&nbsp;&nbsp;%s&nbsp;&nbsp;", chord_buf);
	break;
      case (3):
	fprintf(chord_out, "&nbsp;&nbsp;%s&nbsp;&nbsp;", chord_buf);
	break;
      case (4):
	fprintf(chord_out, "&nbsp;&nbsp;%s&nbsp;", chord_buf);
	break;
      case (5):
	fprintf(chord_out, "&nbsp;%s&nbsp;", chord_buf);
	break;
      default:
	fprintf(chord_out, "%s&nbsp;", chord_buf);
	break;
      }
    }
    (*measures_printed)++;
  }
}

void print_endings(struct CSong *song, struct CPart *part) {
  if (part->next_ending == 0) {
    return;
  }
  int num = 0;
  int measures_printed = 0;
  fprintf(chord_out, "%s<sup>%d</sup>", left_upper_square_bracket, ++num);
  // prevent wrapping (measures_per_line == 100)
  print_measures(song, part->endings[0], 100, 1, &measures_printed);
  fprintf(chord_out, "&nbsp;");
  for (int i=1; i<part->next_ending; i++) {
    fprintf(chord_out, "%s<sup>%d</sup>", left_upper_square_bracket, ++num);
    print_measures(song, part->endings[i], 100, 1, &measures_printed);
  }
}

void print_part(struct CSong *song, struct CPart *part) {
  int measure_count = 0;
  for (struct CMeasure *measure = part->measures; measure != NULL; measure = measure->next) {
    measure_count++;
  }
  log_message(1, "measure_count = %d\n", measure_count);
  // Just count measures in one ending
  if (part->next_ending != 0) {
    for (struct CMeasure *measure = part->endings[0]; measure != NULL; measure = measure->next) {
      measure_count++;
    }
  }
  int measures_per_line = 8;
  if (measure_count <= 10) {
    measures_per_line = measure_count > 8 ? measure_count : 8;
  } else if (measure_count % 10 == 0) {
    measures_per_line = 10;
  }
  int measures_printed = 0;
  print_measures(song, part->measures, measures_per_line, 0, &measures_printed);
  print_endings(song, part);
}

struct CSongs {
  int max;
  int next;
  struct CSong **song;
};

int count_songs() {
  int count = 0;
  for (struct CSong *song = first_song; song != NULL; song = song->next) {
    if (song_empty(song))
      continue;
    count++;
  }
  return count;
}

int compare_songs(const void *lhs, const void *rhs) {
  struct CSong *left = *(struct CSong **)lhs;
  struct CSong *right = *(struct CSong **)rhs;
  return strcmp(left->title, right->title);
}

int compare_songs_by_key(const void *lhs, const void *rhs) {
  struct CSong *left = *(struct CSong **)lhs;
  struct CSong *right = *(struct CSong **)rhs;
  if (left->key < right->key)
    return -1;
  else if (left->key == right->key) {
    if (left->accidental < right->accidental)
      return -1;
    else if (left->accidental == right->accidental) {
      if (left->mode < right->mode)
	return -1;
      else if (left->mode == right->mode) {
	if (left->minor < right->minor)
	  return -1;
	else if (left->minor == right->minor) {
	  return strcmp(left->title, right->title);
	}
      }
    }
  }
  return 1;
}

int same_key(struct CSong *left, struct CSong *right) {
  return left->key == right->key &&
    left->accidental == right->accidental &&
    left->mode == right->mode &&
    left->minor == right->minor;
}

void print_index_key_heading(struct CSong *song) {
  char key_buf[32];
  char *ptr = key_buf;
  *ptr++ = song->key;
  if (song->accidental == -1) {
    strcpy(ptr, "&#9837;");
    ptr += strlen(ptr);
  }
  else if (song->accidental == 1) {
    strcpy(ptr, "&#9839;");
    ptr += strlen(ptr);
  }
  if (song->minor)
    *ptr++ = 'm';
  if (song->mode) {
    *ptr = '\0';
    if (song->mode == 2)
      strcpy(ptr, " Dorian");
    else if (song->mode == 5)
      strcpy(ptr, " Mixolydian");
    ptr += strlen(ptr);
  }
  *ptr = '\0';
  fprintf(chord_out, "<b style=\"font-family: Arial\">%s</b><br/>\n", key_buf);
}

void print_index(struct CSong **original_songs, int max_song) {
  struct CSong **songs = malloc(max_song * sizeof(struct CSong *));
  memcpy(songs, original_songs, max_song * sizeof(struct CSong *));
  qsort(songs, max_song, sizeof(struct CSong *), compare_songs_by_key);
  int base = 0;
  fprintf(chord_out, "<h2 style=\"font-family: Arial\">Tunes by Key</h2>\n");
  int index = 0;
  while (base < max_song) {
    index = base + 1;
    while (index < max_song && same_key(songs[index-1], songs[index])) {
      index++;
    }
    print_index_key_heading(songs[base]);
    int third = ((index - base) + 2) / 3;
    int first = base + third;
    int second = first + third;
    fprintf(chord_out, "<div class=\"row\">\n");
    fprintf(chord_out, "  <div class=\"column\">\n");
    for (int i=base; i<first; i++) {
      fprintf(chord_out, "    %s<br/>\n", songs[i]->title);
    }
    fprintf(chord_out, "  </div>\n");
    if (first < index) {
      fprintf(chord_out, "  <div class=\"column\">\n");
      for (int i=first; i<second; i++) {
	fprintf(chord_out, "    %s<br/>\n", songs[i]->title);
      }
      fprintf(chord_out, "  </div>\n");
      if (second < index) {
	fprintf(chord_out, "  <div class=\"column\">\n");
	for (int i=second; i<index; i++) {
	  fprintf(chord_out, "    %s<br/>\n", songs[i]->title);
	}
	fprintf(chord_out, "  </div>\n");
      }
    }
    fprintf(chord_out, "</div>\n");
    base = index;
  }
  free(songs);
}

struct CSong **dedup_songs(int *max) {
  int max_song = count_songs();
  struct CSong **songs = malloc(max_song * sizeof(struct CSong *));
  int index = 0;
  for (struct CSong *song = first_song; song != NULL; song = song->next) {
    if (song_empty(song))
      continue;
    for (struct CPart *part = song->parts; part != NULL; part = part->next) {
      if (empty_part(part))
	continue;
      squash_identical_repeats(part);
    }
    songs[index++] = song;
  }
  qsort(songs, index, sizeof(struct CSong *), compare_songs);
  *max = 0;
  int previous_song_index = 0;
  for (index = 0; index < max_song; index++) {
    if (index > 0 && equal_songs(songs[previous_song_index], songs[index]) != 0)
      songs[index] = NULL;
    else {
      previous_song_index = index;
      (*max)++;
    }
  }
  struct CSong **old_songs = songs;
  songs = malloc(*max * sizeof(struct CSong *));
  int new_index = 0;
  for (index = 0; index < max_song; index++) {
    if (old_songs[index] != NULL)
      songs[new_index++] = old_songs[index];
  }
  assert(new_index == *max);
  free(old_songs);
  return songs;
}

void generate_chords_file() {

  int max_song;
  struct CSong **songs = dedup_songs(&max_song);

  chord_out = fopen("Chords.html", "w");

  fprintf(chord_out, "<!DOCTYPE html>\n<html>\n<head>\n");
  fprintf(chord_out, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
  fprintf(chord_out, "<style>\n");
  fprintf(chord_out, "p { font-family: Courier; }\n");
  fprintf(chord_out, "* { box-sizing: border-box; }\n");
  fprintf(chord_out, ".column { float: left; width: 33.33%%; padding: 10px; font-family: Arial}\n");
  fprintf(chord_out, ".row:after { content: \"\"; display: table;clear: both; }\n");
  fprintf(chord_out, "</style>\n</head>\n<body>\n");

  print_index(songs, max_song);

  fprintf(chord_out, "<h2 style=\"font-family: Arial\">Alphabetical List of Tunes</h2>\n");
  fprintf(chord_out, "<p style=\"font-family: Courier;\">\n");

  for (int index = 0; index < max_song; index++) {
    if (index > 0 && equal_songs(songs[index-1], songs[index]) != 0)
      continue;
    struct CSong *song = songs[index];
    if (song->time_signature) {
      if (song->meter_change == 0) {
	fprintf(chord_out, "<b>%s (%s)</b><br/>\n", song->title, song->time_signature);
      } else {
	fprintf(chord_out, "<b>%s</b><br/>\n", song->title);
      }
    } else {
      fprintf(chord_out, "<b>%s</b><br/>\n", song->title);
    }
    for (struct CPart *part = song->parts; part != NULL; part = part->next) {
      if (empty_part(part))
	continue;
      fprintf(chord_out, "<i>%c</i>&nbsp;", part->name);
      if (part->repeat) {
	fprintf(chord_out, "|:");
      } else {
	fprintf(chord_out, "&nbsp;&nbsp;");
      }
      print_part(song, part);
      if (part->repeat && part->next_ending == 0) {
	fprintf(chord_out, ":|");
      }
      fprintf(chord_out, "<br/>\n");
    }
    fprintf(chord_out, "<br/>\n");
  }
  fprintf(chord_out, "</body>\n</html>\n");
}
