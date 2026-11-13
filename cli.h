/* cli.h - Command-line option parsing for svg2img. */
#ifndef SVG2IMG_CLI_H
#define SVG2IMG_CLI_H

#include "svg2img.h"

void cli_usage(const char* prog);

/* Returns 0 = run, 1 = clean exit (help), 2 = usage error. */
int cli_parse(int argc, char** argv, svg2img_options* opts,
              const char** in, const char** out);

#endif