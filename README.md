# dg
> (d)ocumentation (g)enerator generates docs from markdown

# Build

Requires libcmark and its development headers.

I'm new to C so I mostly have no clue what I'm doing. Maybe this is not the best dev run command.

```bash
gcc -Wall -Wextra -g main.c -o main -lcmark
```

# Usage

```text
./main [INPUT_DIR [OUTPUT_DIR [CSS_FILE]]]
```

Defaults to the current directory and `output`. CSS is optional.

```bash
./main input output marx.min.css
./main --help
```

# LICENSE
[MIT](LICENSE)
