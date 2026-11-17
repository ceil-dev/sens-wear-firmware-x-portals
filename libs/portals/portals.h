// Copyright (c) 2026 ceil.dev
// SPDX-License-Identifier: MIT

// Portals for the SensWear firmware, in one header and one source: the portals core, the env type `persistence`,
// the ether, and the Bluetooth LE ether with its Zephyr port. `make bundle` of ceil.dev's package senswear-portals
// joins this file from the files named below, each under a line that names it: it is not edited here.

// ════════════════════ portals.h ════════════════════

// Portals in C: the portals core.
//
// It is made for a small device:
// all of the protocol, with no heap, no clock and no threads. Every size is fixed at build time. A
// frame is read in place, where the medium put it, and is never turned into a tree of values.
//
// THE SCOPE
//
// The core is a scope: a table of named functions, `portal_api`, which holds nothing else. Each
// member takes its arguments and then the core, and reaches every other member through the
// core's table. The top of portals.c lists them all. A table holds functions only, so an app's
// table can be const, in flash, shared by every core.
//
// HOW IT IS DRIVEN
//
//   static const portal_api api = { .etherSend = radio_send,         // how a frame leaves
//                                   .etherListen = radio_listen,     // frames to this id are for me from now on
//                                   .etherUnlisten = radio_unlisten, // and no longer
//                                   .allow = allow,                  // may this caller use this action
//                                   .random = random32,              // for my ids, and the index an env counts from
//                                   PORTAL_CBOR,                     // the packager
//                                   PORTAL_OPERATIONS };             // the library's members
//   static portals_core core = { .api = &api };                      // all zero besides
//   portal_expose(&core, "sensor", description);
//
//   for (;;) {
//     if (radio_read(frame, &length, &link)) {                       // the ether: a whole frame arrived
//       portal_arrival arrival = portal_arrive(frame, length, link, 0);
//       portal_event event;
//       while (portal_next(&core, &arrival, &event)) handle(&event); // one thing at a time
//     }
//     portal_flush(&core);                                           // one frame per connection with something to say
//   }
//
// THE FRAMES
//
// `unpack` reads a frame that arrived as pieces: its head first, then one entry at a time. `receive`
// takes one piece. `pack` adds one piece to a frame that leaves. The packager in cbor.c writes the
// frames as CBOR and states that layout. Nothing else in the core knows it, so another packager can
// take its place with no change to the core.
//
// HOW IT BEHAVES
//
//   - Events are asked for one at a time, and each ask reads one more piece of the frame. There is
//     no list of events.
//   - The core keeps no table of calls that wait. A reply event carries the index of the call.
//     When a connection ends, the app gets one PORTAL_CLOSE, not a reply for each call that waited.
//   - What the app ends itself is not reported back to it: leaving, unexposing, and a call or a
//     reply of the app's that would need a number above PORTAL_TOP. portal_call returns 0
//     and portal_reply false for that one.
//   - Ended frames leave at once through `etherSend`. Connection frames leave at portal_flush, one
//     per connection.
//   - A frame carries as many unconfirmed entries as fit PORTAL_FRAME, oldest first.
//   - A call the core answers itself is not taken while its queue has no room for the answer.
//     It comes again, like any entry that was not confirmed. An answer that can never fit is failed.
//     An env takes a call for the app only while none of its own entries waits to be confirmed, so
//     an answer the app gives at once finds the queue empty. Until then that call comes
//     again too. A visitor takes a call of the env as it comes: if both sides waited for the other,
//     two calls that cross would wait for ever. An env does not call a reserved name on its visitor:
//     portal_call returns 0 for it, as the visitor's answer, unknown, could wait behind the visitor's
//     own calls for ever.
//   - The core has no clock: portal_repeat is the only time it knows. An env counts the calls of
//     portal_repeat since each visitor's last frame, and at PORTAL_QUIET of them the connection is
//     quiet. Being quiet does not end it, and what is not confirmed is still said again.
//   - When every place is taken, a new visitor gets the place of a hosted connection that has not
//     opened yet. When there is none, a quiet connection gives its place: it ends with gone, its
//     visitor gets the ended frame and the app PORTAL_CLOSE. When there is neither, the new visitor
//     is answered with the ended frame and failed, and so is a visitor whose id is longer than
//     PORTAL_ID.
//   - Who the medium vouches for is a number, `who`, and an env compares it as a number.
//   - A frame reaches the core only on a link of its own or addressed to an id it listens for: on
//     a medium shared with other ends, the ether lets through nothing else. So every frame
//     of a connection the core does not know is answered with the ended frame and gone, unless
//     it is a beginning.
//   - A message that is not text makes a reply unreadable, and an unreadable reply ends the
//     connection with failed. An unreadable message in the ended frame is left out.
//
// IN MEMORY
//
//   - A connection and an env are named by a pointer to their place. No place is NULL.
//   - Ids, names and messages are bytes, as they are in the frame: `portal_bytes`, or a C string
//     where the app gives one. A body is a packed value, in bytes.
//   - An id is measured in bytes, not in characters.
//   - The queue is bytes: each entry as it was packed, with its length in two bytes in front of it.
//   - The frame handed to `etherSend` is the core's own buffer, where its next frame is built:
//     the ether sends it or copies it before it returns.
//
// LEFT OUT
//
//   - The other side's frame limit. This side limits what it sends, not what it is sent.
//   - Most error messages. The code travels, the text mostly does not.
//   - Entering again by itself after a "gone".

#ifndef PORTALS_H
#define PORTALS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ───────────────────────────────── the limits ────────────────────────────────
// Override any of them with -D.

#ifndef PORTAL_ENVS
#define PORTAL_ENVS 2 // envs this side can expose
#endif
#ifndef PORTAL_CONNECTIONS
#define PORTAL_CONNECTIONS 4 // connections at once, hosted and entered together
#endif
#ifndef PORTAL_QUEUE
#define PORTAL_QUEUE 256 // bytes of entries not confirmed yet, per connection
#endif
#ifndef PORTAL_FRAME
#define PORTAL_FRAME 256 // the largest frame this side sends
#endif
#ifndef PORTAL_ID
#define PORTAL_ID 36 // the longest visitor id this side can hold
#endif
#ifndef PORTAL_QUIET
#define PORTAL_QUIET 8 // calls of portal_repeat without a frame from a visitor, after which its connection is quiet
#endif
#ifndef PORTAL_TOP
#define PORTAL_TOP 4294967295u // the largest index
#endif

// ───────────────────────────────── the state ─────────────────────────────────
// A field of a core, of an exposed env and of a connection is read and written only through
// these two. They are bound at build time and cost nothing.

#define getFromState(of, field) ((of)->field)
#define setInState(of, field, value) ((of)->field = (value))

typedef struct portal_api portal_api;
typedef struct portals_core portals_core;

// ───────────────────────────────── bytes ─────────────────────────────────────

// A piece of text or of bytes that lives somewhere else. `at` is NULL when there is none.
typedef struct {
  const uint8_t *at;
  size_t length;
} portal_bytes;

// Reads forward through bytes. After anything it cannot read, `at` is NULL and stays NULL.
typedef struct {
  const uint8_t *at, *end;
} portal_reader;

// Writes forward into bytes. It did not fit when `length` has passed `capacity`. A writer with no
// room writes nothing and only counts.
typedef struct {
  uint8_t *bytes;
  size_t capacity, length;
} portal_writer;

static inline portal_bytes portal_text(const char *text) {
  portal_bytes bytes = { (const uint8_t *)text, strlen(text) };
  return bytes;
}
static inline bool portal_is(portal_bytes text, const char *word) {
  return text.at && text.length == strlen(word) && memcmp(text.at, word, text.length) == 0;
}
static inline portal_reader portal_reader_of(portal_bytes bytes) {
  portal_reader reader = { bytes.at, bytes.at ? bytes.at + bytes.length : NULL };
  return reader;
}

// ───────────────────────────────── values ────────────────────────────────────
// The few pieces of CBOR the frames are made of, from the packager in cbor.c. They are public so
// that an action can read and write simple payloads with them.

enum { PORTAL_UINT = 0, PORTAL_BYTES = 2, PORTAL_TEXT = 3, PORTAL_LIST = 4, PORTAL_MAP = 5 };

// The number at the head of the next item: a whole number itself, the length of a text or of
// bytes, how many items a list has, how many pairs a map has.
uint32_t portal_read(portal_reader *reader, uint8_t type);
// The next item, a text or a byte string.
portal_bytes portal_read_bytes(portal_reader *reader, uint8_t type);
void portal_write(portal_writer *writer, uint8_t type, uint32_t number);
void portal_write_bytes(portal_writer *writer, uint8_t type, portal_bytes bytes);

// ───────────────────────────────── what happens ──────────────────────────────

// How a call ends, and why a connection is over. `gone` and `timeout` mean the result is unknown.
typedef enum { PORTAL_OK, PORTAL_DENIED, PORTAL_UNKNOWN, PORTAL_FAILED, PORTAL_GONE, PORTAL_TIMEOUT } portal_code;

// Which call. It holds nothing that points into a frame, so it may be kept for a later reply.
typedef struct {
  uint32_t connection; // what portal_enter returned, or what PORTAL_OPEN named
  uint32_t index;      // the call's index in that connection
  portal_bytes env;    // the exposed env it is about: mine, or the one I entered
} portal_ref;

typedef enum {
  PORTAL_CALL = 1, // run `method` with the payload in `body`, then hand `ref` to portal_reply
  PORTAL_REPLY,    // a call of mine ended: `ref.index` is what portal_call returned
  PORTAL_OPEN,     // a visitor confirmed my reply to its `@enter`, or my portal_enter was let in
  PORTAL_CLOSE,    // a connection that opened is over, or my portal_enter ended before it opened
} portal_event_type;

// One thing that happened. `method` and `body` point into the frame: read them before the next one.
typedef struct {
  portal_event_type type; // 0: nothing happened
  portal_ref ref;
  uint32_t who;        // who the medium vouches for
  portal_bytes method; // CALL
  portal_code code;    // REPLY, CLOSE
  portal_bytes body;   // CALL, and REPLY with code 0: a payload. Otherwise a message
} portal_event;

// ───────────────────────────────── pieces ────────────────────────────────────
// A frame, a part at a time: what `unpack` reads and `pack` writes. What a piece names points into
// the frame it was read from, or at what is to be packed.

typedef enum {
  PORTAL_HEAD = 1, // a connection frame up to its entries: from, to, sync, and how many entries `count`
  PORTAL_ENDED,    // the connection is over: from, to, code, and a message in `body`
  PORTAL_CALLS,    // an entry that calls: index, name, and a payload in `body`
  PORTAL_REPLIES,  // an entry that replies: index, reply, code, and in `body` a payload for code 0, a message otherwise
  PORTAL_PACKED,   // packed only: an entry of mine packed already, in `body`, after `count` others of its frame
  PORTAL_FOOT,     // packed only: what ends a connection frame
} portal_piece_kind;

typedef struct {
  uint8_t kind;           // portal_piece_kind. 0: nothing read yet
  bool whole;             // unpacked: every part it names could be read. What that means is the core's to decide
  uint32_t link, who;     // where the frame came from, and who the medium vouches for
  uint32_t sync, count;   // the frame's: an entry carries its frame's sync
  uint32_t index, reply, code; // an entry's numbers. A code is as it was read
  portal_bytes from, to;  // the frame's ids: an entry carries them too
  portal_bytes name, body;
} portal_piece;

// ───────────────────────────────── the core ──────────────────────────────────

typedef struct {
  uint32_t handle;        // 0: this place is free. The app's name for the connection
  uint16_t quiet;         // an env's count of portal_repeat since the visitor's last frame, up to PORTAL_QUIET
  portal_bytes env;      // the exposed end's fixed id: mine when hosted, theirs when I entered
  uint32_t link, who;     // the sender: where their latest frame came from, who the medium vouches for
  uint32_t first;         // the index I started counting from: random when hosted, 1 for a visitor
  uint32_t next, sync;    // my next index, and how far I have taken theirs
  uint16_t count, length; // my entries not confirmed yet: how many, and their bytes in `queue`
  bool hosted, opened, dirty;
  uint8_t id_length;
  char id[PORTAL_ID];     // the visitor's id, made by the visitor
  uint8_t queue[PORTAL_QUEUE];
} portal_connection;

typedef struct {
  const char *id;
  portal_bytes description;
} portal_env;

// The scope. Every member takes the core last. portals.c says what each does.
struct portal_api {
  // Given by the app.
  // The frame is the core's own buffer, where its next frame is built: send it or copy it before returning.
  void (*etherSend)(uint32_t link, const uint8_t *frame, size_t length, portals_core *m);
  void (*etherListen)(portal_bytes id, uint32_t link, portals_core *m);   // link 0: every link
  void (*etherUnlisten)(portal_bytes id, uint32_t link, portals_core *m);
  bool (*allow)(const portal_event *call, portals_core *m); // asked for every call into an exposed env, `@enter` first
  uint32_t (*random)(portals_core *m);                      // 32 bits nobody can guess

  // The packager.
  bool (*unpack)(portal_reader *from, portal_piece *piece, portals_core *m);     // the next piece. False: nothing more
  bool (*pack)(portal_writer *into, const portal_piece *piece, portals_core *m); // adds one piece. False: it does not fit

  // Asked from outside.
  bool (*expose)(const char *id, portal_bytes description, portals_core *m);
  void (*unexpose)(const char *id, portals_core *m);
  bool (*receive)(const portal_piece *piece, portal_event *event, portals_core *m);
  uint32_t (*enter)(const char *env, uint32_t link, portals_core *m);
  uint32_t (*call)(uint32_t connection, const char *method, portal_bytes payload, portals_core *m);
  bool (*reply)(const portal_ref *ref, portal_code code, portal_bytes body, portals_core *m);
  void (*leave)(uint32_t connection, portals_core *m);
  void (*flush)(portals_core *m);
  void (*repeat)(portals_core *m);

  // Inner steps.
  portal_connection *(*begin)(portal_bytes env, bool hosted, uint32_t link, uint32_t who, portals_core *m);
  void (*end)(portal_connection *c, portal_event *event, portal_code code, portal_bytes message, bool answer, portals_core *m);
  bool (*push)(portal_connection *c, portal_event *event, portal_bytes method, uint32_t reply, portal_code code, portal_bytes body,
               portals_core *m);
  bool (*decide)(const portal_connection *c, portal_event *call, portals_core *m);
  bool (*start)(portal_connection *c, const portal_piece *head, portal_event *event, portals_core *m);
  bool (*take)(portal_connection *c, const portal_piece *entry, portal_event *event, portals_core *m);
  bool (*confirm)(portal_connection *c, uint32_t sync, portals_core *m);
  void (*say)(portal_connection *c, portals_core *m);
  void (*over)(uint32_t link, portal_bytes from, portal_bytes to, portal_code code, portal_bytes message, portals_core *m);
};

struct portals_core {
  const portal_api *api; // the scope. Set by the app before use
  void *context;         // the app's, for its own members

  // The core's own. All zero to start with.
  portal_env envs[PORTAL_ENVS];
  portal_connection connections[PORTAL_CONNECTIONS];
  uint32_t serial;
  uint8_t out[PORTAL_FRAME];
};

// A frame that arrived, while it is read: what is left of it, and the piece read last. An entry is
// read over the head, so it carries the head's ids and sync. The app keeps it while it handles the
// frame.
typedef struct {
  portal_reader frame;
  portal_piece piece;
} portal_arrival;

// ───────────────────────────────── the defaults ──────────────────────────────
// The library's own members, and the binary packager's. A table starts from these.

bool portal_op_expose(const char *id, portal_bytes description, portals_core *m);
void portal_op_unexpose(const char *id, portals_core *m);
bool portal_op_receive(const portal_piece *piece, portal_event *event, portals_core *m);
uint32_t portal_op_enter(const char *env, uint32_t link, portals_core *m);
uint32_t portal_op_call(uint32_t connection, const char *method, portal_bytes payload, portals_core *m);
bool portal_op_reply(const portal_ref *ref, portal_code code, portal_bytes body, portals_core *m);
void portal_op_leave(uint32_t connection, portals_core *m);
void portal_op_flush(portals_core *m);
void portal_op_repeat(portals_core *m);
portal_connection *portal_op_begin(portal_bytes env, bool hosted, uint32_t link, uint32_t who, portals_core *m);
void portal_op_end(portal_connection *c, portal_event *event, portal_code code, portal_bytes message, bool answer, portals_core *m);
bool portal_op_push(portal_connection *c, portal_event *event, portal_bytes method, uint32_t reply, portal_code code, portal_bytes body,
                    portals_core *m);
bool portal_op_decide(const portal_connection *c, portal_event *call, portals_core *m);
bool portal_op_start(portal_connection *c, const portal_piece *head, portal_event *event, portals_core *m);
bool portal_op_take(portal_connection *c, const portal_piece *entry, portal_event *event, portals_core *m);
bool portal_op_confirm(portal_connection *c, uint32_t sync, portals_core *m);
void portal_op_say(portal_connection *c, portals_core *m);
void portal_op_over(uint32_t link, portal_bytes from, portal_bytes to, portal_code code, portal_bytes message, portals_core *m);

bool portal_cbor_unpack(portal_reader *from, portal_piece *piece, portals_core *m);
bool portal_cbor_pack(portal_writer *into, const portal_piece *piece, portals_core *m);

#define PORTAL_OPERATIONS                                                                                                        \
  .expose = portal_op_expose, .unexpose = portal_op_unexpose, .receive = portal_op_receive, .enter = portal_op_enter,            \
  .call = portal_op_call, .reply = portal_op_reply, .leave = portal_op_leave, .flush = portal_op_flush,                          \
  .repeat = portal_op_repeat, .begin = portal_op_begin, .end = portal_op_end, .push = portal_op_push, .decide = portal_op_decide, \
  .start = portal_op_start, .take = portal_op_take, .confirm = portal_op_confirm, .say = portal_op_say, .over = portal_op_over

#define PORTAL_CBOR .unpack = portal_cbor_unpack, .pack = portal_cbor_pack

// ───────────────────────────────── the app's calls ───────────────────────────
// Each calls its member through the core's table.
//
// Ids and action names are not copied: a text given here has to stay where it is while it is in use.
// One caller at a time: nothing here is safe to call from two threads, or from an interrupt.

// True when every member of a table is set. A member left out is NULL: the app builds and runs, and
// the core jumps to address 0 when it reaches that member. Call it once at start.
static inline bool portal_complete(const portal_api *api) {
  static const uint8_t unset[sizeof(void (*)(void))] = { 0 };
  for (size_t at = 0; at < sizeof *api; at += sizeof unset)
    if (!memcmp((const uint8_t *)api + at, unset, sizeof unset)) return false;
  return true;
}

// Makes an env of mine enterable. `description` is the packed value `@describe` answers with. False:
// the id is not 1 to 36 bytes, or every place for an env is taken.
static inline bool portal_expose(portals_core *m, const char *id, portal_bytes description) {
  return m->api->expose(id, description, m);
}
// Takes it back. Its visitors get the ended frame.
static inline void portal_unexpose(portals_core *m, const char *id) {
  m->api->unexpose(id, m);
}
// Begins a connection with an exposed env: makes my id and queues the call `@enter`. The
// connection is usable at once: calls made before it opens wait behind `@enter`. PORTAL_OPEN or
// PORTAL_CLOSE follows. 0: no free place, or the env's id is not 1 to 36 bytes or fits no frame.
static inline uint32_t portal_enter(portals_core *m, const char *env, uint32_t link) {
  return m->api->enter(env, link, m);
}
// Queues a call. Returns its index, which the PORTAL_REPLY will carry. 0: it could not be queued, the
// connection is over, or an env named a reserved name for its visitor.
static inline uint32_t portal_call(portals_core *m, uint32_t connection, const char *method, portal_bytes payload) {
  return m->api->call(connection, method, payload, m);
}
// Ends a call that came as PORTAL_CALL, now or later. False: no room for the reply at the moment, or
// the connection is over.
static inline bool portal_reply(portals_core *m, const portal_ref *ref, portal_code code, portal_bytes body) {
  return m->api->reply(ref, code, body, m);
}
// Ends a connection from this side: the ended frame with gone. Before PORTAL_OPEN it gives up.
static inline void portal_leave(portals_core *m, uint32_t connection) {
  m->api->leave(connection, m);
}
// Sends one frame for each connection that has something to say. Call it once per turn.
static inline void portal_flush(portals_core *m) {
  m->api->flush(m);
}
// Marks everything not confirmed to be said again at the next flush, a waiting `@enter` included.
// It is also the core's only clock: an env counts its calls since each visitor's last frame, and at
// PORTAL_QUIET of them that connection is quiet: it gives its place to a new visitor when every place
// is taken. Call it from a timer, well above the round trip, on a medium that repeats by itself too.
static inline void portal_repeat(portals_core *m) {
  m->api->repeat(m);
}

// A whole frame arrived. It is read by portal_next, and has to stay where it is until then.
portal_arrival portal_arrive(const uint8_t *frame, size_t length, uint32_t link, uint32_t who);
// What the frame comes to, one thing at a time. False when it has nothing more.
bool portal_next(portals_core *m, portal_arrival *arrival, portal_event *event);

#ifdef __cplusplus
}
#endif

#endif

// ════════════════════ persistence.h ════════════════════

// The env type `persistence`. An env of this type holds its properties by name: a property is a value,
// a function, or has properties of its own. Each is reached by its path, a list of names from the env
// down, as CBOR: `["limits", "high"]`, and `[]` for the env itself. The four actions of the type are
// four functions here:
//
//   persistence_schema                           what the env holds, as one node: { type, children: [{ key, type, meta }] }
//   persistence_get    ["limits", "high"]        the value there. For what has properties of its own, a map of
//                                                their values by name, and so on down. A function has no value
//   persistence_set    ["limits", "high"], 28    writes one value
//   persistence_call   ["nudge"], 2              runs a function with a payload, or with none, and writes what it gives
//
// Each answers PORTAL_OK or an error: PORTAL_UNKNOWN when nothing is at the path or the path is not a list
// of texts, PORTAL_DENIED when what is there cannot be read, written or called, PORTAL_FAILED when a value
// is not taken or a function failed. An answer goes into the caller's writer, which shows one that did not
// fit as it always does: `length` past `capacity`.
//
// The app's functions take and return plain C values, and the library reads and writes the CBOR. A number
// is a whole number, an int32_t: a number that comes in outside its range is not taken. A text is a C
// string when it goes out, and its bytes with their length when it comes in. A boolean is true or false.
//
// The env needs nothing of the portals core but its reader and writer: build it with the core's cbor.c.
// portal-persistence.h connects it to the core.
#ifndef PERSISTENCE_H
#define PERSISTENCE_H


#ifdef __cplusplus
extern "C" {
#endif

#ifndef PERSISTENCE_INPUTS
#define PERSISTENCE_INPUTS 8 // the most children an `input` of type "object" has
#endif

// A value that a function takes or gives. The field of its type is the one that counts. A text that came
// from the other side is `length` bytes at `string`, with no zero after them. A text that a function gives
// is a C string.
typedef struct {
  int32_t number;
  bool boolean;
  const char *string;
  size_t length;
} persistence_value;

// One property. A value has the reader of its type, which returns it, and the writer of the same type
// when it can be set, which takes the new value and returns false to refuse it. A function has `call`.
// What has properties of its own has `children`.
//
// A row's type follows from what it has: the type of its reader, "function" with `call`, else what
// `type` says, else "object". So `type` is written only in the rows that describe what a function takes
// and what it gives.
//
// What a function takes is read from the payload as its `input` says:
//   - no `input`: `takes` is NULL, and a payload that came is not looked at
//   - of type number, string or boolean: one value, `takes[0]`, and the payload is exactly one value of
//     that type
//   - of type object: one value for each of its children, in their order, and the payload is a map that
//     has each of their names once and nothing else, each with a value of its child's type: number,
//     string or boolean. PERSISTENCE_INPUTS children at most
// A payload that does not fit is answered PORTAL_FAILED with the text "the payload is not what it
// takes", and the function is not run.
typedef struct persistence_property persistence_property;
struct persistence_property {
  const char *key;                                       // its name among its siblings
  const char *type;                                      // for a row with no reader and no `call`. NULL: "object"
  const char *unit;                                      // NULL: none
  const char *description;                               // NULL: none
  int32_t (*read_number)(void);
  const char *(*read_string)(void);                      // a C string. NULL reads as an empty text
  bool (*read_boolean)(void);
  bool (*write_number)(int32_t number);
  bool (*write_string)(const char *text, size_t length); // the text's bytes as they came, with no zero after them
  bool (*write_boolean)(bool boolean);
  // Runs the function. `gives` comes zeroed. True: the field of the output's type is what it gives, or
  // nothing is given without an `output`. False: it failed, and a `string` it set in `gives` says why.
  bool (*call)(const persistence_value *takes, persistence_value *gives);
  const persistence_property *input;                     // with a function: what it takes
  const persistence_property *output;                    // with a function: what it gives
  const persistence_property *children;                  // with an object: its properties, in order
  size_t count;
};

// The env: its properties, in order.
typedef struct {
  const persistence_property *children;
  size_t count;
} persistence_env;

// What the env holds, as one node of the schema.
portal_code persistence_schema(const persistence_env *env, portal_writer *into);
// The value at `path`. For what has properties of its own, a map of their values by name, and so on down.
portal_code persistence_get(const persistence_env *env, portal_bytes path, portal_writer *into);
// Writes one value.
portal_code persistence_set(const persistence_env *env, portal_bytes path, portal_bytes value);
// Runs a function with `payload`, whose `at` is NULL when there is none, and writes what it gives, or nothing.
portal_code persistence_call(const persistence_env *env, portal_bytes path, portal_bytes payload, portal_writer *into);

#ifdef __cplusplus
}
#endif

#endif

// ════════════════════ portal-persistence.h ════════════════════

// An env of type `persistence` (persistence.h) through the portals core. Its four actions come as calls,
// each answered by the function of persistence.h of the same name:
//
//   schema                           the payload is not read
//   get    ["limits", "high"]        a path
//   set    [["limits", "high"], 28]  a path and a value. The reply has nothing
//   call   [["nudge"], 2]            a path, and a payload or none
//
// A payload of another form is answered `unknown`, and an answer that does not fit PORTAL_PERSISTENCE_ROOM
// or a frame is answered `failed`.
//
// The core does not know this library: the app hands a call to it from its own handler.
#ifndef PORTAL_PERSISTENCE_H
#define PORTAL_PERSISTENCE_H


#ifdef __cplusplus
extern "C" {
#endif

#ifndef PORTAL_PERSISTENCE_ROOM
#define PORTAL_PERSISTENCE_ROOM 256 // bytes for the largest answer: a value, the schema, or what a function gives
#endif

// A call into the env that came through the core. True: it was `schema`, `get`, `set` or `call`, and it is
// replied to. False: not one of the four, and nothing was done, so the app goes on with its own actions.
bool portal_persistence_call(portals_core *m, const persistence_env *env, const portal_event *call);

// Exposes the env through the core with what `@describe` answers for it, packed into `room`:
// { "id": id, "description": description, "type": "persistence", "actions": {} }. The room has to stay
// while the env is exposed. False: the room is too small, or portal_expose answered false.
bool portal_persistence_expose(portals_core *m, const char *id, const char *description, uint8_t *room, size_t size);

#ifdef __cplusplus
}
#endif

#endif

// ════════════════════ ether.h ════════════════════

// An ether in C: what carries the frames of the portals core over a medium. Every ether has the same four
// functions, whatever its medium, and the core reaches them through the three ether members of its table.
// Everything an ether hands on happens inside its `poll`, in the thread of the loop that calls it.
#ifndef ETHER_H
#define ETHER_H


#ifdef __cplusplus
extern "C" {
#endif

typedef struct ether ether;

// An ether: it knows its medium and carries frames. Four functions, whatever the medium.
struct ether {
  // Frames addressed to this id are wanted from now on, on this link (0: every link). The first one starts
  // the ether.
  void (*listen)(ether *e, portal_bytes id, uint32_t link);
  // And no longer. After the last one the ether lets its medium go.
  void (*unlisten)(ether *e, portal_bytes id, uint32_t link);
  // Carries one frame out on a link. The frame is the caller's own buffer: sent, or copied, before it returns.
  void (*send)(ether *e, uint32_t link, const uint8_t *frame, size_t length);
  // Looks at the medium, waiting at most `wait_ms`, and hands on what arrived, with ether_receive and
  // ether_ended. While nothing is listened for it hands on nothing. True: something arrived.
  bool (*poll)(ether *e, int32_t wait_ms);

  // Set by ether_plug.
  portals_core *core;                                         // the core it carries frames for
  void (*handle)(const portal_event *event, portals_core *m); // the app's: one thing that happened
};

// Plugs an ether into a core, before the core is used: the ether hands what arrives to this core, each event
// goes to `handle`, and the core's `context` is the ether from now on.
void ether_plug(ether *e, portals_core *core, void (*handle)(const portal_event *event, portals_core *m));

// For the core's table: its three ether members, which reach the ether through the core's `context`.
void ether_send(uint32_t link, const uint8_t *frame, size_t length, portals_core *m);
void ether_listen(portal_bytes id, uint32_t link, portals_core *m);
void ether_unlisten(portal_bytes id, uint32_t link, portals_core *m);
#define ETHER_MEMBERS .etherSend = ether_send, .etherListen = ether_listen, .etherUnlisten = ether_unlisten

// What an ether calls from its `poll`. Neither sends anything: the loop's portal_flush does.

// A whole frame arrived: it goes through the core, and each event to `handle`. `who` is who the medium vouches
// for, or 0.
void ether_receive(ether *e, uint32_t link, uint32_t who, const uint8_t *frame, size_t length);
// A link is over: every connection the core holds on it ends with `gone` and the message `why`, nothing is
// sent for it, and each close goes to `handle`.
void ether_ended(ether *e, uint32_t link, const char *why);

#ifdef __cplusplus
}
#endif

#endif

// ════════════════════ ether-ble.h ════════════════════

// The Bluetooth LE ether of the portals core: an ether of ether.h, for any Bluetooth stack. The wire is below:
// the service, and how a frame travels as packets.
//
// No Bluetooth stack is in here. A stack gives three functions: `room`, `send` and `receive`.
#ifndef ETHER_BLE_H
#define ETHER_BLE_H


#ifdef __cplusplus
extern "C" {
#endif

// ───────────────────────────────── the wire ──────────────────────────────────

#define ETHER_BLE_SERVICE "7cbf0001-c9bd-463f-8789-e53d718f0ac1" // the primary service
#define ETHER_BLE_INPUT "7cbf0002-c9bd-463f-8789-e53d718f0ac1"   // the visitor writes packets to it
#define ETHER_BLE_OUTPUT "7cbf0003-c9bd-463f-8789-e53d718f0ac1"  // the device notifies packets from it

// The same three as the 16 bytes Bluetooth sends, the text's last byte first, as a list for an initializer.
#define ETHER_BLE_SERVICE_BYTES 0xc1, 0x0a, 0x8f, 0x71, 0x3d, 0xe5, 0x89, 0x87, 0x3f, 0x46, 0xbd, 0xc9, 0x01, 0x00, 0xbf, 0x7c
#define ETHER_BLE_INPUT_BYTES 0xc1, 0x0a, 0x8f, 0x71, 0x3d, 0xe5, 0x89, 0x87, 0x3f, 0x46, 0xbd, 0xc9, 0x02, 0x00, 0xbf, 0x7c
#define ETHER_BLE_OUTPUT_BYTES 0xc1, 0x0a, 0x8f, 0x71, 0x3d, 0xe5, 0x89, 0x87, 0x3f, 0x46, 0xbd, 0xc9, 0x03, 0x00, 0xbf, 0x7c

#define ETHER_BLE_LAST 0x80u  // in a packet's first byte: the frame ends with this packet
#define ETHER_BLE_PLACE 0x7Fu // the rest of that byte: the packet's place in its frame, from 0
#define ETHER_BLE_PACKETS 128 // the most packets a frame takes

// ───────────────────────────────── the limits ────────────────────────────────
// Override any of them with -D.

#ifndef ETHER_BLE_LINKS
#define ETHER_BLE_LINKS 1 // Bluetooth connections served at once
#endif
#ifndef ETHER_BLE_FRAME
#define ETHER_BLE_FRAME 512 // the largest frame taken from the other side
#endif
#ifndef ETHER_BLE_PACKET
#define ETHER_BLE_PACKET 244 // the largest packet sent
#endif

// ───────────────────────────────── packets ───────────────────────────────────
// Cutting a frame, and joining one. They touch only what they are handed.

typedef struct {
  size_t at;     // the bytes of the frame that have left
  uint8_t place; // the place of the next packet
} ether_ble_cut; // all zero before a frame's first packet

// Writes the frame's next packet, of at most `room` bytes. Returns its size. 0: the frame has left whole,
// `room` is under 2, or the frame needs more than 128 packets.
size_t ether_ble_next(ether_ble_cut *cut, const uint8_t *frame, size_t length, uint8_t *packet, size_t room);

typedef struct {
  uint8_t *frame;          // where the frame is joined
  size_t capacity, length; // the largest frame taken, and the bytes joined
  uint8_t place;           // the place that is due
  bool open;               // a frame was begun and has not ended
} ether_ble_joined; // `frame` and `capacity` are set before use, and the rest is zero

// Takes one packet. True: it ended a frame, which is whole in `frame`, `length` bytes, until the next packet.
bool ether_ble_take(ether_ble_joined *joined, const uint8_t *packet, size_t size);

// ───────────────────────────────── the ether ─────────────────────────────────

typedef struct ether_ble ether_ble;

// What a Bluetooth stack gives. All three are called only from inside the ether's four functions. A link is
// the stack's number for one Bluetooth connection, and is never 0.
typedef struct {
  // The most bytes one packet may have on this link now: the ATT MTU less 3. 0: the link is not there.
  size_t (*room)(ether_ble *ble, uint32_t link);
  // Sends one packet on the link: a device notifies it from `output`, a visitor writes it to `input` without
  // response. False: it was not sent.
  bool (*send)(ether_ble *ble, uint32_t link, const uint8_t *packet, size_t size);
  // The next thing that arrived, waiting at most `wait_ms` for it: a packet on a link, or, with size 0, the
  // end of a link. The packet stays where it is until the next call. False: nothing came in that time.
  bool (*receive)(ether_ble *ble, uint32_t *link, const uint8_t **packet, size_t *size, int32_t wait_ms);
} ether_ble_stack;

struct ether_ble {
  ether ether;                  // first: a BLE ether is an ether
  const ether_ble_stack *stack; // set before use
  void *context;                // the stack's own

  // The ether's own, all zero to start with.
  uint32_t listened;                        // the ids listened for
  uint32_t links[ETHER_BLE_LINKS];          // the link that holds each place. 0: the place is free
  ether_ble_joined joined[ETHER_BLE_LINKS]; // the frame being joined in each place
  uint8_t frames[ETHER_BLE_LINKS][ETHER_BLE_FRAME];
  uint8_t packet[ETHER_BLE_PACKET]; // the packet being sent
};

// The four functions of the ether.
//
// Listening is counted: a Bluetooth connection carries frames for this end alone, so an id and a link are not
// kept. When the count comes back to 0, every place is free again, and what was half joined there is forgotten.
void ether_ble_listen(ether *e, portal_bytes id, uint32_t link);
void ether_ble_unlisten(ether *e, portal_bytes id, uint32_t link);
// Asks `room` once, and cuts the frame into packets of that size, at most ETHER_BLE_PACKET. Nothing is sent when
// the room is under 2, or when the frame would take more than 128 packets. When `send` answers false, the rest of
// the frame is given up: the core says it again later.
void ether_ble_send(ether *e, uint32_t link, const uint8_t *frame, size_t length);
// Asks the stack's `receive` once. A packet joins its link's frame: a link the ether does not know takes a free
// place, and with none free the packet is dropped. A frame that became whole goes to ether_receive. An end frees
// its link's place and goes to ether_ended. While nothing is listened for, what arrives is dropped, and it
// answers false. True: a packet or an end came.
bool ether_ble_poll(ether *e, int32_t wait_ms);

// A BLE ether over a stack, as an initializer: the four functions and the stack.
#define ETHER_BLE(stack_) { .ether = { ether_ble_listen, ether_ble_unlisten, ether_ble_send, ether_ble_poll, NULL, NULL }, .stack = (stack_) }

#ifdef __cplusplus
}
#endif

#endif

// ════════════════════ ether-ble-zephyr.h ════════════════════

// The Bluetooth LE ether on Zephyr: the stack under ether-ble.h on Zephyr's own Bluetooth, for a device that
// visitors connect to, and the service that ether-ble.h names.
#ifndef ETHER_BLE_ZEPHYR_H
#define ETHER_BLE_ZEPHYR_H


#ifdef __cplusplus
extern "C" {
#endif

#ifndef ETHER_BLE_WAITING
#define ETHER_BLE_WAITING 8 // packets that wait for the core's thread
#endif

// The Bluetooth LE ether of this firmware, to plug into its core.
ether *ether_ble_zephyr(void);
// For the core's table: 32 bits from Bluetooth's own source, or the clock's count when that fails.
uint32_t ether_ble_zephyr_random(portals_core *m);

#ifdef __cplusplus
}
#endif

#endif
