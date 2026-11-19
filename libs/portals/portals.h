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
// member but `random` takes its arguments and then the core, and reaches every other member through
// the core's table. The top of portals.c lists them all. A table holds functions only, so a program's
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
//   static const portal_action actions[] = { { "read", read_celsius } }; // the env's actions, by name
//   static portals_core core;                                        // all zero, in RAM only
//
//   core.api = &api;                                                 // first: an initialiser would keep the whole core in flash
//   portal_expose(&core, "sensor", "a sensor", PORTAL_ACTIONS(actions), NULL); // the core runs its actions
//
//   for (;;) {
//     if (radio_read(frame, &length, &link)) {                       // the ether: a whole frame arrived
//       portal_arrival arrival = portal_arrive(link, 0, frame, length);
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
//     When a connection ends, the program gets one PORTAL_CLOSE, not a reply for each call that waited.
//   - What the program ends itself is not reported back to it: leaving, unexposing, and a call or a
//     reply of the program's that would need a number above PORTAL_MAX_INDEX. portal_call returns 0,
//     and portal_reply and portal_fail false, for that one.
//   - Ended frames leave at once through `etherSend`. Connection frames leave at portal_flush, one
//     per connection.
//   - A frame carries as many unconfirmed entries as fit PORTAL_FRAME_SIZE, oldest first.
//   - An env answers a call itself with the action the call names: the core runs it and replies with
//     what it gives, and the program is told nothing of that call. A name the env has no action for is
//     unknown. An env exposed with no actions hands every call to the program.
//   - A call the core answers itself is not taken while its queue has no room for the answer.
//     It comes again, like any entry that was not confirmed. An answer that can never fit is failed.
//     An env takes a call for an action or for the program only while none of its own entries waits to
//     be confirmed, so the action's answer, or one the program gives at once, finds the queue empty, and
//     an action runs once. Until then that call comes again too. A visitor takes a call of the env
//     as it comes: if both sides waited for the other, two calls that cross would wait for ever. An
//     env does not call a reserved name on its visitor: portal_call returns 0 for it, as the
//     visitor's answer, unknown, could wait behind the visitor's own calls for ever.
//   - The core has no clock: portal_tick is the only time it knows. An env counts the calls of
//     portal_tick since each visitor's last frame, and at PORTAL_QUIET_TICKS of them the connection is
//     quiet. Being quiet does not end it, and what is not confirmed is still said again.
//   - When every place is taken, a new visitor gets the place of a hosted connection that has not
//     opened yet. When there is none, a quiet connection gives its place: it ends with gone, its
//     visitor gets the ended frame and the program PORTAL_CLOSE. When there is neither, the new visitor
//     is answered with the ended frame and failed, and so is a visitor whose id is longer than
//     PORTAL_ID_SIZE.
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
//     where the program gives one. A body is a packed value, in bytes.
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

#ifndef PORTAL_MAX_ENVS
#define PORTAL_MAX_ENVS 2 // envs this side can expose
#endif
#ifndef PORTAL_MAX_CONNECTIONS
#define PORTAL_MAX_CONNECTIONS 4 // connections at once, hosted and entered together
#endif
#ifndef PORTAL_QUEUE_SIZE
#define PORTAL_QUEUE_SIZE PORTAL_FRAME_SIZE // bytes of entries not confirmed yet, per connection
#endif
#ifndef PORTAL_FRAME_SIZE
#define PORTAL_FRAME_SIZE 256 // the largest frame this side sends
#endif
#ifndef PORTAL_ID_SIZE
#define PORTAL_ID_SIZE 36 // the longest visitor id this side can hold
#endif
#ifndef PORTAL_QUIET_TICKS
#define PORTAL_QUIET_TICKS 8 // calls of portal_tick without a frame from a visitor, after which its connection is quiet
#endif
#ifndef PORTAL_MAX_INDEX
#define PORTAL_MAX_INDEX 4294967295u // the largest index
#endif

// ───────────────────────────────── bytes ─────────────────────────────────────

// A piece of text or of bytes that lives somewhere else. `at` is NULL when there is none.
typedef struct {
  const uint8_t *at;
  size_t length;
} portal_bytes;

// None: no payload, no message.
#define PORTAL_NONE ((portal_bytes){ NULL, 0 })

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

static inline portal_bytes portal_bytes_of(const char *text);
static inline bool portal_is(portal_bytes text, const char *word);
static inline portal_reader portal_reader_of(portal_bytes bytes);

// ───────────────────────────────── values ────────────────────────────────────
// The few pieces of CBOR the frames are made of, from the packager in cbor.c. They are public so
// that an action can read and write simple payloads with them.

enum { PORTAL_UINT = 0, PORTAL_NEGATIVE = 1, PORTAL_BYTES = 2, PORTAL_TEXT = 3, PORTAL_LIST = 4, PORTAL_MAP = 5, PORTAL_SIMPLE = 7 };
enum { PORTAL_FALSE = 20, PORTAL_TRUE = 21 }; // the two simple values a boolean is

// The number at the head of the next item: a whole number itself, the length of a text or of
// bytes, how many items a list has, how many pairs a map has.
uint32_t portal_read(portal_reader *reader, uint8_t type);
// The next item, a text or a byte string.
portal_bytes portal_read_bytes(portal_reader *reader, uint8_t type);
void portal_write(portal_writer *writer, uint8_t type, uint32_t number);
void portal_write_bytes(portal_writer *writer, uint8_t type, portal_bytes bytes);

// A writer over a buffer of the program's: portal_writer w = PORTAL_WRITER(room);
#define PORTAL_WRITER(buffer) ((portal_writer){ (buffer), sizeof(buffer), 0 })
// What a writer wrote, or PORTAL_NONE when it did not fit.
portal_bytes portal_written(const portal_writer *writer);

static inline void portal_write_text(portal_writer *writer, const char *text); // a C string, as a text
static inline void portal_write_int(portal_writer *writer, int32_t number);    // a whole number, below zero too
static inline void portal_write_bool(portal_writer *writer, bool value);
void portal_write_packed(portal_writer *writer, portal_bytes value);           // a value that is packed already, as it is
// The next item as a whole number, below zero too. One that is no whole number, or does not fit, fails the reader.
int32_t portal_read_int(portal_reader *reader);
// The next item as true or false: the one byte f4 or f5. Anything else fails the reader.
bool portal_read_bool(portal_reader *reader);

// ───────────────────────────────── what happens ──────────────────────────────

// How a call ends, and why a connection is over. `gone` and `timeout` mean the result is unknown.
// PORTAL_LATER never travels: an action returns it when the program ends the call itself. It is far from
// the six codes, and a code read from a frame is never it.
typedef enum { PORTAL_OK, PORTAL_DENIED, PORTAL_UNKNOWN, PORTAL_FAILED, PORTAL_GONE, PORTAL_TIMEOUT, PORTAL_LATER = 255 } portal_code;

// Which call. It holds nothing that points into a frame, so it may be kept for a later reply.
typedef struct {
  uint32_t connection; // what portal_enter returned, or what PORTAL_OPEN named
  uint32_t index;      // the call's index in that connection
  portal_bytes env;    // the exposed env it is about: mine, or the one I entered
} portal_ref;

typedef enum {
  PORTAL_CALL = 1, // into an env of mine with no actions, or into my visit: run `method` with the payload in `body`, then hand `ref` to portal_reply or portal_fail
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

// ───────────────────────────────── actions ───────────────────────────────────

// An action of an env. It reads the call (`call->body` is its payload, `call->who` who the medium vouches for)
// and writes its answer. What it returns is how the call ends:
//   PORTAL_OK      with one packed value written, or nothing written for no value;
//   another code   with nothing written, or one text written, which is the message;
//   PORTAL_LATER   the program ends the call itself, later, with portal_reply or portal_fail and a copy of `call->ref`.
// `context` is the pointer the env was exposed with. While it runs it does not make the core send.
typedef portal_code (*portal_run)(const portal_event *call, portal_writer *answer, const void *context);

typedef struct {
  const char *name;
  portal_run run;
} portal_action;

// The actions of an env. A set that makes up an env type says the type's name: `@describe` then answers with
// that type and lists no actions. `check`, when it is set, is asked at portal_expose whether the env behind
// `context` can be answered.
typedef struct {
  const char *type;
  const portal_action *actions;
  size_t count;
  bool (*check)(const void *context);
} portal_actions;

// The actions of an env of the program's own, from its list.
#define PORTAL_ACTIONS(list) ((portal_actions){ NULL, (list), sizeof(list) / sizeof *(list), NULL })
// No actions: every call into the env goes to the program as PORTAL_CALL, and the program replies.
#define PORTAL_NO_ACTIONS ((portal_actions){ NULL, NULL, 0, NULL })

// ───────────────────────────────── what a program calls ──────────────────────
// Each calls its member through the core's table.
//
// The name portal_call is given is copied at the call. Only an env's id, given to portal_expose or portal_enter,
// and its description and actions are kept by pointer: they have to stay where they are while they are in use.
// One caller at a time: nothing here is safe to call from two threads, or from an interrupt.

typedef struct portal_api portal_api;
typedef struct portals_core portals_core;

// True when every member of a table is set. A member left out is NULL: the program builds and runs, and
// the core jumps to address 0 when it reaches that member. Call it once at start.
static inline bool portal_complete(const portal_api *api);

// Makes an env of mine enterable: its id, what it is in words (or NULL), its actions, and a pointer of the
// program's own that each action is handed. The id, the description and the actions are kept by pointer: they
// stay where they are while the env is exposed. False: the id is not 1 to 36 bytes, every place for an env is
// taken, an action has no name or no function, or the set's `check` answered false.
static inline bool portal_expose(portals_core *core, const char *id, const char *description, portal_actions actions,
                                 const void *context);
// Takes it back. Its visitors get the ended frame.
static inline void portal_unexpose(portals_core *core, const char *id);
// Begins a connection with an exposed env: makes my id and queues the call `@enter`. The
// connection is usable at once: calls made before it opens wait behind `@enter`. PORTAL_OPEN or
// PORTAL_CLOSE follows. 0: no free place, or the env's id is not 1 to 36 bytes or fits no frame.
static inline uint32_t portal_enter(portals_core *core, const char *env, uint32_t link);
// Queues a call. Returns its index, which the PORTAL_REPLY will carry. 0: it could not be queued, the
// connection is over, or an env named a reserved name for its visitor.
static inline uint32_t portal_call(portals_core *core, uint32_t connection, const char *method, portal_bytes payload);
// Ends a call with a value: a packed value, or PORTAL_NONE for nothing. The call is one that came as PORTAL_CALL,
// or one that an action left for later with PORTAL_LATER: `ref` is the event's, or a copy of it. True: the call
// has ended, with what was given, or with failed when that fits no frame. False: nothing was said: entries of
// this side wait to be confirmed, and the program says it again after they are, or the connection is over.
static inline bool portal_reply(portals_core *core, const portal_ref *ref, portal_bytes value);
// Ends it with an error: one of the five codes that are not PORTAL_OK, and why in words, or NULL for no message.
// Any other code ends it failed. True and false mean what they mean for portal_reply.
static inline bool portal_fail(portals_core *core, const portal_ref *ref, portal_code code, const char *why);
// Ends a connection from this side: the ended frame with gone. Before PORTAL_OPEN it gives up.
static inline void portal_leave(portals_core *core, uint32_t connection);
// Sends one frame for each connection that has something to say. Call it once per turn.
static inline void portal_flush(portals_core *core);
// Marks everything not confirmed to be said again at the next flush, a waiting `@enter` included.
// It is also the core's only clock: an env counts its calls since each visitor's last frame, and at
// PORTAL_QUIET_TICKS of them that connection is quiet: it gives its place to a new visitor when every place
// is taken. Call it from a timer, well above the round trip, on a medium that repeats by itself too.
static inline void portal_tick(portals_core *core);

// Two of the program's members, ready: an `allow` that lets everyone in, and a listen and unlisten that do nothing,
// for a medium that carries frames for this end alone.
bool portal_anyone(const portal_event *call, portals_core *core);
void portal_unshared(portal_bytes id, uint32_t link, portals_core *core);

// The rest of this file is for someone who writes an ether or a packager, or replaces a member of the table.

// ───────────────────────────────── the state ─────────────────────────────────
// A field of a core, of an exposed env and of a connection is read and written only through
// these two. They are bound at build time and cost nothing.

#define getFromState(of, field) ((of)->field)
#define setInState(of, field, value) ((of)->field = (value))

// ───────────────────────────────── pieces ────────────────────────────────────
// A frame, a part at a time: what `unpack` reads and `pack` writes. What a piece names points into
// the frame it was read from, or at what is to be packed.

typedef enum {
  PORTAL_PIECE_HEAD = 1, // a connection frame up to its entries: from, to, sync, and how many entries `count`
  PORTAL_PIECE_ENDED,    // the connection is over: from, to, code, and a message in `body`
  PORTAL_PIECE_CALL,     // an entry that calls: index, name, and a payload in `body`
  PORTAL_PIECE_REPLY,    // an entry that replies: index, reply, code, and in `body` a payload for code 0, a message otherwise
  PORTAL_PIECE_PACKED,   // packed only: an entry of mine packed already, in `body`, after `count` others of its frame
  PORTAL_PIECE_FOOT,     // packed only: what ends a connection frame
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
  uint32_t handle;        // 0: this place is free. The program's name for the connection
  uint16_t quiet;         // an env's count of portal_tick since the visitor's last frame, up to PORTAL_QUIET_TICKS
  portal_bytes env;      // the exposed end's fixed id: mine when hosted, theirs when I entered
  uint32_t link, who;     // the sender: where their latest frame came from, who the medium vouches for
  uint32_t first;         // the index I started counting from: random when hosted, 1 for a visitor
  uint32_t next, sync;    // my next index, and how far I have taken theirs
  uint16_t count, length; // my entries not confirmed yet: how many, and their bytes in `queue`
  bool hosted, opened, dirty;
  uint8_t id_length;
  char id[PORTAL_ID_SIZE]; // the visitor's id, made by the visitor
  uint8_t queue[PORTAL_QUEUE_SIZE];
} portal_connection;

typedef struct {
  const char *id;
  const char *description; // what the env is, in words, or NULL
  portal_actions actions;
  const void *context; // the program's, handed to each action
} portal_env;

// The scope. Every member but `random` takes the core last. portals.c says what each does.
struct portal_api {
  // Given by the program.
  // The frame is the core's own buffer, where its next frame is built: send it or copy it before returning.
  void (*etherSend)(uint32_t link, const uint8_t *frame, size_t length, portals_core *core);
  void (*etherListen)(portal_bytes id, uint32_t link, portals_core *core);   // link 0: every link
  void (*etherUnlisten)(portal_bytes id, uint32_t link, portals_core *core);
  bool (*allow)(const portal_event *call, portals_core *core); // asked for every call into an exposed env, `@enter` first
  // 32 bits nobody can guess. The one member that does not take the core: the bits are the board's, so its own
  // function, such as arc4random or the Pico's get_rand_32, fits as it is.
  uint32_t (*random)(void);

  // The packager.
  bool (*unpack)(portal_reader *from, portal_piece *piece, portals_core *core);     // the next piece. False: nothing more
  bool (*pack)(portal_writer *into, const portal_piece *piece, portals_core *core); // adds one piece. False: it does not fit

  // Asked from outside.
  bool (*expose)(const char *id, const char *description, portal_actions actions, const void *context, portals_core *core);
  void (*unexpose)(const char *id, portals_core *core);
  bool (*receive)(const portal_piece *piece, portal_event *event, portals_core *core);
  uint32_t (*enter)(const char *env, uint32_t link, portals_core *core);
  uint32_t (*call)(uint32_t connection, const char *method, portal_bytes payload, portals_core *core);
  bool (*reply)(const portal_ref *ref, portal_code code, portal_bytes body, portals_core *core);
  void (*leave)(uint32_t connection, portals_core *core);
  void (*flush)(portals_core *core);
  void (*repeat)(portals_core *core);

  // Inner steps.
  portal_connection *(*begin)(portal_bytes env, bool hosted, uint32_t link, uint32_t who, portals_core *core);
  void (*end)(portal_connection *c, portal_event *event, portal_code code, portal_bytes message, bool answer, portals_core *core);
  bool (*push)(portal_connection *c, portal_event *event, portal_bytes method, uint32_t reply, portal_code code, portal_bytes body,
               portals_core *core);
  bool (*decide)(const portal_connection *c, portal_event *call, portals_core *core);
  bool (*start)(portal_connection *c, const portal_piece *head, portal_event *event, portals_core *core);
  bool (*take)(portal_connection *c, const portal_piece *entry, portal_event *event, portals_core *core);
  bool (*confirm)(portal_connection *c, uint32_t sync, portals_core *core);
  void (*say)(portal_connection *c, portals_core *core);
  void (*over)(uint32_t link, portal_bytes from, portal_bytes to, portal_code code, portal_bytes message, portals_core *core);
};

struct portals_core {
  const portal_api *api; // the scope. Set by the program before use
  void *context;         // the program's own, for its own members: no library sets it
  void *ether;           // an ether's own, for the table's ether members: ether_plug of packages/ether-c sets it

  // The core's own. All zero to start with.
  portal_env envs[PORTAL_MAX_ENVS];
  portal_connection connections[PORTAL_MAX_CONNECTIONS];
  uint32_t serial;
  uint8_t out[PORTAL_FRAME_SIZE];
};

// A frame that arrived, while it is read: what is left of it, and the piece read last. An entry is
// read over the head, so it carries the head's ids and sync. The program keeps it while it handles the
// frame.
typedef struct {
  portal_reader frame;
  portal_piece piece;
} portal_arrival;

// ───────────────────────────────── what an ether calls ───────────────────────

// A whole frame arrived. It is read by portal_next, and has to stay where it is until then.
portal_arrival portal_arrive(uint32_t link, uint32_t who, const uint8_t *frame, size_t length);
// What the frame comes to, one thing at a time. False when it has nothing more.
bool portal_next(portals_core *core, portal_arrival *arrival, portal_event *event);
// A link is over: every connection the core holds on it ends with `gone` and the message `why` (NULL: none),
// and nothing is sent for it. `told` is called with the PORTAL_CLOSE of each one the program is to hear of
// (NULL: nobody is told). It is one pass over the core's places, so it ends even when the program enters
// again on that link while it is told.
void portal_link_ended(portals_core *core, uint32_t link, const char *why, void (*told)(const portal_event *event, portals_core *core));

// ───────────────────────────────── the defaults ──────────────────────────────
// The library's own members, and the binary packager's. A table starts from these.

bool portal_op_expose(const char *id, const char *description, portal_actions actions, const void *context, portals_core *core);
void portal_op_unexpose(const char *id, portals_core *core);
bool portal_op_receive(const portal_piece *piece, portal_event *event, portals_core *core);
uint32_t portal_op_enter(const char *env, uint32_t link, portals_core *core);
uint32_t portal_op_call(uint32_t connection, const char *method, portal_bytes payload, portals_core *core);
bool portal_op_reply(const portal_ref *ref, portal_code code, portal_bytes body, portals_core *core);
void portal_op_leave(uint32_t connection, portals_core *core);
void portal_op_flush(portals_core *core);
void portal_op_repeat(portals_core *core);
portal_connection *portal_op_begin(portal_bytes env, bool hosted, uint32_t link, uint32_t who, portals_core *core);
void portal_op_end(portal_connection *c, portal_event *event, portal_code code, portal_bytes message, bool answer, portals_core *core);
bool portal_op_push(portal_connection *c, portal_event *event, portal_bytes method, uint32_t reply, portal_code code, portal_bytes body,
                    portals_core *core);
bool portal_op_decide(const portal_connection *c, portal_event *call, portals_core *core);
bool portal_op_start(portal_connection *c, const portal_piece *head, portal_event *event, portals_core *core);
bool portal_op_take(portal_connection *c, const portal_piece *entry, portal_event *event, portals_core *core);
bool portal_op_confirm(portal_connection *c, uint32_t sync, portals_core *core);
void portal_op_say(portal_connection *c, portals_core *core);
void portal_op_over(uint32_t link, portal_bytes from, portal_bytes to, portal_code code, portal_bytes message, portals_core *core);

bool portal_cbor_unpack(portal_reader *from, portal_piece *piece, portals_core *core);
bool portal_cbor_pack(portal_writer *into, const portal_piece *piece, portals_core *core);

#define PORTAL_OPERATIONS                                                                                                        \
  .expose = portal_op_expose, .unexpose = portal_op_unexpose, .receive = portal_op_receive, .enter = portal_op_enter,            \
  .call = portal_op_call, .reply = portal_op_reply, .leave = portal_op_leave, .flush = portal_op_flush,                          \
  .repeat = portal_op_repeat, .begin = portal_op_begin, .end = portal_op_end, .push = portal_op_push, .decide = portal_op_decide, \
  .start = portal_op_start, .take = portal_op_take, .confirm = portal_op_confirm, .say = portal_op_say, .over = portal_op_over

#define PORTAL_CBOR .unpack = portal_cbor_unpack, .pack = portal_cbor_pack

// ───────────────────────────────── the bodies of the calls ───────────────────

static inline portal_bytes portal_bytes_of(const char *text) {
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

static inline void portal_write_text(portal_writer *writer, const char *text) {
  portal_write_bytes(writer, PORTAL_TEXT, portal_bytes_of(text));
}
static inline void portal_write_int(portal_writer *writer, int32_t number) {
  portal_write(writer, (uint8_t)(number < 0 ? PORTAL_NEGATIVE : PORTAL_UINT), (uint32_t)(number < 0 ? -1 - number : number));
}
static inline void portal_write_bool(portal_writer *writer, bool value) {
  portal_write(writer, PORTAL_SIMPLE, value ? PORTAL_TRUE : PORTAL_FALSE);
}

static inline bool portal_complete(const portal_api *api) {
  static const uint8_t unset[sizeof(void (*)(void))] = { 0 };
  for (size_t at = 0; at < sizeof *api; at += sizeof unset)
    if (!memcmp((const uint8_t *)api + at, unset, sizeof unset)) return false;
  return true;
}
static inline bool portal_expose(portals_core *core, const char *id, const char *description, portal_actions actions,
                                 const void *context) {
  return core->api->expose(id, description, actions, context, core);
}
static inline void portal_unexpose(portals_core *core, const char *id) {
  core->api->unexpose(id, core);
}
static inline uint32_t portal_enter(portals_core *core, const char *env, uint32_t link) {
  return core->api->enter(env, link, core);
}
static inline uint32_t portal_call(portals_core *core, uint32_t connection, const char *method, portal_bytes payload) {
  return core->api->call(connection, method, payload, core);
}
static inline bool portal_reply(portals_core *core, const portal_ref *ref, portal_bytes value) {
  return core->api->reply(ref, PORTAL_OK, value, core);
}
static inline bool portal_fail(portals_core *core, const portal_ref *ref, portal_code code, const char *why) {
  portal_bytes message = { (const uint8_t *)why, why ? strlen(why) : 0 };
  return core->api->reply(ref, code >= PORTAL_DENIED && code <= PORTAL_TIMEOUT ? code : PORTAL_FAILED, message, core);
}
static inline void portal_leave(portals_core *core, uint32_t connection) {
  core->api->leave(connection, core);
}
static inline void portal_flush(portals_core *core) {
  core->api->flush(core);
}
static inline void portal_tick(portals_core *core) {
  core->api->repeat(core);
}

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
// is not taken or a function failed, or the code that a failed function gives. An answer goes into the
// caller's writer, which shows one that did not fit as it always does: `length` past `capacity`.
//
// A program writes its env in one statement, PERSISTENCE_ENV, as rows made by their types: NUMBER,
// STRING, BOOLEAN, FUNCTION and OBJECT, with every fact of a row a named field. A value lives in a
// variable of the program, which its row points at with `.at`, and the program's functions step in where
// a row names them: `.get` gives a value each time it is read, and `.set` takes a value that is set, or
// refuses it, before it is written to the variable.
//
// A number is a whole number, an int32_t: a number that comes in outside its range is not taken. A text
// is a C string when it goes out, and its bytes with their length when it comes in. A boolean is true or
// false.
//
// The env needs nothing of the portals core but its types, its reader and its writer: build it with the core's
// cbor.c. Through the core it is exposed with the type's four actions, persistence_actions, and the core answers
// its calls: portal_expose(&core, "pico", "a Pico W", persistence_actions, &pico).
#ifndef PERSISTENCE_H
#define PERSISTENCE_H


#ifdef __cplusplus
extern "C" {
#endif

#ifndef PERSISTENCE_MAX_INPUTS
#define PERSISTENCE_MAX_INPUTS 8 // the most rows an `.input` of type object has
#endif

// ───────────────────────────────── values ────────────────────────────────────

// A value that a function takes or gives. The field of its type is the one that counts. A text is `length`
// bytes at `string`, or a C string when `length` is 0, and NULL is the empty text: a text that came from
// the other side is its bytes and their length, with no zero after them.
typedef struct {
  int32_t number;
  bool boolean;
  bool failed;  // made by persistence_error
  uint8_t code; // with `failed`: a portal_code
  const char *string;
  size_t length;
  const char *reason; // with `failed`: why, a C string. NULL: nothing is said
} persistence_value;

// What a function returns: a value made by one of these, or one of the values it takes.
#define persistence_number(n) ((persistence_value){ .number = (n) })
#define persistence_boolean(b) ((persistence_value){ .boolean = (b) })
#define persistence_text(s) ((persistence_value){ .string = (s) }) // a C string
#define persistence_nothing ((persistence_value){ .number = 0 })
// The function failed: persistence_error(.code = PORTAL_DENIED, .reason = "locked"), each field when there
// is something to say. The code is PORTAL_FAILED when it is left out or is not one of PORTAL_DENIED,
// PORTAL_UNKNOWN, PORTAL_FAILED, PORTAL_GONE and PORTAL_TIMEOUT. The reason is the message of the answer.
#define persistence_error(...) ((persistence_value){ .failed = true, __VA_ARGS__ })

// ───────────────────────────────── rows ──────────────────────────────────────

// A row's type, which its maker sets, in the order of their names in the schema.
enum { PERSISTENCE_TYPE_NUMBER, PERSISTENCE_TYPE_STRING, PERSISTENCE_TYPE_BOOLEAN, PERSISTENCE_TYPE_FUNCTION, PERSISTENCE_TYPE_OBJECT };

// A row: a property of the env, or what a function takes or gives, which has no key. Each type has the
// fields that mean something for it, and only those. All start as `any` does.
//
// A number, a text or a boolean:
//   at        the program's variable that holds the value: an int32_t, a bool, or for a text an array of
//             `size` chars that holds a C string. A read takes the variable, unless the row has a `.get`
//   value     the value of a row with no `.at` and no `.get`, which never changes: 0, the empty text or false
//             when it is left out
//   get       gives the value each time it is read
//   set       takes a value that is set, before it is written to `.at`. False refuses it, and the variable stays
//   writable  the other side may set it, when there is a place for the value: a `.set`, or `.at`
// A text that does not fit `.size` with its zero is refused as a `.set` that returns false is.
// A function:
//   call      runs it, with `takes` NULL when it has no `.input`
//   input     what it takes: one value, or an object with a row for each of several, which come in its order
//   output    what it gives
// An object has its rows, written with ROWS.
//
// Through the portals core an answer is built in the core's own frame buffer, so a `.get`, `.set` or `.call`
// does not make the core send, with portal_leave or portal_flush, while it runs.
typedef union persistence_row persistence_row;
union persistence_row {
  struct {
    const char *key;         // its name among the rows beside it
    const char *description; // NULL: none
    uint8_t type;
  } any;
  struct {
    const char *key, *description;
    uint8_t type;
    bool writable;
    const char *unit; // NULL: none
    int32_t value, *at;
    int32_t (*get)(void);
    bool (*set)(int32_t number);
  } number;
  struct {
    const char *key, *description;
    uint8_t type;
    bool writable, value, *at;
    bool (*get)(void);
    bool (*set)(bool boolean);
  } boolean;
  struct {
    const char *key, *description;
    uint8_t type;
    bool writable;
    const char *value;                            // a C string
    char *at;                                     // `size` chars of the program's, which hold a C string
    size_t size;
    const char *(*get)(void);                     // a C string. NULL reads as the empty text
    bool (*set)(const char *text, size_t length); // the bytes as they came, with no zero after them
  } string;
  struct {
    const char *key, *description;
    uint8_t type;
    persistence_value (*call)(const persistence_value *takes);
    const persistence_row *input, *output;
  } function;
  struct {
    const char *key, *description;
    uint8_t type;
    const persistence_row *const *children;
    size_t count;
  } object;
};

// The makers of rows, each with the named fields of its type: PERSISTENCE_NUMBER(.key = "period", .at = &period).
// A row is const: of an env, only the program's own variables are in RAM.
#define PERSISTENCE_NUMBER(...) (&(const persistence_row){ .number = { .type = PERSISTENCE_TYPE_NUMBER, __VA_ARGS__ } })
#define PERSISTENCE_BOOLEAN(...) (&(const persistence_row){ .boolean = { .type = PERSISTENCE_TYPE_BOOLEAN, __VA_ARGS__ } })
#define PERSISTENCE_STRING(...) (&(const persistence_row){ .string = { .type = PERSISTENCE_TYPE_STRING, __VA_ARGS__ } })
#define PERSISTENCE_FUNCTION(...) (&(const persistence_row){ .function = { .type = PERSISTENCE_TYPE_FUNCTION, __VA_ARGS__ } })
#define PERSISTENCE_OBJECT(...) (&(const persistence_row){ .object = { .type = PERSISTENCE_TYPE_OBJECT, __VA_ARGS__ } })
// An object's rows, in order: PERSISTENCE_OBJECT(.key = "limits", PERSISTENCE_ROWS(row, row)).
#define PERSISTENCE_ROWS(...)                                    \
  .children = (const persistence_row *const[]){ __VA_ARGS__ }, \
  .count = sizeof((const persistence_row *const[]){ __VA_ARGS__ }) / sizeof(const persistence_row *)

// The makers without their prefix, when the program defines PERSISTENCE_SHORT_NAMES before it includes this.
#ifdef PERSISTENCE_SHORT_NAMES
#define NUMBER PERSISTENCE_NUMBER
#define STRING PERSISTENCE_STRING
#define BOOLEAN PERSISTENCE_BOOLEAN
#define FUNCTION PERSISTENCE_FUNCTION
#define OBJECT PERSISTENCE_OBJECT
#define ROWS PERSISTENCE_ROWS
#endif

// An env, as PERSISTENCE_ENV makes it.
typedef struct {
  const persistence_row *root; // an object with no key: the env's rows
} persistence_env;

// Defines `name`, a static const persistence_env with these rows. A comma after the last row is taken.
#define PERSISTENCE_ENV(name, ...) static const persistence_env name = { PERSISTENCE_OBJECT(PERSISTENCE_ROWS(__VA_ARGS__)) }
// The same, with the rows in an array the program wrote itself, where a directive such as #if can stand between
// two rows, as it cannot in PERSISTENCE_ENV: `static const persistence_row *const rows[] = { row, row };`.
#define PERSISTENCE_ENV_FROM(name, rows) \
  static const persistence_env name = { PERSISTENCE_OBJECT(.children = (rows), .count = sizeof(rows) / sizeof *(rows)) }

// True when every row of the env can be answered: each row of an object has a key, each function a `.call`,
// each row that says `.writable` a place for the value, and each text with `.at` its `.size`. A row that lacks
// one builds, and fails when it is reached.
bool persistence_complete(const persistence_env *env);

// ───────────────────────────────── the actions ───────────────────────────────

// What the env holds, as one node of the schema.
portal_code persistence_schema(const persistence_env *env, portal_writer *into);
// The value at `path`. For what has properties of its own, a map of their values by name, and so on down.
portal_code persistence_get(const persistence_env *env, portal_bytes path, portal_writer *into);
// Writes one value.
portal_code persistence_set(const persistence_env *env, portal_bytes path, portal_bytes value);
// Runs a function with `payload`, whose `at` is NULL when there is none, and writes what it gives, or nothing.
// A function that failed is answered with its code, and its reason is written as a text.
portal_code persistence_call(const persistence_env *env, portal_bytes path, portal_bytes payload, portal_writer *into);

// ───────────────────────────────── through the core ──────────────────────────

// The type `persistence`, its four actions, and persistence_complete as its check: an env exposed with it is
// answered by the core, and the pointer it is exposed with is its persistence_env. Each action runs the function
// above of its name: `schema` takes no payload, `get` a path, `set` [path, value], and `call` [path] or
// [path, payload]. A payload of another form is unknown.
extern const portal_actions persistence_actions;

#ifdef __cplusplus
}
#endif

#endif

// ════════════════════ ether.h ════════════════════

// An ether in C: what carries the frames of the portals core over a medium. Every ether has the same four
// functions, whatever its medium, two of which it may leave out, and the core reaches them through the three
// ether members of its table. Everything an ether hands on happens inside its `poll`, in the thread of the loop
// that calls it.
#ifndef ETHER_H
#define ETHER_H


#ifdef __cplusplus
extern "C" {
#endif

typedef struct ether ether;

// An ether: it knows its medium and carries frames. Four functions, whatever the medium. `listen` and `unlisten`
// are for a medium that is shared with other ends, or that the ether lets rest while nothing is listened for. NULL:
// the ether has nothing to do there.
struct ether {
  // Frames addressed to this id are wanted from now on, on this link (0: every link). The first one starts
  // the ether.
  void (*listen)(ether *e, portal_bytes id, uint32_t link);
  // And no longer. After the last one the ether lets its medium go.
  void (*unlisten)(ether *e, portal_bytes id, uint32_t link);
  // Carries one frame out on a link. The frame is the caller's own buffer: sent, or copied, before it returns.
  void (*send)(ether *e, uint32_t link, const uint8_t *frame, size_t length);
  // Looks at the medium, waiting at most `wait_ms`, and hands on what arrived, with ether_receive and
  // ether_ended. An ether with a `listen` hands on nothing while nothing is listened for. One without hands on
  // every frame that arrives, and the core answers what it does not know with the ended frame and gone. True:
  // something arrived.
  bool (*poll)(ether *e, int32_t wait_ms);

  portals_core *core;                                            // the core it carries frames for: set by ether_plug
  void (*handle)(const portal_event *event, portals_core *core); // the program's, for what happens to the core: set by ether_on_event
};

// Plugs an ether into a core, before the core is used: the core gets its table, and the ether hands what
// arrives to this core. The core keeps the ether in its `ether`, and its `context` stays the program's own.
// False: a member of the table is not set, and nothing is done.
bool ether_plug(ether *e, portals_core *core, const portal_api *api);

// What happens to the core, told to the program: PORTAL_OPEN, PORTAL_CLOSE, PORTAL_REPLY, and a PORTAL_CALL
// that no action answers. A program that only exposes envs with actions needs none.
void ether_on_event(ether *e, void (*handle)(const portal_event *event, portals_core *core));

// One turn of a program's loop: polls the ether for at most `wait_ms`, and when nothing arrived marks what is not
// confirmed to be said again (portal_tick), then sends what waits (portal_flush). So `wait_ms` is also how long
// a silence lasts before what is not confirmed is said again.
void ether_step(ether *e, int32_t wait_ms);

// For the core's table: its three ether members, which reach the ether through the core's `ether`.
void ether_send(uint32_t link, const uint8_t *frame, size_t length, portals_core *core);
void ether_listen(portal_bytes id, uint32_t link, portals_core *core);
void ether_unlisten(portal_bytes id, uint32_t link, portals_core *core);
#define ETHER_MEMBERS .etherSend = ether_send, .etherListen = ether_listen, .etherUnlisten = ether_unlisten

// The whole table for a core on an ether, from the two functions that are the program's own:
//   static const portal_api api = ETHER_API(portal_anyone, get_rand_32);
// A function left out does not build.
#define ETHER_API(allow_, random_) { ETHER_MEMBERS, .allow = (allow_), .random = (random_), PORTAL_CBOR, PORTAL_OPERATIONS }

// What an ether calls from its `poll`. Neither sends anything: the loop's ether_step does.

// A whole frame arrived: it goes through the core, and each event to the program's function, when it gave one.
// `who` is who the medium vouches for, or 0.
void ether_receive(ether *e, uint32_t link, uint32_t who, const uint8_t *frame, size_t length);
// A link is over: every connection the core holds on it ends with `gone` and the message `why`, nothing is
// sent for it, and each close goes to the program's function, when it gave one.
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

// The same three as the 16 bytes Bluetooth sends, the text's last byte first, as a list for an initialiser.
#define ETHER_BLE_SERVICE_BYTES 0xc1, 0x0a, 0x8f, 0x71, 0x3d, 0xe5, 0x89, 0x87, 0x3f, 0x46, 0xbd, 0xc9, 0x01, 0x00, 0xbf, 0x7c
#define ETHER_BLE_INPUT_BYTES 0xc1, 0x0a, 0x8f, 0x71, 0x3d, 0xe5, 0x89, 0x87, 0x3f, 0x46, 0xbd, 0xc9, 0x02, 0x00, 0xbf, 0x7c
#define ETHER_BLE_OUTPUT_BYTES 0xc1, 0x0a, 0x8f, 0x71, 0x3d, 0xe5, 0x89, 0x87, 0x3f, 0x46, 0xbd, 0xc9, 0x03, 0x00, 0xbf, 0x7c

#define ETHER_BLE_LAST 0x80u      // in a packet's first byte: the frame ends with this packet
#define ETHER_BLE_PLACE 0x7Fu     // the rest of that byte: the packet's place in its frame, from 0
#define ETHER_BLE_MAX_PACKETS 128 // the most packets a frame takes

// ───────────────────────────────── the limits ────────────────────────────────
// Override any of them with -D.

#ifndef ETHER_BLE_MAX_LINKS
#define ETHER_BLE_MAX_LINKS 1 // Bluetooth connections served at once
#endif
#ifndef ETHER_BLE_FRAME_SIZE
#define ETHER_BLE_FRAME_SIZE 512 // the largest frame taken from the other side
#endif
#ifndef ETHER_BLE_PACKET_SIZE
#define ETHER_BLE_PACKET_SIZE 244 // the largest packet sent
#endif

// ───────────────────────────────── packets ───────────────────────────────────
// Cutting a frame, and joining one. They touch only what they are handed.

typedef struct {
  size_t at;     // the bytes of the frame that have left
  uint8_t place; // the place of the next packet
} ether_ble_cut; // all zero before a frame's first packet

// Writes the frame's next packet, of at most `room` bytes. Returns its size. 0: the frame has left whole,
// `room` is under 2, or the frame needs more than 128 packets.
size_t ether_ble_cut_next(ether_ble_cut *cut, const uint8_t *frame, size_t length, uint8_t *packet, size_t room);

typedef struct {
  uint8_t *frame;          // where the frame is joined
  size_t capacity, length; // the largest frame taken, and the bytes joined
  uint8_t place;           // the place that is due
  bool open;               // a frame was begun and has not ended
} ether_ble_joined; // `frame` and `capacity` are set before use, and the rest is zero

// Takes one packet. True: it ended a frame, which is whole in `frame`, `length` bytes, until the next packet.
bool ether_ble_join(ether_ble_joined *joined, const uint8_t *packet, size_t size);

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
  ether base;                   // first: a BLE ether is an ether
  const ether_ble_stack *stack; // set by ether_ble_init
  void *context;                // the stack's own

  // The ether's own, all zero to start with.
  uint32_t listened;                            // the ids listened for
  uint32_t links[ETHER_BLE_MAX_LINKS];          // the link that holds each place. 0: the place is free
  ether_ble_joined joined[ETHER_BLE_MAX_LINKS]; // the frame being joined in each place
  uint8_t frames[ETHER_BLE_MAX_LINKS][ETHER_BLE_FRAME_SIZE];
  uint8_t packet[ETHER_BLE_PACKET_SIZE]; // the packet being sent
};

// The four functions of the ether.
//
// Listening is counted: a Bluetooth connection carries frames for this end alone, so an id and a link are not
// kept. When the count comes back to 0, every place is free again, and what was half joined there is forgotten.
void ether_ble_listen(ether *e, portal_bytes id, uint32_t link);
void ether_ble_unlisten(ether *e, portal_bytes id, uint32_t link);
// Asks `room` once, and cuts the frame into packets of that size, at most ETHER_BLE_PACKET_SIZE. Nothing is sent when
// the room is under 2, or when the frame would take more than 128 packets. When `send` answers false, the rest of
// the frame is given up: the core says it again later.
void ether_ble_send(ether *e, uint32_t link, const uint8_t *frame, size_t length);
// Asks the stack's `receive` once. A packet joins its link's frame: a link the ether does not know takes a free
// place, and with none free the packet is dropped. A frame that became whole goes to ether_receive. An end frees
// its link's place and goes to ether_ended. While nothing is listened for, what arrives is dropped, and it
// answers false. True: a packet or an end came.
bool ether_ble_poll(ether *e, int32_t wait_ms);

// Makes a BLE ether of one that is all zero: its functions and its stack. Returns it as an ether, to plug into
// a core. An ether_ble started with an initialiser would be stored in flash as well, nearly all zeros.
ether *ether_ble_init(ether_ble *ble, const ether_ble_stack *stack);

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

#ifndef ETHER_BLE_MAX_WAITING
#define ETHER_BLE_MAX_WAITING 8 // packets that wait for the core's thread
#endif

// The Bluetooth LE ether of this firmware, to plug into its core.
ether *ether_ble_zephyr(void);
// For the core's table: 32 bits from Bluetooth's own source, or the clock's count when that fails.
uint32_t ether_ble_zephyr_random(void);

#ifdef __cplusplus
}
#endif

#endif
