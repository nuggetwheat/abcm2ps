#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "abcm2ps.h"
#include "chords.h"

char bar_buf[8];
int duration = 0;
int note_count = 0;
int leadin_duration = 0;
char next_part = 'A';
int unit_note_length = -1;
int meter_note_length = -1;
int previous_bar_type = 0;
int ending = 0;
FILE *chord_out = NULL;
int l_divisor = 0;
int meter_num = 0;
int meter_denom = 0;
int measure_duration = BASE_LEN;
int skipping_voice = 0;
int song_finished = 0;
int hard_finished = 0;
char primary_voice[64];
char *next_time_signature = NULL;
int auto_detect_parts = 1;
char last_note_pitch = 0;
int new_measure_needed = 0;

struct Auxillary aux = { 0, 0, (char *)0, (char *)0 };

struct CSong *first_song = NULL;
struct CSong *cur_song = NULL;
struct CPart *cur_part = NULL;
struct CMeasure *cur_measure = NULL;
struct CChord *cur_chord = NULL;
struct CChord *previous_chord = NULL;
struct CChord *previous_ending_chord = NULL;

#define VERBOSE 1

#ifdef VERBOSE
#define LOG_MESSAGE(...) \
  fprintf (stderr, "%d ", __LINE__); \
  fprintf (stderr, __VA_ARGS__); \
  fprintf (stderr, "\n")
#else
#define LOG_MESSAGE(...)
#endif

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
  previous_ending_chord = NULL;
  next_part = 'A';
  unit_note_length = -1;
  meter_note_length = -1;
  l_divisor = 0;
  meter_num = 0;
  meter_denom = 0;
  measure_duration = BASE_LEN;
  cur_song->measure_duration = BASE_LEN;
  duration = 0;
  note_count = 0;
  leadin_duration = 0;
  previous_bar_type = 0;
  ending = 0;
  skipping_voice = 0;
  primary_voice[0] = '\0';
  song_finished = 0;
  hard_finished = 0;
  next_time_signature = NULL;
  auto_detect_parts = 1;
  last_note_pitch = 0;
  new_measure_needed = 0;
}

void allocate_part() {
  if (cur_song == NULL)
    allocate_song();
  struct CPart *part = allocate_bytes(sizeof(struct CPart));
  memset(part, 0, sizeof(*part));
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
  previous_ending_chord = NULL;
  cur_chord = NULL;
  duration = 0;
  leadin_duration = 0;
  LOG_MESSAGE("Setting leadin duration to 0");
  ending = 0;
  cur_part->name = (char *)allocate_bytes(2);
  cur_part->name[0] = next_part++;
  cur_part->name[1] = '\0';
  last_note_pitch = 0;
}

void allocate_named_part(const char *name) {
  allocate_part();
  cur_part->name = (char *)allocate_bytes(strlen(name)+1);
  strcpy(cur_part->name, name);
}

void allocate_measure() {
  if (cur_part == NULL) {
    allocate_part();
    LOG_MESSAGE("Allocated part %s (%p)", cur_part->name, (void *)cur_part);
  }
  new_measure_needed = 0;
  if (cur_measure != NULL) {
    if (cur_measure->chords == NULL)
      return;
    cur_measure->notes = note_count;
  }
  note_count = 0;
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
  cur_measure->beats = meter_num;
}

void allocate_ending() {
  if (cur_part == NULL) {
    allocate_part();
    LOG_MESSAGE("Allocated part %s (%p)", cur_part->name, (void *)cur_part);
  }
  struct CMeasure *measure = allocate_bytes(sizeof(struct CMeasure));
  memset(measure, 0, sizeof(struct CMeasure));
  assert(cur_part->next_ending < MAX_ENDINGS);
  cur_part->endings[cur_part->next_ending++] = measure;
  cur_measure = measure;
  cur_measure->ending = 1;
  ending++;
  previous_chord = NULL;
  cur_chord = NULL;
  next_time_signature = NULL;
#if 0
  if (next_time_signature != NULL) {
    cur_measure->time_signature = next_time_signature;
    next_time_signature = NULL;
  }
#endif
  cur_measure->beats = meter_num;
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
    LOG_MESSAGE("Allocated measure %p", cur_measure);
  }
  if (cur_measure->chords == NULL) {
    cur_measure->chords = cur_measure->last_chord = cur_chord;
  }
  else {
    assert(cur_measure->last_chord != cur_chord);
    cur_measure->last_chord->next = cur_chord;
    cur_measure->last_chord = cur_chord;
  }
  cur_measure->duration += cur_chord->duration;
  LOG_MESSAGE("Added %s to measure %p (duration=%d)", cur_chord->name, (void *)cur_measure, cur_chord->duration);
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

// fix me !! (get rid of measure == NULL)
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

  strip_empty_measures(&part->measures);

  if (part->next_ending == 0)
    return;
  part->repeat = 1;

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

void set_accidentals(const char *text, struct CSong *song) {
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
  for (char *textp = beginp+1; *textp; textp++) {
    if (isalpha(*textp) && !islower(*textp))
      *textp = tolower(*textp);
  }
  // Check for minor
  if (*(endp-1) == 'm' || strstr(beginp, "minor")) {
    cur_song->minor = 1;
    cur_song->key = beginp[0];
    set_accidentals(beginp, cur_song);
  } else if (strstr(beginp, "mix")) {
    cur_song->mode = 5;
    cur_song->key = beginp[0];
    set_accidentals(beginp, cur_song);
  } else if (strstr(beginp, "dor")) {
    cur_song->mode = 2;
    cur_song->key = beginp[0];
    set_accidentals(beginp, cur_song);
  } else {
    const char *key = convert_sf_to_key(sym->u.key.sf);
    cur_song->key = key[0];
    set_accidentals(key, cur_song);
  }
  const char *key = convert_sf_to_key(sym->u.key.sf);
  cur_song->key_signature = key[0];
  set_accidentals(key, cur_song);
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

int abc_type_is_meta(struct SYMBOL *sym) {
  switch (sym->abc_type) {
  case(ABC_T_INFO):
  case(ABC_T_PSCOM):
  case(ABC_T_CLEF):
  case(ABC_T_EOLN):
  case(ABC_T_V_OVER):
    return 1;
  default:
    break;
  }
  return 0;
}


const char *DIMINISHED = "&#x05AF;";
char g_tmp_chord_buf[256];
const char *clean_chord(const char *text, int *diminished) {
  char *bufp = g_tmp_chord_buf;
  memset(g_tmp_chord_buf, 0, 256);
  *diminished = 0;

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

  LOG_MESSAGE("Name = '%s'", g_tmp_chord_buf);

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
      if (isupper(*p) && *(p-1) != '(')
	*p = tolower(*p);
      p++;
    }
    p = &g_tmp_chord_buf[index+1];
    if (strstr(p, "maj") == NULL && strstr(p, "min") == NULL &&
	strstr(p, "dim") == NULL && strstr(p, "aug") == NULL) {
      for (p = &g_tmp_chord_buf[index+1]; *p; p++) {
	if (*p != 'm' && *p != '#' && *p != '(' && *p != ')' && !isdigit(*p) &&
	    !((*p >= 'a' && *p <= 'g') || (*p >= 'A' && *p <= 'G'))) {
	  g_tmp_chord_buf[0] = '\0';
	  break;
	}
      }
    }
  }

  // Replaced "dim" with symbol
  bufp = strstr(g_tmp_chord_buf, "dim");
  if (bufp) {
    *diminished = 1;
    char temp_buf[256];
    strcpy(temp_buf, g_tmp_chord_buf);
    temp_buf[bufp-g_tmp_chord_buf] = '\0';
    bufp += 3;
    // copy everything after "dim"
    strcat(temp_buf, bufp);
    strcpy(g_tmp_chord_buf, temp_buf);
  }
  return g_tmp_chord_buf;
}

char g_chord_buf[256];
const char *get_chord_name(struct SYMBOL *sym, int *repeatp, int *dal_segno, int *diminished) {
  char tmp_buf[256];
  char *tptr = tmp_buf;
  char *parts[MAXGCH];
  int part_index = 0;
  *dal_segno = 0;
  for (struct gch *gch = sym->gch; gch->type; gch++) {
    int idx = gch->idx;
    if (gch->type == 'r')
      *repeatp = 1;
    LOG_MESSAGE("chord_name = '%s'", &sym->text[idx]);
    if (strstr(&sym->text[idx], "D.S.")) {
      *dal_segno = 1;
    }
    if (sym->text[idx] == '?' || sym->text[idx] == '@' ||
	sym->text[idx] == '<' || sym->text[idx] == '>' ||
	sym->text[idx] == '^' || sym->text[idx] == '_' ||
	sym->text[idx] == '$' || (idx > 0 && sym->text[idx-1] == '^') || isspace(sym->text[idx]))
      continue;
    const char *clean = clean_chord(&sym->text[gch->idx], diminished);
    int len = strlen(clean);
    if (len) {
      strcpy(tptr, clean);
      parts[part_index++] = tptr;
      tptr += strlen(clean) + 1;
    }
  }
  // Move parenthetical chord to after primary chord
  if (part_index > 1) {
    if (parts[part_index-1][0] == '(') {
      char *tmp_part = parts[part_index-1];
      parts[part_index-1] = parts[part_index-2];
      parts[part_index-2] = tmp_part;
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

int compare_intervals(const void *lhs, const void *rhs) {
  return *(const char *)lhs - *(const char *)rhs;
}


void add_interval(struct SYMBOL *sym) {
  char pitch = 0;
  for (int i=0; sym->u.note.notes[i].len; i++) {
    // select highest pitch note
    if (sym->u.note.notes[i].pit > pitch) {
      pitch = sym->u.note.notes[i].pit;
      LOG_MESSAGE("Pitch=%d (last=%d)", pitch, last_note_pitch);
    }
  }
  if (last_note_pitch) {
    char interval = (last_note_pitch < pitch) ? (pitch - last_note_pitch) : (last_note_pitch - pitch);
    if (cur_song->longest_intervals[0] < interval) {
      cur_song->longest_intervals[0] = interval;
      qsort(cur_song->longest_intervals, MAX_LONGEST_INTERVALS, sizeof(char), compare_intervals);
    }
  }
  last_note_pitch = pitch;
}

void process_symbol(struct SYMBOL *sym) {

  if (hard_finished || sym == NULL)
    return;

  LOG_MESSAGE("%s '%s' sflags=0x%x", abc_type(sym), sym->text ? sym->text : "", sym->sflags);

  if (sym->abc_type == ABC_T_NOTE) {
    add_interval(sym);
  }

  if (sym->abc_type == ABC_T_INFO) {
    if (sym->text != NULL) {
      if (sym->text[0] == 'X') {
	allocate_song();
	cur_song->index = (int)strtol(&sym->text[2], NULL, 0);
      } else {
	if (song_finished) {
	  if (sym->text[0] != 'P')
	    return;
	  else if (sym->text[2] == 'A') {
	    // Sometimes abc files contain two versions of the song with the second
	    // prefixed with "Alternatively, you could play it like this ..."
	    // This causes the second version to be skipped over.
	    hard_finished = 1;
	    return;
	  }
	}
	if (sym->text[0] == 'T') {
	  if (cur_song->title == NULL) {
	    int offset = 2;
	    if (!strncasecmp(&sym->text[2], "The ", 4))
	      offset = 6;
	    cur_song->title = allocate_bytes(strlen(sym->text));
	    strcpy(cur_song->title, &sym->text[offset]);
	  }
	} else if (sym->text[0] == 'C') {
	  if (cur_song->composer == NULL) {
	    int index = 2;
	    while (isspace(sym->text[index]))
	      index++;
	    cur_song->composer = allocate_bytes(strlen(&sym->text[index])+1);
	    strcpy(cur_song->composer, &sym->text[index]);
	  }
	} else if (sym->text[0] == 'K') {
	  if (strstr(sym->text, "staffscale=0.")) {
	    song_finished = 1;
	  }
	  if (cur_song->key == '\0')
	    set_key(sym);
	  LOG_MESSAGE("Key=%c, Mode=%d, sf=%d", cur_song->key, cur_song->mode, sym->u.key.sf);
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
	    cur_song->measure_duration = measure_duration;
	  }
	} else if (sym->text[0] == 'M') {
	  if (cur_measure == NULL) {
	    allocate_measure();
	  }
	  char *time_signature = &sym->text[2];
	  if (!strcmp(time_signature, "C")) {
	    time_signature = "4/4";
	  } else if (!strcmp(time_signature, "C|")) {
	    time_signature = "2/2";
	  }
	  next_time_signature = allocate_bytes(strlen(time_signature)+1);
	  strcpy(next_time_signature, time_signature);
	  if (cur_song->time_signature == NULL) {
	    cur_song->time_signature = next_time_signature;
	  } else if (cur_song->meter_change == 0 &&
		     strcmp(next_time_signature, cur_song->time_signature) != 0) {
	    cur_song->meter_change = 1;
	  }
	  // If current measure is still being populated, add time signature
	  if (cur_measure != NULL && cur_measure->finished != 1 && cur_measure->time_signature == NULL) {
	    cur_measure->time_signature = next_time_signature;
	    LOG_MESSAGE("Setting next ts to NULL");
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
	    if (cur_song->measure_duration == 0 || cur_song->beat_duration == 0) {
	      cur_song->measure_duration = measure_duration;
	      cur_song->beat_duration = measure_duration / meter_num;
	      cur_song->beats_per_measure = meter_num;
	    }
	    if (measure_empty(cur_measure) || cur_measure->beats == 0) {
	      cur_measure->beats = meter_num;
	    }
	  }
	} else if (sym->text[0] == 'P' && strcmp(sym->text, "P:W") && strlen(sym->text) < 5 &&
		   !(cur_measure && cur_measure->ending && cur_measure->chords == NULL)) {
	  song_finished = 0;
	  skipping_voice = 0;
	  auto_detect_parts = 0;
	  LOG_MESSAGE("Got %s", sym->text);
	  if (cur_part && cur_part->measures && cur_part->measures->next == 0) {
	    LOG_MESSAGE("Renaming current part '%s' to '%s'", cur_part->name, &sym->text[2]);
	    // hack to handle Elzics Farewell.
	    if (strcmp(cur_part->name, "Z") == 0 && !ending) {
	      cur_part->measures = cur_part->last_measure = NULL;
	      cur_measure = NULL;
	    }
	    cur_part->name = (char *)allocate_bytes(strlen(&sym->text[2])+1);
	    strcpy(cur_part->name, &sym->text[2]);
	  } else {
	    allocate_named_part(&sym->text[2]);
	    LOG_MESSAGE("Allocated part %s (%p)", cur_part->name, (void *)cur_part);
	  }
	} else if (sym->text[0] == 'V') {
	  LOG_MESSAGE("Voice = '%s'", &sym->text[2]);
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
	    LOG_MESSAGE("Primary Voice = '%s'", primary_voice);
	  }
	}
      }
    }
  }

  if (abc_type_is_meta(sym)) {
    return;
  }

  if (song_finished || skipping_voice)
    return;

  if (new_measure_needed) {
    allocate_measure();
    LOG_MESSAGE("Allocated measure %p because it was needed", cur_measure);
  }

  if (sym->gch) {
    int repeat = 0;
    int dal_segno = 0;
    int diminished = 0;
    const char *name = get_chord_name(sym, &repeat, &dal_segno, &diminished);
    // The following condition is a hack for Ookpik Waltz.  Should probably
    // create a new part named "D.S." and add chord to that new part
    if (!ending && dal_segno)
      return;
    if (name || repeat) {
      if (name)
	LOG_MESSAGE("name = '%s'", name);
      LOG_MESSAGE("gch='%s' (duration=%d, ending=%d)", sym->text, duration, ending);
      if (duration != 0) {
	if (cur_chord) {
	  cur_chord->duration = duration;
	} else if (previous_chord) {
	  allocate_chord();
	  cur_chord->name = previous_chord->name;
	  cur_chord->duration = duration;
	} else if (ending && previous_ending_chord) {
	  LOG_MESSAGE("Adding previous_ending_chord");
	  allocate_chord();
	  cur_chord->name = previous_ending_chord->name;
	  cur_chord->duration = duration;
	}
	if (cur_chord) {
	  LOG_MESSAGE("Adding chord %s to measure %p",
		      cur_chord->name, cur_measure);
	}
	add_chord_to_measure();
	duration = 0;
      }
      if (repeat) {
	LOG_MESSAGE("prev_chord=%s, cur_chord=%s",
		    previous_chord ? previous_chord->name : "",
		    cur_chord ? cur_chord->name : "");
	if (ending == 0 && previous_chord) {
	  LOG_MESSAGE("Setting previous_ending_chord to %s", previous_chord->name);
	  previous_ending_chord = previous_chord;
	  previous_chord = NULL;
	}
      } else {
	allocate_chord();
	previous_chord = NULL;
	cur_chord->name = allocate_bytes(strlen(name)+1);
	cur_chord->diminished = diminished;
	strcpy(cur_chord->name, name);
	LOG_MESSAGE("new chord %s %s", cur_chord->name, repeat ? "repeat" : "");
      }
    }
  }

  if (sym->abc_type == ABC_T_NOTE || sym->abc_type == ABC_T_REST) {
    if ((sym->sflags & S_SEQST) != 0) {
      duration += sym->dur;
    }
    if (sym->abc_type == ABC_T_NOTE) {
      note_count++;
    }
  } else if (sym->abc_type == ABC_T_BAR) {
    LOG_MESSAGE("bar %s cur=%s prev=%s dur=%d measure_duration=%d repeat_bar=%d",
		bar_type(sym->u.bar.type),
		cur_chord ? cur_chord->name : "",
		previous_chord ? previous_chord->name : "", duration,
		measure_duration, sym->u.bar.repeat_bar);
    if (sym->u.bar.type == B_OBRA && sym->u.bar.repeat_bar) {
      int save_duration = duration;
      struct CChord *save_chord = cur_chord;
      allocate_ending();
      LOG_MESSAGE("Allocated ending %d, measure %p", ending, (void *)cur_measure);
      duration = save_duration;
      cur_chord = save_chord;
    }
    if (duration != 0) {
      if (cur_chord) {
	cur_chord->duration = duration;
	if (sym->u.bar.type == B_RREP) {
	  if (cur_measure->duration + cur_chord->duration < measure_duration) {
	    LOG_MESSAGE("Adding leadin_duration %d", leadin_duration);
	    cur_chord->duration += leadin_duration;
	  }
	  LOG_MESSAGE("Setting leadin duration to 0");
	  duration = leadin_duration = 0;
	}
	// Leadin durtaion
	int tmp_duration = cur_measure ? cur_measure->duration + cur_chord->duration : cur_chord->duration;
	if (tmp_duration < measure_duration &&
	    (previous_bar_type == 0 ||
	     (sym->u.bar.type == B_SINGLE && sym->u.bar.dotted == 0 && !ending && cur_measure != NULL))) {
	  LOG_MESSAGE("Setting leadin_duration to %d", tmp_duration);
	  leadin_duration = tmp_duration;
	} else {
	  if (sym->u.bar.dotted)
	    cur_chord->broken_bar = 1;
	  LOG_MESSAGE("Adding chord %s to measure %p",
		      cur_chord->name, cur_measure);
	  add_chord_to_measure();
	}
      } else if (previous_chord) {
	if (previous_bar_type != B_RREP) {
	  allocate_chord();
	  cur_chord->name = previous_chord->name;
	  cur_chord->duration = duration;
	  if (cur_measure->duration + duration + leadin_duration < measure_duration) {
	    cur_measure->leadin = 1;
	    leadin_duration = duration;
	  }
	  previous_chord = NULL;
	  LOG_MESSAGE("Adding previous chord %s to %s %p",
		      cur_chord->name, 
		      cur_measure->leadin ? "leadin measure" : "measure",
		      cur_measure);
	  add_chord_to_measure();
	}
      } else if (previous_ending_chord) {
	allocate_chord();
	cur_chord->name = previous_ending_chord->name;
	cur_chord->duration = duration;
	LOG_MESSAGE("Adding previous ending chord %s to measure %p",
		    cur_chord->name, cur_measure);
	add_chord_to_measure();
      }
      else {
	LOG_MESSAGE("Setting leadin_duration to %d", duration);
	leadin_duration = duration;
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
      if (cur_measure && cur_measure->duration < measure_duration &&
	  cur_measure == cur_part->measures) {
	LOG_MESSAGE("Setting measure as leadin (%p)", (void *)cur_measure);
	cur_measure->leadin = 1;
      }
      new_measure_needed = 1;
    }
    if (sym->u.bar.type == B_LREP) {
      LOG_MESSAGE("ending = 0");
      ending = 0;
      previous_ending_chord = NULL;
      if (previous_bar_type) {
	if (!empty_part(cur_part)) {
	  if (auto_detect_parts) {
	    allocate_part();
	    LOG_MESSAGE("Allocated part %s (%p)", cur_part->name, (void *)cur_part);
	  } else {
	    allocate_named_part("");
	    LOG_MESSAGE("Allocated part %s (%p)", cur_part->name, (void *)cur_part);
	  }
	}
	cur_part->repeat = 1;
      }
    }
    if (sym->u.bar.type == B_DOUBLE) {
      LOG_MESSAGE("ending=%d, repeat=%d", ending, cur_part->repeat);
      if (cur_part->repeat) {
	if (ending) {
	  if (auto_detect_parts) {
	    allocate_part();
	    LOG_MESSAGE("Allocating part %c (bar = %s)", next_part, bar_type(sym->u.bar.type));
	    previous_ending_chord = NULL;
	  }
	} else {
	  new_measure_needed = 1;
	}
      } else if (cur_measure && cur_measure->duration == measure_duration) {
	new_measure_needed = 1;
      }
    }
    if (sym->u.bar.type == B_RREP) {
      cur_part->repeat = 1;
      if (ending == 2 && !measure_empty(cur_measure)) {
	ending = 0;
	previous_ending_chord = NULL;
      } else if (!ending && auto_detect_parts) {
	allocate_part();
	LOG_MESSAGE("Allocating part %c (bar = %s)", next_part, bar_type(sym->u.bar.type));
      }
    } else if (sym->u.bar.type == B_DREP) {
      cur_part->repeat = 1;
      allocate_part();
      LOG_MESSAGE("Allocating part %c (bar = %s)", next_part, bar_type(sym->u.bar.type));
      cur_part->repeat = 1;
    }
    previous_bar_type = sym->u.bar.type;
    if (!ending && (cur_measure == NULL || cur_measure->chords) && sym->u.bar.dotted == 0) {
      new_measure_needed = 1;
    }
    if (sym->u.bar.type == B_THIN_THICK) {
      song_finished = 1;
      if (cur_measure) {
	cur_measure->notes = note_count;
	note_count = 0;
      }
      cur_measure = NULL;
    }
  }
}

void add_chords() {
  int old_arena_level = lvlarena(0);
  for (struct SYMBOL *sym = parse.first_sym; sym != NULL; sym = sym->abc_next) {
    process_symbol(sym);
  }
  if (cur_measure && cur_measure->notes != 0) {
    cur_measure->notes = note_count;
  }
  lvlarena(old_arena_level);
}

const char *left_upper_square_bracket = "&#x23A1;";
const char *right_upper_square_bracket = "&#x23A4;";

int skip_part(struct CPart *part) {
  return empty_part(part) ||
    (part->measures->next == NULL && part->next_ending == 0);
}

int count_measures(struct CSong* song, struct CPart *part) {
  int measure_count = 0;
  for (struct CMeasure *measure = part->measures; measure != NULL; measure = measure->next) {
    if (measure->leadin)
      continue;
    measure_count++;
    if (measure->time_signature && song->meter_change)
      measure_count++;
  }
  return measure_count;
}

int count_ending_measures(struct CMeasure *measure) {
  int measure_count = 0;
  for (; measure != NULL; measure = measure->next) {
    if (measure->leadin)
      continue;
    measure_count++;
    if (measure->time_signature)
      measure_count++;
  }
  LOG_MESSAGE("Ending measures = %d", measure_count);
  return measure_count;
}

int count_measures_with_endings(struct CSong* song, struct CPart *part) {
  int measure_count = count_measures(song, part);
  for (int i=0; i<part->next_ending; i++) {
    measure_count += count_ending_measures(part->endings[i]);
  }
  return measure_count;
}

#define TEXTBUF_SIZE 4096
char text_buf[TEXTBUF_SIZE];
char *text_ptr;

struct MeasureFormat {
  int ending;
  int time_signature;
  int chords_len;
  const char *chords;
  int width;
  int leading_space;
};

struct PartFormat {
  struct CPart *part;
  int measure_count;
  struct MeasureFormat *measures;
  int ending_measure_count[10];
  struct MeasureFormat *ending[10];
  int ending_count;
  int line_count;
  struct MeasureFormat **lines[10];
};

void ConstructPartFormat(struct CSong *song, struct CPart *part, struct PartFormat* part_format) {
  int measure_count = count_measures(song, part);
  part_format->measure_count = measure_count;
  part_format->measures = malloc(measure_count*sizeof(struct MeasureFormat));
  memset(part_format->measures, 0, measure_count*sizeof(struct MeasureFormat));
  // Endings
  part_format->ending_count = part->next_ending;
  for (int i=0; i<part->next_ending; i++) {
    measure_count = count_ending_measures(part->endings[i]);
    part_format->ending_measure_count[i] = measure_count;
    part_format->ending[i] = malloc(measure_count*sizeof(struct MeasureFormat));
    memset(part_format->ending[i], 0, measure_count*sizeof(struct MeasureFormat));
  }
}

int visible_chord_length(struct CChord *chord) {
  const char *base = chord->name;
  int unicode_char_count = 0;
  char *ptr = strstr(base, "&#x");
  while (ptr != NULL) {
    unicode_char_count++;
    base = ptr + 3;
    ptr = strstr(base, "&#x");
  }
  int vlen = strlen(chord->name) - (8 * unicode_char_count);
  assert(vlen > 0);
  return vlen;
}

char *scale_degrees[7] = { "I", "ii", "iii", "IV", "V", "vi", "vii" };
char *scale_degrees_major[7] = { "I", "II", "III", "IV", "V", "VI", "VII" };
const char *scale_degree(int index, int is_major) {
  if (is_major)
    return scale_degrees_major[index];
  return scale_degrees[index];
}

char chord_text_buf[256];
const char *chord_text(struct CChord *chord, char key_signature, int* chords_visible_length) {
  const char *iptr = chord->name;
  char *optr = chord_text_buf;
  if (aux.flag & AUX_FLAG_CHORDS_BY_SCALEDEGREE) {
    while (*iptr) {
      if (*iptr < 'A' || *iptr > 'G') {
	if (strncmp(iptr, "&#", 2) == 0) {
	  strncpy(optr, iptr, 8);
	  iptr += 8;
	  optr += 8;
	} else {
	  if (isdigit(*iptr)) {
	    sprintf(optr, "<span class=\"super\">%c</span>", *iptr);
	    optr += strlen(optr);
	    iptr++;
	  } else if (strlen(iptr) > 2 && iptr[0] == '(' && isdigit(iptr[1]) && iptr[2] == ')') {
	    sprintf(optr, "<span class=\"super\" style=\"width: 24px;\">(%c)</span>", iptr[1]);
	    optr += strlen(optr);
	    iptr += 3;
	    (*chords_visible_length) += 1;
	  } else {
	    *optr++ = *iptr++;
	  }
	}
	(*chords_visible_length)++;
      } else {
	int index = (*iptr >= key_signature) ? *iptr - key_signature : (*iptr + 7) - key_signature;
	assert(index >= 0 && index < 8);
	iptr++;
	if (*iptr == '#') {
	  iptr++;
	}
	int is_minor = 0;
	if (*iptr && (*iptr == 'm' || (*iptr == 'M' && *(iptr+1) == 'i'))) {
	  is_minor = 1;
	}
	strcpy(optr, scale_degree(index, is_minor == 0));
	(*chords_visible_length) += strlen(optr);
	optr += strlen(optr);
	while (*iptr && (isalpha(*iptr) || isspace(*iptr) || *iptr == '#'))
	  iptr++;
	if (*iptr == '(' && *(iptr+1) >= 'A' && *(iptr+1) <= 'G') {
	  *optr++ = *iptr++;
	}
      }
    }
    *optr = '\0';
    return chord_text_buf;
  }
  *chords_visible_length += visible_chord_length(chord);
  strcpy(chord_text_buf, chord->name);
  if (chord->diminished)
    strcat(chord_text_buf, DIMINISHED);
  return chord_text_buf;
}


const char *populate_measure_text(struct CSong* song, struct CMeasure *measure, int* chords_visible_length) {
  int first_chord = 1;
  int next_bar_broken = 0;
  char *chords = text_ptr;
  *chords_visible_length = 0;
  for (struct CChord *chord = measure->chords; chord != NULL; chord = chord->next) {
    const char *chord_str = chord_text(chord, song->key_signature, chords_visible_length);
    if (first_chord == 0) {
      if (next_bar_broken)
	sprintf(text_ptr, "&#x00A6;%s", chord_str);
      else
	sprintf(text_ptr, "|%s", chord_str);
      (*chords_visible_length)++;
    } else {
      sprintf(text_ptr, "%s", chord_str);
      first_chord = 0;
    }
    next_bar_broken = chord->broken_bar ? 1 : 0;
    //LOG_MESSAGE("%s (%ld)", text_ptr, text_ptr-text_buf);
    text_ptr += strlen(text_ptr);
  }
  int buf_len = strlen(chords);
  // Squash chords in form of A|A
  if (buf_len > 1 && buf_len % 2 == 1) {
    int mid = buf_len / 2;
    if (chords[mid] == '|') {
      if (!strncmp(chords, &chords[mid+1], mid)) {
	if (strlen(&chords[mid]) < *chords_visible_length)
	  *chords_visible_length -= strlen(&chords[mid]);
	chords[mid] = '\0';
	text_ptr = &chords[mid];
      }
    }
  }
  // skip past trailing '\0'
  text_ptr++;
  assert(text_ptr - text_buf < TEXTBUF_SIZE);
  return chords;
}

void populate_time_signature(struct CMeasure *measure, struct MeasureFormat *measure_format) {
  measure_format->time_signature = 1;
  measure_format->chords = text_ptr;
  strcpy(text_ptr, measure->time_signature);
  measure_format->chords_len = strlen(text_ptr);
  text_ptr += strlen(text_ptr) + 1;
}

#define NBSP "&nbsp;"

void populate_part_text(struct CSong *song, struct CPart *part, struct PartFormat *part_format) {
  int next_measure = 0;
  for (struct CMeasure *measure = part->measures; measure != NULL; measure = measure->next) {
    if (measure->leadin)
      continue;
    if (measure->time_signature && song->meter_change) {
      populate_time_signature(measure, &part_format->measures[next_measure++]);
    }
    struct MeasureFormat *measure_format = &part_format->measures[next_measure++];
    measure_format->chords = populate_measure_text(song, measure, &measure_format->chords_len);
  }
  assert(next_measure == part_format->measure_count);
  if (part->next_ending != 0) {
    for (int i=0; i<part->next_ending; i++) {
      next_measure = 0;
      int first = 1;
      for (struct CMeasure *measure = part->endings[i]; measure != NULL; measure = measure->next) {
	if (measure->time_signature) {
	  populate_time_signature(measure, &part_format->ending[i][next_measure++]);
	}
	struct MeasureFormat *measure_format = &part_format->ending[i][next_measure++];
	if (first) {
	  measure_format->ending = i+1;
	  //measure_format->chords = populate_ending_text(i, measure, &measure_format->chords_len);
	  first = 0;
	}
	measure_format->chords = populate_measure_text(song, measure, &measure_format->chords_len);
      }
    }
  }
}

int same_key(struct CSong *left, struct CSong *right) {
  return left->key == right->key &&
    left->accidental == right->accidental;
#if 0
 && left->mode == right->mode
 && left->minor == right->minor;
#endif
}

int same_key_signature(struct CSong *left, struct CSong *right) {
  return left->key_signature == right->key_signature &&
    left->accidental == right->accidental;
}


void write_key_to_string(struct CSong *song, char *buf, int include_mode) {
  char *ptr = buf;
  *ptr++ = song->key;
  if (song->accidental == -1) {
    strcpy(ptr, "&#9837;");
    ptr += strlen(ptr);
  }
  else if (song->accidental == 1) {
    strcpy(ptr, "&#9839;");
    ptr += strlen(ptr);
  }
  if (include_mode) {
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
  }
  *ptr = '\0';
}

void write_key_signature_to_string(struct CSong *song, char *buf) {
  char *ptr = buf;
  *ptr++ = song->key_signature;
  if (song->accidental == -1) {
    strcpy(ptr, "&#9837;");
    ptr += strlen(ptr);
  }
  else if (song->accidental == 1) {
    strcpy(ptr, "&#9839;");
    ptr += strlen(ptr);
  }
  *ptr = '\0';
}

int newline_per_ending(struct PartFormat *part, int measures_per_line) {
  if (part->ending_count > 1) {
    int ending_measure_count = 0;
    for (int i=0; i<part->ending_count; i++) {
      ending_measure_count += part->ending_measure_count[i];
    }
    int lastline_measure_count = (part->measure_count % measures_per_line);
    if (lastline_measure_count + part->ending_measure_count[0] <= measures_per_line &&
	lastline_measure_count + ending_measure_count > measures_per_line) {
      return 1;
    }
  }
  return 0;
}

int count_lines(struct PartFormat *parts, int max_part, int measures_per_line) {
  int line_count = 0;
  for (int i=0; i<max_part; i++) {
    int measure_count = parts[i].measure_count;
    if (newline_per_ending(&parts[i], measures_per_line)) {
      line_count += parts[i].ending_count - 1;
    }
    for (int j=0; j<parts[i].ending_count; j++) {
      measure_count += parts[i].ending_measure_count[j];
    }
    line_count += (measure_count + (measures_per_line - 1)) / measures_per_line;
  }
  return line_count;
}

int print_measure(struct MeasureFormat *measure_fmt) {
  int visible_length = 0;
  char buf[4096];
  char *ptr = buf;
  int padding_len = (measure_fmt->width - measure_fmt->chords_len) / 2;
  int leading_space = measure_fmt->leading_space + padding_len;
  visible_length += leading_space;
  if (measure_fmt->ending) {
    leading_space -= 3;
  }
  for (int i=0; i<leading_space; i++) {
    strcpy(ptr, NBSP);
    ptr += strlen(ptr);
  }
  if (measure_fmt->ending) {
    sprintf(ptr, "<span class=\"ending\">%s<sup>%d</sup></span>&nbsp;", left_upper_square_bracket, measure_fmt->ending);
    ptr += strlen(ptr);
  }
  if (measure_fmt->time_signature) {
    strcpy(ptr, "<b>");
    ptr += strlen(ptr);
  }
  visible_length += measure_fmt->chords_len;
  strcpy(ptr, measure_fmt->chords);
  ptr += strlen(ptr);
  if (measure_fmt->time_signature) {
    strcpy(ptr, "</b>");
    ptr += strlen(ptr);
  }
  if ((measure_fmt->width - measure_fmt->chords_len) % 2 == 1) {
    strcpy(ptr, NBSP);
    ptr += strlen(ptr);
    visible_length++;
  }
  visible_length += padding_len;
  for (int i=0; i<padding_len; i++) {
    strcpy(ptr, NBSP);
    ptr += strlen(ptr);
  }
  fprintf(chord_out, "%s", buf);
  return visible_length;
}

#define MAX_LINES_PER_PAGE 51
#define MAX_LINE_LENGTH 63
#define DEFAULT_MAX_COLUMNS 8

void print_nbsp(int count) {
  for (int i=0; i<count; i++) {
    fprintf(chord_out, NBSP);
  }
}

int lines_printed = 3;  // Approximate lines for header

void print_song(struct CSong *song) {
  int max_part = 0;
  struct PartFormat parts[32];

  memset(parts, 0, 32*sizeof(struct PartFormat));
  memset(text_buf, 0, TEXTBUF_SIZE);
  text_ptr = text_buf;

  for (struct CPart *part = song->parts; part != NULL; part = part->next) {
    if (skip_part(part))
      continue;
    parts[max_part].part = part;
    ConstructPartFormat(song, part, &parts[max_part]);
    populate_part_text(song, part, &parts[max_part]);
    max_part++;
    assert(max_part < 32);
  }
  assert(text_ptr - text_buf < TEXTBUF_SIZE);
  
  int line_count = 0;
  int next_line = 0;
  int total_width = 0;
  struct MeasureFormat **lines[64];

  int max_columns = 8;
  for (int i=0; i<max_part; i++) {
    int measure_count = count_measures_with_endings(song, parts[i].part);
    if (measure_count == 9 && max_columns != 10) {
      max_columns = 9;
    } else if  (measure_count % 10 == 0) {
      max_columns = 10;
    }
  }

  line_count = count_lines(parts, max_part, max_columns);
  for (int i=0; i<line_count; i++) {
    lines[i] = malloc(max_columns*sizeof(struct MeasureFormat *));
    memset(lines[i], 0, max_columns*sizeof(struct MeasureFormat *));
  }

  char key[32];
  char *class_attribute = " style=\"font-family: Arial;\"";
  int song_line_count = 3 + line_count;
  write_key_to_string(song, key, 1);
  //fprintf(chord_out, "<!-- %d %d -->\n", lines_printed, song_line_count);
  if (lines_printed + song_line_count > MAX_LINES_PER_PAGE) {
    class_attribute = " class=\"page-break-before\" style=\"font-family: Arial;\"";
    lines_printed = song_line_count + 2;
  } else {
    lines_printed += song_line_count + 2;
  }
  if (song->time_signature && song->meter_change == 0) {
    fprintf(chord_out, "<h4%s>%s (%s %s)</h4>\n", class_attribute, song->title, key, song->time_signature);
  } else {
    fprintf(chord_out, "<h4%s>%s (%s)</h4>\n", class_attribute, song->title, key);
  }

  // Setup parts format structures to point into temporary line memory allocated
  // above
  struct MeasureFormat empty_measure;
  memset(&empty_measure, 0, sizeof(empty_measure));
  empty_measure.chords = "";
  next_line = 0;
  for (int i=0; i<max_part; i++) {
    int next_measure = 0;
    parts[i].lines[parts[i].line_count++] = lines[next_line++];
    for (int j=0; j<parts[i].measure_count; j++) {
      int next_column = next_measure % max_columns;
      if (next_measure > 0 && next_column == 0) {
	parts[i].lines[parts[i].line_count++] = lines[next_line++];
      }
      lines[next_line-1][next_column] = &parts[i].measures[next_measure];
      next_measure++;
    }
    int insert_newlines = newline_per_ending(&parts[i], max_columns);
    int first_ending_start_column = next_measure % max_columns;
    for (int j=0; j<parts[i].ending_count; j++) {
      if (j > 0 && insert_newlines) {
	parts[i].lines[parts[i].line_count++] = lines[next_line++];
	int ending_start_column = first_ending_start_column;
	// Prevent endings of differing lengths from wrapping
	if (first_ending_start_column + parts[i].ending_measure_count[j] > max_columns &&
	    parts[i].ending_measure_count[j] <= max_columns) {
	  ending_start_column = max_columns - parts[i].ending_measure_count[j];
	}
	for (int k=0; k<ending_start_column; k++) {
	  lines[next_line-1][k] = malloc(sizeof(struct MeasureFormat));
	  memset(lines[next_line-1][k], 0, sizeof(struct MeasureFormat));
	  lines[next_line-1][k]->chords = "";
	}
	next_measure = ending_start_column;
      }
      for (int k=0; k<parts[i].ending_measure_count[j]; k++) {
	int next_column = next_measure % max_columns;
	if (next_measure > 0 && next_column == 0 && insert_newlines == 0) {
	  parts[i].lines[parts[i].line_count++] = lines[next_line++];
	}
	lines[next_line-1][next_column] = &parts[i].ending[j][k];
	next_measure++;
      }
    }
  }

  // Set column "width" for each column.  The column width is defined as the
  // largest visible chord text width (number of chars) of all rows in the
  // column
  total_width = 0;
  for (int i=0; i<max_columns; i++) {
    int width = 0;
    for (int j=0; j<line_count; j++) {
      if (lines[j][i] && lines[j][i]->chords_len > width)
	width = lines[j][i]->chords_len;
    }
    for (int j=0; j<line_count; j++) {
      if (lines[j][i]) {
	lines[j][i]->width = width;
	if (i > 0)
	  lines[j][i]->leading_space = 1;
      }
    }
    total_width += width + 1;
  }

  // Compute extra (leading) space to add to each column.
  if (total_width < MAX_LINE_LENGTH) {
    int extra_space = (MAX_LINE_LENGTH - total_width) / (max_columns-1);
    if (extra_space > 0) {
      for (int i=1; i<max_columns; i++) {
	for (int j=0; j<line_count; j++) {
	  if (lines[j][i]) {
	    lines[j][i]->leading_space += extra_space;
	  }
	}
      }
      total_width += (max_columns-1) * extra_space;
    }
  }

  // If there are any extra spaces, distribute them to crowded adjacent columns
  // to improve readability
  if (total_width < MAX_LINE_LENGTH) {
    int extra_space = (MAX_LINE_LENGTH - total_width) % (max_columns-1);
    for (int i=1; i<max_columns && extra_space; i++) {
      for (int j=0; j<line_count && extra_space; j++) {
	if (lines[j][i] && lines[j][i]->leading_space == 1 &&
	    lines[j][i]->width == lines[j][i]->chords_len &&
	    lines[j][i-1]->width == lines[j][i-1]->chords_len) {
	  // Add extra space for all rows in this column
	  for (int k=0; k<line_count; k++) {
	    lines[k][i]->leading_space++;
	  }
	  extra_space--;
	}
      }
    }
  }

  for (int i=0; i<max_part; i++) {
    if (parts[i].part->name)
      fprintf(chord_out, "<span class=\"part-name\">%s</span>&nbsp;&nbsp;", parts[i].part->name);
    else
      fprintf(chord_out, "<span class=\"part-name\">&nbsp;</span>&nbsp;&nbsp;");
    if (parts[i].part->repeat) {
      fprintf(chord_out, "|:&nbsp;");
    } else {
      fprintf(chord_out, "&nbsp;&nbsp;&nbsp;");
    }
    int output_chars = 0;
    for (int j=0; j<parts[i].line_count; j++) {
      struct MeasureFormat *next_measure_fmt = parts[i].lines[j][0];
      if (j > 0 && next_measure_fmt != NULL &&
	  next_measure_fmt->chords != NULL) {
	if (next_measure_fmt->ending > 0) {
	  fprintf(chord_out, "<br/>\n&nbsp;&nbsp;&nbsp;");
	} else {
	  fprintf(chord_out, "<br/>\n<span class=\"part-name\">&nbsp;</span>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;");
	}
	output_chars = 0;
      }
      for (int k=0; k<max_columns; k++) {
	struct MeasureFormat *measure_fmt = parts[i].lines[j][k];
	if (measure_fmt == NULL || measure_fmt->chords == NULL) {
	  break;
	}
	output_chars += print_measure(measure_fmt);
      }
    }
    if (parts[i].ending_count == 0 && parts[i].part->repeat) {
      fprintf(chord_out, "&nbsp;:|");
    }
    fprintf(chord_out, "<br/>\n");
  }

  // Free temporary storage
  for (int i=0; i<line_count; i++) {
    free(lines[i]);
  }

}

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
      return strcmp(left->title, right->title);
#if 0
      if (left->mode < right->mode)
	return -1;
      else if (left->mode == right->mode) {
	if (left->minor < right->minor)
	  return -1;
	else if (left->minor == right->minor) {
	  return strcmp(left->title, right->title);
	}
      }
#endif
    }
  }
  return 1;
}

int compare_songs_by_key_signature(const void *lhs, const void *rhs) {
  struct CSong *left = *(struct CSong **)lhs;
  struct CSong *right = *(struct CSong **)rhs;
  if (left->key_signature < right->key_signature)
    return -1;
  else if (left->key_signature == right->key_signature) {
    if (left->accidental < right->accidental)
      return -1;
    else if (left->accidental == right->accidental) {
      return strcmp(left->title, right->title);
    }
  }
  return 1;
}

void print_index_key_heading(struct CSong *song, int break_before) {
  char key_buf[32];
  if (aux.flag & AUX_FLAG_CHORDS_INDEX_KEYSIGNATURE) {
    write_key_signature_to_string(song, key_buf);
  } else {
    write_key_to_string(song, key_buf, 0);
  }
  if (break_before)
    fprintf(chord_out, "<div class=\"page-break-before\">\n");
  fprintf(chord_out, "<h4 style=\"font-family: Arial\">%s</h4>\n", key_buf);
  if (break_before)
    fprintf(chord_out, "</div>\n");
}

void print_index(struct CSong **original_songs, int max_song) {
  struct CSong **songs = malloc(max_song * sizeof(struct CSong *));
  memcpy(songs, original_songs, max_song * sizeof(struct CSong *));
  if (aux.flag & AUX_FLAG_CHORDS_INDEX_KEYSIGNATURE) {
    qsort(songs, max_song, sizeof(struct CSong *), compare_songs_by_key_signature);
  } else {
    qsort(songs, max_song, sizeof(struct CSong *), compare_songs_by_key);
  }
  // unique the list
  int dst = 1;
  for (int i=1; i<max_song; i++) {
    if (strcmp(songs[dst-1]->title, songs[i]->title) == 0) {
      continue;
    }
    songs[dst++] = songs[i];
  }
  max_song = dst;
  int base = 0;
  if (aux.flag & AUX_FLAG_CHORDS_INDEX_KEYSIGNATURE) {
    fprintf(chord_out, "<h2 style=\"font-family: Arial\">Tunes by Key Signature</h2>\n");
  } else {
    fprintf(chord_out, "<h2 style=\"font-family: Arial\">Tunes by Key</h2>\n");
  }
  int index = 0;
  while (base < max_song) {
    index = base + 1;
    if (aux.flag & AUX_FLAG_CHORDS_INDEX_KEYSIGNATURE) {
      while (index < max_song && same_key_signature(songs[index-1], songs[index])) {
	index++;
      }
    } else {
      while (index < max_song && same_key(songs[index-1], songs[index])) {
	index++;
      }
    }
    print_index_key_heading(songs[base], songs[base]->key_signature == 'D' ||
			    songs[base]->key_signature == 'G');
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
      while (part->next != NULL && empty_part(part->next)) {
	part->next = part->next->next;
      }
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

  chord_out = fopen("chords.html", "w");

  fprintf(chord_out, "<!DOCTYPE html>\n<html>\n<head>\n");
  fprintf(chord_out, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
  fprintf(chord_out, "<style>\n");
  fprintf(chord_out, "* { box-sizing: border-box; }\n");
  fprintf(chord_out, ".fixed { font-family: Courier; font-size: 1.25em; white-space: nowrap; }\n");
  fprintf(chord_out, ".column { float: left; width: 33.33%%; padding: 10px; font-family: Arial}\n");
  fprintf(chord_out, ".row:after { content: \"\"; display: table;clear: both; }\n");
  fprintf(chord_out, ".page-break-before { page-break-before: always; }\n");
  fprintf(chord_out, ".part-name { display: inline-block; width: 15px; text-align: center; "
	  "font-family: \"Times New Roman\"; font-weight: bold; }\n");
  fprintf(chord_out, ".super { display: inline-block; width: 12px; text-align: center; vertical-align: top; font-size: 75%%;}\n");
  fprintf(chord_out, ".ending { display: inline-block; width: 24px; text-align: left; }\n");
  fprintf(chord_out, "</style>\n</head>\n<body>\n");

  fprintf(chord_out, "<div style=\"font-family: Arial\">\n");
  fprintf(chord_out, "<h1 style=\"text-align: center;\">%s</h1>\n", aux.title ? aux.title : "[INSERT TITLE HERE]");
  fprintf(chord_out, "<br/>\n<br/>\n");
  print_index(songs, max_song);
  fprintf(chord_out, "</div>\n");  

  fprintf(chord_out, "<h2 class=\"page-break-before\" style=\"font-family: Arial\">Alphabetical List of Tunes</h2>\n");
  fprintf(chord_out, "<div class=\"fixed\">\n");

  for (int index = 0; index < max_song; index++) {
    if (index > 0 && equal_songs(songs[index-1], songs[index]) != 0)
      continue;
    print_song(songs[index]);
    fprintf(chord_out, "<br/>\n");
  }
  fprintf(chord_out, "</div>\n");
  fprintf(chord_out, "</body>\n</html>\n");
  fclose(chord_out);
}

float notes_per_measure(struct CSong *song) {
  int total_measures = 0;
  int total_notes = 0;
  for (struct CPart *part = song->parts; part != NULL; part = part->next) {
    if (skip_part(part))
      continue;
    for (struct CMeasure *measure = part->measures; measure != NULL; measure = measure->next) {
      if (measure->leadin)
	continue;
      total_measures++;
      total_notes += measure->notes;
    }
    for (int i=0; i<part->next_ending; i++) {
      for (struct CMeasure *measure = part->endings[i]; measure != NULL; measure = measure->next) {
	if (measure->leadin)
	  continue;
	total_measures++;
	total_notes += measure->notes;
      }
    }
  }
  return (float)total_notes / (float)total_measures;
}

void escape_string(const char *str, char *dst) {
  const char *src = str;
  while (*src) {
    if (*src == '"') {
      *dst++ = '\\';
    }
    *dst++ = *src++;
  }
  *dst = '\0';
}

void generate_complexity_file() {
  int max_song;
  struct CSong **songs = dedup_songs(&max_song);
  char escaped_title[256];

  chord_out = fopen("complexity.csv", "w");
  fprintf(chord_out, "Key,\"Time\nSignature\",\"Notes Per\nMeasure\",\"Average\nLong Interval\",Title\n");
  for (int index = 0; index < max_song; index++) {
    if (index > 0 && equal_songs(songs[index-1], songs[index]) != 0)
      continue;
    float npm = notes_per_measure(songs[index]);
    escape_string(songs[index]->title, escaped_title);
    float avg_largest_interval = 0.0;
    for (int i=0; i<MAX_LONGEST_INTERVALS; i++) {
      avg_largest_interval += songs[index]->longest_intervals[i];
    }
    avg_largest_interval /= (float)MAX_LONGEST_INTERVALS;
    fprintf(chord_out, "%c,%s,%.2f,%.1f,\"%s\"\n", songs[index]->key,
	    songs[index]->time_signature, npm, avg_largest_interval, escaped_title);
  }

  fclose(chord_out);
}

char *strcpy_irealpro_escape(char *dst, const char *src) {
  while (*src) {
    if (*src == '=' || *src == '%' || *src == '&' || *src == '?' || *src == '#' || *src == '"' || *src == '*') {
      sprintf(dst, "%%%X", *src);
      dst += strlen(dst);
      src++;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';
  return dst;
}

char *strcpy_irealpro_chord(char *dst, const char *src) {
  while (*src) {
    if (*src == '(' || *src == '/') {
      break;
    } else if (*src == '=' || *src == '%' || *src == '&' || *src == '?' || *src == '#' || *src == '"' || *src == '*') {
      sprintf(dst, "%%%X", *src);
      dst += strlen(dst);
      src++;
    } else if (*src == 'm') {
      *dst++ = '-';
      src++;
    } else if (isspace(*src)) {
      src++;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';
  return dst;
}

char tsig_buf[4];
const char *irealpro_convert_timesignature(const char *time_signature) {
  char *dst = tsig_buf;
  int got_first_digit = 0;
  *dst++ = 'T';
  for (const char *src = time_signature; *src; src++) {
    if (isdigit(*src)) {
      *dst++ = *src;
      if (got_first_digit) break;
      got_first_digit = 1;
    }
  }
  *dst = '\0';
  if (strcmp(tsig_buf, "T12") == 0)
    strcpy(tsig_buf, "T24");
  return tsig_buf;
}

int irealpro_chords_equal(struct CChord *lhs, struct CChord *rhs) {
  const char *lptr = lhs->name;
  const char *rptr = rhs->name;
  while (*lptr && *rptr && *lptr == *rptr) {
    if (*lptr == '(')
      break;
    lptr++;
    rptr++;
  }
  if (*lptr == '\0' || *lptr == '(' || *lptr == '/')
    lptr = NULL;
  if (*rptr == '\0' || *rptr == '(' || *rptr == '(')
    rptr = NULL;
  if (lptr || rptr)
    return 0;
  return lhs->diminished == rhs->diminished;
}

#define MAX_DIVISIONS 64

void print_chord_map(struct CChord **chords, int max) {
  char buf[MAX_DIVISIONS+1];
  for (int i=0; i<max; i++) {
    if (chords[i])
      buf[i] = 'X';
    else
      buf[i] = '0';
  }
  buf[max] = '\0';
  LOG_MESSAGE("[macon] %s", buf);
}

void set_run_lengths(int run_length, int *min_run_length, int *max_run_length) {
  if (run_length == 0)
    return;
  if (run_length < *min_run_length)
    *min_run_length = run_length;
  if (run_length > *max_run_length)
    *max_run_length = run_length;
}

char *irealpro_write_measure(struct CSong *song, struct CMeasure *measure, struct CChord **previous_chord, char *dst) {
  struct CChord *chords[MAX_DIVISIONS];
  int unit = song->beat_duration / 4;
  int max = (measure->duration ? measure->duration : song->measure_duration) / unit;
  LOG_MESSAGE("[macon] unit=%d, beat_duration=%d, measure->duration=%d, measure->beats=%d, song->measure_duration=%d", unit, song->beat_duration, measure->duration, measure->beats, song->measure_duration);
  assert(song->beat_duration % 4 == 0);
  assert(song->measure_duration % unit == 0);

  assert(max % 2 == 0);
  assert(max <= MAX_DIVISIONS);

  memset(chords, 0, MAX_DIVISIONS*sizeof(struct CChord *));
  struct CChord *current_chord = NULL;
  int index = 0;
  for (struct CChord *chord = measure->chords; chord != NULL; chord = chord->next) {
    LOG_MESSAGE("[macon] '%s' chord->duration=%d, unit=%d, beat_duration=%d", chord->name, chord->duration, unit, song->beat_duration);
    assert(chord->duration % unit == 0);
    if (chord->name[0] == '(') {
      if (current_chord == NULL) {
	assert(*previous_chord);
	current_chord = *previous_chord;
	chords[index] = current_chord;
      }
    } else if (!current_chord || !irealpro_chords_equal(chord, current_chord)) {
      current_chord = chord;
      chords[index] = current_chord;
    }
    index += chord->duration / unit;
  }

  int chord_count = 0;
  int got_chord_count = 0;

  print_chord_map(chords, max);
  while (max > measure->beats) {
    int src_index = 0;
    int dst_index = 0;
    int run_length = 0;
    int max_run_length = 0;
    int min_run_length = max;
    while (src_index < max) {
      /*
      if (chords[src_index+1] != NULL) {
	LOG_MESSAGE("[weird] %s", song->title);
      }
      assert(chords[src_index+1] == NULL);*/
      if (chords[src_index]) {
	if (!got_chord_count)
	  chord_count++;
	chords[dst_index] = chords[src_index];
	set_run_lengths(run_length, &min_run_length, &max_run_length);
	run_length = 1;
      } else {
	chords[dst_index] = NULL;
	run_length++;
      }
      src_index += 2;
      dst_index++;
    }
    got_chord_count = 1;
    if (max % 2)
      max = (max / 2) + 1;
    else
      max /= 2;
    set_run_lengths(run_length, &min_run_length, &max_run_length);
    LOG_MESSAGE("[macon] chord_count=%d max=%d run_length=%d min_run_length=%d max_run_length=%d", chord_count, max, run_length, min_run_length, max_run_length);
    print_chord_map(chords, max);
    if (max_run_length <= 2 || min_run_length == 1)
      break;
  }

  int need_trailing_space = 1;
  for (int i=0; i<max; i++) {
    if (i>0) {
      if (chords[i] && chords[i-1])
	*dst++ = ',';
      else if (chords[i] == NULL) {
	*dst++ = ' ';
	need_trailing_space = 0;
      }
    }
    if (chords[i]) {
      LOG_MESSAGE("[macon] Copying chord %s", chords[i]->name);
      strcpy_irealpro_chord(dst, chords[i]->name);
      dst += strlen(dst);
      if (chords[i]->diminished) {
	*dst++ = 'o';
      }
      *previous_chord = chords[i];
      need_trailing_space = 1;
      if (chord_count == 1)
	break;
    }
  }
  if (chord_count == 1 && need_trailing_space)
    *dst++ = ' ';

  return dst;
}

char song_buf[TEXTBUF_SIZE];
const char *song_to_irealpro_format(struct CSong *song) {
  char *dst = song_buf;

  // Title
  char *title = dst;
  strcpy_irealpro_escape(dst, song->title);
  dst += strlen(dst);
  if (aux.song_title_suffix) {
    *dst++ = ' ';
    strcpy_irealpro_escape(dst, aux.song_title_suffix);
    dst += strlen(dst);
  }
  LOG_MESSAGE("%s", title);

  // Composer
  *dst++ = '=';
  if (song->composer && song->composer[0]) {
    strcpy_irealpro_escape(dst, song->composer);
  } else {
    strcpy(dst, "Unknown");
  }
  dst += strlen(dst);

  // Style
  if (song->time_signature && strcmp(song->time_signature, "3/4") == 0) {
    strcpy(dst, "=Waltz");
  } else if (song->time_signature && strcmp(song->time_signature, "6/8") == 0) {
    strcpy(dst, "=Jig");
  } else {
    strcpy(dst, "=Fiddle Tune");
  }
  dst += strlen(dst);

  // Key Signature
  *dst++ = '=';
  *dst++ = song->key_signature;
  if (song->accidental == -1)
    *dst++ = 'b';
  else if (song->accidental == -1) {
    strcpy_irealpro_escape(dst, "#");
    dst += strlen(dst);
  }

  // Unused
  strcpy(dst, "=n");
  dst += strlen(dst);

  // Chord Progression
  *dst++ = '=';

  // Time signature
  // todo turn this into a function
  if (!song->meter_change && song->time_signature) {
    strcpy(dst, irealpro_convert_timesignature(song->time_signature));
    dst += strlen(dst);
  }

  char part_name = 'A';
  char chord_buf[32];
  for (struct CPart *part = song->parts; part != NULL; part = part->next) {
    if (skip_part(part))
      continue;
    LOG_MESSAGE("Part %s", part->name);
    if (part->repeat) {
      *dst++ = '{';
    } else {
      *dst++ = '[';
    }
    if (part->name && part->name[0]) {
      *dst++ = '*';
      *dst++ = part_name++;
    }

    int measure_count = 0;
    struct CChord *previous_measure_chord = NULL;
    for (struct CMeasure *measure = part->measures; measure != NULL; measure = measure->next) {
      if (measure->leadin)
	continue;
      if (measure_count) {
	if (measure_count == 4) {
	  *dst++ = '|';
	}
	*dst++ = '|';
      }
      if (measure->time_signature) {
	LOG_MESSAGE("Distance = %ld", dst-song_buf);
	strcpy(dst, irealpro_convert_timesignature(measure->time_signature));
	dst += strlen(dst);
      }
      dst = irealpro_write_measure(song, measure, &previous_measure_chord, dst);
      measure_count++;
    }
    LOG_MESSAGE("Measure count = %d", measure_count);
    LOG_MESSAGE("Ending count = %d", part->next_ending);
    for (int i=0; i<part->next_ending; i++) {
      if (i == 0) {
	sprintf(dst, "|N%d", i+1);
      } else {
	sprintf(dst, "} |N%d", i+1);
      }
      dst += strlen(dst);
      int measure_count = 0;
      for (struct CMeasure *measure = part->endings[i]; measure != NULL; measure = measure->next) {
	if (measure->leadin)
	  continue;
	if (measure_count) {
	  *dst++ = '|';
	}
	if (measure->time_signature) {
	  strcpy(dst, irealpro_convert_timesignature(measure->time_signature));
	  dst += strlen(dst);
	}
	struct CChord *tmp_chord = previous_measure_chord;
	dst = irealpro_write_measure(song, measure, &tmp_chord, dst);
	measure_count++;
      }
    }
    if (part->next_ending == 0 && part->repeat) {
      strcpy(dst, "} ");
    } else if (part->next == 0) {
      strcpy(dst, "Z ");
    } else {
      strcpy(dst, "] ");
    }
    dst += strlen(dst);
  }
  *dst = '\0';
  assert(dst-song_buf < TEXTBUF_SIZE);
  return song_buf;
}

void generate_irealpro_file() {
  int max_song;
  struct CSong **songs = dedup_songs(&max_song);
  char escaped_title[256];

  chord_out = fopen("irealpro.html", "w");

  fprintf(chord_out, "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n");
  fprintf(chord_out, "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n");
  fprintf(chord_out, "<head>\n");
  fprintf(chord_out, "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n");
  fprintf(chord_out, "<meta name=\"viewport\" content=\"width=device-width, minimum-scale=1, maximum-scale=1\" />\n");
  fprintf(chord_out, "<title>iReal Pro</title>\n");
  fprintf(chord_out, "<style type=\"text/css\">\n");
  fprintf(chord_out, ".help {\n");
  fprintf(chord_out, "font-size: small;\n");
  fprintf(chord_out, "color: #999999;\n");
  fprintf(chord_out, "}\n");
  fprintf(chord_out, "</style>\n");
  fprintf(chord_out, "</head>\n");
  fprintf(chord_out, "<body style=\"color: rgb(230, 227, 218); background-color: rgb(27, 39, 48); font-family: Helvetica,Arial,sans-serif;\" alink=\"#b2e0ff\" link=\"#94d5ff\" vlink=\"#b2e0ff\">\n");
  fprintf(chord_out, "<br/><br/>\n");
  fprintf(chord_out, "<h3><a href=\"irealbook://");

  int first_song = 1;
  int song_count = 0;
  for (int index = 0; index < max_song; index++) {
    if (index > 0 && equal_songs(songs[index-1], songs[index]) != 0)
      continue;
    if (first_song) {
      fprintf(chord_out, "%s", song_to_irealpro_format(songs[index]));
      first_song = 0;
    } else {
      fprintf(chord_out, "=%s", song_to_irealpro_format(songs[index]));
    }
    song_count++;
  }

  const char *title = aux.title ? aux.title : (max_song == 1 ? songs[0]->title : "Dummy Title");
  fprintf(chord_out, "=%s\">%s</a>  (%d)<br /></h3><br />", title, title, song_count);
  //<p>Test Song 1<br/>Test Song 2<br/></p>

  fprintf(chord_out, "<br/>Made with iReal Pro \n");
  fprintf(chord_out, "<a href=\"http://www.irealpro.com\"><img src=\"http://www.irealb.com/forums/images/images/misc/ireal-pro-logo-50.png\" width=\"25\" height=\"25\" hspace=\"10\" alt=\"\"/></a>\n");
  fprintf(chord_out, "   <br/><br/><span class=\"help\">- iOS: tap Share/Export and choose Copy to iReal Pro.<br />- Mac: drag the .html file on the iReal Pro App icon in the dock.</span><br/>\n");
  fprintf(chord_out, "</body>\n");

  fclose(chord_out);
}
