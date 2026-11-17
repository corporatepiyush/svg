/* cli.h - Command-line option parsing for svg2img. */
#ifndef SVG2IMG_CLI_H
#define SVG2IMG_CLI_H

#include "svg2img.h"

void cli_usage(const char* prog);

/* Returns 0 = run, 1 = clean exit (help), 2 = usage error. */
int cli_parse(int argc, char** argv, svg2img_options* opts,
              const char** in, const char** out);

/* Override individual resource limits from SVG2IMG_LIMIT_<NAME> environment
 * variables.  Used by fuzz/limit_test.py to exercise every limit without
 * growing the option surface; production callers use opts->limits.  A value
 * of 0 or an unparsable value leaves the default in place. */
void cli_limits_from_env(svg2img_options* opts);

#endif