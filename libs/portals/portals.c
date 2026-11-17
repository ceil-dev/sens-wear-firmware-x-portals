// Copyright (c) 2026 ceil.dev
// SPDX-License-Identifier: MIT

// Portals for the SensWear firmware, in one header and one source: the portals core, the env type `persistence`,
// the ether, and the Bluetooth LE ether with its Zephyr port. `make bundle` of ceil.dev's package senswear-portals
// joins this file from the files named below, each under a line that names it: it is not edited here.

#include "portals.h"

// ════════════════════ portals.c ════════════════════

// Portals in C: the portals core. portals.h says what it is and how it is driven.
//
// THE SCOPE, MEMBER BY MEMBER
//
// Every member takes its arguments and then the core, and calls the others through the
// core's table: m->api->push(..., m). The library's own are portal_op_<name> here, and the
// binary packager's are in cbor.c.
//
// Given by the app
//   etherSend        hands a frame to the medium: sends it, or copies it, before it returns, as the
//                    core builds its next frame in the same buffer
//   etherListen      frames to this id are for this core from now on: an env I expose, or a visit of mine
//   etherUnlisten    and no longer
//   allow            may this caller use this action
//   random           32 bits nobody can guess: my ids, and the index an env counts from
//
// The packager
//   unpack           the next piece of a frame that arrived: its head, then one entry at a time
//   pack             adds one piece to a frame that leaves, or only counts it
//
// Asked from outside
//   expose           makes an env of mine enterable, and listens for its id
//   unexpose         takes it back: its visitors get the ended frame
//   receive          takes one piece: at most one event, and whether the next piece of the frame is wanted
//   enter            makes my id, begins a connection, queues `@enter` and listens for the id
//   call             queues a call
//   reply            queues what a call of theirs came to
//   leave            ends a connection with gone
//   flush            sends one frame for each connection that has something to say
//   repeat           marks what is not confirmed, to be said again, and counts since each visitor was heard
//
// Inner steps
//   begin            a place for a new connection, and the index it counts from
//   end              forgets a connection, sends the ended frame, tells the app, stops listening for my id
//   push             queues an entry of mine, or ends the connection when no number is left
//   decide           what a call of theirs comes to: the core's own answer, or the app's to run
//   start            takes the head of a connection frame: whose it is, what they confirmed, whether its sync is from another connection
//   take             takes an entry when it comes next, or begins a connection with it
//   confirm          drops the entries of mine they have taken
//   say              sends where I stand, with as many of my entries as fit
//   over             sends the ended frame
//
// Plain helpers only work out or look up: find, held, quiet, exposed, mine, theirs, id_of, ref_of,
// same, queued, around, carried. The ether's side, at the bottom, turns a frame into pieces:
// portal_arrive and portal_next.


static const portal_bytes none = { NULL, 0 };

// The limits the code relies on: this line does not compile when one of them is broken.
typedef char limits[PORTAL_ID >= 16 && PORTAL_ID < 256 && PORTAL_CONNECTIONS < 256 && PORTAL_QUEUE < 65536 && PORTAL_QUIET < 65536 ? 1 : -1];

// ───────────────────────────────── helpers ───────────────────────────────────

static portal_bytes id_of(const portal_connection *c) {
  return (portal_bytes){ (const uint8_t *)getFromState(c, id), getFromState(c, id_length) };
}
static portal_bytes mine(const portal_connection *c) {
  return getFromState(c, hosted) ? getFromState(c, env) : id_of(c);
}
static portal_bytes theirs(const portal_connection *c) {
  return getFromState(c, hosted) ? id_of(c) : getFromState(c, env);
}
static portal_ref ref_of(const portal_connection *c, uint32_t index) {
  return (portal_ref){ getFromState(c, handle), index, getFromState(c, env) };
}
static bool same(portal_bytes one, portal_bytes other) {
  return one.length == other.length && (!one.length || memcmp(one.at, other.at, one.length) == 0);
}

// The connection a handle stands for, while it lasts. A handle is a serial number over the place
// plus one, so it is never 0 and a place that is used again gets a new one.
static portal_connection *held(portals_core *m, uint32_t handle) {
  size_t place = (handle & 255) - 1; // wraps for 0, and then it is no place
  return place < PORTAL_CONNECTIONS && getFromState(&getFromState(m, connections)[place], handle) == handle
           ? &getFromState(m, connections)[place]
           : NULL;
}

// The connection a frame names: `to` is my id in it, `from` is theirs.
static portal_connection *find(portals_core *m, portal_bytes to, portal_bytes from) {
  for (size_t i = 0; i < PORTAL_CONNECTIONS; i++) {
    portal_connection *c = &getFromState(m, connections)[i];
    if (getFromState(c, handle) && same(mine(c), to) && same(theirs(c), from)) return c;
  }
  return NULL;
}

// A connection of an env whose visitor has not been heard for PORTAL_QUIET calls of portal_repeat.
static portal_connection *quiet(portals_core *m) {
  for (size_t i = 0; i < PORTAL_CONNECTIONS; i++) {
    portal_connection *c = &getFromState(m, connections)[i];
    if (getFromState(c, handle) && getFromState(c, hosted) && getFromState(c, quiet) == PORTAL_QUIET) return c;
  }
  return NULL;
}

static portal_env *exposed(portals_core *m, portal_bytes id) {
  for (size_t i = 0; i < PORTAL_ENVS; i++) {
    portal_env *env = &getFromState(m, envs)[i];
    if (getFromState(env, id) && portal_is(id, getFromState(env, id))) return env;
  }
  return NULL;
}

// An entry of mine in the queue, as it was packed. The core keeps its length in front of it.
static portal_bytes queued(const uint8_t *at) {
  uint16_t length;
  memcpy(&length, at, sizeof length);
  return (portal_bytes){ at + sizeof length, length };
}

// What a frame of this connection spends besides its entries, when it carries `count` of them
// with this `sync`. `pack` counts it, with a writer that has no room.
static size_t around(portals_core *m, const portal_connection *c, uint32_t sync, uint32_t count) {
  portal_writer w = { NULL, 0, 0 };
  portal_piece piece = { .kind = PORTAL_HEAD, .from = mine(c), .to = theirs(c), .sync = sync, .count = count };
  m->api->pack(&w, &piece, m);
  piece.kind = PORTAL_FOOT;
  m->api->pack(&w, &piece, m);
  return w.length;
}

// What an entry of mine takes in a frame, after `before` others.
static size_t carried(portals_core *m, portal_bytes entry, uint32_t before) {
  portal_writer w = { NULL, 0, 0 };
  portal_piece piece = { .kind = PORTAL_PACKED, .count = before, .body = entry };
  m->api->pack(&w, &piece, m);
  return w.length;
}

// ───────────────────────────────── connections ───────────────────────────────

// A place for a new connection: a free one, or failing that a hosted one that has not opened,
// which an env may forget at any time.
portal_connection *portal_op_begin(portal_bytes env, bool hosted, uint32_t link, uint32_t who, portals_core *m) {
  portal_connection *c = NULL;
  for (size_t i = 0; i < PORTAL_CONNECTIONS && (!c || getFromState(c, handle)); i++) {
    portal_connection *at = &getFromState(m, connections)[i];
    if (!getFromState(at, handle) || (!c && getFromState(at, hosted) && !getFromState(at, opened))) c = at;
  }
  if (!c) return NULL;
  setInState(m, serial, getFromState(m, serial) + 1);
  setInState(c, handle, getFromState(m, serial) << 8 | (uint32_t)(c - getFromState(m, connections) + 1));
  setInState(c, env, env);
  setInState(c, link, link);
  setInState(c, who, who);
  setInState(c, hosted, hosted);
  setInState(c, opened, false);
  setInState(c, dirty, false);
  setInState(c, quiet, 0);
  setInState(c, count, 0);
  setInState(c, length, 0);
  setInState(c, sync, 0);
  setInState(c, id_length, 0);
  setInState(c, first, hosted ? 1 + (m->api->random(m) & 0xFFFF) : 1); // 16 random bits
  setInState(c, next, getFromState(c, first));
  return c;
}

// Forgets a connection. `answer`: the other side gets the ended frame with `code`. The app gets
// PORTAL_CLOSE in `event` when the connection had opened or is my own visit. Without an `event` the
// app ended it itself, and is not told.
void portal_op_end(portal_connection *c, portal_event *event, portal_code code, portal_bytes message, bool answer,
                   portals_core *m) {
  if (answer) m->api->over(getFromState(c, link), mine(c), theirs(c), code, message, m);
  if (event && (getFromState(c, opened) || !getFromState(c, hosted)))
    *event = (portal_event){ PORTAL_CLOSE, ref_of(c, 0), getFromState(c, who), none, code, message };
  if (!getFromState(c, hosted)) m->api->etherUnlisten(id_of(c), getFromState(c, link), m);
  setInState(c, handle, 0);
}

// Queues an entry of mine. False: it has no room now, it can never fit a frame, or no number is left
// and the connection has ended with gone. An env may meet that at a reply, after the
// action has run.
bool portal_op_push(portal_connection *c, portal_event *event, portal_bytes method, uint32_t reply, portal_code code,
                    portal_bytes body, portals_core *m) {
  uint32_t index = getFromState(c, next);
  if (index - 1u >= PORTAL_TOP) {
    m->api->end(c, event, PORTAL_GONE, portal_text("out of numbers"), true, m);
    return false;
  }
  portal_piece entry = { .kind = method.at ? PORTAL_CALLS : PORTAL_REPLIES, .index = index, .name = method, .reply = reply,
                         .code = code, .body = body };
  portal_writer w = { NULL, 0, 0 };
  m->api->pack(&w, &entry, m); // counted only
  size_t at = getFromState(c, length);
  uint16_t size = (uint16_t)w.length;
  if (sizeof size + w.length > sizeof getFromState(c, queue) - at) return false;
  memcpy(getFromState(c, queue) + at, &size, sizeof size);
  w = (portal_writer){ getFromState(c, queue) + at + sizeof size, size, 0 };
  m->api->pack(&w, &entry, m);
  if (around(m, c, PORTAL_TOP, 1) + carried(m, queued(getFromState(c, queue) + at), 0) > sizeof getFromState(m, out)) return false;
  setInState(c, length, (uint16_t)(at + sizeof size + size));
  setInState(c, count, (uint16_t)(getFromState(c, count) + 1));
  setInState(c, next, index + 1);
  setInState(c, dirty, true);
  return true;
}

// Drops the entries of mine they have taken. True if any went.
bool portal_op_confirm(portal_connection *c, uint32_t sync, portals_core *m) {
  (void)m;
  uint32_t oldest = getFromState(c, next) - getFromState(c, count), taken = sync < oldest ? 0 : sync - oldest + 1;
  if (taken > getFromState(c, count)) taken = getFromState(c, count);
  const uint8_t *at = getFromState(c, queue);
  for (uint32_t i = 0; i < taken; i++) {
    portal_bytes entry = queued(at);
    at = entry.at + entry.length;
  }
  setInState(c, length, (uint16_t)(getFromState(c, length) - (at - getFromState(c, queue))));
  memmove(getFromState(c, queue), at, getFromState(c, length));
  setInState(c, count, (uint16_t)(getFromState(c, count) - taken));
  return taken > 0;
}

// Sends where I stand: how far I have taken theirs, and as many of mine as fit, oldest first.
// Until `@enter` is replied to, a visitor's frame carries it alone.
void portal_op_say(portal_connection *c, portals_core *m) {
  uint32_t sync = getFromState(c, sync), fit = 0;
  uint32_t most = getFromState(c, hosted) || getFromState(c, opened) ? getFromState(c, count) : 1;
  const uint8_t *at = getFromState(c, queue);
  for (size_t entries = 0; fit < most; fit++) {
    portal_bytes entry = queued(at);
    size_t more = entries + carried(m, entry, fit);
    if (around(m, c, sync, fit + 1) + more > sizeof getFromState(m, out)) break;
    entries = more;
    at = entry.at + entry.length;
  }
  portal_writer w = { getFromState(m, out), sizeof getFromState(m, out), 0 };
  portal_piece piece = { .kind = PORTAL_HEAD, .from = mine(c), .to = theirs(c), .sync = sync, .count = fit };
  m->api->pack(&w, &piece, m);
  piece.kind = PORTAL_PACKED;
  for (at = getFromState(c, queue), piece.count = 0; piece.count < fit; piece.count++) {
    piece.body = queued(at);
    m->api->pack(&w, &piece, m);
    at = piece.body.at + piece.body.length;
  }
  piece.kind = PORTAL_FOOT;
  if (m->api->pack(&w, &piece, m)) m->api->etherSend(getFromState(c, link), w.bytes, w.length, m);
  setInState(c, dirty, false);
}

// The ended frame. A message that does not fit is left out.
void portal_op_over(uint32_t link, portal_bytes from, portal_bytes to, portal_code code, portal_bytes message, portals_core *m) {
  portal_writer w = { getFromState(m, out), sizeof getFromState(m, out), 0 };
  portal_piece ended = { .kind = PORTAL_ENDED, .from = from, .to = to, .code = code, .body = message };
  if (m->api->pack(&w, &ended, m)) m->api->etherSend(link, w.bytes, w.length, m);
  else if (message.at) m->api->over(link, from, to, code, none, m);
}

// ───────────────────────────────── frames in ─────────────────────────────────

// What a call of theirs comes to. True: the core answers it itself, and the event holds the answer
// in `code` and `body`. False: it is the app's to run. Whether a call into an exposed env is allowed
// is asked first. A visitor answers `@describe` with unknown, like every reserved name it does not know.
bool portal_op_decide(const portal_connection *c, portal_event *call, portals_core *m) {
  portal_env *env = getFromState(c, hosted) ? exposed(m, getFromState(c, env)) : NULL;
  if (env && !m->api->allow(call, m)) call->code = PORTAL_DENIED;
  else if (portal_is(call->method, "@enter")) call->code = PORTAL_FAILED;
  else if (env && portal_is(call->method, "@describe")) call->code = PORTAL_OK;
  else if (call->method.length && call->method.at[0] == '@') call->code = PORTAL_UNKNOWN; // any other name that starts with @ is reserved, and unknown here
  else return false;
  call->body = call->code ? none : getFromState(env, description);
  return true;
}

// The head of a connection frame. One that can be read, with a sync that is not from another connection, is heard: its connection is not quiet.
// True: its entries are wanted.
bool portal_op_start(portal_connection *c, const portal_piece *head, portal_event *event, portals_core *m) {
  uint32_t sync = head->sync;
  if (!c) { // a connection I do not know: only a frame that can be a beginning goes on, to its first entry
    if (exposed(m, head->to) && head->whole && !sync && head->count) return true;
    m->api->over(head->link, head->to, head->from, PORTAL_GONE, none, m);
    return false;
  }
  bool hosted = getFromState(c, hosted);
  if (hosted) setInState(c, link, head->link); // answer where their latest frame came from
  if (!head->whole || sync > PORTAL_TOP) { // a head that cannot be read
    m->api->end(c, event, PORTAL_FAILED, none, true, m);
    return false;
  }
  // A `sync` that names no entry I sent in this connection is from another one.
  if (hosted && sync && sync - getFromState(c, first) >= getFromState(c, next) - getFromState(c, first)) {
    m->api->end(c, event, PORTAL_GONE, none, true, m);
    return false;
  }
  setInState(c, quiet, 0); // they were heard
  // They have these: stop sending them. A visitor keeps `@enter` until it is replied to.
  bool confirmed = (hosted || getFromState(c, opened)) && m->api->confirm(c, sync, m);
  // Say where I stand when they confirmed something, when their frame carries entries (they have not
  // seen my sync), when they have not confirmed all of mine, or when they have taken none of mine.
  if (confirmed || head->count || getFromState(c, count) || !sync) setInState(c, dirty, true);
  if (!hosted) return true;
  if (!sync) return false; // from sync 0 an env takes nothing
  if (getFromState(c, opened)) return true;
  setInState(c, opened, true); // the first frame that confirms one of mine
  *event = (portal_event){ PORTAL_OPEN, ref_of(c, 0), head->who, none, PORTAL_OK, none };
  return true;
}

// An entry, taken when it comes next. True: the next entry of the frame is wanted. An env takes a call
// for the app only while none of its own entries waits to be confirmed. A beginning that finds every
// place taken gets the place of a quiet connection, which ends with gone.
bool portal_op_take(portal_connection *c, const portal_piece *entry, portal_event *event, portals_core *m) {
  if (!c) {
    // The first entry of a frame that can be a beginning. An entry of a connection that ended
    // earlier in this frame is not read.
    portal_env *env = exposed(m, entry->to);
    if (!env || entry->sync) return false;
    if (entry->index != 1 || !portal_is(entry->name, "@enter")) { // only `@enter` at index 1 begins a connection
      m->api->over(entry->link, entry->to, entry->from, PORTAL_GONE, none, m);
      return false;
    }
    // A beginning. A visitor that is not let in is answered, and nothing is kept.
    portal_event asked = { PORTAL_CALL, { 0, 1, portal_text(getFromState(env, id)) }, entry->who, entry->name, PORTAL_OK, none };
    bool allowed = m->api->allow(&asked, m), fits = entry->from.length <= PORTAL_ID;
    c = allowed && fits ? m->api->begin(asked.ref.env, true, entry->link, entry->who, m) : NULL;
    portal_connection *old = allowed && fits && !c ? quiet(m) : NULL;
    if (old) { // every place is taken: a quiet connection gives its place
      m->api->end(old, event, PORTAL_GONE, none, true, m);
      c = m->api->begin(asked.ref.env, true, entry->link, entry->who, m);
    }
    if (!c) { // not let in, no room, or an id too long to hold
      m->api->over(entry->link, entry->to, entry->from, allowed ? PORTAL_FAILED : PORTAL_DENIED, none, m);
      return false;
    }
    memcpy(getFromState(c, id), entry->from.at, entry->from.length);
    setInState(c, id_length, (uint8_t)entry->from.length);
    setInState(c, sync, 1);
    m->api->push(c, NULL, none, 1, PORTAL_OK, none, m); // the reply to `@enter`, numbered from the random first index
    return false;
  }
  if (entry->index - 1u >= PORTAL_TOP) { // an index that cannot be read
    m->api->end(c, event, PORTAL_FAILED, none, true, m);
    return false;
  }
  // A repeat, or one that is early, is skipped by its index alone. A visitor that is
  // entering takes the reply to `@enter` whatever its index, and nothing else.
  bool call = entry->kind == PORTAL_CALLS, entering = !getFromState(c, hosted) && !getFromState(c, opened);
  if (entering ? entry->reply != 1 : entry->index != getFromState(c, sync) + 1) return true;
  // An entry that cannot be read ends the connection: the entries before it stay taken, and nothing after it is read.
  if (!entry->whole || (call ? !entry->name.length : entry->reply - 1u >= PORTAL_TOP)) {
    m->api->end(c, event, PORTAL_FAILED, none, true, m);
    return false;
  }
  *event = (portal_event){ call ? PORTAL_CALL : PORTAL_REPLY, ref_of(c, call ? entry->index : entry->reply), getFromState(c, who),
                           entry->name, entry->code > PORTAL_TIMEOUT ? PORTAL_FAILED : (portal_code)entry->code, entry->body }; // a code above 5 is read as failed
  if (call && m->api->decide(c, event, m)) {
    // The core's own answer. While its queue has no room for it, the call is not taken: it comes
    // again, and so does the rest of the frame. An answer that can never fit is failed.
    bool answered = m->api->push(c, event, none, entry->index, event->code, event->body, m);
    if (!answered && getFromState(c, handle) && !getFromState(c, count))
      answered = m->api->push(c, event, none, entry->index, PORTAL_FAILED, none, m);
    if (!getFromState(c, handle)) return false; // no number was left: the event holds PORTAL_CLOSE
    if (answered) setInState(c, sync, entry->index);
    *event = (portal_event){ 0 };
    return answered;
  }
  // A call for the app is answered later, through portal_reply, and that answer has to find room. So
  // an env takes the call only while none of its own entries waits to be confirmed: the answer then finds the
  // queue empty. Until then the call is not taken. It comes again, and so does the rest of the frame.
  if (call && getFromState(c, hosted) && getFromState(c, count)) {
    *event = (portal_event){ 0 };
    return false;
  }
  setInState(c, sync, entry->index);
  if (!entering) return true;
  // An error means that I am not let in, and my ended frame passes it on. A payload is not read.
  if (event->code) {
    m->api->end(c, event, event->code, event->body, true, m);
    return false;
  }
  setInState(c, opened, true);
  m->api->confirm(c, 1, m); // `@enter` is replied to: it goes
  event->type = PORTAL_OPEN;
  event->ref.index = 0;
  event->body = none;
  return true;
}

// One piece of a frame. True: the next piece of the same frame is wanted. Nothing is kept between
// two pieces: each names its connection by the frame's ids.
bool portal_op_receive(const portal_piece *piece, portal_event *event, portals_core *m) {
  if (piece->from.length - 1u >= 36 || piece->to.length - 1u >= 36) return false; // both have to be ids: 1 to 36 bytes
  portal_connection *c = find(m, piece->to, piece->from);
  if (c && getFromState(c, hosted) && getFromState(c, who) != piece->who) return false; // the right ids, but the medium vouches for someone else
  if (piece->kind == PORTAL_HEAD) return m->api->start(c, piece, event, m);
  if (piece->kind != PORTAL_ENDED) return m->api->take(c, piece, event, m);
  // The ended frame ends the connection, and nothing answers it.
  if (c) m->api->end(c, event, piece->code - 1u < PORTAL_TIMEOUT ? (portal_code)piece->code : PORTAL_FAILED, piece->body, false, m);
  return false;
}

// ───────────────────────────────── the app's side ────────────────────────────

bool portal_op_expose(const char *id, portal_bytes description, portals_core *m) {
  portal_bytes text = portal_text(id);
  if (text.length - 1u >= 36) return false; // an id is 1 to 36 bytes
  portal_env *env = exposed(m, text);
  for (size_t i = 0; !env && i < PORTAL_ENVS; i++)
    if (!getFromState(&getFromState(m, envs)[i], id)) env = &getFromState(m, envs)[i];
  if (!env) return false;
  if (!getFromState(env, id)) m->api->etherListen(text, 0, m);
  setInState(env, id, id);
  setInState(env, description, description);
  return true;
}

void portal_op_unexpose(const char *id, portals_core *m) {
  portal_env *env = exposed(m, portal_text(id));
  if (!env) return;
  for (size_t i = 0; i < PORTAL_CONNECTIONS; i++) {
    portal_connection *c = &getFromState(m, connections)[i];
    if (getFromState(c, handle) && getFromState(c, hosted) && portal_is(getFromState(c, env), id))
      m->api->end(c, NULL, PORTAL_GONE, portal_text("closed"), true, m);
  }
  setInState(env, id, NULL);
  m->api->etherUnlisten(portal_text(id), 0, m);
}

uint32_t portal_op_enter(const char *env, uint32_t link, portals_core *m) {
  portal_bytes text = portal_text(env);
  if (text.length - 1u >= 36) return 0; // an id is 1 to 36 bytes
  portal_connection *c = m->api->begin(text, false, link, 0, m);
  if (!c) return 0;
  for (size_t i = 0; i < 16; i++) getFromState(c, id)[i] = "0123456789abcdef"[m->api->random(m) & 15]; // 64 random bits, as 16 hex digits
  setInState(c, id_length, 16);
  if (!m->api->push(c, NULL, portal_text("@enter"), 0, PORTAL_OK, none, m)) {
    setInState(c, handle, 0); // the env's id does not fit a frame
    return 0;
  }
  m->api->etherListen(id_of(c), link, m);
  return getFromState(c, handle);
}

// An env does not call a reserved name on its visitor. The visitor answers it only with unknown, and that
// answer waits for room behind the visitor's own calls, which the env takes only once its own call is confirmed.
uint32_t portal_op_call(uint32_t connection, const char *method, portal_bytes payload, portals_core *m) {
  portal_connection *c = held(m, connection);
  if (!c || (getFromState(c, hosted) && method[0] == '@')) return 0;
  return m->api->push(c, NULL, portal_text(method), 0, PORTAL_OK, payload, m) ? getFromState(c, next) - 1 : 0;
}

bool portal_op_reply(const portal_ref *ref, portal_code code, portal_bytes body, portals_core *m) {
  portal_connection *c = held(m, ref->connection);
  return c && m->api->push(c, NULL, none, ref->index, code, body, m);
}

void portal_op_leave(uint32_t connection, portals_core *m) {
  portal_connection *c = held(m, connection);
  if (c) m->api->end(c, NULL, PORTAL_GONE, portal_text("left"), true, m);
}

void portal_op_flush(portals_core *m) {
  for (size_t i = 0; i < PORTAL_CONNECTIONS; i++) {
    portal_connection *c = &getFromState(m, connections)[i];
    if (getFromState(c, handle) && getFromState(c, dirty)) m->api->say(c, m);
  }
}

// Marks what is not confirmed, to be said again, on every connection, quiet or not. It is the
// core's only clock: an env counts the calls since each visitor's last frame, up to PORTAL_QUIET. A
// visitor's own connection is marked with no count.
void portal_op_repeat(portals_core *m) {
  for (size_t i = 0; i < PORTAL_CONNECTIONS; i++) {
    portal_connection *c = &getFromState(m, connections)[i];
    if (!getFromState(c, handle)) continue;
    if (getFromState(c, hosted) && getFromState(c, quiet) < PORTAL_QUIET) setInState(c, quiet, (uint16_t)(getFromState(c, quiet) + 1));
    if (getFromState(c, count)) setInState(c, dirty, true);
  }
}

// ───────────────────────────────── the ether's side ──────────────────────────
// A frame becomes pieces through `unpack`, and each piece goes to `receive`, until one is not wanted.

portal_arrival portal_arrive(const uint8_t *frame, size_t length, uint32_t link, uint32_t who) {
  return (portal_arrival){ { frame, frame ? frame + length : frame }, { .link = link, .who = who } };
}

bool portal_next(portals_core *m, portal_arrival *arrival, portal_event *event) {
  while (m->api->unpack(&arrival->frame, &arrival->piece, m)) {
    *event = (portal_event){ 0 };
    if (!m->api->receive(&arrival->piece, event, m)) arrival->frame = (portal_reader){ NULL, NULL }; // the rest is not read
    if (event->type) return true;
  }
  arrival->frame = (portal_reader){ NULL, NULL };
  return false;
}

// ════════════════════ cbor.c ════════════════════

// The binary packager: the frames written as CBOR lists in a fixed order. It is the one part of the
// library that knows the layout. It reads a frame where it arrived, one piece at a time, and copies
// nothing out of it.
//
// THE WIRE
//
// The first item says which frame it is.
//
//   A connection frame         [0, from, to, sync, [entry, ...]]
//   The connection is over     [3, from, to, code, message?]
//
//   An entry that calls        [index, name, payload?]
//   An entry that replies      [index, reply, code, body?]
//
//   - Ids, names and messages are text. An id is 1 to 36 bytes. Indexes, sync and codes are whole
//     numbers up to 4294967295.
//   - A code of 0 is a value, and its body is the payload. Any other code is an error (1 denied,
//     2 unknown, 3 failed, 4 gone, 5 timeout), and its body is the message, as text. A code above 5
//     is read as failed, and so is a code of 0, or none, in the ended frame. A reply with code 0 and
//     no body carries nothing, like the reply to `@enter`.
//   - A payload is any CBOR value, wrapped as a byte string. Each entry is wrapped as a byte string
//     too. So every part states its length in bytes: stepping over one is a single jump, and the
//     packager never reads into a payload.
//   - A call's second item is text and a reply's is a number. That is how the two are told apart.
//   - Items after the ones named here are ignored, in a frame and in an entry.
//   - A frame that is not a list starting with 0 or 3, with a `from` and a `to` as text, gives no
//     piece.
//
// How a connection begins. The visitor makes its id, and the env counts from a random number:
//
//   -> [0, "3a1a...", "sensor", 0,     [<[1, "@enter"]>]]
//   <- [0, "sensor", "3a1a...", 1,     [<[40961, 1, 0]>]]
//   -> [0, "3a1a...", "sensor", 40961, [<[2, "read"]>]]
//   <- [0, "sensor", "3a1a...", 2,     [<[40962, 2, 0, <{"celsius": 21}>]>]]
//
// THE PIECES
//
// unpack: the head gives PORTAL_HEAD or PORTAL_ENDED, and leaves the reader on the entries alone, so
// that items after them are never read as entries. Each entry gives PORTAL_CALLS or PORTAL_REPLIES,
// read over the head's piece. An entry whose index can be read gives it, whatever else is broken.
// pack: an entry of mine is packed bare, and PORTAL_PACKED wraps it as it joins a frame. The list of
// entries states its length at the head, so PORTAL_FOOT adds nothing.


enum { CONNECTION = 0, ENDED = 3 }; // which frame: its first item


// In this layout, 36 and 10 are the smallest frame and queue with which a core can let one visitor
// into an env whose id is one byte, when the visitor's id is 16 bytes like the ones this library makes.
// This line does not compile below them.
typedef char smallest[PORTAL_FRAME >= 36 && PORTAL_QUEUE >= 10 ? 1 : -1];

// ───────────────────────────────── values ────────────────────────────────────

static uint32_t fail(portal_reader *r) {
  r->at = r->end = NULL;
  return 0;
}

static size_t head_size(uint32_t number) {
  return number < 24 ? 1 : number < 0x100 ? 2 : number < 0x10000 ? 3 : 5;
}

uint32_t portal_read(portal_reader *r, uint8_t type) {
  if (!r->at || r->at == r->end || *r->at >> 5 != type) return fail(r);
  uint8_t info = *r->at++ & 31;
  if (info < 24) return info;
  size_t more = (size_t)1 << (info - 24); // 1, 2 or 4 bytes follow
  if (info > 26 || (size_t)(r->end - r->at) < more) return fail(r);
  uint32_t number = 0;
  while (more--) number = number << 8 | *r->at++;
  return number;
}

portal_bytes portal_read_bytes(portal_reader *r, uint8_t type) {
  uint32_t length = portal_read(r, type);
  if (!r->at || (size_t)(r->end - r->at) < length) {
    fail(r);
    return none;
  }
  portal_bytes bytes = { r->at, length };
  r->at += length;
  return bytes;
}

static void put(portal_writer *w, const void *bytes, size_t length) {
  if (length && w->length + length <= w->capacity) memcpy(w->bytes + w->length, bytes, length);
  w->length += length;
}

void portal_write(portal_writer *w, uint8_t type, uint32_t number) {
  size_t size = head_size(number);
  uint8_t head[5] = { (uint8_t)((uint32_t)type << 5 | (size == 1 ? number : size == 2 ? 24 : size == 3 ? 25 : 26)) };
  for (size_t i = 1; i < size; i++) head[i] = (uint8_t)(number >> 8 * (size - 1 - i));
  put(w, head, size);
}

void portal_write_bytes(portal_writer *w, uint8_t type, portal_bytes bytes) {
  portal_write(w, type, (uint32_t)bytes.length);
  put(w, bytes.at, bytes.length);
}

// ───────────────────────────────── pieces ────────────────────────────────────

bool portal_cbor_unpack(portal_reader *r, portal_piece *p, portals_core *m) {
  (void)m;
  if (!p->kind) { // [0, from, to, sync, [entry, ...]] or [3, from, to, code, message?]
    uint32_t items = portal_read(r, PORTAL_LIST), which = portal_read(r, PORTAL_UINT);
    p->from = portal_read_bytes(r, PORTAL_TEXT);
    p->to = portal_read_bytes(r, PORTAL_TEXT);
    if (!r->at || items < 3 || (which != CONNECTION && which != ENDED)) return false;
    if (which == ENDED) {
      p->kind = PORTAL_ENDED;
      p->code = items > 3 ? portal_read(r, PORTAL_UINT) : 0;
      p->body = items > 4 ? portal_read_bytes(r, PORTAL_TEXT) : none;
      p->whole = r->at != NULL;
      r->end = r->at; // no entry follows
      return true;
    }
    p->kind = PORTAL_HEAD;
    p->sync = portal_read(r, PORTAL_UINT);
    p->count = portal_read(r, PORTAL_LIST);
    p->whole = r->at && items > 4;
    portal_reader entries = *r; // the reader ends where the entries end, when that can be found
    for (uint32_t i = 0; i < p->count && entries.at; i++) portal_read_bytes(&entries, PORTAL_BYTES);
    if (entries.at) r->end = entries.at;
    return true;
  }
  if (r->at == r->end) return false;
  portal_reader e = portal_reader_of(portal_read_bytes(r, PORTAL_BYTES));
  uint32_t items = portal_read(&e, PORTAL_LIST);
  p->index = portal_read(&e, PORTAL_UINT);
  bool calls = e.at && e.at < e.end && *e.at >> 5 == PORTAL_TEXT;
  p->kind = calls ? PORTAL_CALLS : PORTAL_REPLIES;
  p->name = calls ? portal_read_bytes(&e, PORTAL_TEXT) : none;
  p->reply = calls ? 0 : portal_read(&e, PORTAL_UINT);
  p->code = calls ? 0 : portal_read(&e, PORTAL_UINT);
  p->body = items > (calls ? 2u : 3u) ? portal_read_bytes(&e, calls || !p->code ? PORTAL_BYTES : PORTAL_TEXT) : none;
  p->whole = e.at != NULL;
  return true;
}

// Both frames start [which, from, to, ...].
static void frame(portal_writer *w, uint32_t items, uint32_t which, const portal_piece *p) {
  portal_write(w, PORTAL_LIST, items);
  portal_write(w, PORTAL_UINT, which);
  portal_write_bytes(w, PORTAL_TEXT, p->from);
  portal_write_bytes(w, PORTAL_TEXT, p->to);
}

bool portal_cbor_pack(portal_writer *w, const portal_piece *p, portals_core *m) {
  (void)m;
  uint32_t more = p->body.at != NULL;
  switch (p->kind) {
  case PORTAL_HEAD: // [0, from, to, sync, [: the entries follow
    frame(w, 5, CONNECTION, p);
    portal_write(w, PORTAL_UINT, p->sync);
    portal_write(w, PORTAL_LIST, p->count);
    break;
  case PORTAL_ENDED: // [3, from, to, code, message?]
    frame(w, 4 + more, ENDED, p);
    portal_write(w, PORTAL_UINT, p->code);
    if (more) portal_write_bytes(w, PORTAL_TEXT, p->body);
    break;
  case PORTAL_CALLS: // [index, name, payload?]
    portal_write(w, PORTAL_LIST, 2 + more);
    portal_write(w, PORTAL_UINT, p->index);
    portal_write_bytes(w, PORTAL_TEXT, p->name);
    if (more) portal_write_bytes(w, PORTAL_BYTES, p->body);
    break;
  case PORTAL_REPLIES: // [index, reply, code, body?]
    portal_write(w, PORTAL_LIST, 3 + more);
    portal_write(w, PORTAL_UINT, p->index);
    portal_write(w, PORTAL_UINT, p->reply);
    portal_write(w, PORTAL_UINT, p->code);
    if (more) portal_write_bytes(w, p->code ? PORTAL_TEXT : PORTAL_BYTES, p->body);
    break;
  case PORTAL_PACKED: // an entry joins its frame wrapped as a byte string
    portal_write_bytes(w, PORTAL_BYTES, p->body);
    break;
  default: // PORTAL_FOOT
    break;
  }
  return w->length <= w->capacity;
}

// ════════════════════ persistence.c ════════════════════

// The env type `persistence`: the four functions persistence.h names, answered from the app's table.
// A path, a value and a payload come from the other side: they are read only with the core's reader,
// and a name is only compared, whole, with the keys.

static const char number[] = "number", string[] = "string", boolean[] = "boolean", function[] = "function", object[] = "object";
static const char *const types[] = { number, string, boolean, function, object };

// A row's type: the type of its reader, "function" with `call`, else what `type` says, else "object".
// It is one of `types` when it is any of them, so it is compared with them as a pointer.
static const char *type_of(const persistence_property *at) {
  const char *type = at->read_number ? number : at->read_string ? string : at->read_boolean ? boolean : at->call ? function : at->type;
  for (size_t i = 0; type && i < 5; i++)
    if (!strcmp(type, types[i])) return types[i];
  return type ? type : object;
}

// A value can be set when it has the writer of its reader's type.
static bool settable(const persistence_property *at) {
  return at->read_number   ? at->write_number != NULL
         : at->read_string ? at->write_string != NULL
                           : at->read_boolean && at->write_boolean;
}

// Writes a value of `type`. A number below 0 is CBOR's negative number, which holds n as -1 - n.
static void write_value(const char *type, const persistence_value *v, portal_writer *w) {
  if (type == number) portal_write(w, v->number < 0, (uint32_t)(v->number < 0 ? -1 - v->number : v->number));
  if (type == string) portal_write_bytes(w, PORTAL_TEXT, portal_text(v->string ? v->string : ""));
  if (type == boolean) portal_write(w, 7, v->boolean ? 21 : 20); // true and false: the simple values 21 and 20 of CBOR
}

// Reads one value of `type`. False: the next item is not one, or it is a number outside int32_t.
static bool take(const char *type, portal_reader *r, persistence_value *v) {
  if (type == string) {
    portal_bytes text = portal_read_bytes(r, PORTAL_TEXT);
    v->string = (const char *)text.at;
    v->length = text.length;
    return r->at != NULL;
  }
  if (type == boolean) { // false and true are the one byte f4 and f5: a longer item that reads as 20 or 21 is refused
    bool one = r->at && r->at != r->end && (*r->at == 0xf4 || *r->at == 0xf5);
    v->boolean = portal_read(r, 7) == 21;
    return one;
  }
  bool negative = r->at && r->at != r->end && *r->at >> 5 == 1;
  uint32_t n = portal_read(r, negative);
  if (type != number || !r->at || n > INT32_MAX) return false;
  v->number = negative ? -1 - (int32_t)n : (int32_t)n;
  return true;
}

// The property `path` names, from `at` down. NULL: nothing is there, or `path` is not a list of texts
// and nothing after it. Only an object has something below it.
static const persistence_property *find_property(const persistence_property *at, portal_bytes path) {
  portal_reader r = portal_reader_of(path);
  for (uint32_t names = portal_read(&r, PORTAL_LIST); at && r.at && names; names--) {
    portal_bytes name = portal_read_bytes(&r, PORTAL_TEXT);
    size_t i = 0, count = type_of(at) == object ? at->count : 0;
    while (i < count && !portal_is(name, at->children[i].key)) i++;
    at = i < count ? &at->children[i] : NULL;
  }
  return r.at && r.at == r.end ? at : NULL;
}

// What `get` answers for a property: the value its reader returns, or a map of its properties by name,
// and so on down. A function has no value: the map leaves it out.
static void answer(const persistence_property *at, portal_writer *w) {
  if (at->read_number || at->read_string || at->read_boolean) {
    persistence_value v = { at->read_number ? at->read_number() : 0, at->read_boolean && at->read_boolean(),
                            at->read_string ? at->read_string() : NULL, 0 };
    write_value(type_of(at), &v, w);
    return;
  }
  uint32_t values = 0;
  for (size_t i = 0; i < at->count; i++) values += !at->children[i].call;
  portal_write(w, PORTAL_MAP, values);
  for (size_t i = 0; i < at->count; i++)
    if (!at->children[i].call) {
      portal_write_bytes(w, PORTAL_TEXT, portal_text(at->children[i].key));
      answer(&at->children[i], w);
    }
}

// A node of the schema: `key` in a list of children, `type`, `meta` when something is said of it, then
// `children` with an object. A node under `input` or `output` describes a value that travels: it says
// its type, description, unit and children.
static void node(const persistence_property *at, const char *key, bool travels, portal_writer *w) {
  const char *type = type_of(at);
  const char *const said[][2] = { { "key", key }, { "type", type }, { "description", at->description }, { "unit", at->unit } };
  const persistence_property *const inout[] = { travels ? NULL : at->input, travels ? NULL : at->output };
  bool writable = !travels && settable(at);
  uint32_t meta = (at->description != NULL) + (at->unit != NULL) + writable + (inout[0] != NULL) + (inout[1] != NULL);
  portal_write(w, PORTAL_MAP, (uint32_t)(key != NULL) + 1 + (meta > 0) + (type == object));
  for (size_t i = 0; i < 4; i++) {
    if (i == 2 && meta) { // what is said after the type is under `meta`
      portal_write_bytes(w, PORTAL_TEXT, portal_text("meta"));
      portal_write(w, PORTAL_MAP, meta);
    }
    if (said[i][1]) {
      portal_write_bytes(w, PORTAL_TEXT, portal_text(said[i][0]));
      portal_write_bytes(w, PORTAL_TEXT, portal_text(said[i][1]));
    }
  }
  if (writable) {
    portal_write_bytes(w, PORTAL_TEXT, portal_text("writable"));
    portal_write(w, 7, 21); // true, the simple value 21 of CBOR
  }
  for (size_t i = 0; i < 2; i++)
    if (inout[i]) {
      portal_write_bytes(w, PORTAL_TEXT, portal_text(i ? "output" : "input"));
      node(inout[i], NULL, true, w);
    }
  if (type != object) return;
  portal_write_bytes(w, PORTAL_TEXT, portal_text("children"));
  portal_write(w, PORTAL_LIST, (uint32_t)at->count);
  for (size_t i = 0; i < at->count; i++) node(&at->children[i], at->children[i].key, travels, w);
}

// Fills what a function takes from a payload, as its `input` says. False: the payload does not fit.
static bool fill(const persistence_property *input, portal_bytes payload, persistence_value *takes) {
  portal_reader r = portal_reader_of(payload);
  const char *type = type_of(input);
  bool seen[PERSISTENCE_INPUTS] = { false };
  if (type != object) return take(type, &r, takes) && r.at == r.end;
  uint32_t pairs = portal_read(&r, PORTAL_MAP);
  if (pairs != input->count || pairs > PERSISTENCE_INPUTS) return false;
  for (uint32_t i = 0; i < pairs && r.at; i++) {
    portal_bytes name = portal_read_bytes(&r, PORTAL_TEXT);
    size_t child = 0;
    while (child < pairs && !portal_is(name, input->children[child].key)) child++;
    if (child == pairs || seen[child] || !take(type_of(&input->children[child]), &r, &takes[child])) return false;
    seen[child] = true;
  }
  return r.at && r.at == r.end;
}

portal_code persistence_schema(const persistence_env *env, portal_writer *into) {
  const persistence_property root = { .children = env->children, .count = env->count };
  node(&root, NULL, false, into);
  return PORTAL_OK;
}

portal_code persistence_get(const persistence_env *env, portal_bytes path, portal_writer *into) {
  const persistence_property root = { .children = env->children, .count = env->count };
  const persistence_property *at = find_property(&root, path);
  if (!at) return PORTAL_UNKNOWN;
  if (at->call) return PORTAL_DENIED; // a function has no value
  answer(at, into);
  return PORTAL_OK;
}

portal_code persistence_set(const persistence_env *env, portal_bytes path, portal_bytes value) {
  const persistence_property root = { .children = env->children, .count = env->count };
  const persistence_property *at = find_property(&root, path);
  portal_reader r = portal_reader_of(value);
  persistence_value v = { 0, false, NULL, 0 };
  if (!at) return PORTAL_UNKNOWN;
  if (!settable(at)) return PORTAL_DENIED;
  if (!take(type_of(at), &r, &v) || r.at != r.end) return PORTAL_FAILED;
  bool done = at->read_number   ? at->write_number(v.number)
              : at->read_string ? at->write_string(v.string, v.length)
                                : at->write_boolean(v.boolean);
  return done ? PORTAL_OK : PORTAL_FAILED;
}

portal_code persistence_call(const persistence_env *env, portal_bytes path, portal_bytes payload, portal_writer *into) {
  const persistence_property root = { .children = env->children, .count = env->count };
  const persistence_property *at = find_property(&root, path);
  persistence_value takes[PERSISTENCE_INPUTS] = { { 0, false, NULL, 0 } }, gives = { 0, false, NULL, 0 };
  if (!at) return PORTAL_UNKNOWN;
  if (!at->call) return PORTAL_DENIED;
  bool fits = !at->input || fill(at->input, payload, takes), done = fits && at->call(at->input ? takes : NULL, &gives);
  if (!fits) gives.string = "the payload is not what it takes";
  if (done && at->output) write_value(type_of(at->output), &gives, into);
  if (!done && gives.string) portal_write_bytes(into, PORTAL_TEXT, portal_text(gives.string));
  return done ? PORTAL_OK : PORTAL_FAILED;
}

// ════════════════════ portal-persistence.c ════════════════════

// The env's four actions as calls through the portals core: the call's payload is read into a path and
// what follows it, one of the four functions of persistence.h answers, and that answer is the reply.
// And the env exposed with its description.


bool portal_persistence_call(portals_core *m, const persistence_env *env, const portal_event *call) {
  bool schema = portal_is(call->method, "schema"), get = portal_is(call->method, "get"), set = portal_is(call->method, "set");
  if (!schema && !get && !set && !portal_is(call->method, "call")) return false;
  uint8_t room[PORTAL_PERSISTENCE_ROOM];
  portal_writer w = { room, sizeof room, 0 };
  portal_code code;
  if (schema) code = persistence_schema(env, &w);
  else if (get) code = persistence_get(env, call->body, &w);
  else { // `set` takes [path, value], `call` [path] or [path, payload]
    portal_reader r = portal_reader_of(call->body);
    uint32_t items = portal_read(&r, PORTAL_LIST);
    portal_bytes path = { r.at, 0 };
    for (uint32_t names = portal_read(&r, PORTAL_LIST); r.at && names; names--) portal_read_bytes(&r, PORTAL_TEXT);
    path.length = r.at ? (size_t)(r.at - path.at) : 0;
    portal_bytes after = { r.at, r.at ? (size_t)(r.end - r.at) : 0 }; // what follows the path
    if (!r.at || items != (after.length ? 2u : 1u) || (set && items != 2)) code = PORTAL_UNKNOWN;
    else code = set ? persistence_set(env, path, after) : persistence_call(env, path, items == 2 ? after : none, &w);
  }
  // An answer that does not fit its room is failed. With an error, a text that was written is the message.
  portal_bytes body = { room, w.length <= w.capacity ? w.length : 0 };
  portal_reader message = portal_reader_of(body);
  if (w.length > w.capacity) code = PORTAL_FAILED;
  if (code) body = portal_read_bytes(&message, PORTAL_TEXT);
  if (!body.length) body = none;
  // So is an answer that does not fit a frame.
  if (!portal_reply(m, &call->ref, code, body) && body.at) portal_reply(m, &call->ref, PORTAL_FAILED, none);
  return true;
}

bool portal_persistence_expose(portals_core *m, const char *id, const char *description, uint8_t *room, size_t size) {
  const char *const said[] = { "id", id, "description", description, "type", "persistence", "actions" };
  portal_writer w = { room, size, 0 };
  portal_write(&w, PORTAL_MAP, 4);
  for (size_t i = 0; i < 7; i++) portal_write_bytes(&w, PORTAL_TEXT, portal_text(said[i]));
  portal_write(&w, PORTAL_MAP, 0); // no actions of its own
  return w.length <= w.capacity && portal_expose(m, id, (portal_bytes){ room, w.length });
}

// ════════════════════ ether.c ════════════════════

// What joins an ether to the portals core: plugging it in, the three ether members of the core's table, and
// what an ether hands on from its poll. ether.h says what each does.

void ether_plug(ether *e, portals_core *core, void (*handle)(const portal_event *event, portals_core *m)) {
  e->core = core;
  e->handle = handle;
  setInState(core, context, e);
}

void ether_send(uint32_t link, const uint8_t *frame, size_t length, portals_core *m) {
  ether *e = getFromState(m, context);
  e->send(e, link, frame, length);
}

void ether_listen(portal_bytes id, uint32_t link, portals_core *m) {
  ether *e = getFromState(m, context);
  e->listen(e, id, link);
}

void ether_unlisten(portal_bytes id, uint32_t link, portals_core *m) {
  ether *e = getFromState(m, context);
  e->unlisten(e, id, link);
}

void ether_receive(ether *e, uint32_t link, uint32_t who, const uint8_t *frame, size_t length) {
  portal_arrival arrival = portal_arrive(frame, length, link, who);
  for (portal_event event; portal_next(e->core, &arrival, &event);) e->handle(&event, e->core);
}

void ether_ended(ether *e, uint32_t link, const char *why) {
  portal_bytes message = portal_text(why);
  for (size_t at = 0; at < PORTAL_CONNECTIONS; at++) {
    portal_connection *c = &getFromState(e->core, connections)[at];
    portal_event event = { 0 };
    if (!getFromState(c, handle) || getFromState(c, link) != link) continue;
    e->core->api->end(c, &event, PORTAL_GONE, message, false, e->core);
    if (event.type) e->handle(&event, e->core);
  }
}

// ════════════════════ ether-ble.c ════════════════════

// The Bluetooth LE ether: frames cut into packets and joined again, and the four functions of an ether over
// the links of a stack. ether-ble.h says what each function does.

// The limits the code relies on: this line does not compile when one of them is broken.
typedef char limits[ETHER_BLE_LINKS >= 1 && ETHER_BLE_FRAME >= 1 && ETHER_BLE_PACKET >= 2 ? 1 : -1];

// ───────────────────────────────── packets ───────────────────────────────────

size_t ether_ble_next(ether_ble_cut *cut, const uint8_t *frame, size_t length, uint8_t *packet, size_t room) {
  size_t left = length - cut->at;
  // What is left has to fit the places that are left, at this room.
  if (room < 2 || !left || (left - 1) / (room - 1) >= (size_t)ETHER_BLE_PACKETS - cut->place) return 0;
  size_t size = left < room - 1 ? left : room - 1;
  packet[0] = (uint8_t)(cut->place | (size == left ? ETHER_BLE_LAST : 0u));
  memcpy(packet + 1, frame + cut->at, size);
  cut->at += size;
  cut->place++;
  return size + 1;
}

bool ether_ble_take(ether_ble_joined *joined, const uint8_t *packet, size_t size) {
  if (size && !(packet[0] & ETHER_BLE_PLACE)) { // place 0 begins a frame, and drops one that was unfinished
    joined->length = 0;
    joined->place = 0;
    joined->open = true;
  }
  // A frame goes on with a packet that has bytes, at the place that is due, while it fits. Otherwise it is
  // dropped, and packets are passed over until the next place 0.
  joined->open = joined->open && size > 1 && (packet[0] & ETHER_BLE_PLACE) == joined->place &&
                 size - 1 <= joined->capacity - joined->length;
  if (!joined->open) return false;
  memcpy(joined->frame + joined->length, packet + 1, size - 1);
  joined->length += size - 1;
  joined->place++;
  joined->open = !(packet[0] & ETHER_BLE_LAST);
  return !joined->open;
}

// ───────────────────────────────── the ether ─────────────────────────────────
// A place that a link takes again starts with nothing joined, so freeing it forgets what was half joined there.

// The place of a link: its own, or else a free one. ETHER_BLE_LINKS: neither, or the link is 0.
static size_t place(const ether_ble *ble, uint32_t link) {
  size_t spare = ETHER_BLE_LINKS;
  for (size_t i = 0; link && i < ETHER_BLE_LINKS; i++) {
    if (ble->links[i] == link) return i;
    if (!ble->links[i] && spare == ETHER_BLE_LINKS) spare = i;
  }
  return spare;
}

void ether_ble_listen(ether *e, portal_bytes id, uint32_t link) {
  (void)id, (void)link;
  ((ether_ble *)e)->listened++;
}

void ether_ble_unlisten(ether *e, portal_bytes id, uint32_t link) {
  ether_ble *ble = (ether_ble *)e;
  (void)id, (void)link;
  if (ble->listened && !--ble->listened) memset(ble->links, 0, sizeof ble->links);
}

void ether_ble_send(ether *e, uint32_t link, const uint8_t *frame, size_t length) {
  ether_ble *ble = (ether_ble *)e;
  size_t room = ble->stack->room(ble, link), size;
  ether_ble_cut cut = { 0, 0 };
  if (room > ETHER_BLE_PACKET) room = ETHER_BLE_PACKET;
  do size = ether_ble_next(&cut, frame, length, ble->packet, room);
  while (size && ble->stack->send(ble, link, ble->packet, size));
}

bool ether_ble_poll(ether *e, int32_t wait_ms) {
  ether_ble *ble = (ether_ble *)e;
  uint32_t link;
  const uint8_t *packet;
  size_t size;
  if (!ble->stack->receive(ble, &link, &packet, &size, wait_ms) || !ble->listened) return false;
  size_t i = place(ble, link);
  if (!size) { // the end of the link
    if (i < ETHER_BLE_LINKS) ble->links[i] = 0;
    ether_ended(e, link, "the Bluetooth connection ended");
    return true;
  }
  if (i < ETHER_BLE_LINKS && !ble->links[i]) {
    ble->links[i] = link;
    ble->joined[i] = (ether_ble_joined){ ble->frames[i], ETHER_BLE_FRAME, 0, 0, false };
  }
  if (i < ETHER_BLE_LINKS && ether_ble_take(&ble->joined[i], packet, size)) ether_receive(e, link, 0, ble->frames[i], ble->joined[i].length);
  return true;
}

// ════════════════════ ether-ble-zephyr.c ════════════════════

// The Bluetooth LE ether on Zephyr: the stack under ether-ble.h. Bluetooth's own thread only queues what
// arrives: a packet a visitor wrote, or the end of a connection. The core's thread takes them one at a time in
// `receive`, and its frames leave as notifications. A link is a connection's index plus one.
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/crypto.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

// Something Bluetooth brought, waiting for the core's thread.
struct item {
  uint32_t link;
  uint16_t size;                              // 0: the connection ended
  uint8_t packet[CONFIG_BT_L2CAP_TX_MTU - 3]; // the longest value a write can carry in this build
};

// CONFIG_BT_MAX_CONN places of the queue are kept for the ends: this line does not compile when none is left.
typedef char places[ETHER_BLE_WAITING > CONFIG_BT_MAX_CONN ? 1 : -1];

K_MSGQ_DEFINE(ether_ble_zephyr_waiting, sizeof(struct item), ETHER_BLE_WAITING, 4);

// ───────────────────────────── Bluetooth's thread ────────────────────────────

static ssize_t written(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset,
                       uint8_t flags) {
  static struct item item;
  (void)attr;
  if (offset) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  if (flags & (BT_GATT_WRITE_FLAG_PREPARE | BT_GATT_WRITE_FLAG_EXECUTE)) return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
  if (!len || len > sizeof item.packet) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  if (k_msgq_num_free_get(&ether_ble_zephyr_waiting) <= CONFIG_BT_MAX_CONN) return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
  item.link = bt_conn_index(conn) + 1u;
  item.size = len;
  memcpy(item.packet, buf, len);
  (void)k_msgq_put(&ether_ble_zephyr_waiting, &item, K_NO_WAIT);
  return len;
}

// The attributes, in order: the service, `input` (declaration, value), `output` (declaration, value), and the
// descriptor a visitor subscribes to `output` with.
BT_GATT_SERVICE_DEFINE(ether_ble_zephyr_service, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ETHER_BLE_SERVICE_BYTES)),
                       BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ETHER_BLE_INPUT_BYTES),
                                              BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE, NULL,
                                              written, NULL),
                       BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ETHER_BLE_OUTPUT_BYTES), BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_NONE, NULL, NULL, NULL),
                       BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

enum { OUTPUT = 4 }; // the value of `output`, among the attributes

// The end goes into the queue after the connection's packets. When no place is left, the packets give way.
static void disconnected(struct bt_conn *conn, uint8_t reason) {
  static struct item end;
  (void)reason;
  end.link = bt_conn_index(conn) + 1u;
  end.size = 0;
  if (k_msgq_put(&ether_ble_zephyr_waiting, &end, K_NO_WAIT)) {
    k_msgq_purge(&ether_ble_zephyr_waiting);
    (void)k_msgq_put(&ether_ble_zephyr_waiting, &end, K_NO_WAIT);
  }
}

BT_CONN_CB_DEFINE(ether_ble_zephyr_connections) = { .disconnected = disconnected };

// ───────────────────────────── the core's thread ─────────────────────────────

// The connection behind a link, with a reference that the caller gives back. NULL: it is not there.
struct found {
  uint32_t link;
  struct bt_conn *conn;
};
static void match(struct bt_conn *conn, void *data) {
  struct found *found = data;
  if (!found->conn && bt_conn_index(conn) + 1u == found->link) found->conn = bt_conn_ref(conn);
}
static struct bt_conn *connection(uint32_t link) {
  struct found found = { link, NULL };
  bt_conn_foreach(BT_CONN_TYPE_LE, match, &found);
  return found.conn;
}

static size_t room(ether_ble *ble, uint32_t link) {
  struct bt_conn *conn = connection(link);
  uint16_t mtu = conn ? bt_gatt_get_mtu(conn) : 0;
  (void)ble;
  if (conn) bt_conn_unref(conn);
  return mtu > 3 ? mtu - 3u : 0;
}

// One notification. Zephyr keeps a thread of its own, as the core's is, waiting until it has a buffer. An
// error gives the frame up, and the core says it again later.
static bool notify(ether_ble *ble, uint32_t link, const uint8_t *packet, size_t size) {
  struct bt_conn *conn = connection(link);
  bool sent = conn && !bt_gatt_notify(conn, &ether_ble_zephyr_service.attrs[OUTPUT], packet, (uint16_t)size);
  (void)ble;
  if (conn) bt_conn_unref(conn);
  return sent;
}

// The next item of the queue, which stays where it is until the next call.
static bool receive(ether_ble *ble, uint32_t *link, const uint8_t **packet, size_t *size, int32_t wait_ms) {
  static struct item item;
  (void)ble;
  if (k_msgq_get(&ether_ble_zephyr_waiting, &item, K_MSEC(wait_ms))) return false;
  *link = item.link;
  *packet = item.packet;
  *size = item.size;
  return true;
}

static const ether_ble_stack stack = { room, notify, receive };
// Filled when it is first asked for. Started here, the whole of it, nearly all zeros, would be stored in flash as well.
static ether_ble ble;

ether *ether_ble_zephyr(void) {
  if (!ble.stack) {
    ble.ether = (ether){ .listen = ether_ble_listen, .unlisten = ether_ble_unlisten, .send = ether_ble_send, .poll = ether_ble_poll };
    ble.stack = &stack;
  }
  return &ble.ether;
}

uint32_t ether_ble_zephyr_random(portals_core *m) {
  uint32_t number;
  (void)m;
  if (bt_rand(&number, sizeof number)) number = k_cycle_get_32();
  return number;
}
