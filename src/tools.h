/* tools.h - agent tools for amicode, implemented with AmigaDOS.
 *
 * The agent's tools run Amiga-side because the whole point is an agent that
 * manipulates *this* Amiga. Each tool maps to native dos.library calls:
 *   read_file    -> Open/Read
 *   write_file   -> Open(MODE_NEWFILE)/Write     (approval-gated)
 *   list_dir     -> Lock/Examine/ExNext
 *   run_command  -> SystemTags (output captured to a temp file)  (approval-gated)
 */
#ifndef AMICODE_TOOLS_H
#define AMICODE_TOOLS_H

#include <exec/types.h>
#include "json/cJSON.h"

/* Build the Anthropic "tools" array (name/description/input_schema per tool).
 * Caller owns the returned cJSON (cJSON_Delete). NULL on allocation failure. */
cJSON *tools_definitions(void);

/* Execute tool `name` with the given `input` object. Returns a malloc'd,
 * NUL-terminated result string (caller frees). On failure returns an error
 * message string and sets *is_error = TRUE. Write/command tools may prompt the
 * user for approval on the console and return a "declined" result if refused. */
char *tools_execute(const char *name, const cJSON *input, BOOL *is_error);

#endif /* AMICODE_TOOLS_H */
