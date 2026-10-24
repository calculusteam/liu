/*
 * Liu — single-line text editing for chrome fields.
 *
 * Liu's chrome inputs (command palette, tab rename, search bar, browser URL
 * bar, dialog fields) get the editing the rest of macOS provides — caret
 * movement, selection, word and line deletion, undo, clipboard — from this
 * module, the one implementation of that behaviour; a field adopts it by
 * keeping two extra i32s (caret + selection anchor) and routing its key
 * handler here.
 *
 * Storage stays caller-owned — AppState already owns every field's buffer and
 * length, and plenty of code resets those directly — so TextEdit is a
 * short-lived view onto them, constructed at the call site each time.
 *
 * Offsets are BYTE offsets into a UTF-8 buffer and always land on a character
 * boundary. `sel` is the fixed end of the selection and `caret` the moving
 * end, so a selection can be extended in either direction; sel == caret means
 * no selection.
 */
#ifndef UI_TEXTEDIT_H
#define UI_TEXTEDIT_H

#include "core/types.h"

/* Field is a secret (passphrase, vault password, SSH password, keyboard-
 * interactive response). Cmd+C / Cmd+X are refused so the value cannot be
 * lifted out of a masked field onto the system clipboard; paste still works,
 * and callers should pass a NULL undo history so the plaintext is not also
 * held in a snapshot ring. */
#define TEXTEDIT_SECRET  (1u << 0)
/* Field accepts digits only — inserts and pastes are filtered, so a pasted
 * "8080\n" or ":3000" cannot smuggle non-numeric bytes into a port field. */
#define TEXTEDIT_DIGITS  (1u << 1)

typedef struct {
    char *buf;      /* NUL-terminated UTF-8, caller-owned */
    i32  *len;      /* bytes in use, excluding the NUL */
    i32   cap;      /* sizeof(buf) — the NUL must fit inside it */
    i32  *caret;    /* moving end of the selection / insertion point */
    i32  *sel;      /* fixed end of the selection; == *caret ⇒ no selection */
    u32   flags;    /* TEXTEDIT_* */
} TextEdit;

/* Undo uses whole-buffer snapshots. Chrome fields are short, so snapshots
 * avoid per-operation bookkeeping and record each edit. If a field exceeds
 * TEXTEDIT_UNDO_CAP, edits continue without adding undo snapshots; existing
 * text is not truncated. */
#define TEXTEDIT_UNDO_CAP    256
#define TEXTEDIT_UNDO_DEPTH  24

typedef struct {
    char text[TEXTEDIT_UNDO_DEPTH][TEXTEDIT_UNDO_CAP];
    i32  caret[TEXTEDIT_UNDO_DEPTH];
    i32  count;      /* snapshots currently held */
    i32  pos;        /* index of the snapshot matching the live buffer */
    bool typing;     /* a run of plain insertions is being coalesced */
} TextEditUndo;

void textedit_undo_reset(TextEditUndo *u);

/* Insert one codepoint, replacing any selection. Control characters and DEL
 * are ignored. Consecutive calls coalesce into a single undo step, so Cmd+Z
 * removes a typed word rather than one letter. */
void textedit_insert_cp(TextEdit *e, TextEditUndo *u, u32 cp);

/* Handle a key press. Returns true when the key was consumed, false when the
 * caller should fall through to its own handling.
 *
 * Deliberately NOT consumed, so list-style hosts keep their bindings:
 *   Up / Down / PageUp / PageDown / Home / End / Enter / Escape / Tab
 * Line-start and line-end are reachable through Cmd+Left / Cmd+Right and the
 * Ctrl+A / Ctrl+E emacs pair instead.
 *
 * Consumed:
 *   Left / Right                 char motion        (+Shift extends)
 *   Opt+Left / Opt+Right         word motion        (+Shift extends)
 *   Cmd+Left / Cmd+Right         line ends          (+Shift extends)
 *   Ctrl+A / Ctrl+E              line ends
 *   Backspace / Delete           char, or selection
 *   Opt+Backspace / Opt+Delete   word back / forward
 *   Cmd+Backspace / Ctrl+U       delete to line start
 *   Ctrl+K                       delete to line end
 *   Ctrl+W                       delete word back
 *   Cmd+A                        select all
 *   Cmd+C / Cmd+X / Cmd+V        copy / cut / paste
 *   Cmd+Z / Cmd+Shift+Z          undo / redo
 */
bool textedit_key(TextEdit *e, TextEditUndo *u, i32 key, u32 mods);

/* True when a non-empty selection exists. */
bool textedit_has_selection(const TextEdit *e);

/* Selection bounds as byte offsets, normalised so *from <= *to. */
void textedit_selection(const TextEdit *e, i32 *from, i32 *to);

/* Character (not byte) index of a byte offset — what a monospaced renderer
 * needs to place the caret and the selection rectangle. */
i32 textedit_cell_index(const char *buf, i32 byte_off);

/* Inverse of textedit_cell_index: byte offset of the `cell`-th character.
 * Clamps to `len`. Lets a renderer scroll a long value horizontally by
 * character rather than by byte. */
i32 textedit_byte_offset(const char *buf, i32 len, i32 cell);

/* Clamp caret/anchor into range and onto UTF-8 boundaries. Call after code
 * outside this module rewrites the buffer (a mode switch clearing the query,
 * a programmatic prefill, …). */
void textedit_clamp(TextEdit *e);

#endif /* UI_TEXTEDIT_H */
