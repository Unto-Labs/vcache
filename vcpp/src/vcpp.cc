// vcpp -- a preprocessor for compilation caching.
//
// Copyright (C) 2026 Unto Labs
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Why GPLv3 while the rest of this repository is Apache-2.0: vcpp is built on
// GCC's libcpp, which is GPLv3 with no runtime-library exception. vcache runs
// vcpp the way it already runs gcc -- as a separate program -- so nothing here
// is linked into vcache and vcache's own licence is unaffected. See README.md.
//
// The output half of this file -- the token streamer, the linemarker logic and
// the option preamble -- is a port of GCC's gcc/c-family/c-ppoutput.cc and the
// preamble sequence in c_finish_options from gcc/c-family/c-opts.cc, both
// Copyright (C) 1995-2026 Free Software Foundation, Inc. and GPLv3. It is a
// port rather than a copy because those files include gcc front-end headers
// (c-common.h, langhooks.h, c-pragma.h) that pull in most of the compiler; the
// gcc-side helpers they use are reimplemented here over libcpp directly.
//
// Byte-identity with the installed gcc is the acceptance test, because that is
// what lets vcache keep exactly the guarantee it has today: the text it hashes
// is the text the compiler will consume. Anything "equivalent" is a wrong
// object waiting to happen, so the spacing rules below are followed exactly
// rather than approximated.

#include "config.h"
#include "system.h"
#include "line-map.h"
#include "cpplib.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <map>

class line_maps *line_table;

// libcpp expects its client (normally gcc) to supply these two.
void fancy_abort (const char *file, int line, const char *fn)
{ // _exit, not abort: abort re-enters libcpp's diagnostic path and recurses.
  fprintf (stderr, "vcpp: internal error at %s:%d in %s\n", file, line, fn);
  fflush (stderr); _exit (2); }

// libcpp requires this one -- cpp_diagnostic_at aborts outright without it.
static bool cb_diagnostic (cpp_reader *, enum cpp_diagnostic_level level,
                           enum cpp_warning_reason, rich_location *,
                           const char *msgid, va_list *ap)
{
  fprintf (stderr, "vcpp: ");
  vfprintf (stderr, msgid, *ap);
  fputc ('\n', stderr);
  return level != CPP_DL_ERROR && level != CPP_DL_ICE && level != CPP_DL_FATAL;
}

// gcc keeps this in input.h; vcpp owns its own. libcpp 16 passes the line_maps
// explicitly, where 13 read the global.
#if VCPP_LIBCPP_MAJOR >= 14
expanded_location
linemap_client_expand_location_to_spelling_point (const line_maps *set,
                                                  location_t loc,
                                                  enum location_aspect)
{ return linemap_expand_location (set, linemap_lookup (set, loc), loc); }
#else
expanded_location
linemap_client_expand_location_to_spelling_point (location_t loc,
                                                  enum location_aspect)
{ return linemap_expand_location (line_table,
                                  linemap_lookup (line_table, loc), loc); }
#endif
static cpp_reader *pfile;
static FILE *out;

// -P.  Named for the gcc variable this logic came from.
static bool flag_no_line_commands = false;

// gcc/input.h spells these; libcpp only defines RESERVED_LOCATION_COUNT.
static const location_t UNKNOWN_LOCATION = 0;
static const location_t BUILTINS_LOCATION = 1;

static const char *special_fname_builtin () { return "<built-in>"; }

// gcc/input.cc:expand_location_1, for the caret aspect at the expansion point,
// which is all the output driver asks for.
static expanded_location expand_loc_1 (location_t loc)
{
  expanded_location xloc;
  memset (&xloc, 0, sizeof xloc);
  if (loc >= RESERVED_LOCATION_COUNT)
    {
      const line_map_ordinary *map;
      loc = linemap_resolve_location (line_table, loc,
                                      LRK_MACRO_EXPANSION_POINT, &map);
      xloc = linemap_expand_location (line_table, map, loc);
    }
  if (loc <= BUILTINS_LOCATION)
    xloc.file = loc == UNKNOWN_LOCATION ? nullptr : special_fname_builtin ();
  return xloc;
}

// Resolving a location is the single most expensive thing this program does:
// linemap_lookup_macro_index alone was 19.6% of the run before this cache.
// Two things make it cacheable.
//
// The driver asks about the same location several times in a row -- the
// streamer wants its line, maybe_print_line wants its line and its file, and
// do_line_change wants its column -- so an exact-match memo collapses those to
// one resolution.
//
// Better, every token produced by one macro expansion resolves to the *same*
// expansion point. linemap_macro_map_loc_to_exp_point takes a location but
// marks it ATTRIBUTE_UNUSED and returns the map's expansion point, so the
// answer depends on the map alone. Caching the map's whole location range
// turns one resolution per expanded token into one per expansion, which is
// where the bulk of the macro-index lookups were going.
//
// Safe because a location's resolution never changes: line maps are
// append-only, and the map a location belongs to is fixed when the location is
// handed out. Ad-hoc locations live above LINE_MAP_MAX_LOCATION and so can
// never fall inside a cached range.
// One-entry memo of "which macro map does this location belong to". Finding
// that map is a binary search (linemap_lookup_macro_index), and libcpp has no
// cache for it the way it does for ordinary maps -- but one macro expansion
// hands out a contiguous run of locations, so one search serves the whole run.
static location_t mm_lo = 1, mm_hi = 0;              // empty: lo > hi
static const line_map_macro *mm_map = nullptr;

// The macro map containing LOC, or null if LOC is not from a macro expansion.
static inline const line_map_macro *macro_map_for (location_t loc)
{
  if (loc >= mm_lo && loc < mm_hi)
    return mm_map;
  if (loc < RESERVED_LOCATION_COUNT)
    return nullptr;
  const line_map *map = linemap_lookup (line_table, loc);
  if (!map || !linemap_macro_expansion_map_p (map))
    return nullptr;
  mm_map = linemap_check_macro (map);
  mm_lo = MAP_START_LOCATION (mm_map);
  mm_hi = mm_lo + MACRO_MAP_NUM_MACRO_TOKENS (mm_map);
  return mm_map;
}

static location_t xloc_lo = 1, xloc_hi = 0;   // empty: lo > hi
static expanded_location xloc_cached;

static inline expanded_location expand_loc (location_t loc)
{
  if (loc >= xloc_lo && loc < xloc_hi)
    return xloc_cached;

  xloc_cached = expand_loc_1 (loc);

  // Widen the cache to the whole macro map when this location came from one.
  const line_map_macro *mm = macro_map_for (loc);
  if (mm)
    { xloc_lo = mm_lo; xloc_hi = mm_hi; }
  else
    { xloc_lo = loc; xloc_hi = loc + 1; }
  return xloc_cached;
}

static const char *LOCATION_FILE (location_t l) { return expand_loc (l).file; }
static int LOCATION_LINE   (location_t l) { return expand_loc (l).line; }
static int LOCATION_COLUMN (location_t l) { return expand_loc (l).column; }

// Asked once per token, and again by print_line_1 for the location it just
// expanded. Same reasoning as the cache above.
static location_t sysp_cached_at = UNKNOWN_LOCATION;
static int sysp_cached = 0;
static bool sysp_cache_valid = false;

static inline int in_system_header_at (location_t loc)
{
  if (sysp_cache_valid && loc == sysp_cached_at)
    return sysp_cached;

  // System-ness is *not* a property of the macro map -- libcpp unwinds toward
  // the spelling location, so a token from the macro body answers for the file
  // the macro was defined in while a token from an argument answers for where
  // the argument was written. But that unwind is an array index once the map is
  // known, and the map is what the memo above supplies, so this does libcpp's
  // first iteration without its binary search and hands the rest back.
  location_t q = loc;
  const line_map_macro *mm = macro_map_for (loc);
  if (mm)
    {
      location_t spelt
        = linemap_macro_map_loc_unwind_toward_spelling (line_table, mm, loc);
      q = (spelt < RESERVED_LOCATION_COUNT)
            ? MACRO_MAP_EXPANSION_POINT_LOCATION (mm) : spelt;
    }

  sysp_cached = linemap_location_in_system_header_p (line_table, q);
  sysp_cached_at = loc;
  sysp_cache_valid = true;
  return sysp_cached;
}

static bool is_location_from_builtin_token (location_t loc)
{
  const line_map_ordinary *map = nullptr;
  loc = linemap_resolve_location (line_table, loc, LRK_SPELLING_LOCATION, &map);
  return loc == BUILTINS_LOCATION;
}

// ---------------------------------------------------------------------------
// The print state, from c-ppoutput.cc.

static struct
{
  FILE *outf;                 // stream to write to
  const cpp_token *prev;      // previous token
  const cpp_token *source;    // source token for spacing
  unsigned src_line;          // line number currently being written
  bool printed;               // true if something output at line
  bool first_time;            // file_change hasn't been called yet
  bool prev_was_system_token;
  const char *src_file;       // current source file
} print;

static bool print_line_1 (location_t src_loc, const char *special_flags,
                          FILE *stream)
{
  bool emitted_line_marker = false;

  if (print.printed)
    putc ('\n', stream);
  print.printed = false;

  if (src_loc != UNKNOWN_LOCATION && !flag_no_line_commands)
    {
      const char *file_path = LOCATION_FILE (src_loc);
      size_t to_file_len = strlen (file_path);
      // cpp_quote_string does not NUL-terminate; we do it ourselves.
      std::vector<unsigned char> quoted (to_file_len * 4 + 1);
      unsigned char *p = cpp_quote_string (quoted.data (),
                                           (const unsigned char *) file_path,
                                           to_file_len);
      *p = '\0';

      print.src_line = LOCATION_LINE (src_loc);
      print.src_file = file_path;

      fprintf (stream, "# %u \"%s\"%s", print.src_line, quoted.data (),
               special_flags);

      int sysp = in_system_header_at (src_loc);
      if (sysp == 2)      fputs (" 3 4", stream);
      else if (sysp == 1) fputs (" 3", stream);

      putc ('\n', stream);
      emitted_line_marker = true;
    }

  return emitted_line_marker;
}

static bool print_line (location_t src_loc, const char *special_flags)
{ return print_line_1 (src_loc, special_flags, print.outf); }

static bool maybe_print_line_1 (location_t src_loc, FILE *stream)
{
  bool emitted_line_marker = false;
  unsigned src_line = LOCATION_LINE (src_loc);
  const char *src_file = LOCATION_FILE (src_loc);

  // End the previous line of text.
  if (print.printed)
    {
      putc ('\n', stream);
      print.src_line++;
      print.printed = false;
    }

  // Within a few lines of where we are, walk there with newlines rather than
  // spending a linemarker -- this is what keeps the output compact.
  if (!flag_no_line_commands
      && src_line >= print.src_line
      && src_line < print.src_line + 8
      && src_loc != UNKNOWN_LOCATION
      && src_file && print.src_file
      && strcmp (src_file, print.src_file) == 0)
    {
      while (src_line > print.src_line)
        {
          putc ('\n', stream);
          print.src_line++;
        }
    }
  else
    emitted_line_marker = print_line_1 (src_loc, "", stream);

  return emitted_line_marker;
}

static bool maybe_print_line (location_t src_loc)
{ return maybe_print_line_1 (src_loc, print.outf); }

static bool do_line_change (cpp_reader *pf, const cpp_token *token,
                            location_t src_loc, int parsing_args)
{
  if (token->type == CPP_EOF || parsing_args)
    return false;

  bool emitted_line_marker = maybe_print_line (src_loc);
  print.prev = 0;
  print.source = 0;

  // Supply enough spaces to put this token in its original column, one space
  // per column greater than 2, since the streamer will provide a space if
  // PREV_WHITE. Tabs are deliberately not reconstructed -- gcc does not
  // either, and matching gcc is the whole point.
  if (!cpp_get_options (pf)->traditional && token->type != CPP_PRAGMA)
    {
      int spaces = LOCATION_COLUMN (src_loc) - 2;
      print.printed = true;
      while (-- spaces >= 0)
        putc (' ', print.outf);
    }

  return emitted_line_marker;
}

static void cb_line_change (cpp_reader *pf, const cpp_token *token,
                            int parsing_args)
{ do_line_change (pf, token, token->src_loc, parsing_args); }

static void account_for_newlines (const unsigned char *str, size_t len)
{
  while (len--)
    if (*str++ == '\n')
      print.src_line++;
}

// ---------------------------------------------------------------------------
// The token streamer: c-ppoutput.cc's token_streamer::stream. The spacing
// rules here are the subtle part, and are followed to the letter.

// gcc registers a few pragmas with libcpp even when only preprocessing, and
// that changes the output: a registered pragma arrives as a CPP_PRAGMA token
// and is printed by the streamer, while an unregistered one goes through the
// def_pragma callback and leaves a CPP_PADDING behind, which the next token
// then turns into a spurious linemarker. So the set registered here has to
// match the set gcc registers, not merely be non-empty.
//
// c-pragma.cc:c_register_pragma_1 keeps a pragma in preprocess-only mode only
// if it allows expansion or has an early handler. Of gcc's list that is "GCC
// diagnostic"; "weak", "visibility", "target", "pack" and the rest are dropped
// and correctly reach def_pragma.
struct pp_pragma { const char *space; const char *name; };
static const pp_pragma pp_pragmas[] = {
  { "GCC", "diagnostic" },
};
static const unsigned PRAGMA_ID_BASE = 1;

static void lookup_pragma (unsigned id, const char **space, const char **name)
{
  unsigned i = id - PRAGMA_ID_BASE;
  if (i < sizeof pp_pragmas / sizeof pp_pragmas[0])
    { *space = pp_pragmas[i].space; *name = pp_pragmas[i].name; }
  else
    { *space = nullptr; *name = "<unknown>"; }
}

static bool avoid_paste = false;
static bool in_pragma = false;
static bool do_line_adjustments = true;

static void stream_token (cpp_reader *pf, const cpp_token *token, location_t loc)
{
  if (token->type == CPP_PADDING)
    {
      avoid_paste = true;
      if (print.source == NULL
          || (!(print.source->flags & PREV_WHITE)
              && token->val.source == NULL))
        print.source = token->val.source;
      return;
    }

  if (token->type == CPP_EOF)
    return;

  // Track moving into and out of system headers, so a linemarker can record
  // the change.
  const bool is_system_token = in_system_header_at (loc);
  const bool system_state_changed
    = (is_system_token != print.prev_was_system_token);
  print.prev_was_system_token = is_system_token;

  // Output a space if and only if necessary.
  bool line_marker_emitted = false;
  if (avoid_paste)
    {
      unsigned src_line = LOCATION_LINE (loc);

      if (print.source == NULL)
        print.source = token;

      if (src_line != print.src_line && do_line_adjustments && !in_pragma)
        {
          line_marker_emitted = do_line_change (pf, token, loc, false);
          putc (' ', print.outf);
          print.printed = true;
        }
      else if (print.source->flags & PREV_WHITE
               || (print.prev && cpp_avoid_paste (pf, print.prev, token))
               || (print.prev == NULL && token->type == CPP_HASH))
        {
          putc (' ', print.outf);
          print.printed = true;
        }
    }
  else if (token->flags & PREV_WHITE && token->type != CPP_PRAGMA)
    {
      unsigned src_line = LOCATION_LINE (loc);

      if (src_line != print.src_line && do_line_adjustments && !in_pragma)
        line_marker_emitted = do_line_change (pf, token, loc, false);
      putc (' ', print.outf);
      print.printed = true;
    }

  avoid_paste = false;
  print.source = NULL;
  print.prev = token;

  if (token->type == CPP_PRAGMA)
    {
      in_pragma = true;
      const char *space, *name;
      line_marker_emitted = maybe_print_line (token->src_loc);
      fputs ("#pragma ", print.outf);
      lookup_pragma (token->val.pragma, &space, &name);
      if (space) fprintf (print.outf, "%s %s", space, name);
      else       fprintf (print.outf, "%s", name);
      print.printed = true;
    }
  else if (token->type == CPP_PRAGMA_EOL)
    {
      maybe_print_line (UNKNOWN_LOCATION);
      in_pragma = false;
    }
  else
    {
      if (do_line_adjustments
          && !in_pragma
          && !line_marker_emitted
          && system_state_changed
          && !is_location_from_builtin_token (loc))
        // This token's system-ness differs from the previous one's; mark it
        // before emitting the token.
        line_marker_emitted = do_line_change (pf, token, loc, false);

      cpp_output_token (token, print.outf);
      print.printed = true;
    }

  // Comments and raw strings can carry embedded newlines.
  if (cpp_token_val_index (token) == CPP_TOKEN_FLD_STR)
    account_for_newlines (token->val.str.text, token->val.str.len);
}

// ---------------------------------------------------------------------------
// File and directive callbacks.

static void push_command_line_include ();

// c-ppoutput.cc:pp_file_change.
static void cb_file_change (cpp_reader *, const line_map_ordinary *map)
{
  const char *flags = "";

  if (!flag_no_line_commands && map != NULL)
    {
      if (print.first_time)
        {
          print_line (map->start_location, flags);
          print.first_time = false;
        }
      else
        {
          // Bring the current file to the right line before entering a new one.
          if (map->reason == LC_ENTER)
            {
              maybe_print_line (linemap_included_from (map));
              flags = " 1";
            }
          else if (map->reason == LC_LEAVE)
            flags = " 2";
          print_line (map->start_location, flags);
        }
    }

  // c-opts.cc:cb_file_change -- leaving the main file is what advances the
  // command-line include sequence.
  if (map == 0 || (map->reason == LC_LEAVE && MAIN_FILE_P (map)))
    push_command_line_include ();
}

static void cb_def_pragma (cpp_reader *pf, location_t line)
{
  maybe_print_line (line);
  fputs ("#pragma ", print.outf);
  cpp_output_line (pf, print.outf);
  print.printed = false;
  print.src_line++;
}

static void cb_ident (cpp_reader *, location_t line, const cpp_string *str)
{
  maybe_print_line (line);
  fprintf (print.outf, "#ident %s\n", str->text);
  print.src_line++;
}

// ---------------------------------------------------------------------------

static const char *main_input_filename;
static std::vector<std::string> include_opts;   // -include
static size_t include_cursor = 0;
static bool done_preinclude = false;

// c-opts.cc:push_command_line_include.
static void push_command_line_include ()
{
  if (include_cursor > include_opts.size ())
    return;

  if (!done_preinclude)
    {
      done_preinclude = true;
      // The gcc driver pulls this in on glibc targets; it is where the
      // "# 1 \"/usr/include/stdc-predef.h\" 1 3 4" in gcc's output comes from.
      if (cpp_push_default_include (pfile, "stdc-predef.h"))
        return;
    }

  while (include_cursor < include_opts.size ())
    {
      const std::string &inc = include_opts[include_cursor++];
      if (cpp_push_include (pfile, inc.c_str ()))
        return;
    }

  if (include_cursor == include_opts.size ())
    {
      include_cursor++;
      // Restore the line map back to the main file.
      cpp_change_file (pfile, LC_RENAME, main_input_filename);
    }
}

// Ask the compiler vcpp stands in for where its system headers are, by parsing
// the "#include <...> search starts here:" block of `cc -E -v`. This is the
// same trick vcpp will use for predefined macros, and the same reason: the
// answer belongs to that compiler, not to libcpp.
static enum c_lang lang_from_std (const std::string &s)
{
  if (s.empty ())                       return CLK_GNUC17;
  if (s == "c89" || s == "c90")         return CLK_STDC89;
  if (s == "gnu89" || s == "gnu90")     return CLK_GNUC89;
  if (s == "c99" || s == "c9x")         return CLK_STDC99;
  if (s == "gnu99" || s == "gnu9x")     return CLK_GNUC99;
  if (s == "c11" || s == "c1x")         return CLK_STDC11;
  if (s == "gnu11" || s == "gnu1x")     return CLK_GNUC11;
  if (s == "c17" || s == "c18")         return CLK_STDC17;
  if (s == "gnu17" || s == "gnu18")     return CLK_GNUC17;
  if (s == "c2x" || s == "c23")         return CLK_STDC2X;
  if (s == "gnu2x" || s == "gnu23")     return CLK_GNUC2X;
  fprintf (stderr, "vcpp: declining: unrecognised -std=%s\n", s.c_str ());
  _exit (3);
}

static const char *target_cc ()
{
  const char *cc = getenv ("VCPP_CC");
  return (cc && *cc) ? cc : "cc";
}

static void probe_system_dirs (std::vector<std::string> &dirs)
{
  std::string cmd = std::string (target_cc ())
                    + " -E -v -xc /dev/null -o /dev/null 2>&1";
  FILE *p = popen (cmd.c_str (), "r");
  if (!p) return;

  char buf[4096];
  bool in_list = false;
  while (fgets (buf, sizeof buf, p))
    {
      std::string line (buf);
      while (!line.empty () && (line.back () == '\n' || line.back () == '\r'))
        line.pop_back ();
      if (line.find ("#include <...> search starts here:") != std::string::npos)
        { in_list = true; continue; }
      if (line.find ("End of search list.") != std::string::npos)
        break;
      if (in_list && !line.empty () && line[0] == ' ')
        dirs.push_back (line.substr (1));
    }
  pclose (p);
}

// The predefined macros are the compiler's, not libcpp's: __GNUC__, the target
// macros, the feature-test macros. gcc defines them in c_cpp_builtins, which
// lives in the front end. vcpp asks the compiler it is standing in for.
static void probe_predefined (std::vector<std::string> &defs)
{
  std::string cmd = std::string (target_cc ()) + " -dM -E -xc /dev/null 2>/dev/null";
  FILE *p = popen (cmd.c_str (), "r");
  if (!p) return;

  char buf[8192];
  while (fgets (buf, sizeof buf, p))
    {
      std::string line (buf);
      while (!line.empty () && (line.back () == '\n' || line.back () == '\r'))
        line.pop_back ();
      if (line.compare (0, 8, "#define ") != 0)
        continue;
      // cpp_define wants "NAME=body", not "#define NAME body".
      std::string rest = line.substr (8);
      size_t sp = rest.find (' ');
      if (sp == std::string::npos)
        defs.push_back (rest);
      else
        defs.push_back (rest.substr (0, sp) + "=" + rest.substr (sp + 1));
    }
  pclose (p);
}

// The same dump, read from a file instead of a fork.
static void load_predefined (const char *path, std::vector<std::string> &defs)
{
  FILE *f = fopen (path, "r");
  if (!f) { fprintf (stderr, "vcpp: cannot read %s\n", path); exit (2); }
  char buf[8192];
  while (fgets (buf, sizeof buf, f))
    {
      std::string line (buf);
      while (!line.empty () && (line.back () == '\n' || line.back () == '\r'))
        line.pop_back ();
      if (line.compare (0, 8, "#define ") != 0) continue;
      std::string rest = line.substr (8);
      size_t sp = rest.find (' ');
      defs.push_back (sp == std::string::npos
                      ? rest : rest.substr (0, sp) + "=" + rest.substr (sp + 1));
    }
  fclose (f);
}

// __has_attribute, __has_builtin and __has_feature are answered by gcc from
// tables that live in the front end, so they cannot be dumped as -D and vcpp
// cannot compute them. It answers from a table and declines outright on
// anything it has not been taught -- see the exit(3) below. Guessing here
// would put a wrong object in the cache, which is the one outcome that costs
// more than a slow build.
static std::map<std::string, int> has_table;   // "kind name" -> value
static bool has_table_loaded = false;

static void load_has_table (const char *path)
{
  FILE *f = fopen (path, "r");
  if (!f) { fprintf (stderr, "vcpp: cannot read %s\n", path); exit (2); }
  char kind[64], name[512];
  int val;
  while (fscanf (f, "%63s %511s %d", kind, name, &val) == 3)
    has_table[std::string (kind) + " " + name] = val;
  fclose (f);
  has_table_loaded = true;
}

static int answer_has (const char *kind, const std::string &name)
{
  auto it = has_table.find (std::string (kind) + " " + name);
  if (it != has_table.end ())
    return it->second;

  // Fail closed. vcache treats this exit status as "preprocess it with the
  // real compiler", so the build stays correct and merely loses a hit.
  fprintf (stderr,
           "vcpp: declining: no answer for __has_%s(%s)%s\n",
           kind, name.c_str (),
           has_table_loaded ? "" : " (no --has-table given)");
  fflush (stderr);
  _exit (3);
}

static const cpp_token *token_no_padding (cpp_reader *pf)
{
  for (;;)
    {
      const cpp_token *r = cpp_get_token (pf);
      if (r->type != CPP_PADDING)
        return r;
    }
}

// Shared by all four operators: parse "( name )", optionally "ns :: name",
// then consume to the closing parenthesis the way c-lex.cc does.
static std::string parse_has_operand (cpp_reader *pf)
{
  const cpp_token *token = token_no_padding (pf);
  if (token->type != CPP_OPEN_PAREN)
    return "";

  std::string name;
  token = token_no_padding (pf);
  if (token->type == CPP_NAME)
    {
      name = (const char *) cpp_token_as_text (pf, token);
      token = token_no_padding (pf);
      if (token->type == CPP_SCOPE)
        {
          token = token_no_padding (pf);
          if (token->type == CPP_NAME)
            {
              name += "::";
              name += (const char *) cpp_token_as_text (pf, token);
              token = token_no_padding (pf);
            }
        }
    }

  for (unsigned nparen = 1; ; token = token_no_padding (pf))
    {
      if (token->type == CPP_OPEN_PAREN)       ++nparen;
      else if (token->type == CPP_CLOSE_PAREN) --nparen;
      else if (token->type == CPP_EOF)         break;
      if (!nparen) break;
    }
  return name;
}

static int cb_has_attribute (cpp_reader *pf, bool std_syntax)
{ return answer_has (std_syntax ? "std-attribute" : "attribute",
                     parse_has_operand (pf)); }

static int cb_has_builtin (cpp_reader *pf)
{ return answer_has ("builtin", parse_has_operand (pf)); }

int main (int argc, char **argv)
{
  std::vector<std::string> brack_dirs, defines, undefs;
  std::vector<std::string> sys_dirs;
  // Held by value: argv-derived std::strings in the parse loop below do not
  // outlive an iteration.
  std::string predef_file, std_opt;
  const char *input = nullptr, *output = nullptr;

  for (int i = 1; i < argc; i++)
    {
      std::string a = argv[i];
      if (a == "-I" && i + 1 < argc)            brack_dirs.push_back (argv[++i]);
      else if (a.rfind ("-I", 0) == 0)          brack_dirs.push_back (a.substr (2));
      else if (a == "-D" && i + 1 < argc)       defines.push_back (argv[++i]);
      else if (a.rfind ("-D", 0) == 0)          defines.push_back (a.substr (2));
      else if (a == "-U" && i + 1 < argc)       undefs.push_back (argv[++i]);
      else if (a.rfind ("-U", 0) == 0)          undefs.push_back (a.substr (2));
      else if (a == "-include" && i + 1 < argc) include_opts.push_back (argv[++i]);
      else if (a == "-isystem" && i + 1 < argc) sys_dirs.push_back (argv[++i]);
      else if (a.rfind ("--has-table=", 0) == 0) load_has_table (a.c_str () + 12);
      else if (a.rfind ("--predef=", 0) == 0)    predef_file = a.substr (9);
      else if (a == "-o" && i + 1 < argc)       output = argv[++i];
      else if (a == "-P")                       flag_no_line_commands = true;
      else if (a.rfind ("-std=", 0) == 0)       std_opt = a.substr (5);
      else if (a == "-E")                       ;
      else if (a[0] != '-')                     input = argv[i];
    }
  if (!input) { fprintf (stderr, "vcpp: no input\n"); return 1; }
  main_input_filename = input;

  // line_maps carries allocator hooks the client must supply; gcc points these
  // at its garbage collector, vcpp at plain realloc.
  line_table = new line_maps ();
  linemap_init (line_table, BUILTINS_LOCATION);
  // After linemap_init: it placement-news the struct and would wipe these.
  // libcpp 16 renamed these with an m_ prefix; 13 spells them without it.
#if VCPP_LIBCPP_MAJOR >= 14
  line_table->m_reallocator = [] (void *p, size_t n) { return xrealloc (p, n); };
  line_table->m_round_alloc_size = [] (size_t n) { return n; };
#else
  line_table->reallocator = [] (void *p, size_t n) { return xrealloc (p, n); };
  line_table->round_alloc_size = [] (size_t n) { return n; };
#endif
  // gcc/toplev.cc sets this immediately after linemap_init, and leaving it at
  // zero is expensive as well as unfaithful: the lexer gives every token a
  // range, and with no range bits reserved in the location itself every one of
  // them has to be recorded as an ad-hoc location in a side hash table.
  // (gcc drops it back to 0 only for -flarge-source-files.)
  line_table->default_range_bits = 5;

  // The reader's language decides both how tokens are lexed and which
  // __STDC_* macros libcpp predefines, so it has to match what the target
  // compiler would use -- gcc 13 defaults to gnu17, not gnu11.
  pfile = cpp_create_reader (lang_from_std (std_opt), nullptr, line_table);
  cpp_options *opts = cpp_get_options (pfile);
  opts->traditional = 0;


  out = output ? fopen (output, "w") : stdout;
  if (!out) { perror ("vcpp"); return 1; }

  print.outf = out;
  print.src_line = 1;
  print.printed = false;
  print.prev = 0;
  print.source = 0;
  print.first_time = true;
  print.src_file = "";
  print.prev_was_system_token = false;
  do_line_adjustments = !flag_no_line_commands;

  cpp_callbacks *cb = cpp_get_callbacks (pfile);
  cb->line_change = cb_line_change;
  cb->file_change = cb_file_change;
  cb->def_pragma  = cb_def_pragma;
  cb->ident       = cb_ident;
  cb->diagnostic  = cb_diagnostic;
  // Registering these is what makes libcpp define __has_attribute and
  // __has_builtin at all -- without the callback it silently leaves them
  // undefined, and #ifdef __has_attribute then takes the wrong branch.
  cb->has_attribute = cb_has_attribute;
  cb->has_builtin   = cb_has_builtin;

  // vcpp is standing in for a specific compiler, so it has to search that
  // compiler's system directories, not its own guesses. vcache will pass them;
  // absent that, probe once the same way vcpp will probe predefined macros.
  if (sys_dirs.empty ())
    probe_system_dirs (sys_dirs);

  cpp_dir *head = nullptr, *tail = nullptr;
  for (auto &d : brack_dirs)
    {
      cpp_dir *n = new cpp_dir;
      memset (n, 0, sizeof *n);
      n->name = xstrdup (d.c_str ());
      n->len = d.size ();
      if (tail) tail->next = n; else head = n;
      tail = n;
    }
  // System directories come after the user ones, and are marked so that
  // linemarkers naming them carry gcc's " 3" system-header flag.
  for (auto &d : sys_dirs)
    {
      cpp_dir *n = new cpp_dir;
      memset (n, 0, sizeof *n);
      n->name = xstrdup (d.c_str ());
      n->len = d.size ();
      // gcc/incpath.cc: sysp = 1 + !cxx_aware, and in C mode nothing on the
      // system chain is C++-aware, so it is 2 throughout -- which is what
      // makes gcc's linemarkers say " 3 4" rather than " 3". The C++ header
      // directories are the exception, and vcpp does not do C++ yet.
      n->sysp = 2;
      if (tail) tail->next = n; else head = n;
      tail = n;
    }
  // libcpp 16 takes a fourth chain, for #embed.
#if VCPP_LIBCPP_MAJOR >= 15
  cpp_set_include_chains (pfile, head, head, nullptr, 0);
#else
  cpp_set_include_chains (pfile, head, head, 0);
#endif

  for (unsigned i = 0; i < sizeof pp_pragmas / sizeof pp_pragmas[0]; i++)
    cpp_register_deferred_pragma (pfile, pp_pragmas[i].space,
                                  pp_pragmas[i].name, PRAGMA_ID_BASE + i,
                                  /*allow_expansion=*/true, /*internal=*/false);

  cpp_post_options (pfile);
  // injecting=true: we inject a preamble (<built-in>, <command-line>,
  // stdc-predef.h) below, and libcpp starts the main file on line 0 for that
  // case so the preamble does not look included from line 1.
  if (!cpp_read_main_file (pfile, input, true))
    return 1;

  // c-opts.cc:c_finish_options. The "# 0" linemarkers at the top of gcc's
  // output are these three renames; getting them in this order, with the
  // linemap_line_start calls between, is what reproduces the preamble.
  {
    const line_map_ordinary *bltin_map
      = linemap_check_ordinary (linemap_add (line_table, LC_RENAME, 0,
                                             special_fname_builtin (), 0));
    cb_file_change (pfile, bltin_map);
    linemap_line_start (line_table, 0, 1);

    // Every builtin must carry BUILTINS_LOCATION.
    cpp_force_token_locations (pfile, BUILTINS_LOCATION);
    cpp_init_builtins (pfile, 1 /* hosted */);
    // gcc's c_cpp_builtins goes here; vcpp uses the target compiler's own dump.
    // The target compiler is authoritative where the two overlap -- its
    // __STDC_VERSION__ reflects its default -std=, libcpp's reflects whatever
    // language we opened the reader with -- so undefine first and let the dump
    // win. __STDC__ is the exception: libcpp owns it as a builtin macro and
    // warns if it is touched, and the value is 1 either way.
    // Probing costs two forks of the real compiler, which dwarfs the
    // preprocessing itself. vcache knows these already and passes them in;
    // the probe is the fallback for running vcpp by hand.
    std::vector<std::string> predefined;
    if (!predef_file.empty ())
      load_predefined (predef_file.c_str (), predefined);
    else
      probe_predefined (predefined);

    for (auto &d : predefined)
      {
        // "NAME=body" for object-like, "NAME(args)=body" for function-like:
        // the macro name ends at the first '(' or '='.
        size_t end = d.find_first_of ("(=");
        std::string name = d.substr (0, end);

        // libcpp defines a handful itself (__STDC__, __STDC_VERSION__,
        // __STDC_HOSTED__, the __STDC_UTF_*), flags them so that touching them
        // warns, and -- now that the reader is opened in the target's language
        // -- already agrees with the dump. Leave those alone.
        if (cpp_defined (pfile, (const unsigned char *) name.data (),
                         (int) name.size ()))
          continue;
        cpp_define (pfile, d.c_str ());
      }
    cpp_stop_forcing_token_locations (pfile);

    const line_map_ordinary *cmd_map
      = linemap_check_ordinary (linemap_add (line_table, LC_RENAME, 0,
                                             "<command-line>", 0));
    cb_file_change (pfile, cmd_map);
    linemap_line_start (line_table, 0, 1);

    // All command-line defines share one location. libcpp 14 added a field
    // recording it; 13 only forces the location.
#if VCPP_LIBCPP_MAJOR >= 14
    line_table->cmdline_location = line_table->highest_line;
#endif
    cpp_force_token_locations (pfile, line_table->highest_line);
    for (auto &d : defines) cpp_define (pfile, d.c_str ());
    for (auto &u : undefs)  cpp_undef  (pfile, u.c_str ());
    cpp_stop_forcing_token_locations (pfile);
  }

  include_cursor = 0;
  push_command_line_include ();

  // c-ppoutput.cc:scan_translation_unit.
  print.source = NULL;
  for (;;)
    {
      location_t loc;
      const cpp_token *tok = cpp_get_token_with_location (pfile, &loc);
      stream_token (pfile, tok, loc);
      if (tok->type == CPP_EOF)
        break;
    }

  if (print.printed)
    putc ('\n', print.outf);

  cpp_finish (pfile, nullptr);
  cpp_destroy (pfile);
  if (output) fclose (out);
  return 0;
}
